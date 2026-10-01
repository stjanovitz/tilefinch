#include "psp_entropy_pool.h"

#include <string.h>

static const uint8_t seed_file_magic[4] = {'T', 'F', 'E', 'S'};

void psp_entropy_zeroize(void *data, size_t length)
{
    volatile uint8_t *bytes = data;
    while (length-- != 0) *bytes++ = 0u;
}

static void put_le32(uint8_t *output, uint32_t value)
{
    output[0] = (uint8_t) value;
    output[1] = (uint8_t) (value >> 8);
    output[2] = (uint8_t) (value >> 16);
    output[3] = (uint8_t) (value >> 24);
}

static void put_le64(uint8_t *output, uint64_t value)
{
    put_le32(output, (uint32_t) value);
    put_le32(output + 4, (uint32_t) (value >> 32));
}

static uint32_t get_le32(const uint8_t *input)
{
    return (uint32_t) input[0] | (uint32_t) input[1] << 8
        | (uint32_t) input[2] << 16 | (uint32_t) input[3] << 24;
}

/* Domain byte and length frame every input, so concatenations of different
   inputs can never hash identically. */
static void absorb_header(PspEntropyPool *pool, PspEntropyDomain domain,
                          size_t length)
{
    uint8_t header[5];
    header[0] = (uint8_t) domain;
    put_le32(header + 1, (uint32_t) length);
    (void) tilefinch_sha256_update(&pool->absorb, header, sizeof(header));
}

static void prime(PspEntropyPool *pool)
{
    tilefinch_sha256_init(&pool->absorb);
    absorb_header(pool, PSP_ENTROPY_DOMAIN_KEY, sizeof(pool->key));
    (void) tilefinch_sha256_update(&pool->absorb, pool->key,
                                   sizeof(pool->key));
}

void psp_entropy_pool_init(PspEntropyPool *pool)
{
    if (pool == NULL) return;
    memset(pool, 0, sizeof(*pool));
    prime(pool);
}

void psp_entropy_pool_mix(PspEntropyPool *pool, PspEntropyDomain domain,
                          const void *data, size_t length)
{
    if (pool == NULL || (data == NULL && length != 0)) return;
    absorb_header(pool, domain, length);
    (void) tilefinch_sha256_update(&pool->absorb, data, length);
}

static uint64_t isqrt64(uint64_t value)
{
    uint64_t root = 0;
    uint64_t bit = UINT64_C(1) << 62;
    while (bit > value) bit >>= 2;
    while (bit != 0) {
        if (value >= root + bit) {
            value -= root + bit;
            root = (root >> 1) + bit;
        } else {
            root >>= 1;
        }
        bit >>= 2;
    }
    return root;
}

/* 99% upper confidence bound (z = 2.576) on hits/trials, in Q16. Integer
   only: the launcher links no libm, and host and PSP must agree exactly. */
static uint32_t upper_bound_q16(uint32_t hits, uint32_t trials)
{
    if (trials < 2u || hits >= trials) return 65536u;
    uint64_t rate = ((uint64_t) hits << 16) / trials;
    uint64_t variance_q32 = rate * (65536u - rate) / (trials - 1u);
    uint64_t bound = rate + (isqrt64(variance_q32) * 2576u + 999u) / 1000u;
    return bound > 65536u ? 65536u : (uint32_t) bound;
}

/* -log2(q / 65536) in thousandths of a bit, q in (0, 65536]. */
static uint32_t neg_log2_q16_millibits(uint32_t q)
{
    if (q >= 65536u) return 0u;
    if (q == 0u) q = 1u;
    uint32_t whole = 0u;
    while ((q >> (whole + 1u)) != 0u) whole++;
    /* Normalize to [1, 2) in Q16, then square repeatedly: each squaring
       that reaches 2 contributes the next fractional bit of log2. */
    uint64_t y = ((uint64_t) q << 16) >> whole;
    uint32_t fraction = 0u;
    for (unsigned bit = 0; bit < 16u; bit++) {
        y = (y * y) >> 16;
        fraction <<= 1;
        if (y >= (UINT64_C(2) << 16)) {
            y >>= 1;
            fraction |= 1u;
        }
    }
    uint32_t log_q16 = (whole << 16) | fraction;
    uint32_t negative_q16 = (16u << 16) - log_q16;
    return (uint32_t) (((uint64_t) negative_q16 * 1000u) >> 16);
}

static uint32_t max_u32(uint32_t left, uint32_t right)
{
    return left > right ? left : right;
}

uint32_t psp_entropy_estimate_millibits(uint32_t *samples, size_t count)
{
    if (samples == NULL || count < PSP_ENTROPY_MIN_BATCH
        || count > PSP_ENTROPY_MAX_BATCH) return 0u;
    uint32_t trials = (uint32_t) count;
    uint32_t worst = 0u;

    /* Repeat-at-lag: catches constants, alternation and any short cycle a
       scheduler tick or cache pattern could impose. */
    for (uint32_t lag = 1u; lag <= PSP_ENTROPY_LAG_MAX; lag++) {
        uint32_t hits = 0u;
        for (uint32_t index = lag; index < trials; index++) {
            if (samples[index] == samples[index - lag]) hits++;
        }
        worst = max_u32(worst, upper_bound_q16(hits, trials - lag));
    }

    /* Linear trend: a drifting phase between two clocks produces values
       that step by a constant. */
    uint32_t trend_hits = 0u;
    for (uint32_t index = 2u; index < trials; index++) {
        uint32_t guess = 2u * samples[index - 1u] - samples[index - 2u];
        if (samples[index] == guess) trend_hits++;
    }
    worst = max_u32(worst, upper_bound_q16(trend_hits, trials - 2u));

    /* Most common value, from runs after an in-place insertion sort. */
    for (uint32_t index = 1u; index < trials; index++) {
        uint32_t value = samples[index];
        uint32_t cursor = index;
        while (cursor > 0u && samples[cursor - 1u] > value) {
            samples[cursor] = samples[cursor - 1u];
            cursor--;
        }
        samples[cursor] = value;
    }
    uint32_t longest = 1u, run = 1u;
    for (uint32_t index = 1u; index < trials; index++) {
        run = samples[index] == samples[index - 1u] ? run + 1u : 1u;
        if (run > longest) longest = run;
    }
    worst = max_u32(worst, upper_bound_q16(longest, trials));

    return neg_log2_q16_millibits(worst);
}

uint32_t psp_entropy_credit_bits(uint32_t millibits, size_t count)
{
    if (millibits > PSP_ENTROPY_SAMPLE_CAP_MILLIBITS)
        millibits = PSP_ENTROPY_SAMPLE_CAP_MILLIBITS;
    if (count > PSP_ENTROPY_MAX_BATCH) count = PSP_ENTROPY_MAX_BATCH;
    return (uint32_t) (((uint64_t) millibits * count)
                       / (1000u * PSP_ENTROPY_SAFETY_DIVISOR));
}

uint32_t psp_entropy_pool_add_batch(PspEntropyPool *pool,
                                    PspEntropyDomain domain,
                                    uint32_t *samples, size_t count,
                                    uint32_t *credited)
{
    if (credited != NULL) *credited = 0u;
    if (pool == NULL || samples == NULL || count == 0u) return 0u;
    absorb_header(pool, domain, count * 4u);
    for (size_t index = 0; index < count; index++) {
        uint8_t bytes[4];
        put_le32(bytes, samples[index]);
        (void) tilefinch_sha256_update(&pool->absorb, bytes, sizeof(bytes));
    }
    uint32_t millibits = psp_entropy_estimate_millibits(samples, count);
    uint32_t bits = psp_entropy_credit_bits(millibits, count);
    pool->credited_bits = bits > UINT32_MAX - pool->credited_bits
        ? UINT32_MAX : pool->credited_bits + bits;
    if (credited != NULL) *credited = bits;
    return millibits;
}

bool psp_entropy_pool_ready(const PspEntropyPool *pool)
{
    return pool != NULL && pool->credited_bits >= PSP_ENTROPY_TARGET_BITS;
}

void psp_entropy_pool_generate(PspEntropyPool *pool, void *output,
                               size_t length)
{
    if (pool == NULL || (output == NULL && length != 0)) return;
    uint8_t frame[12];
    put_le64(frame, pool->generation);
    put_le32(frame + 8, (uint32_t) length);
    (void) tilefinch_sha256_update(&pool->absorb, frame, sizeof(frame));
    uint8_t digest[TILEFINCH_SHA256_DIGEST_BYTES];
    (void) tilefinch_sha256_final(&pool->absorb, digest);

    uint8_t *bytes = output;
    uint32_t block = 0u;
    while (length != 0) {
        uint8_t input[1 + sizeof(digest) + 4];
        input[0] = 2u;
        memcpy(input + 1, digest, sizeof(digest));
        put_le32(input + 1 + sizeof(digest), block++);
        uint8_t out[TILEFINCH_SHA256_DIGEST_BYTES];
        (void) tilefinch_sha256_digest(input, sizeof(input), out);
        size_t take = length < sizeof(out) ? length : sizeof(out);
        memcpy(bytes, out, take);
        bytes += take;
        length -= take;
        psp_entropy_zeroize(input, sizeof(input));
        psp_entropy_zeroize(out, sizeof(out));
    }

    /* The next key is a one-way function of this digest; the digest and the
       old key are gone once this returns. */
    uint8_t rekey[1 + sizeof(digest)];
    rekey[0] = 1u;
    memcpy(rekey + 1, digest, sizeof(digest));
    (void) tilefinch_sha256_digest(rekey, sizeof(rekey), pool->key);
    pool->generation++;
    psp_entropy_zeroize(rekey, sizeof(rekey));
    psp_entropy_zeroize(digest, sizeof(digest));
    prime(pool);
}

static void seed_file_check(const uint8_t *file, uint8_t check[8])
{
    uint8_t digest[TILEFINCH_SHA256_DIGEST_BYTES];
    (void) tilefinch_sha256_digest(
        file, PSP_ENTROPY_SEED_FILE_BYTES - 8u, digest);
    memcpy(check, digest, 8u);
}

void psp_entropy_seed_file_encode(
    const uint8_t seed[PSP_ENTROPY_SEED_BYTES],
    uint8_t file[PSP_ENTROPY_SEED_FILE_BYTES])
{
    memcpy(file, seed_file_magic, sizeof(seed_file_magic));
    put_le32(file + 4, PSP_ENTROPY_SEED_FILE_VERSION);
    memcpy(file + 8, seed, PSP_ENTROPY_SEED_BYTES);
    seed_file_check(file, file + 8 + PSP_ENTROPY_SEED_BYTES);
}

bool psp_entropy_seed_file_decode(const uint8_t *file, size_t length,
                                  uint8_t seed[PSP_ENTROPY_SEED_BYTES])
{
    if (file == NULL || seed == NULL
        || length != PSP_ENTROPY_SEED_FILE_BYTES
        || memcmp(file, seed_file_magic, sizeof(seed_file_magic)) != 0
        || get_le32(file + 4) != PSP_ENTROPY_SEED_FILE_VERSION)
        return false;
    uint8_t check[8];
    seed_file_check(file, check);
    if (memcmp(check, file + 8 + PSP_ENTROPY_SEED_BYTES, sizeof(check))
        != 0) return false;
    uint8_t any = 0u;
    for (size_t index = 0; index < PSP_ENTROPY_SEED_BYTES; index++)
        any |= file[8 + index];
    if (any == 0u) return false;
    memcpy(seed, file + 8, PSP_ENTROPY_SEED_BYTES);
    return true;
}
