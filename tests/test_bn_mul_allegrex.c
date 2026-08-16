/*
 * Host-side semantic proof for the Allegrex MULADDC core added by
 * patches/mbedtls-3.6.6-psp-bnmul.patch.
 *
 * The patch replaces mbed TLS's generic MIPS32 multiply-accumulate inner
 * loop with an Allegrex maddu sequence.  A wrong carry there corrupts
 * signatures silently, so the algorithm is proved here, on the host,
 * before any device build runs it: the maddu accumulator is modelled
 * exactly (mtlo/mthi seed a 64-bit HI:LO pair; each maddu adds a 32x32
 * unsigned product to it; mflo/mfhi split it) and the model is compared,
 * limb for limb, with both portable MULADDC_X1_CORE variants that mbed
 * TLS ships in library/bn_mul.h.
 *
 * This is a semantic check, not an instruction check: it proves the
 * algorithm the assembly encodes, and it is the gate that must pass
 * before the PSP selftest EBOOT is even built.  The assembly itself is
 * proved on the emulated CPU by the crypto selftest EBOOT (which runs
 * mbed TLS's own mpi/rsa/ecp selftests against the patched library).
 *
 * Keep this file in sync with the asm in the patch by hand: the three
 * reference implementations below are transcriptions, and any edit to
 * the accumulator order in the patch must be mirrored here.
 */

#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define CHECK(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "CHECK failed at %s:%d: %s\n", \
                __FILE__, __LINE__, #condition); \
        return 1; \
    } \
} while (0)

/* mbedtls_mpi_uint on the PSP: MBEDTLS_HAVE_INT32. */
typedef uint32_t mpi_uint;

#define BIL 32
#define BIH 16

/*
 * Reference 1: mbed TLS's portable MULADDC_X1_CORE, MBEDTLS_HAVE_UDBL
 * branch (bn_mul.h, "C (longlong)").  This is the definition of correct.
 */
static mpi_uint ref_udbl(mpi_uint s, mpi_uint b, mpi_uint c, mpi_uint *d)
{
    uint64_t r;
    mpi_uint r0, r1;

    r  = s * (uint64_t) b;
    r0 = (mpi_uint) r;
    r1 = (mpi_uint) (r >> BIL);
    r0 += c;   r1 += (r0 < c);
    r0 += *d;  r1 += (r0 < *d);
    *d = r0;
    return r1;
}

/*
 * Reference 2: mbed TLS's portable MULADDC_X1_CORE, no-UDBL branch
 * (bn_mul.h, "C (generic)").  Independent derivation of the same value;
 * agreeing with both is a stronger statement than agreeing with one.
 */
static mpi_uint ref_generic(mpi_uint s, mpi_uint b, mpi_uint c, mpi_uint *d)
{
    mpi_uint s0, s1, b0, b1;
    mpi_uint r0, r1, rx, ry;

    b0 = (mpi_uint) ((b << BIH) >> BIH);
    b1 = (mpi_uint) (b >> BIH);

    s0 = (mpi_uint) ((s << BIH) >> BIH);
    s1 = (mpi_uint) (s >> BIH);
    rx = s0 * b1;  r0 = s0 * b0;
    ry = s1 * b0;  r1 = s1 * b1;
    r1 += (rx >> BIH);
    r1 += (ry >> BIH);
    rx = (mpi_uint) (rx << BIH);
    ry = (mpi_uint) (ry << BIH);
    r0 += rx; r1 += (r0 < rx);
    r0 += ry; r1 += (r0 < ry);
    r0 +=  c; r1 += (r0 <  c);
    r0 += *d; r1 += (r0 < *d);
    *d = r0;
    return r1;
}

/*
 * The model under test: the Allegrex sequence, instruction for
 * instruction.
 *
 *     mtlo  c            LO = c
 *     mthi  $0           HI = 0
 *     maddu t, b         HI:LO += (uint64) t * (uint64) b
 *     maddu u, one       HI:LO += (uint64) u * 1
 *     mflo  t            t = LO
 *     mfhi  c            c = HI
 *
 * maddu adds into the full 64-bit HI:LO pair with the carry out of LO
 * propagating into HI in hardware; that is what "acc +=" models.  The
 * accumulator is proved not to wrap below.
 */
static mpi_uint model_allegrex(mpi_uint s, mpi_uint b, mpi_uint c, mpi_uint *d)
{
    uint64_t acc;
    mpi_uint t = s;
    mpi_uint u = *d;
    const mpi_uint one = 1;

    acc  = (uint64_t) c;                       /* mtlo c ; mthi $0 */
    acc += (uint64_t) t * (uint64_t) b;        /* maddu t, b       */
    acc += (uint64_t) u * (uint64_t) one;      /* maddu u, one     */

    *d = (mpi_uint) acc;                       /* mflo             */
    return (mpi_uint) (acc >> BIL);            /* mfhi             */
}

/* xoshiro-ish deterministic PRNG; no libc rand dependence. */
static uint64_t rng_state = 0x9E3779B97F4A7C15ull;

static uint64_t rng_next(void)
{
    uint64_t x = rng_state;
    x ^= x << 13;
    x ^= x >> 7;
    x ^= x << 17;
    rng_state = x;
    return x;
}

static mpi_uint rng_limb(void)
{
    /*
     * Mix uniform limbs with limbs biased toward the carry-producing
     * extremes (0, 1, 0xffffffff, and values one step either side of a
     * 16-bit boundary), because that is where a wrong carry hides.
     */
    uint64_t r = rng_next();
    switch ((unsigned) (r & 7u)) {
        case 0: return 0u;
        case 1: return 0xffffffffu;
        case 2: return 1u;
        case 3: return 0xfffffffeu;
        case 4: return (mpi_uint) (0xffff0000u | (r >> 48));
        case 5: return (mpi_uint) ((r >> 32) & 0xffffu);
        default: return (mpi_uint) (r >> 32);
    }
}

static int check_one(mpi_uint s, mpi_uint b, mpi_uint c, mpi_uint d,
                     unsigned long long *checked)
{
    mpi_uint d_ref_udbl = d, d_ref_generic = d, d_model = d;
    mpi_uint c_ref_udbl = ref_udbl(s, b, c, &d_ref_udbl);
    mpi_uint c_ref_generic = ref_generic(s, b, c, &d_ref_generic);
    mpi_uint c_model = model_allegrex(s, b, c, &d_model);

    if (c_ref_udbl != c_ref_generic || d_ref_udbl != d_ref_generic) {
        fprintf(stderr,
                "reference disagreement s=%08" PRIx32 " b=%08" PRIx32
                " c=%08" PRIx32 " d=%08" PRIx32 "\n", s, b, c, d);
        return 1;
    }
    if (c_model != c_ref_udbl || d_model != d_ref_udbl) {
        fprintf(stderr,
                "MISMATCH s=%08" PRIx32 " b=%08" PRIx32 " c=%08" PRIx32
                " d=%08" PRIx32 ": model d=%08" PRIx32 " c=%08" PRIx32
                ", reference d=%08" PRIx32 " c=%08" PRIx32 "\n",
                s, b, c, d, d_model, c_model, d_ref_udbl, c_ref_udbl);
        return 1;
    }
    (*checked)++;
    return 0;
}

int main(void)
{
    static const mpi_uint edges[] = {
        0u, 1u, 2u, 3u,
        0x0000ffffu, 0x00010000u, 0x00010001u,
        0x7fffffffu, 0x80000000u, 0x80000001u,
        0xfffefffeu, 0xffff0000u, 0xfffffffdu, 0xfffffffeu, 0xffffffffu,
    };
    const size_t edge_count = sizeof(edges) / sizeof(edges[0]);
    unsigned long long checked = 0;

    puts("test: Allegrex MULADDC core matches the portable mbed TLS core");

    /*
     * 1. Exhaustive over the carry-critical corners: 15^4 = 50625 vectors
     *    covering every combination of the values that straddle a carry
     *    boundary in either half of the accumulator.
     */
    for (size_t i = 0; i < edge_count; i++) {
        for (size_t j = 0; j < edge_count; j++) {
            for (size_t k = 0; k < edge_count; k++) {
                for (size_t l = 0; l < edge_count; l++) {
                    CHECK(check_one(edges[i], edges[j], edges[k], edges[l],
                                    &checked) == 0);
                }
            }
        }
    }
    printf("  corner vectors: %llu\n", checked);

    /*
     * 2. The exact maximum: every operand at its ceiling.  This is the
     *    vector that proves the accumulator cannot wrap --
     *    (2^32-1)^2 + (2^32-1) + (2^32-1) == 2^64 - 1.
     */
    {
        const uint64_t umax = 0xffffffffu;
        const uint64_t worst = umax * umax + umax + umax;
        CHECK(worst == 0xffffffffffffffffull);
        mpi_uint d = 0xffffffffu;
        mpi_uint c = model_allegrex(0xffffffffu, 0xffffffffu, 0xffffffffu, &d);
        CHECK(d == 0xffffffffu && c == 0xffffffffu);
        puts("  accumulator ceiling: 2^64-1 exactly, no wrap");
    }

    /*
     * 3. Randomized bulk fuzz.  Four million vectors with limbs biased
     *    toward the carry extremes.
     */
    {
        const unsigned long long rounds = 4000000ull;
        for (unsigned long long n = 0; n < rounds; n++) {
            CHECK(check_one(rng_limb(), rng_limb(), rng_limb(), rng_limb(),
                            &checked) == 0);
        }
    }

    /*
     * 4. Whole-row fuzz: run the model and the reference as complete
     *    mbedtls_mpi_core_mla rows (64 limbs, the 2048-bit shape) so the
     *    carry chain between limbs is exercised, not just one limb in
     *    isolation.  Rows are the shape RSA and ECP actually use.
     */
    {
        enum { ROW_LIMBS = 64, ROWS = 20000 };
        for (unsigned row = 0; row < ROWS; row++) {
            mpi_uint s[ROW_LIMBS], d_ref[ROW_LIMBS], d_model[ROW_LIMBS];
            mpi_uint b = rng_limb();
            for (size_t i = 0; i < ROW_LIMBS; i++) {
                s[i] = rng_limb();
                d_ref[i] = rng_limb();
                d_model[i] = d_ref[i];
            }
            mpi_uint c_ref = 0, c_model = 0;
            for (size_t i = 0; i < ROW_LIMBS; i++) {
                c_ref = ref_udbl(s[i], b, c_ref, &d_ref[i]);
                c_model = model_allegrex(s[i], b, c_model, &d_model[i]);
                checked++;
            }
            CHECK(c_ref == c_model);
            CHECK(memcmp(d_ref, d_model, sizeof(d_ref)) == 0);
        }
        puts("  row fuzz: 20000 rows of 64 limbs, carry chain identical");
    }

    printf("  total limb comparisons: %llu\n", checked);
    puts("ok");
    return 0;
}
