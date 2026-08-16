/* Crypto selftest EBOOT -- the device-side gate for the Allegrex bignum
   core (docs/engineering/PSP_TRANSPORT.md) and for the
   Project Everest x25519 swap (same doc, Everest note).

   A sibling of the qualification fixture EBOOT (src/psp_main.c): same harness
   shape (a PSP frontend that prints tab-separated lines to stdout, which
   PPSSPP folds into its --log= file and a script greps), its own output
   directory so it can never replace the browser or fixture EBOOT, and no
   engine dependency at all. Unlike the fixture it exits on its own, so a
   headless runner does not have to kill it.

   Three things run here, in this order:

     1. Mbed TLS's own selftests -- mpi, rsa, ecp, plus an ECDSA P-256
        sign/verify round trip that the library has no packaged selftest
        for. These bottom out in mbedtls_mpi_core_mla, i.e. in MULADDC,
        i.e. in the assembly under test. A wrong carry fails them.
     2. Baked-in known-answer vectors for mbedtls_mpi_mul_mpi, computed
        on the host and checked here as hex strings. The selftests prove
        the library agrees with itself; these prove it agrees with a
        second implementation on another machine.
     2b. RFC 7748 X25519 known-answer vectors (the two section-5.2 scalar
        mults and the section-6.1 Diffie-Hellman), each computed both by
        the generic ECP Montgomery ladder (the Everest-OFF path) and, when
        the library is built with MBEDTLS_ECDH_VARIANT_EVEREST_ENABLED, by
        Everest's HACL* scalarmult. Every result is checked against the
        published constant and the two paths against each other.
     3. Timing: N iterations of a 2048x2048-bit mbedtls_mpi_mul_mpi, one
        ECDSA P-256 sign plus one verify, and N X25519 scalar mults on the
        generic and Everest paths (the OFF vs ON ratio), via
        sceKernelGetSystemTimeWide.

   Everything is reported as `tilefinch-crypto: key=value` lines, with a
   final `tilefinch-crypto: outcome=pass|fail` sentinel. The build stamps
   which MULADDC core it was compiled against so a log can never be
   mistaken for the other configuration's. */

#include <pspkernel.h>
#include <pspdebug.h>

#include <errno.h>
#include <stdbool.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "tilefinch/psp_display.h"
#include "tilefinch/psp_threads.h"

#include "mbedtls/bignum.h"
#include "mbedtls/ctr_drbg.h"
#include "mbedtls/ecdh.h"
#include "mbedtls/ecdsa.h"
#include "mbedtls/ecp.h"
#include "mbedtls/entropy.h"
#include "mbedtls/rsa.h"
#include "mbedtls/sha256.h"

#if defined(MBEDTLS_ECDH_VARIANT_EVEREST_ENABLED)
/* Project Everest's RFC 7748 scalar multiplication, as linked from
   libeverest.a when the mbedTLS build defines
   MBEDTLS_ECDH_VARIANT_EVEREST_ENABLED. This is exactly the primitive
   mbed TLS's ecdh routes Curve25519 through with Everest on; declaring the
   prototype here (rather than pulling in everest/Hacl_Curve25519.h and its
   kremlib dependencies) keeps the fixture's include surface small. Every
   argument is a 32-byte little-endian buffer; clamping and u-coordinate
   masking happen inside HACL* per the RFC. */
extern void Hacl_Curve25519_crypto_scalarmult(uint8_t *mypublic,
                                              uint8_t *secret,
                                              uint8_t *basepoint);
#endif

PSP_MODULE_INFO("Tilefinch Crypto Selftest", 0, 0, 1);
PSP_MAIN_THREAD_ATTR(PSP_THREAD_ATTR_USER | PSP_THREAD_ATTR_VFPU);
PSP_HEAP_SIZE_KB(-1024);

#if defined(TILEFINCH_ALLEGREX_MULADDC)
#define TILEFINCH_MULADDC_CORE "allegrex-maddu"
#else
#define TILEFINCH_MULADDC_CORE "stock-mips32"
#endif

/* 2048-bit multiplies to time. Small enough that the whole EBOOT stays
   well inside PPSSPP's patience, large enough that the per-call overhead
   is noise next to the inner loop. */
#define MUL_ITERATIONS 200

static int failures;

static mbedtls_entropy_context entropy;
static mbedtls_ctr_drbg_context drbg;

#define CRYPTO_LOG_PATH_LIMIT 512u

static FILE *crypto_log_file;
static char crypto_log_path[CRYPTO_LOG_PATH_LIMIT];
static char crypto_log_temporary[CRYPTO_LOG_PATH_LIMIT];
static int crypto_log_error;
static PspDisplay crypto_display;

static void crypto_sibling_path(
    char *output, size_t capacity, const char *argv0, const char *leaf)
{
    if (output == NULL || capacity == 0) return;
    const char *program = argv0 == NULL || argv0[0] == '\0'
        ? "ms0:/PSP/GAME/TILEFINCH-CRYPTO-SELFTEST/EBOOT.PBP" : argv0;
    const char *slash = strrchr(program, '/');
    const char *backslash = strrchr(program, '\\');
    if (backslash != NULL && (slash == NULL || backslash > slash))
        slash = backslash;
    char current_directory[CRYPTO_LOG_PATH_LIMIT];
    if (slash == NULL && getcwd(
            current_directory, sizeof(current_directory)) != NULL) {
        size_t used = strlen(current_directory);
        snprintf(output, capacity, "%s%s%s",
                 current_directory,
                 used != 0u && current_directory[used - 1u] == '/' ? "" : "/",
                 leaf == NULL ? "" : leaf);
        return;
    }
    size_t directory_length = slash == NULL
        ? strlen("ms0:/PSP/GAME/TILEFINCH-CRYPTO-SELFTEST/")
        : (size_t) (slash - program + 1);
    if (slash == NULL)
        program = "ms0:/PSP/GAME/TILEFINCH-CRYPTO-SELFTEST/";
    if (directory_length >= capacity) directory_length = capacity - 1u;
    memcpy(output, program, directory_length);
    output[directory_length] = '\0';
    if (leaf != NULL && directory_length < capacity - 1u)
        snprintf(output + directory_length, capacity - directory_length,
                 "%s", leaf);
}

static void crypto_log_open(const char *argv0)
{
    crypto_sibling_path(
        crypto_log_path, sizeof(crypto_log_path), argv0,
        "crypto-selftest.txt");
    crypto_sibling_path(
        crypto_log_temporary, sizeof(crypto_log_temporary), argv0,
        "crypto-selftest.tmp");
    crypto_log_file = fopen(crypto_log_temporary, "wb");
    if (crypto_log_file == NULL) crypto_log_error = errno;
}

static int crypto_printf(const char *format, ...)
{
    va_list console_arguments;
    va_start(console_arguments, format);
    int result = vfprintf(stdout, format, console_arguments);
    va_end(console_arguments);
    if (crypto_log_file != NULL) {
        va_list log_arguments;
        va_start(log_arguments, format);
        (void) vfprintf(crypto_log_file, format, log_arguments);
        va_end(log_arguments);
    }
    return result;
}

static bool crypto_screen_prepare(void)
{
    uint16_t *pixels = psp_display_back_buffer(&crypto_display);
    if (pixels == NULL) return false;
    pspDebugScreenInitEx(pixels, PSP_DISPLAY_FORMAT_RGB565, 0);
    pspDebugScreenSetBackColor(0xFF171312u);
    pspDebugScreenSetTextColor(0xFFF4E9DFu);
    pspDebugScreenClear();
    pspDebugScreenSetXY(0, 0);
    return true;
}

static bool crypto_log_publish(void)
{
    if (crypto_log_file == NULL) return false;
    if (fflush(crypto_log_file) != 0) crypto_log_error = errno;
    if (fclose(crypto_log_file) != 0 && crypto_log_error == 0)
        crypto_log_error = errno;
    crypto_log_file = NULL;
    if (crypto_log_error != 0) return false;
    /* PSP FAT does not replace an existing destination with rename(). */
    if (remove(crypto_log_path) != 0 && errno != ENOENT) {
        crypto_log_error = errno;
        return false;
    }
    if (rename(crypto_log_temporary, crypto_log_path) != 0) {
        crypto_log_error = errno;
        return false;
    }
    return true;
}

/* Keep PPSSPP/stdout telemetry and mirror the exact same bounded lines to the
   Memory Stick for a physical-device run. */
#define printf crypto_printf

static int psp_exit_callback(int arg1, int arg2, void *common)
{
    (void) arg1; (void) arg2; (void) common;
    sceKernelExitGame();
    return 0;
}

static int psp_callback_thread(SceSize args, void *argp)
{
    (void) args; (void) argp;
    int id = sceKernelCreateCallback("exit", psp_exit_callback, NULL);
    if (id < 0) {
        printf("crypto-selftest: exit callback create failed 0x%08x\n",
               (unsigned) id);
        return id;
    }
    int registered = sceKernelRegisterExitCallback(id);
    if (registered < 0) {
        printf("crypto-selftest: exit callback register failed 0x%08x\n",
               (unsigned) registered);
        return registered;
    }
    sceKernelSleepThreadCB();
    return 0;
}

static void psp_setup_callbacks(void)
{
    int thid = sceKernelCreateThread(
        "update_thread", psp_callback_thread,
        TILEFINCH_PSP_THREAD_PRIORITY_CALLBACK, 0xFA0, 0, 0);
    int started = thid < 0 ? thid : sceKernelStartThread(thid, 0, 0);
    printf("crypto-selftest: callback thread=%d started=0x%08x\n",
           thid, (unsigned) started);
}

static void report_step(const char *name, int rc)
{
    if (rc != 0) {
        failures++;
    }
    printf("tilefinch-crypto: step=%s result=%s rc=%d\n",
           name, rc == 0 ? "pass" : "FAIL", rc);
}

/* ------------------------------------------------------------------ */
/* 1. Mbed TLS selftests                                               */
/* ------------------------------------------------------------------ */

static int run_library_selftests(void)
{
    /* verbose=0: these print their own noise otherwise, and the return
       code is the whole signal. */
    report_step("mpi_self_test", mbedtls_mpi_self_test(0));
    report_step("rsa_self_test", mbedtls_rsa_self_test(0));
    report_step("ecp_self_test", mbedtls_ecp_self_test(0));
    return failures;
}

/* ------------------------------------------------------------------ */
/* 2. Known-answer mpi_mul_mpi vectors                                 */
/* ------------------------------------------------------------------ */

/* Each vector is {a, b, a*b} in hex, computed on the host by an
   independent bignum (see docs/engineering/PSP_TRANSPORT.md).
   The last one is the all-ones square at 2048 bits: the single worst
   case for carry propagation, every limb saturated. */
struct mul_vector {
    const char *name;
    const char *a;
    const char *b;
    const char *product;
};

#include "psp_crypto_selftest_vectors.h"

static int run_mul_vectors(void)
{
    mbedtls_mpi a, b, want, got;
    char text[2048];
    size_t len;
    int bad = 0;

    mbedtls_mpi_init(&a);
    mbedtls_mpi_init(&b);
    mbedtls_mpi_init(&want);
    mbedtls_mpi_init(&got);

    for (size_t i = 0; i < sizeof(mul_vectors) / sizeof(mul_vectors[0]); i++) {
        const struct mul_vector *v = &mul_vectors[i];
        int rc = mbedtls_mpi_read_string(&a, 16, v->a);
        if (rc == 0) rc = mbedtls_mpi_read_string(&b, 16, v->b);
        if (rc == 0) rc = mbedtls_mpi_read_string(&want, 16, v->product);
        if (rc == 0) rc = mbedtls_mpi_mul_mpi(&got, &a, &b);
        if (rc != 0) {
            printf("tilefinch-crypto: vector=%s result=FAIL rc=%d\n",
                   v->name, rc);
            bad++;
            continue;
        }
        if (mbedtls_mpi_cmp_mpi(&got, &want) != 0) {
            bad++;
            printf("tilefinch-crypto: vector=%s result=FAIL reason=mismatch\n",
                   v->name);
            if (mbedtls_mpi_write_string(&got, 16, text, sizeof(text),
                                         &len) == 0) {
                printf("tilefinch-crypto: vector=%s got=%s\n", v->name, text);
            }
            continue;
        }
        /* Commutativity and the squaring path share the same inner loop
           but not the same operand lengths; check both directions. */
        if (mbedtls_mpi_mul_mpi(&got, &b, &a) != 0
            || mbedtls_mpi_cmp_mpi(&got, &want) != 0) {
            bad++;
            printf("tilefinch-crypto: vector=%s result=FAIL reason=commute\n",
                   v->name);
            continue;
        }
        printf("tilefinch-crypto: vector=%s result=pass\n", v->name);
    }

    mbedtls_mpi_free(&a);
    mbedtls_mpi_free(&b);
    mbedtls_mpi_free(&want);
    mbedtls_mpi_free(&got);
    report_step("mul_vectors", bad);
    return bad;
}

/* A self-consistency sweep that does not need baked answers: for random
   operand shapes, check (a*b)*c == a*(b*c) and a*(b+c) == a*b + a*c over
   sizes that cross the X8/X1 boundary in mbedtls_mpi_core_mla (limb
   counts 1..24 hit every remainder). Deterministic input, no RNG needed
   on device -- the values are derived from a counter. */
static int run_mul_identities(void)
{
    mbedtls_mpi a, b, c, l, r, t;
    int bad = 0;

    mbedtls_mpi_init(&a); mbedtls_mpi_init(&b); mbedtls_mpi_init(&c);
    mbedtls_mpi_init(&l); mbedtls_mpi_init(&r); mbedtls_mpi_init(&t);

    unsigned long long seed = 0x9E3779B97F4A7C15ull;
    for (int limbs = 1; limbs <= 24 && bad == 0; limbs++) {
        for (int round = 0; round < 8 && bad == 0; round++) {
            if (mbedtls_mpi_grow(&a, (size_t) limbs) != 0
                || mbedtls_mpi_grow(&b, (size_t) limbs) != 0
                || mbedtls_mpi_grow(&c, (size_t) limbs) != 0) {
                bad++;
                break;
            }
            for (int i = 0; i < limbs; i++) {
                seed ^= seed << 13; seed ^= seed >> 7; seed ^= seed << 17;
                a.MBEDTLS_PRIVATE(p)[i] = (mbedtls_mpi_uint) (seed >> 32);
                seed ^= seed << 13; seed ^= seed >> 7; seed ^= seed << 17;
                b.MBEDTLS_PRIVATE(p)[i] = (mbedtls_mpi_uint) (seed >> 32);
                seed ^= seed << 13; seed ^= seed >> 7; seed ^= seed << 17;
                /* Saturate one limb per round so the carry chain is
                   forced, not merely likely. */
                c.MBEDTLS_PRIVATE(p)[i] =
                    (i == round % (limbs ? limbs : 1))
                        ? (mbedtls_mpi_uint) -1
                        : (mbedtls_mpi_uint) (seed >> 32);
            }

            /* (a*b)*c == a*(b*c) */
            if (mbedtls_mpi_mul_mpi(&t, &a, &b) != 0
                || mbedtls_mpi_mul_mpi(&l, &t, &c) != 0
                || mbedtls_mpi_mul_mpi(&t, &b, &c) != 0
                || mbedtls_mpi_mul_mpi(&r, &a, &t) != 0
                || mbedtls_mpi_cmp_mpi(&l, &r) != 0) {
                printf("tilefinch-crypto: identity=assoc limbs=%d round=%d "
                       "result=FAIL\n", limbs, round);
                bad++;
                break;
            }

            /* a*(b+c) == a*b + a*c */
            if (mbedtls_mpi_add_mpi(&t, &b, &c) != 0
                || mbedtls_mpi_mul_mpi(&l, &a, &t) != 0
                || mbedtls_mpi_mul_mpi(&r, &a, &b) != 0
                || mbedtls_mpi_mul_mpi(&t, &a, &c) != 0
                || mbedtls_mpi_add_mpi(&r, &r, &t) != 0
                || mbedtls_mpi_cmp_mpi(&l, &r) != 0) {
                printf("tilefinch-crypto: identity=distrib limbs=%d round=%d "
                       "result=FAIL\n", limbs, round);
                bad++;
                break;
            }
        }
    }

    mbedtls_mpi_free(&a); mbedtls_mpi_free(&b); mbedtls_mpi_free(&c);
    mbedtls_mpi_free(&l); mbedtls_mpi_free(&r); mbedtls_mpi_free(&t);
    report_step("mul_identities", bad);
    return bad;
}

/* ------------------------------------------------------------------ */
/* 2b. X25519 known-answer vectors, generic path vs Everest            */
/* ------------------------------------------------------------------ */

/* RFC 7748 section 5.2 and 6.1 constants. Every string is a 32-byte
   little-endian value in hex, exactly as the RFC prints them: the raw
   buffers X25519 consumes and produces, no byte-order fixups. */
/* Correctness is covered by the RFC vectors above. This loop is telemetry,
   not additional coverage: 200 generic scalar multiplications took more
   than 23 seconds on a PSP-3000 and made a passing self-test look hung. */
#define X25519_TIMING_ITERATIONS 20

static int hex32(const char *hex, unsigned char out[32])
{
    for (int i = 0; i < 32; i++) {
        unsigned int byte;
        if (sscanf(hex + 2 * i, "%2x", &byte) != 1) {
            return -1;
        }
        out[i] = (unsigned char) byte;
    }
    return 0;
}

/* The generic ECP Montgomery ladder -- the path that runs with Everest
   OFF. We clamp the scalar and mask the u-coordinate exactly as RFC 7748's
   decodeScalar25519/decodeUCoordinate do, because mbedtls_ecp_mul consumes
   an already-decoded scalar and point (its own ecdh does the same before
   calling it). Randomized projective coordinates need an RNG, hence f_rng. */
static int generic_x25519(mbedtls_ctr_drbg_context *rng,
                          const unsigned char scalar[32],
                          const unsigned char point[32],
                          unsigned char out[32])
{
    mbedtls_ecp_group grp;
    mbedtls_ecp_point P, R;
    mbedtls_mpi m;
    unsigned char k[32], u[32];
    int rc;

    mbedtls_ecp_group_init(&grp);
    mbedtls_ecp_point_init(&P);
    mbedtls_ecp_point_init(&R);
    mbedtls_mpi_init(&m);

    memcpy(k, scalar, sizeof(k));
    k[0] &= 248;
    k[31] &= 127;
    k[31] |= 64;
    memcpy(u, point, sizeof(u));
    u[31] &= 127;

    rc = mbedtls_ecp_group_load(&grp, MBEDTLS_ECP_DP_CURVE25519);
    if (rc == 0) rc = mbedtls_mpi_read_binary_le(&m, k, sizeof(k));
    if (rc == 0) rc = mbedtls_mpi_read_binary_le(&P.MBEDTLS_PRIVATE(X),
                                                 u, sizeof(u));
    if (rc == 0) rc = mbedtls_mpi_lset(&P.MBEDTLS_PRIVATE(Z), 1);
    if (rc == 0) rc = mbedtls_ecp_mul(&grp, &R, &m, &P,
                                      mbedtls_ctr_drbg_random, rng);
    if (rc == 0) rc = mbedtls_mpi_write_binary_le(&R.MBEDTLS_PRIVATE(X),
                                                  out, sizeof(k));

    mbedtls_mpi_free(&m);
    mbedtls_ecp_point_free(&P);
    mbedtls_ecp_point_free(&R);
    mbedtls_ecp_group_free(&grp);
    return rc;
}

/* Compute one X25519(scalar, point), check both the generic path and (when
   built) Everest against the published constant, and check they agree with
   each other. Returns nonzero on any failure. */
static int check_x25519(mbedtls_ctr_drbg_context *rng, const char *name,
                        const char *scalar_hex, const char *point_hex,
                        const char *want_hex)
{
    unsigned char scalar[32], point[32], want[32], got_generic[32];
    int bad = 0;

    if (hex32(scalar_hex, scalar) != 0 || hex32(point_hex, point) != 0
        || hex32(want_hex, want) != 0) {
        printf("tilefinch-crypto: x25519=%s result=FAIL reason=hex\n", name);
        return 1;
    }

    if (generic_x25519(rng, scalar, point, got_generic) != 0) {
        printf("tilefinch-crypto: x25519=%s result=FAIL reason=generic-error\n",
               name);
        bad++;
    } else if (memcmp(got_generic, want, 32) != 0) {
        printf("tilefinch-crypto: x25519=%s path=generic result=FAIL "
               "reason=mismatch\n", name);
        bad++;
    } else {
        printf("tilefinch-crypto: x25519=%s path=generic result=pass\n", name);
    }

#if defined(MBEDTLS_ECDH_VARIANT_EVEREST_ENABLED)
    {
        unsigned char got_everest[32];
        Hacl_Curve25519_crypto_scalarmult(got_everest, scalar, point);
        if (memcmp(got_everest, want, 32) != 0) {
            printf("tilefinch-crypto: x25519=%s path=everest result=FAIL "
                   "reason=mismatch\n", name);
            bad++;
        } else if (memcmp(got_everest, got_generic, 32) != 0) {
            printf("tilefinch-crypto: x25519=%s result=FAIL "
                   "reason=everest-vs-generic-disagree\n", name);
            bad++;
        } else {
            printf("tilefinch-crypto: x25519=%s path=everest result=pass\n",
                   name);
        }
    }
#endif
    return bad;
}

static int run_x25519_vectors(mbedtls_ctr_drbg_context *rng)
{
    int bad = 0;

    /* RFC 7748 section 5.2: the two documented scalar-mult results. */
    bad += check_x25519(rng, "rfc7748-5.2-1",
        "a546e36bf0527c9d3b16154b82465edd62144c0ac1fc5a18506a2244ba449ac4",
        "e6db6867583030db3594c1a424b15f7c726624ec26b3353b10a903a6d0ab1c4c",
        "c3da55379de9c6908e94ea4df28d084f32eccf03491c71f754b4075577a28552");
    bad += check_x25519(rng, "rfc7748-5.2-2",
        "4b66e9d4d1b4673c5ad22691957d6af5c11b6421e0ea01d42ca4169e7918ba0d",
        "e5210f12786811d3f4b7959d0538ae2c31dbe7106fc03c3efc4cd549c715a493",
        "95cbde9476e8907d7aade45cb4b873f88b595a68799fa152e6f8f7647aac7957");

    /* RFC 7748 section 6.1: a full X25519 Diffie-Hellman. Derive each
       public key from base point 9, then both shared secrets, and require
       them to match the published values and each other. */
    {
        static const char base9[] =
            "0900000000000000000000000000000000000000000000000000000000000000";
        static const char alice_sk[] =
            "77076d0a7318a57d3c16c17251b26645df4c2f87ebc0992ab177fba51db92c2a";
        static const char alice_pk[] =
            "8520f0098930a754748b7ddcb43ef75a0dbf3a0d26381af4eba4a98eaa9b4e6a";
        static const char bob_sk[] =
            "5dab087e624a8a4b79e17f8b83800ee66f3bb1292618b6fd1c2f8b27ff88e0eb";
        static const char bob_pk[] =
            "de9edb7d7b7dc1b4d35b61c2ece435373f8343c85b78674dadfc7e146f882b4f";
        static const char shared[] =
            "4a5d9d5ba4ce2de1728e3bf480350f25e07e21c947d19e3376f09b3c1e161742";

        bad += check_x25519(rng, "rfc7748-6.1-alice-pub", alice_sk, base9,
                            alice_pk);
        bad += check_x25519(rng, "rfc7748-6.1-bob-pub", bob_sk, base9, bob_pk);
        /* alice_sk * bob_pub and bob_sk * alice_pub both yield the secret. */
        bad += check_x25519(rng, "rfc7748-6.1-shared-ab", alice_sk, bob_pk,
                            shared);
        bad += check_x25519(rng, "rfc7748-6.1-shared-ba", bob_sk, alice_pk,
                            shared);
    }

    report_step("x25519_vectors", bad);
    return bad;
}

/* Time N scalar multiplications on each path so the OFF vs ON ratio comes
   out of one run: the generic ladder is the Everest-OFF cost, Hacl* the
   Everest-ON cost. Emulated PPSSPP cycles, not hardware -- a ratio, not an
   absolute. Uses the RFC 7748 5.2-1 operands so the work is representative
   and its result is already known-good. */
static void run_x25519_timing(void)
{
    unsigned char scalar[32], point[32], out[32];
    unsigned long long generic_us = 0;

    if (hex32("a546e36bf0527c9d3b16154b82465edd62144c0ac1fc5a18506a2244ba449ac4",
              scalar) != 0
        || hex32("e6db6867583030db3594c1a424b15f7c726624ec26b3353b10a903a6d0ab1c4c",
                 point) != 0) {
        return;
    }

    {
        SceInt64 t0 = sceKernelGetSystemTimeWide();
        for (int i = 0; i < X25519_TIMING_ITERATIONS; i++) {
            if (generic_x25519(&drbg, scalar, point, out) != 0) {
                printf("tilefinch-crypto: timing=x25519-generic result=FAIL\n");
                failures++;
                return;
            }
        }
        generic_us =
            (unsigned long long) (sceKernelGetSystemTimeWide() - t0);
    }
    printf("tilefinch-crypto: timing=x25519-generic iterations=%d total-us=%llu "
           "per-op-us=%llu\n",
           X25519_TIMING_ITERATIONS, generic_us,
           generic_us / (unsigned long long) X25519_TIMING_ITERATIONS);

#if defined(MBEDTLS_ECDH_VARIANT_EVEREST_ENABLED)
    {
        unsigned long long everest_us;
        SceInt64 t0 = sceKernelGetSystemTimeWide();
        for (int i = 0; i < X25519_TIMING_ITERATIONS; i++) {
            Hacl_Curve25519_crypto_scalarmult(out, scalar, point);
        }
        everest_us =
            (unsigned long long) (sceKernelGetSystemTimeWide() - t0);
        printf("tilefinch-crypto: timing=x25519-everest iterations=%d "
               "total-us=%llu per-op-us=%llu\n",
               X25519_TIMING_ITERATIONS, everest_us,
               everest_us
                   / (unsigned long long) X25519_TIMING_ITERATIONS);
        if (everest_us > 0) {
            /* Ratio in milli-units so no float is needed: 1000 = parity,
               >1000 = Everest faster. generic/everest. */
            printf("tilefinch-crypto: timing=x25519-ratio "
                   "generic-over-everest-milli=%llu\n",
                   (generic_us * 1000ull) / everest_us);
        }
    }
#endif
}

/* ------------------------------------------------------------------ */
/* 3. ECDSA P-256 round trip, and timing                               */
/* ------------------------------------------------------------------ */

static int run_ecdsa(unsigned long long *sign_us, unsigned long long *verify_us)
{
    mbedtls_ecdsa_context ctx;
    mbedtls_mpi r, s;
    unsigned char hash[32];
    static const unsigned char message[] =
        "tilefinch T2 Allegrex bignum selftest message";
    int rc;

    *sign_us = 0;
    *verify_us = 0;

    mbedtls_ecdsa_init(&ctx);
    mbedtls_mpi_init(&r);
    mbedtls_mpi_init(&s);

    rc = mbedtls_sha256(message, sizeof(message) - 1, hash, 0);
    if (rc == 0) {
        rc = mbedtls_ecdsa_genkey(&ctx, MBEDTLS_ECP_DP_SECP256R1,
                                  mbedtls_ctr_drbg_random, &drbg);
    }
    if (rc == 0) {
        SceInt64 t0 = sceKernelGetSystemTimeWide();
        rc = mbedtls_ecdsa_sign(&ctx.MBEDTLS_PRIVATE(grp), &r, &s,
                                &ctx.MBEDTLS_PRIVATE(d), hash, sizeof(hash),
                                mbedtls_ctr_drbg_random, &drbg);
        *sign_us = (unsigned long long) (sceKernelGetSystemTimeWide() - t0);
    }
    if (rc == 0) {
        SceInt64 t0 = sceKernelGetSystemTimeWide();
        rc = mbedtls_ecdsa_verify(&ctx.MBEDTLS_PRIVATE(grp), hash,
                                  sizeof(hash), &ctx.MBEDTLS_PRIVATE(Q),
                                  &r, &s);
        *verify_us = (unsigned long long) (sceKernelGetSystemTimeWide() - t0);
    }
    if (rc == 0) {
        /* A signature that verifies proves nothing unless a corrupted one
           does not. Flip a bit in r and require the verify to reject. */
        if (mbedtls_mpi_set_bit(&r, 3,
                                mbedtls_mpi_get_bit(&r, 3) ? 0 : 1) != 0) {
            rc = -1;
        } else if (mbedtls_ecdsa_verify(&ctx.MBEDTLS_PRIVATE(grp), hash,
                                        sizeof(hash), &ctx.MBEDTLS_PRIVATE(Q),
                                        &r, &s) == 0) {
            printf("tilefinch-crypto: step=ecdsa_negative result=FAIL "
                   "reason=tampered-signature-accepted\n");
            rc = -2;
        }
    }

    mbedtls_mpi_free(&r);
    mbedtls_mpi_free(&s);
    mbedtls_ecdsa_free(&ctx);
    report_step("ecdsa_p256_roundtrip", rc);
    return rc;
}

static void run_mul_timing(void)
{
    mbedtls_mpi a, b, product;
    unsigned long long elapsed = 0;

    mbedtls_mpi_init(&a);
    mbedtls_mpi_init(&b);
    mbedtls_mpi_init(&product);

    /* 2048 bits = 64 limbs at 32 bits: 8 full X8 batches, no remainder,
       which is exactly the shape RSA-2048 modular arithmetic runs. */
    if (mbedtls_mpi_grow(&a, 64) != 0 || mbedtls_mpi_grow(&b, 64) != 0) {
        printf("tilefinch-crypto: timing=mul2048 result=FAIL reason=alloc\n");
        failures++;
        goto done;
    }
    for (int i = 0; i < 64; i++) {
        a.MBEDTLS_PRIVATE(p)[i] = (mbedtls_mpi_uint) (0x9E3779B9u * (unsigned) (i + 1));
        b.MBEDTLS_PRIVATE(p)[i] = (mbedtls_mpi_uint) (0x85EBCA6Bu ^ (unsigned) (i * 7 + 3));
    }
    a.MBEDTLS_PRIVATE(p)[63] |= 0x80000000u;
    b.MBEDTLS_PRIVATE(p)[63] |= 0x80000000u;

    {
        SceInt64 t0 = sceKernelGetSystemTimeWide();
        for (int i = 0; i < MUL_ITERATIONS; i++) {
            if (mbedtls_mpi_mul_mpi(&product, &a, &b) != 0) {
                printf("tilefinch-crypto: timing=mul2048 result=FAIL\n");
                failures++;
                goto done;
            }
        }
        elapsed = (unsigned long long) (sceKernelGetSystemTimeWide() - t0);
    }

    printf("tilefinch-crypto: timing=mul2048 iterations=%d total-us=%llu "
           "per-call-us=%llu\n",
           MUL_ITERATIONS, elapsed,
           elapsed / (unsigned long long) MUL_ITERATIONS);

done:
    mbedtls_mpi_free(&a);
    mbedtls_mpi_free(&b);
    mbedtls_mpi_free(&product);
}

static bool crypto_screen_status(const char *stage, const char *detail)
{
    if (!crypto_screen_prepare()) return false;
    pspDebugScreenPrintf("TILEFINCH CRYPTO SELF-TEST\n\n");
    pspDebugScreenPrintf("%s\n", stage == NULL ? "WORKING..." : stage);
    if (detail != NULL && detail[0] != '\0') {
        pspDebugScreenSetTextColor(0xFFB9AAA0u);
        pspDebugScreenPrintf("\n%s\n", detail);
    }
    return psp_display_publish(&crypto_display);
}

int main(int argc, char **argv)
{
    bool screen_ready = psp_display_begin(
        &crypto_display, psp_display_system_backend());
    if (screen_ready)
        screen_ready = crypto_screen_status(
            "CHECKING SIGNATURE PRIMITIVES...", "This takes a few seconds.");
    crypto_log_open(argc > 0 && argv != NULL ? argv[0] : NULL);
    psp_setup_callbacks();

    printf("tilefinch-crypto: log=%s state=%s\n", crypto_log_path,
           crypto_log_file == NULL ? "unavailable" : "open");
    printf("tilefinch-crypto: boot core=%s everest=%s mbedtls=%s\n",
           TILEFINCH_MULADDC_CORE,
#if defined(MBEDTLS_ECDH_VARIANT_EVEREST_ENABLED)
           "on",
#else
           "off",
#endif
           MBEDTLS_VERSION_STRING);

    mbedtls_entropy_init(&entropy);
    mbedtls_ctr_drbg_init(&drbg);
    {
        static const unsigned char pers[] = "tilefinch-crypto-selftest";
        int rc = mbedtls_ctr_drbg_seed(&drbg, mbedtls_entropy_func, &entropy,
                                       pers, sizeof(pers) - 1);
        report_step("ctr_drbg_seed", rc);
        if (rc != 0) {
            goto finish;
        }
    }

    if (screen_ready)
        screen_ready = crypto_screen_status(
            "CHECKING CRYPTO LIBRARY...", "Step 1 of 5");
    run_library_selftests();
    if (screen_ready)
        screen_ready = crypto_screen_status(
            "CHECKING BIG INTEGERS...", "Step 2 of 5");
    run_mul_vectors();
    run_mul_identities();
    if (screen_ready)
        screen_ready = crypto_screen_status(
            "CHECKING X25519 VECTORS...", "Step 3 of 5");
    run_x25519_vectors(&drbg);

    {
        unsigned long long sign_us = 0, verify_us = 0;
        if (screen_ready)
            screen_ready = crypto_screen_status(
                "CHECKING UPDATE SIGNATURES...", "Step 4 of 5");
        run_ecdsa(&sign_us, &verify_us);
        printf("tilefinch-crypto: timing=ecdsa-p256 sign-us=%llu "
               "verify-us=%llu\n", sign_us, verify_us);
    }

    if (screen_ready)
        screen_ready = crypto_screen_status(
            "MEASURING DEVICE SPEED...", "Step 5 of 5");
    run_mul_timing();
    run_x25519_timing();

finish:
    mbedtls_ctr_drbg_free(&drbg);
    mbedtls_entropy_free(&entropy);

    printf("tilefinch-crypto: core=%s failures=%d\n",
           TILEFINCH_MULADDC_CORE, failures);
    printf("tilefinch-crypto: outcome=%s\n", failures == 0 ? "pass" : "fail");
    printf("tilefinch-crypto: complete=1 auto-exit-ms=2000\n");
    fflush(stdout);
    bool log_saved = crypto_log_publish();

    if (screen_ready && crypto_screen_prepare()) {
        pspDebugScreenPrintf("TILEFINCH CRYPTO SELF-TEST\n\n");
        if (failures == 0) {
            pspDebugScreenSetTextColor(0xFF8FE0A1u);
            pspDebugScreenPrintf("PASS\n\n");
        } else {
            pspDebugScreenSetTextColor(0xFF8A91FFu);
            pspDebugScreenPrintf("FAILED (%d checks)\n\n", failures);
        }
        pspDebugScreenSetTextColor(0xFFF4E9DFu);
        pspDebugScreenPrintf("Allegrex bignum: %s\n", TILEFINCH_MULADDC_CORE);
        pspDebugScreenPrintf("Everest X25519: %s\n",
#if defined(MBEDTLS_ECDH_VARIANT_EVEREST_ENABLED)
                             "enabled");
#else
                             "disabled");
#endif
        if (log_saved) {
            pspDebugScreenPrintf("\nLog saved beside this app:\n%s\n",
                                 crypto_log_path);
        } else {
            pspDebugScreenSetTextColor(0xFF8A91FFu);
            pspDebugScreenPrintf(
                "\nLog could not be saved (error %d).\n"
                "The result above is authoritative.\n",
                crypto_log_error);
        }
        pspDebugScreenSetTextColor(0xFFB9AAA0u);
        pspDebugScreenPrintf("\nComplete. Returning to XMB...");
        (void) psp_display_publish(&crypto_display);
    }
    sceKernelDelayThread(2000000u);

    /* Exit on our own so a headless runner does not have to kill us; the
       qualification fixture's unbounded input loop is deliberately omitted. */
    sceKernelExitGame();
    if (crypto_screen_prepare()) {
        pspDebugScreenPrintf("TILEFINCH CRYPTO SELF-TEST\n\n");
        pspDebugScreenSetTextColor(0xFF8A91FFu);
        pspDebugScreenPrintf("Automatic close did not complete.\n\n");
        pspDebugScreenSetTextColor(0xFFF4E9DFu);
        pspDebugScreenPrintf("Press HOME to return to XMB.");
        (void) psp_display_publish(&crypto_display);
    }
    sceKernelSleepThreadCB();
    return failures == 0 ? 0 : 1;
}
