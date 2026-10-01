#include "../src/psp_entropy_pool.h"

#include <stdio.h>
#include <string.h>

#define CHECK(condition) do {                                              \
    if (!(condition)) {                                                    \
        fprintf(stderr, "CHECK failed at %s:%d: %s\n",                    \
                __FILE__, __LINE__, #condition);                           \
        return 1;                                                          \
    }                                                                      \
} while (0)

/* A statistically good stand-in for real jitter (splitmix64). */
static uint64_t random_state;

static uint32_t next_random(void)
{
    uint64_t z = (random_state += UINT64_C(0x9e3779b97f4a7c15));
    z = (z ^ (z >> 30)) * UINT64_C(0xbf58476d1ce4e5b9);
    z = (z ^ (z >> 27)) * UINT64_C(0x94d049bb133111eb);
    return (uint32_t) ((z ^ (z >> 31)) >> 32);
}

static void hex_bytes(const char *hex, uint8_t *output, size_t length)
{
    for (size_t index = 0; index < length; index++) {
        unsigned value = 0;
        (void) sscanf(hex + 2 * index, "%2x", &value);
        output[index] = (uint8_t) value;
    }
}

static int test_estimator_refuses_predictable_batches(void)
{
    uint32_t samples[PSP_ENTROPY_MAX_BATCH];

    /* An emulator's instruction-counted clock: every sample identical. */
    for (size_t i = 0; i < 256; i++) samples[i] = 37u;
    CHECK(psp_entropy_estimate_millibits(samples, 256) == 0u);

    /* Two values alternating: half of each value, so most-common-value
       alone would grant about a bit; the lag-2 predictor sees the cycle. */
    for (size_t i = 0; i < 256; i++) samples[i] = (i & 1u) ? 12u : 13u;
    CHECK(psp_entropy_estimate_millibits(samples, 256) == 0u);

    /* A cycle of eight distinct values: lag 8 predicts it exactly. */
    for (size_t i = 0; i < 256; i++) samples[i] = 100u + (uint32_t) (i % 8u);
    CHECK(psp_entropy_estimate_millibits(samples, 256) == 0u);

    /* A drifting phase: every value distinct, so most-common-value would
       grant eight bits; the trend predictor sees the constant step. */
    for (size_t i = 0; i < 256; i++) samples[i] = 5000u + 3u * (uint32_t) i;
    CHECK(psp_entropy_estimate_millibits(samples, 256) == 0u);

    /* A cycle longer than any lag still leaves the batch mostly one value
       apart from a rare excursion: credit must stay near zero. */
    for (size_t i = 0; i < 256; i++) samples[i] = (i % 97u == 0u) ? 20u : 19u;
    CHECK(psp_entropy_estimate_millibits(samples, 256) < 150u);
    return 0;
}

static int test_estimator_credits_unpredictable_batches(void)
{
    uint32_t samples[PSP_ENTROPY_MAX_BATCH];
    random_state = 1;
    for (size_t i = 0; i < 256; i++) samples[i] = next_random() & 0xFFu;
    uint32_t wide = psp_entropy_estimate_millibits(samples, 256);
    /* The 99% bound on 256 samples keeps even uniform bytes near 4-5 bits. */
    CHECK(wide > 3000u && wide < 8000u);

    for (size_t i = 0; i < 256; i++) samples[i] = next_random() & 1u;
    uint32_t coin = psp_entropy_estimate_millibits(samples, 256);
    CHECK(coin > 500u && coin < 1000u);

    /* The order-free predictor runs last and sorts in place (documented). */
    for (size_t i = 1; i < 256; i++) CHECK(samples[i - 1] <= samples[i]);
    return 0;
}

static int test_estimator_bounds(void)
{
    uint32_t samples[PSP_ENTROPY_MAX_BATCH + 1];
    random_state = 7;
    for (size_t i = 0; i <= PSP_ENTROPY_MAX_BATCH; i++)
        samples[i] = next_random();
    CHECK(psp_entropy_estimate_millibits(NULL, 64) == 0u);
    CHECK(psp_entropy_estimate_millibits(samples, PSP_ENTROPY_MIN_BATCH - 1)
          == 0u);
    CHECK(psp_entropy_estimate_millibits(samples, PSP_ENTROPY_MAX_BATCH + 1)
          == 0u);
    CHECK(psp_entropy_estimate_millibits(samples, PSP_ENTROPY_MIN_BATCH)
          > 0u);
    return 0;
}

static int test_credit_is_capped_and_divided(void)
{
    /* One bit per sample at most, then a quarter of that. */
    CHECK(psp_entropy_credit_bits(1000u, 256) == 64u);
    CHECK(psp_entropy_credit_bits(8000u, 256) == 64u);
    CHECK(psp_entropy_credit_bits(500u, 256) == 32u);
    CHECK(psp_entropy_credit_bits(500u, 64) == 8u);
    CHECK(psp_entropy_credit_bits(0u, 256) == 0u);
    /* Fractions round down: a weak batch earns nothing rather than one. */
    CHECK(psp_entropy_credit_bits(15u, 256) == 0u);
    CHECK(psp_entropy_credit_bits(1000u, 100000) == 64u);
    return 0;
}

static int test_pool_requires_target_credit(void)
{
    PspEntropyPool pool;
    psp_entropy_pool_init(&pool);
    uint32_t samples[256];
    uint32_t credited = 99u;

    for (size_t i = 0; i < 256; i++) samples[i] = 41u;
    (void) psp_entropy_pool_add_batch(&pool, PSP_ENTROPY_DOMAIN_WORK_TIMING,
                                      samples, 256, &credited);
    CHECK(credited == 0u);
    CHECK(!psp_entropy_pool_ready(&pool));

    /* A seed file is mixed, never credited. */
    uint8_t seed[PSP_ENTROPY_SEED_BYTES];
    memset(seed, 0xA5, sizeof(seed));
    psp_entropy_pool_mix(&pool, PSP_ENTROPY_DOMAIN_SEED_FILE, seed,
                         sizeof(seed));
    CHECK(pool.credited_bits == 0u);

    random_state = 11;
    for (unsigned batch = 0; batch < 4u; batch++) {
        CHECK(!psp_entropy_pool_ready(&pool));
        for (size_t i = 0; i < 256; i++) samples[i] = next_random() & 0xFFu;
        (void) psp_entropy_pool_add_batch(
            &pool, PSP_ENTROPY_DOMAIN_WORK_TIMING, samples, 256, &credited);
        CHECK(credited == 64u);
    }
    CHECK(pool.credited_bits == PSP_ENTROPY_TARGET_BITS);
    CHECK(psp_entropy_pool_ready(&pool));

    pool.credited_bits = UINT32_MAX - 1u;
    for (size_t i = 0; i < 256; i++) samples[i] = next_random() & 0xFFu;
    (void) psp_entropy_pool_add_batch(&pool, PSP_ENTROPY_DOMAIN_WORK_TIMING,
                                      samples, 256, &credited);
    CHECK(pool.credited_bits == UINT32_MAX);
    return 0;
}

static int test_pool_output_matches_reference(void)
{
    /* Golden values from an independent Python model of the construction
       (hashlib): pins the framing and byte order so host and PSP agree. */
    PspEntropyPool pool;
    psp_entropy_pool_init(&pool);
    uint8_t seed[PSP_ENTROPY_SEED_BYTES];
    for (size_t i = 0; i < sizeof(seed); i++) seed[i] = (uint8_t) (i + 1u);
    psp_entropy_pool_mix(&pool, PSP_ENTROPY_DOMAIN_SEED_FILE, seed,
                         sizeof(seed));
    uint32_t samples[64];
    for (uint32_t i = 0; i < 64u; i++)
        samples[i] = (i * i * 7919u + 13u * i) & 0xFFu;
    uint32_t credited = 0;
    uint32_t millibits = psp_entropy_pool_add_batch(
        &pool, PSP_ENTROPY_DOMAIN_WORK_TIMING, samples, 64, &credited);
    CHECK(millibits > PSP_ENTROPY_SAMPLE_CAP_MILLIBITS);
    CHECK(credited == 16u);

    uint8_t first[40], second[16], expected[40];
    psp_entropy_pool_generate(&pool, first, sizeof(first));
    psp_entropy_pool_generate(&pool, second, sizeof(second));
    hex_bytes("a6b97faaf59b521d2d80fc3932d805f26a45ec159e22b0e2b5f04079"
              "a91da44a03ae675d29904392", expected, 40);
    CHECK(memcmp(first, expected, 40) == 0);
    hex_bytes("f60428ae55c88cff5f835dc3d86b95d9", expected, 16);
    CHECK(memcmp(second, expected, 16) == 0);
    CHECK(pool.generation == 2u);
    return 0;
}

static int test_pool_separates_inputs_and_ratchets(void)
{
    uint8_t data[16];
    memset(data, 0x3C, sizeof(data));
    PspEntropyPool left, right;
    uint8_t left_out[32], right_out[32];

    /* Same bytes under different domains give different pools. */
    psp_entropy_pool_init(&left);
    psp_entropy_pool_init(&right);
    psp_entropy_pool_mix(&left, PSP_ENTROPY_DOMAIN_BOOT_CONTEXT, data, 16);
    psp_entropy_pool_mix(&right, PSP_ENTROPY_DOMAIN_POLL_CONTEXT, data, 16);
    psp_entropy_pool_generate(&left, left_out, 32);
    psp_entropy_pool_generate(&right, right_out, 32);
    CHECK(memcmp(left_out, right_out, 32) != 0);

    /* Framing: one 16-byte input differs from two 8-byte inputs. */
    psp_entropy_pool_init(&left);
    psp_entropy_pool_init(&right);
    psp_entropy_pool_mix(&left, PSP_ENTROPY_DOMAIN_BOOT_CONTEXT, data, 16);
    psp_entropy_pool_mix(&right, PSP_ENTROPY_DOMAIN_BOOT_CONTEXT, data, 8);
    psp_entropy_pool_mix(&right, PSP_ENTROPY_DOMAIN_BOOT_CONTEXT, data, 8);
    psp_entropy_pool_generate(&left, left_out, 32);
    psp_entropy_pool_generate(&right, right_out, 32);
    CHECK(memcmp(left_out, right_out, 32) != 0);

    /* Identical histories agree; each output advances the key. */
    psp_entropy_pool_init(&left);
    psp_entropy_pool_init(&right);
    psp_entropy_pool_mix(&left, PSP_ENTROPY_DOMAIN_BOOT_CONTEXT, data, 16);
    psp_entropy_pool_mix(&right, PSP_ENTROPY_DOMAIN_BOOT_CONTEXT, data, 16);
    psp_entropy_pool_generate(&left, left_out, 32);
    psp_entropy_pool_generate(&right, right_out, 32);
    CHECK(memcmp(left_out, right_out, 32) == 0);
    uint8_t key_before[32];
    memcpy(key_before, left.key, sizeof(key_before));
    psp_entropy_pool_generate(&left, right_out, 32);
    CHECK(memcmp(left_out, right_out, 32) != 0);
    CHECK(memcmp(key_before, left.key, sizeof(key_before)) != 0);
    CHECK(memcmp(left_out, left.key, 32) != 0);

    /* Zero-length output still ratchets; NULL input is ignored. */
    uint64_t generation = left.generation;
    psp_entropy_pool_generate(&left, NULL, 0);
    CHECK(left.generation == generation + 1u);
    psp_entropy_pool_mix(&left, PSP_ENTROPY_DOMAIN_BOOT_CONTEXT, NULL, 4);
    return 0;
}

static int test_seed_file_format(void)
{
    uint8_t seed[PSP_ENTROPY_SEED_BYTES], decoded[PSP_ENTROPY_SEED_BYTES];
    uint8_t file[PSP_ENTROPY_SEED_FILE_BYTES + 1], expected[48];
    for (size_t i = 0; i < sizeof(seed); i++) seed[i] = (uint8_t) (i + 1u);
    CHECK(PSP_ENTROPY_SEED_FILE_BYTES == 48u);
    psp_entropy_seed_file_encode(seed, file);
    hex_bytes("54464553010000000102030405060708090a0b0c0d0e0f1011121314"
              "15161718191a1b1c1d1e1f206ae86d0b2687d7fb", expected, 48);
    CHECK(memcmp(file, expected, 48) == 0);
    CHECK(psp_entropy_seed_file_decode(file, 48, decoded));
    CHECK(memcmp(decoded, seed, sizeof(seed)) == 0);

    /* A truncated or overlong read is a miss, not a partial seed. */
    CHECK(!psp_entropy_seed_file_decode(file, 47, decoded));
    file[48] = 0u;
    CHECK(!psp_entropy_seed_file_decode(file, 49, decoded));
    CHECK(!psp_entropy_seed_file_decode(NULL, 48, decoded));

    /* Any damaged byte fails: magic, version, seed, check. */
    size_t probes[] = {0u, 4u, 8u, 39u, 40u, 47u};
    for (size_t p = 0; p < sizeof(probes) / sizeof(probes[0]); p++) {
        psp_entropy_seed_file_encode(seed, file);
        file[probes[p]] ^= 0x01u;
        CHECK(!psp_entropy_seed_file_decode(file, 48, decoded));
    }

    /* A zero seed with a valid check is still refused. */
    memset(seed, 0, sizeof(seed));
    psp_entropy_seed_file_encode(seed, file);
    CHECK(!psp_entropy_seed_file_decode(file, 48, decoded));
    return 0;
}

static int test_zeroize(void)
{
    uint8_t secret[24];
    memset(secret, 0xEE, sizeof(secret));
    psp_entropy_zeroize(secret, sizeof(secret));
    for (size_t i = 0; i < sizeof(secret); i++) CHECK(secret[i] == 0u);
    return 0;
}

int main(void)
{
    int (*tests[])(void) = {
        test_estimator_refuses_predictable_batches,
        test_estimator_credits_unpredictable_batches,
        test_estimator_bounds,
        test_credit_is_capped_and_divided,
        test_pool_requires_target_credit,
        test_pool_output_matches_reference,
        test_pool_separates_inputs_and_ratchets,
        test_seed_file_format,
        test_zeroize,
    };
    for (size_t i = 0; i < sizeof(tests) / sizeof(tests[0]); i++) {
        if (tests[i]() != 0) return 1;
    }
    puts("psp entropy pool tests passed");
    return 0;
}
