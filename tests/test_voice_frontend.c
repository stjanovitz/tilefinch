#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "audio_gate.h"
#include "resampler.h"
#include "tilefinch/voice_job_lifecycle.h"
#include "tilefinch/voice_model_policy.h"

static uint32_t rng_state = 0x51a7c0deU;

static uint32_t next_random(void)
{
    rng_state = rng_state * 1664525U + 1013904223U;
    return rng_state;
}

static void test_resampler_length(size_t count)
{
    const int16_t canary_left = 12345;
    const int16_t canary_right = -23456;
    size_t capacity = (count * 16000U) / 22050U + 2U;
    int16_t *input_storage = calloc(count + 2U, sizeof(*input_storage));
    int16_t *inplace_storage = calloc(count + 2U, sizeof(*inplace_storage));
    int16_t *output_storage = calloc(capacity + 2U, sizeof(*output_storage));
    int16_t *input;
    int16_t *inplace;
    int16_t *output;
    size_t expected_count;
    size_t inplace_count;
    size_t index;

    assert(input_storage && inplace_storage && output_storage);
    input = input_storage + 1;
    inplace = inplace_storage + 1;
    output = output_storage + 1;
    input_storage[0] = inplace_storage[0] = output_storage[0] = canary_left;
    input_storage[count + 1U] = inplace_storage[count + 1U] = canary_right;
    output_storage[capacity + 1U] = canary_right;
    for (index = 0; index < count; ++index)
        input[index] = (int16_t)(next_random() >> 16);
    memcpy(inplace, input, count * sizeof(*input));

    expected_count = resample_22050_to_16000(
        input,
        count,
        output,
        capacity
    );
    inplace_count = resample_22050_to_16000_inplace(inplace, count);
    assert(expected_count == inplace_count);
    if (memcmp(output, inplace, expected_count * sizeof(*output)) != 0) {
        for (index = 0; index < expected_count; ++index) {
            if (output[index] != inplace[index]) {
                fprintf(
                    stderr,
                    "resampler mismatch: input=%zu output_index=%zu "
                    "reference=%d inplace=%d\n",
                    count,
                    index,
                    output[index],
                    inplace[index]
                );
                break;
            }
        }
    }
    assert(memcmp(output, inplace, expected_count * sizeof(*output)) == 0);
    assert(input_storage[0] == canary_left);
    assert(input_storage[count + 1U] == canary_right);
    assert(inplace_storage[0] == canary_left);
    assert(inplace_storage[count + 1U] == canary_right);
    assert(output_storage[0] == canary_left);
    assert(output_storage[capacity + 1U] == canary_right);

    free(output_storage);
    free(inplace_storage);
    free(input_storage);
}

static void test_audio_gate(void)
{
    const size_t one_second = 22050;
    int16_t *samples = calloc(one_second, sizeof(*samples));
    size_t index;
    int64_t sum;

    assert(samples);
    assert(inspect_and_condition_capture(NULL, 0, 22050) == CAPTURE_EMPTY);
    assert(
        inspect_and_condition_capture(samples, 2205, 22050)
        == CAPTURE_TOO_SHORT
    );
    assert(
        inspect_and_condition_capture(samples, one_second, 22050)
        == CAPTURE_TOO_QUIET
    );
    for (index = 0; index < one_second; ++index)
        samples[index] = index & 1U ? 32767 : -32768;
    assert(
        inspect_and_condition_capture(samples, one_second, 22050)
        == CAPTURE_TOO_CLIPPED
    );
    for (index = 0; index < one_second; ++index)
        samples[index] = (index / 100U) & 1U ? 3000 : 1000;
    assert(inspect_and_condition_capture(samples, one_second, 22050) == CAPTURE_OK);
    sum = 0;
    for (index = 0; index < one_second; ++index)
        sum += samples[index];
    /* Integer mean removal leaves less than one sample unit of DC error. */
    assert(sum > -(int64_t)one_second && sum < (int64_t)one_second);
    for (index = 0; index < one_second; ++index)
        samples[index] = index & 1U ? 2500 : -2500;
    assert(
        inspect_and_condition_capture(samples, one_second, 22050)
        == CAPTURE_TOO_NOISY
    );
    free(samples);
}

static void test_model_policy(void)
{
    const size_t mib = 1024u * 1024u;
    size_t extra_full = voice_model_tier_working_bytes_for_cache(
        VOICE_MODEL_EXTRA_WIDE, VOICE_CACHE_FULL_ROWS);
    size_t extra_balanced = voice_model_tier_working_bytes_for_cache(
        VOICE_MODEL_EXTRA_WIDE, VOICE_CACHE_BALANCED_ROWS);
    size_t extra_compact = voice_model_tier_working_bytes_for_cache(
        VOICE_MODEL_EXTRA_WIDE, VOICE_CACHE_COMPACT_ROWS);
    size_t small_full = voice_model_tier_working_bytes_for_cache(
        VOICE_MODEL_SMALL, VOICE_CACHE_FULL_ROWS);
    assert(voice_model_policy_select(extra_full + 2u * mib, 4u * mib)
           == VOICE_MODEL_EXTRA_WIDE);
    assert(voice_model_policy_select(extra_full + 2u * mib - 1u, 4u * mib)
           == VOICE_MODEL_SMALL);
    assert(voice_model_policy_select(small_full + 2u * mib, 4u * mib)
           == VOICE_MODEL_SMALL);
    assert(voice_model_policy_select(small_full + 2u * mib - 1u, 4u * mib)
           == VOICE_MODEL_NONE);
    assert(voice_model_policy_select(20u * mib, 4u * mib - 1u)
           == VOICE_MODEL_NONE);
    assert(voice_model_policy_select_available(
               24u * mib, 12u * mib, extra_full + 2u * mib)
           == VOICE_MODEL_EXTRA_WIDE);
    assert(voice_model_policy_select_available(
               24u * mib, 12u * mib, small_full + 2u * mib)
           == VOICE_MODEL_SMALL);
    assert(voice_model_policy_select_available(
               24u * mib, 12u * mib, small_full + 2u * mib - 1u)
           == VOICE_MODEL_NONE);
    assert(voice_model_policy_select_available(
               24u * mib, 3u * mib, 20u * mib)
           == VOICE_MODEL_NONE);
    assert(
        strcmp(voice_model_tier_name(VOICE_MODEL_EXTRA_WIDE), "extra-wide")
        == 0);
    assert(voice_model_tier_working_bytes(VOICE_MODEL_SMALL) == small_full);
    assert(voice_model_tier_working_bytes(VOICE_MODEL_EXTRA_WIDE)
           == extra_full);
    assert(voice_model_selection_margin_bytes() == 2u * mib);
    assert(voice_model_largest_allocation_bytes() == 4u * mib);
    VoiceModelAdmission admission = voice_model_policy_admit(
        24u * mib, 12u * mib, extra_full + 2u * mib, false);
    assert(admission.tier == VOICE_MODEL_EXTRA_WIDE
           && admission.cache_rows == VOICE_CACHE_FULL_ROWS
           && admission.working_bytes == extra_full);
    admission = voice_model_policy_admit(
        24u * mib, 12u * mib, extra_balanced + 2u * mib, true);
    assert(admission.tier == VOICE_MODEL_EXTRA_WIDE
           && admission.cache_rows == VOICE_CACHE_BALANCED_ROWS
           && admission.working_bytes == extra_balanced);
    admission = voice_model_policy_admit(
        24u * mib, 12u * mib, extra_compact + 2u * mib, true);
    assert(admission.tier == VOICE_MODEL_EXTRA_WIDE
           && admission.cache_rows == VOICE_CACHE_COMPACT_ROWS
           && admission.working_bytes == extra_compact);
    assert(!voice_model_policy_should_evict(
        VOICE_MODEL_EXTRA_WIDE, 3u * mib, 768u * 1024u));
    assert(voice_model_policy_should_evict(
        VOICE_MODEL_EXTRA_WIDE, 3u * mib - 1u, 768u * 1024u));
    assert(voice_model_policy_should_evict(
        VOICE_MODEL_SMALL, 8u * mib, 768u * 1024u - 1u));
    assert(!voice_model_policy_should_evict(
        VOICE_MODEL_NONE, 0, 0));
}

typedef struct {
    uint64_t now;
    bool spawn_succeeds;
    bool complete;
    unsigned spawn_calls;
    unsigned poll_calls;
} FakeVoicePlatform;

static bool fake_voice_spawn(void *user)
{
    FakeVoicePlatform *fake = user;
    fake->spawn_calls++;
    return fake->spawn_succeeds;
}

static bool fake_voice_poll(void *user)
{
    FakeVoicePlatform *fake = user;
    fake->poll_calls++;
    return fake->complete;
}

static uint64_t fake_voice_now(void *user)
{
    return ((FakeVoicePlatform *) user)->now;
}

static VoiceJobLifecyclePlatform fake_voice_ops(FakeVoicePlatform *fake)
{
    VoiceJobLifecyclePlatform platform = {
        .spawn = fake_voice_spawn,
        .poll_complete = fake_voice_poll,
        .now_microseconds = fake_voice_now,
        .user = fake
    };
    return platform;
}

static void test_voice_job_lifecycle(void)
{
    VoiceJobLifecycle lifecycle;
    FakeVoicePlatform never = {
        .now = 100, .spawn_succeeds = true
    };
    VoiceJobLifecyclePlatform never_ops = fake_voice_ops(&never);
    assert(voice_job_lifecycle_start(&lifecycle, &never_ops, 50)
           == VOICE_JOB_LIFECYCLE_RUNNING);
    never.now = 149;
    assert(voice_job_lifecycle_pump(&lifecycle, &never_ops)
           == VOICE_JOB_LIFECYCLE_RUNNING);
    never.now = 150;
    assert(voice_job_lifecycle_pump(&lifecycle, &never_ops)
           == VOICE_JOB_LIFECYCLE_CANCEL_REQUESTED);
    assert(voice_job_lifecycle_cancelled(&lifecycle));
    assert(voice_job_lifecycle_disables_voice(&lifecycle));
    never.now = 1000;
    assert(voice_job_lifecycle_pump(&lifecycle, &never_ops)
           == VOICE_JOB_LIFECYCLE_CANCEL_REQUESTED);
    assert(!lifecycle.completed);

    /* Completion racing immediately after the timeout remains worker-owned
       until the following poll observes it. */
    never.complete = true;
    assert(voice_job_lifecycle_pump(&lifecycle, &never_ops)
           == VOICE_JOB_LIFECYCLE_COMPLETED_AFTER_TIMEOUT);
    assert(lifecycle.completed);

    FakeVoicePlatform mid_decode = {
        .now = 500, .spawn_succeeds = true
    };
    VoiceJobLifecyclePlatform mid_ops = fake_voice_ops(&mid_decode);
    assert(voice_job_lifecycle_start(&lifecycle, &mid_ops, 1000)
           == VOICE_JOB_LIFECYCLE_RUNNING);
    voice_job_lifecycle_request_cancel(&lifecycle);
    assert(voice_job_lifecycle_cancelled(&lifecycle));
    assert(!voice_job_lifecycle_disables_voice(&lifecycle));
    assert(voice_job_lifecycle_pump(&lifecycle, &mid_ops)
           == VOICE_JOB_LIFECYCLE_RUNNING);
    mid_decode.complete = true;
    assert(voice_job_lifecycle_pump(&lifecycle, &mid_ops)
           == VOICE_JOB_LIFECYCLE_COMPLETED);

    FakeVoicePlatform failed = {
        .now = 1, .spawn_succeeds = false
    };
    VoiceJobLifecyclePlatform failed_ops = fake_voice_ops(&failed);
    assert(voice_job_lifecycle_start(&lifecycle, &failed_ops, 10)
           == VOICE_JOB_LIFECYCLE_START_FAILED);
    assert(failed.spawn_calls == 1);
}

int main(void)
{
    static const size_t edge_lengths[] = {
        0, 1, 2, 7, 8, 9, 16, 17, 18, 63, 64, 65,
        1023, 1024, 1025, 61437, 66150
    };
    size_t index;

    test_audio_gate();
    test_model_policy();
    test_voice_job_lifecycle();
    for (index = 0; index < sizeof(edge_lengths) / sizeof(edge_lengths[0]); ++index)
        test_resampler_length(edge_lengths[index]);
    for (index = 0; index < 2000; ++index)
        test_resampler_length(next_random() % 4097U);
    puts("voice-frontend-tests: ok (model policy + 2,017 resampler cases)");
    return 0;
}
