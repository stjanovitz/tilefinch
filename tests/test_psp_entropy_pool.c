#include "../src/psp_entropy_pool.h"

#include <stdio.h>
#include <string.h>

/* The first estimator's predictor set (repeat at lags 1..8 only), copied
   from src/psp_entropy_pool.c as it shipped before the lag bound followed
   the batch size. The monotonicity test holds the shipped estimator to never
   crediting more than this. */
static uint64_t legacy_isqrt64(uint64_t value)
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

static uint32_t legacy_upper_bound_q16(uint32_t hits, uint32_t trials)
{
    if (trials < 2u || hits >= trials) return 65536u;
    uint64_t rate = ((uint64_t) hits << 16) / trials;
    uint64_t variance_q32 = rate * (65536u - rate) / (trials - 1u);
    uint64_t bound = rate
        + (legacy_isqrt64(variance_q32) * 2576u + 999u) / 1000u;
    return bound > 65536u ? 65536u : (uint32_t) bound;
}

static uint32_t legacy_neg_log2_q16_millibits(uint32_t q)
{
    if (q >= 65536u) return 0u;
    if (q == 0u) q = 1u;
    uint32_t whole = 0u;
    while ((q >> (whole + 1u)) != 0u) whole++;
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

static uint32_t legacy_estimate_millibits(uint32_t *samples, size_t count)
{
    if (samples == NULL || count < PSP_ENTROPY_MIN_BATCH
        || count > PSP_ENTROPY_MAX_BATCH) return 0u;
    uint32_t trials = (uint32_t) count;
    uint32_t worst = 0u;
    for (uint32_t lag = 1u; lag <= 8u; lag++) {
        uint32_t hits = 0u;
        for (uint32_t index = lag; index < trials; index++) {
            if (samples[index] == samples[index - lag]) hits++;
        }
        uint32_t bound = legacy_upper_bound_q16(hits, trials - lag);
        if (bound > worst) worst = bound;
    }
    uint32_t trend_hits = 0u;
    for (uint32_t index = 2u; index < trials; index++) {
        uint32_t guess = 2u * samples[index - 1u] - samples[index - 2u];
        if (samples[index] == guess) trend_hits++;
    }
    uint32_t bound = legacy_upper_bound_q16(trend_hits, trials - 2u);
    if (bound > worst) worst = bound;
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
    bound = legacy_upper_bound_q16(longest, trials);
    if (bound > worst) worst = bound;
    return legacy_neg_log2_q16_millibits(worst);
}

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

/* A zero-jitter two-clock beat: fixed work of `duration` microseconds read
   on a 1 us clock whose phase against the work advances by `step` each
   sample. The samples are floor(phase + duration) - floor(phase) with phase
   in [0, 1), so they are a pure function of the start phase: no entropy.
   Truncation stands in for floor because every operand is non-negative. */
static void beat_batch(uint32_t *samples, size_t count, double duration,
                       double step)
{
    double phase = 0.37;
    for (size_t i = 0; i < count; i++) {
        samples[i] = (uint32_t) (phase + duration);
        phase += step;
        phase -= (double) (uint64_t) phase;
    }
}

/* Worst credit over the review's grid: durations 7.05..7.95 us, 1,999 step
   fractions (37,981 beats). */
static uint32_t worst_beat_credit(size_t count, uint32_t *nonzero)
{
    uint32_t samples[PSP_ENTROPY_MAX_BATCH], worst = 0u;
    *nonzero = 0u;
    for (int d = 1; d < 20; d++) {
        for (int t = 1; t < 2000; t++) {
            beat_batch(samples, count, 7.0 + d / 20.0,
                       11.0 + t / 2000.0 + 0.000123);
            uint32_t bits = psp_entropy_credit_bits(
                psp_entropy_estimate_millibits(samples, count), count);
            if (bits != 0u) (*nonzero)++;
            if (bits > worst) worst = bits;
        }
    }
    return worst;
}

static int test_estimator_refuses_cyclic_beats(void)
{
    /* A zero-jitter beat is fully deterministic, so no case of the grid may
       earn credit, for work or sleep batches. Lags 1..8 credited up to 15
       bits per work batch here (a period-9 cycle at 7.2 us, step fraction
       about 1/9) and 2 per sleep batch, nonzero in 28,884 and 3,598 of the
       37,981 cases (the 2026-11 entropy review, ENTROPY-REC.md). */
    uint32_t samples[PSP_ENTROPY_MAX_BATCH], nonzero = 1u;
    CHECK(worst_beat_credit(256, &nonzero) == 0u);
    CHECK(nonzero == 0u);
    nonzero = 1u;
    CHECK(worst_beat_credit(64, &nonzero) == 0u);
    CHECK(nonzero == 0u);

    beat_batch(samples, 256, 7.2, 11.111123);
    CHECK(psp_entropy_estimate_millibits(samples, 256) == 0u);
    return 0;
}

/* Cycles of arbitrary values longer than eight samples, which neither the
   trend nor the most-common-value predictor sees. */
static int test_estimator_refuses_long_cycles(void)
{
    static const uint32_t periods[] = {9u, 17u, 33u, 64u};
    uint32_t cycle[64], samples[PSP_ENTROPY_MAX_BATCH];
    uint32_t copy[PSP_ENTROPY_MAX_BATCH];
    random_state = 29;
    for (size_t i = 0; i < 64; i++) cycle[i] = next_random() & 0xFFu;
    for (size_t p = 0; p < sizeof(periods) / sizeof(periods[0]); p++) {
        for (size_t i = 0; i < 256; i++) samples[i] = cycle[i % periods[p]];
        memcpy(copy, samples, sizeof(copy));
        /* The gap the longer lags close: eight lags credited these. */
        CHECK(psp_entropy_credit_bits(legacy_estimate_millibits(copy, 256),
                                      256) > 0u);
        CHECK(psp_entropy_estimate_millibits(samples, 256) == 0u);
    }
    /* A sleep batch reaches lag 16. */
    for (size_t period = 9u; period <= 16u; period++) {
        for (size_t i = 0; i < 64; i++) samples[i] = cycle[i % period];
        CHECK(psp_entropy_estimate_millibits(samples, 64) == 0u);
    }
    return 0;
}

/* Approximately normal, mean 0 and deviation 1 (Irwin-Hall, n = 12). */
static double next_normal(void)
{
    double sum = 0.0;
    for (int i = 0; i < 12; i++) sum += (next_random() + 0.5) / 4294967296.0;
    return sum - 6.0;
}

/* Signals with real entropy keep earning: the longer lags must not starve
   the device's work and sleep sources. */
static int test_estimator_keeps_crediting_honest_signals(void)
{
    uint32_t samples[PSP_ENTROPY_MAX_BATCH], total = 0u;

    /* Independent two-valued work samples, 70/30. */
    random_state = 31;
    for (unsigned batch = 0; batch < 200u; batch++) {
        for (size_t i = 0; i < 256; i++)
            samples[i] = 7u + (next_random() % 10u >= 7u);
        total += psp_entropy_credit_bits(
            psp_entropy_estimate_millibits(samples, 256), 256);
    }
    CHECK(total >= 18u * 200u);

    /* A beat whose duration jitters by 0.3 us. */
    random_state = 37;
    total = 0u;
    for (int d = 0; d < 20; d++) {
        for (int rep = 0; rep < 20; rep++) {
            double duration = 7.0 + d / 20.0;
            double phase = (next_random() + 0.5) / 4294967296.0;
            for (size_t i = 0; i < 256; i++) {
                samples[i] = (uint32_t) (phase + duration
                                         + 0.3 * next_normal());
                phase += 11.6180339887;
                phase -= (double) (uint64_t) phase;
            }
            total += psp_entropy_credit_bits(
                psp_entropy_estimate_millibits(samples, 256), 256);
        }
    }
    CHECK(total >= 15u * 400u);

    /* Sixteen-valued sleep samples still earn the batch maximum. */
    random_state = 41;
    for (unsigned batch = 0; batch < 200u; batch++) {
        for (size_t i = 0; i < 64; i++)
            samples[i] = 220u + (next_random() & 15u);
        CHECK(psp_entropy_credit_bits(
                  psp_entropy_estimate_millibits(samples, 64), 64) == 16u);
    }
    return 0;
}

/* Adding predictors can only lower an estimate: the shipped estimator never
   exceeds the eight-lag one, and at the smallest batch, where both stop at
   lag 8, they agree exactly. */
static int test_estimator_never_exceeds_eight_lags(void)
{
    static const size_t counts[] = {PSP_ENTROPY_MIN_BATCH, 64u, 100u, 256u};
    uint32_t samples[PSP_ENTROPY_MAX_BATCH], copy[PSP_ENTROPY_MAX_BATCH];
    random_state = 43;
    for (unsigned input = 0; input < 4000u; input++) {
        size_t count = counts[input % 4u];
        uint32_t kind = (input / 4u) % 5u;
        uint32_t period = 2u + next_random() % 80u;
        uint32_t spread = 1u + next_random() % 16u;
        double phase = 0.25, step = 11.0 + (next_random() % 2000u) / 2000.0;
        for (size_t i = 0; i < count; i++) {
            switch (kind) {
                case 0: /* independent */
                    samples[i] = next_random() % spread;
                    break;
                case 1: /* cyclic */
                    samples[i] = (uint32_t) (i % period) * 7u % spread;
                    break;
                case 2: /* deterministic beat */
                    samples[i] = (uint32_t) (phase + 7.3);
                    phase += step;
                    phase -= (double) (uint64_t) phase;
                    break;
                case 3: /* jittered beat */
                    samples[i] = (uint32_t) (phase + 7.3
                                             + 0.2 * next_normal());
                    phase += step;
                    phase -= (double) (uint64_t) phase;
                    break;
                default: /* held values with random jumps */
                    samples[i] = (i % period == 0u) ? next_random() & 3u
                                                    : samples[i - 1u];
                    break;
            }
        }
        memcpy(copy, samples, count * sizeof(samples[0]));
        uint32_t shipped = psp_entropy_estimate_millibits(samples, count);
        uint32_t legacy = legacy_estimate_millibits(copy, count);
        CHECK(shipped <= legacy);
        if (count == PSP_ENTROPY_MIN_BATCH) CHECK(shipped == legacy);
    }
    return 0;
}

/* Why credit moves with code placement: fixed work whose duration's
   fractional microsecond is `fraction` (thousandths), plus uniform jitter
   half a tick wide, read on a 1 us clock. Each sample is one of two
   integers, and the true per-sample min-entropy depends only on where the
   duration falls between two ticks. A relink that moves the duration by a
   fraction of a microsecond moves the yield, and that is expected. */
static int test_estimator_tracks_tick_quantization(void)
{
    static const struct {
        uint32_t fraction;       /* thousandths of a microsecond */
        uint32_t true_millibits; /* -log2(most likely value), rounded down */
    } models[] = {
        {400u, 0u}, {500u, 0u}, {600u, 321u}, {700u, 736u},
        {750u, 1000u}, {800u, 736u}, {900u, 321u},
    };
    uint32_t samples[PSP_ENTROPY_MAX_BATCH];
    uint32_t mean_bits[sizeof(models) / sizeof(models[0])];
    random_state = 23;
    for (size_t m = 0; m < sizeof(models) / sizeof(models[0]); m++) {
        uint32_t total = 0u;
        for (unsigned batch = 0; batch < 64u; batch++) {
            for (size_t i = 0; i < 256; i++)
                samples[i] = (7000u + models[m].fraction
                              + next_random() % 500u) / 1000u;
            uint32_t millibits = psp_entropy_estimate_millibits(samples, 256);
            uint32_t bits = psp_entropy_credit_bits(millibits, 256);
            /* Credit never exceeds the model's true entropy, and neither
               does the estimate before the safety divisor. */
            CHECK(bits * 1000u <= 256u * models[m].true_millibits);
            CHECK(millibits <= models[m].true_millibits);
            total += bits;
        }
        mean_bits[m] = total / 64u;
    }
    /* Same jitter, same work: one value only when the duration sits early
       in its tick, the most credit when the jitter straddles a tick
       evenly. */
    CHECK(mean_bits[0] == 0u && mean_bits[1] == 0u);
    CHECK(mean_bits[4] > mean_bits[3] && mean_bits[3] > mean_bits[2]);
    CHECK(mean_bits[4] > mean_bits[5] && mean_bits[5] > mean_bits[6]);
    CHECK(mean_bits[4] >= 2u * mean_bits[2]);
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
        test_estimator_refuses_cyclic_beats,
        test_estimator_refuses_long_cycles,
        test_estimator_keeps_crediting_honest_signals,
        test_estimator_never_exceeds_eight_lags,
        test_estimator_tracks_tick_quantization,
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
