#undef NDEBUG
#include <assert.h>
#include <stdio.h>

/* Compile the same mixer with a test-only reference switch. No reference
   implementation or runtime switch is linked into the browser. */
#define TILEFINCH_GAME_AUDIO_MIX_TEST 1
#define TILEFINCH_PSP_VALIDATION_LOG 1
#include "../src/game_audio.c"

static uint32_t random_state = 0x6578419u;
static uint32_t random_word(void)
{
    random_state ^= random_state << 13;
    random_state ^= random_state >> 17;
    random_state ^= random_state << 5;
    return random_state;
}

static void prepare_buffers(TilefinchGameAudio *audio)
{
    for (size_t slot = 0; slot < 2u; slot++) {
        GameAudioBuffer *buffer = &audio->buffers[slot];
        buffer->frames = 97u;
        buffer->sample_rate = 22050u;
        buffer->channels = (uint8_t) (slot + 1u);
        buffer->generation = 1u;
        size_t bytes = buffer->frames * buffer->channels * sizeof(int16_t);
        buffer->samples = budget_malloc_category(
            audio->budget, BUDGET_CATEGORY_JAVASCRIPT, bytes);
        assert(buffer->samples != NULL);
        audio->pcm_bytes += bytes;
        for (size_t at = 0; at < buffer->frames * buffer->channels; at++)
            buffer->samples[at] = (int16_t) (at * 7919u);
    }
}

static uint32_t start_voice(TilefinchGameAudio *audio, size_t slot,
                            double delay)
{
    uint32_t handle = 0;
    if (slot < 2u) {
        assert(tilefinch_game_audio_start_oscillator(audio,
            (TilefinchGameAudioOscillatorType) (slot + 1u),
            173.0 + slot * 137.0, 0, 0, delay, &handle));
    } else {
        assert(tilefinch_game_audio_start(audio,
            (1u << 4) | (uint32_t) (slot - 1u), 0, 0, 1.375,
            0, 0, slot == 2u, 0, 0, delay, &handle));
    }
    return handle;
}

static void compare_block(TilefinchGameAudio *fast,
                          TilefinchGameAudio *reference, size_t frames)
{
    int16_t actual[TILEFINCH_GAME_AUDIO_OUTPUT_FRAMES * 2u];
    int16_t expected[TILEFINCH_GAME_AUDIO_OUTPUT_FRAMES * 2u];
    memset(actual, 0x47, sizeof(actual));
    memset(expected, 0x47, sizeof(expected));
    assert(tilefinch_game_audio_mix(fast, actual, frames));
    assert(tilefinch_game_audio_mix(reference, expected, frames));
    assert(memcmp(actual, expected, sizeof(actual)) == 0);
    /* Includes phase, fractional buffer position, curve interpolation state,
       envelope cursors and generation-tagged completion ownership. */
    assert(memcmp(fast->voices, reference->voices,
                  sizeof(fast->voices)) == 0);
    assert(atomic_load(&fast->output_frame)
           == atomic_load(&reference->output_frame));
}

static void test_block_record_bounds(void)
{
    assert(tilefinch_game_audio_validation_mix_work(false));
    assert(tilefinch_game_audio_validation_mix_measure(true));
    assert(!tilefinch_game_audio_validation_mix_work(true));
    TilefinchGameAudioBlockRecord record = {
        .begin_before_us=100, .begin_after_us=104,
        .mixed_before_us=800, .mixed_after_us=805,
        .submitted_before_us=12000, .submitted_after_us=12005,
        .begin_cpu_us=1000, .mixed_cpu_us=1700, .submitted_cpu_us=1760,
        .frames=512, .silent=1024, .audible=512, .constant=1024,
        .automated=512, .fast_forwarded=512, .clocks_valid=true
    };
    /* The worker holds this latch while storing. Simulate the same bounded
       publication and ensure readers cannot observe a half-published slot. */
    atomic_store(&game_audio_validation_metrics_owned, true);
    for (size_t at = 0; at <= TILEFINCH_GAME_AUDIO_BLOCK_RECORD_LIMIT; at++) {
        record.output_frame = (uint32_t)(at * 512u);
        game_audio_validation_store_block(&record);
    }
    TilefinchGameAudioBlockRecord copied;
    assert(!tilefinch_game_audio_validation_mix_block(0, &copied));
    atomic_store(&game_audio_validation_metrics_owned, false);
    TilefinchGameAudioMixMetrics metrics;
    assert(tilefinch_game_audio_validation_mix_metrics(&metrics));
    assert(metrics.block_records == TILEFINCH_GAME_AUDIO_BLOCK_RECORD_LIMIT);
    assert(metrics.record_overflow == 1);
    for (size_t at = 0; at < TILEFINCH_GAME_AUDIO_BLOCK_RECORD_LIMIT; at++) {
        assert(tilefinch_game_audio_validation_mix_block(at, &copied));
        assert(copied.output_frame == at * 512u && copied.frames == 512);
        assert(copied.begin_before_us == 100 && copied.mixed_cpu_us == 1700);
        assert(copied.silent == 1024 && copied.automated == 512);
        assert(copied.clocks_valid);
    }
    assert(!tilefinch_game_audio_validation_mix_block(
        TILEFINCH_GAME_AUDIO_BLOCK_RECORD_LIMIT, &copied));
    assert(!tilefinch_game_audio_validation_mix_block(0, NULL));
    assert(tilefinch_game_audio_validation_mix_measure(false));
    assert(tilefinch_game_audio_validation_mix_measure(true));
    assert(!tilefinch_game_audio_validation_mix_block(0, &copied));
    assert(tilefinch_game_audio_validation_mix_metrics(&metrics));
    assert(metrics.block_records == 0 && metrics.record_overflow == 0);
    assert(tilefinch_game_audio_validation_mix_measure(false));
    assert(tilefinch_game_audio_validation_mix_work(true));
}

static void test_gain_curve_quantization_boundaries(void)
{
    /* Exercise the public publication contract, not the conversion helper:
       this test intentionally passes with the old correct double arithmetic. */
    const float half_quantum = 0x1p-17f;
    const float one_and_half_quantum = 1.f + half_quantum;
    const struct { float value; uint32_t expected; } cases[] = {
        {0.f, 0u}, {-0.f, 0u}, {nextafterf(0.f, 1.f), 0u},
        {nextafterf(half_quantum, 0.f), 0u}, {half_quantum, 1u},
        {nextafterf(half_quantum, 1.f), 1u}, {1.f, 65536u},
        {nextafterf(one_and_half_quantum, 0.f), 65536u},
        {one_and_half_quantum, 65537u},
        {nextafterf(one_and_half_quantum, 2.f), 65537u},
        {nextafterf(4.f, 0.f), 262144u}, {4.f, 262144u},
    };
    Budget budget;
    budget_init(&budget, 64u * 1024u);
    TilefinchGameAudio *audio = tilefinch_game_audio_create(&budget);
    assert(audio != NULL);
    uint32_t handle = 0;
    assert(tilefinch_game_audio_start_oscillator(audio,
        TILEFINCH_GAME_AUDIO_OSCILLATOR_TRIANGLE, 321, .3, .17, 0, &handle));
    GameAudioVoice *voice = game_audio_voice(audio, handle);
    assert(voice != NULL);
    size_t allocations = budget.allocation_count;
    for (size_t at = 0; at < sizeof(cases) / sizeof(*cases); at++) {
        float values[] = {.25f, cases[at].value, .75f};
        assert(tilefinch_game_audio_cancel_envelope(audio, handle));
        assert(tilefinch_game_audio_schedule_curve(audio, handle, false,
            values, 3u, .31, .71, 0, .02));
        assert(atomic_load(&voice->gain_curve.count) == 3u);
        assert(atomic_load(&voice->gain_curve.points[0]) == 16384u);
        assert(atomic_load(&voice->gain_curve.points[1]) == cases[at].expected);
        assert(atomic_load(&voice->gain_curve.points[2]) == 49152u);
    }
    const float invalid[] = {
        nextafterf(0.f, -1.f), -1.f, nextafterf(4.f, INFINITY),
        INFINITY, -INFINITY, NAN,
    };
    for (size_t at = 0; at < sizeof(invalid) / sizeof(*invalid); at++) {
        float values[] = {.25f, invalid[at], .75f};
        assert(tilefinch_game_audio_cancel_envelope(audio, handle));
        unsigned char before[sizeof(*voice)];
        memcpy(before, voice, sizeof(before));
        assert(!tilefinch_game_audio_schedule_curve(audio, handle, false,
            values, 3u, .31, .71, 0, .02));
        assert(memcmp(before, voice, sizeof(before)) == 0);
    }
    assert(budget.allocation_count == allocations);
    tilefinch_game_audio_destroy(audio);
    assert(budget.current == 0u && budget.allocation_head == NULL);
}

static void test_sequential_curve_state(void)
{
    static const uint32_t counts[] = {2u, 3u, 32u, 64u};
    static const uint32_t starts[] = {0u, 123u, UINT32_MAX - 127u};
    static const uint32_t durations[] = {1u, 2u, 63u, 65537u, 441000u};
    uint32_t seed = 123456789u;
    uint32_t fast_samples = game_audio_test_curve_fast_samples;
    for (size_t c = 0; c < sizeof(counts) / sizeof(*counts); c++)
        for (size_t s = 0; s < sizeof(starts) / sizeof(*starts); s++)
            for (size_t d = 0; d < sizeof(durations) / sizeof(*durations); d++) {
                GameAudioCurveState fast = {0};
                fast.count = counts[c];
                fast.start = starts[s];
                fast.duration = durations[d];
                uint32_t distance = (fast.count - 1u) << 16;
                fast.step = distance / fast.duration;
                fast.remainder = distance % fast.duration;
                for (size_t p = 0; p < fast.count; p++) {
                    seed = seed * 1664525u + 1013904223u;
                    fast.points[p] = seed;
                }
                GameAudioCurveState reference = fast;
                uint32_t frame = fast.start - 2u;
                for (unsigned sample = 0; sample < 256u; sample++, frame++) {
                    if (sample == 128u)
                        frame = fast.start + fast.duration - 2u;
                    if (sample == 200u) frame = fast.start + fast.duration / 2u;
                    uint32_t actual = 0, expected = 0;
                    bool a = game_audio_active_curve_value(&fast, frame, &actual, true);
                    bool b = game_audio_curve_value(&reference, frame, &expected, true);
                    assert(a == b && actual == expected);
                    assert(memcmp(&fast, &reference, sizeof(fast)) == 0);
                }
            }
    assert(game_audio_test_curve_fast_samples > fast_samples);
}

static void test_span_admission_and_refusal(void)
{
    for (unsigned refuse = 0; refuse < 2u; refuse++) {
        Budget budgets[2];
        TilefinchGameAudio *audio[2];
        uint32_t handles[2][2];
        for (unsigned lane = 0; lane < 2u; lane++) {
            budget_init(&budgets[lane], 64u * 1024u);
            if (refuse && lane == 0u)
                budget_inject_failure_after(&budgets[lane], 1u);
            audio[lane] = tilefinch_game_audio_create(&budgets[lane]);
            assert(audio[lane] != NULL);
            assert((audio[lane]->constant_accum == NULL)
                   == (refuse && lane == 0u));
            if (!refuse && lane == 1u) audio[lane]->reference_mix = true;
            assert(tilefinch_game_audio_resume(audio[lane]));
            for (size_t slot = 0; slot < 2u; slot++) {
                assert(tilefinch_game_audio_start_oscillator(audio[lane],
                    (TilefinchGameAudioOscillatorType) (slot + 1u),
                    173 + 137 * slot, .25, .125, 0, &handles[lane][slot]));
            }
        }
        assert(memcmp(handles[0], handles[1], sizeof(handles[0])) == 0);
        size_t allocations[2] = {
            budgets[0].allocation_count, budgets[1].allocation_count};
        compare_block(audio[0], audio[1], 512u);
        assert(audio[0]->constant_span_blocks == (refuse ? 0u : 1u));
        static const float gain[] = {1.f, .10001f, .70003f};
        static const float pitch[] = {1769.f, 601.f, 191.f};
        for (unsigned lane = 0; lane < 2u; lane++) {
            assert(tilefinch_game_audio_schedule_curve(audio[lane],
                handles[lane][0], false, gain, 3u, .7, .2, 0, .05));
            assert(tilefinch_game_audio_schedule_curve(audio[lane],
                handles[lane][0], true, pitch, 3u, 1, 1, 0, .04));
        }
        uint32_t fast_curve_samples = game_audio_test_curve_fast_samples;
        for (unsigned block = 0; block < 3u; block++)
            compare_block(audio[0], audio[1], 512u);
        assert(audio[0]->active_span_blocks == (refuse ? 0u : 3u));
        /* In the refusal case lane 1 remains the admitted optimized oracle. */
        assert(game_audio_test_curve_fast_samples > fast_curve_samples);
        for (unsigned lane = 0; lane < 2u; lane++) {
            assert(tilefinch_game_audio_cancel_envelope(audio[lane], handles[lane][0]));
            assert(tilefinch_game_audio_cancel_pitch_curve(audio[lane], handles[lane][0]));
            tilefinch_game_audio_stop(audio[lane], handles[lane][0], 1.0 / 44100.0);
        }
        compare_block(audio[0], audio[1], 512u);
        uint32_t ended[2] = {0, 0};
        assert(tilefinch_game_audio_take_completion(audio[0], &ended[0]));
        assert(tilefinch_game_audio_take_completion(audio[1], &ended[1]));
        assert(ended[0] == ended[1]);
        compare_block(audio[0], audio[1], 1u);
        for (unsigned lane = 0; lane < 2u; lane++) {
            assert(budgets[lane].allocation_count == allocations[lane]);
            tilefinch_game_audio_destroy(audio[lane]);
            assert(budgets[lane].current == 0u
                   && budgets[lane].allocation_head == NULL);
        }
    }
    Budget refused;
    budget_init(&refused, 64u * 1024u);
    budget_inject_failure_after(&refused, 0u);
    assert(tilefinch_game_audio_create(&refused) == NULL);
    assert(refused.current == 0u && refused.allocation_head == NULL);
}

static void test_silent_loop_bulk_advance(void)
{
    Budget budgets[2];
    TilefinchGameAudio *audio[2];
    uint32_t handle[2];
    for (unsigned lane = 0; lane < 2; lane++) {
        budget_init(&budgets[lane], 64u * 1024u);
        audio[lane] = tilefinch_game_audio_create(&budgets[lane]);
        assert(audio[lane] != NULL);
        prepare_buffers(audio[lane]);
        assert(tilefinch_game_audio_resume(audio[lane]));
        assert(tilefinch_game_audio_start(audio[lane], 17u, 0, 0, 1,
            0, 0, true, 0, 0, 0, &handle[lane]));
    }
    audio[1]->reference_mix = true;
    const uint32_t steps[] = {0, 1, 11888, 32768, 65535};
    const size_t blocks[] = {0, 1, 2, 31, 255, 512};
    const uint32_t lengths[] = {1, 3, 74};
    size_t allocations[2] = {budgets[0].allocation_count,
                             budgets[1].allocation_count};
    unsigned cases = 0;
    for (size_t s = 0; s < sizeof(steps) / sizeof(*steps); s++)
        for (size_t n = 0; n < sizeof(blocks) / sizeof(*blocks); n++)
            for (size_t l = 0; l < sizeof(lengths) / sizeof(*lengths); l++)
                for (unsigned p = 0; p < 4; p++)
                    for (uint32_t fraction = 0; fraction < 65536u; fraction += 21845u) {
                        for (unsigned lane = 0; lane < 2; lane++) {
                            GameAudioVoice *v = game_audio_voice(audio[lane], handle[lane]);
                            assert(v != NULL);
                            /* Seed reachable interpolation/wrap states directly;
                               command admission itself is tested separately. */
                            v->loop_begin_frame = 23u;
                            v->loop_end_frame = 23u + lengths[l];
                            v->position_frame = p == 0 ? 0u : p == 1 ? 23u
                                : p == 2 ? v->loop_end_frame - 1u : v->loop_end_frame;
                            v->fraction_q16 = fraction;
                            v->step_whole = 0;
                            atomic_store(&v->step_fraction, steps[s]);
                        }
                        uint32_t before = audio[0]->fast_forwarded_samples;
                        compare_block(audio[0], audio[1], blocks[n]);
                        assert(audio[0]->fast_forwarded_samples - before == blocks[n]);
                        cases++;
                    }
    /* An audible continuation checks that the retained source position is
       useful, not just equal while output happens to be all zero. */
    for (unsigned lane = 0; lane < 2; lane++)
        assert(tilefinch_game_audio_update_voice(audio[lane], handle[lane], .5, .25));
    compare_block(audio[0], audio[1], 512);
    for (unsigned lane = 0; lane < 2; lane++) {
        assert(budgets[lane].allocation_count == allocations[lane]);
        tilefinch_game_audio_destroy(audio[lane]);
        assert(budgets[lane].current == 0);
    }
    printf("silent-loop parity: %u blocks plus audible continuation\n", cases);
}

int main(void)
{
    test_sequential_curve_state();
    test_silent_loop_bulk_advance();
    test_span_admission_and_refusal();
    test_block_record_bounds();
    test_gain_curve_quantization_boundaries();
    /* Exhaustive fractions at awkward endpoints pin descending truncation.
       A weighted-average rewrite must fail this independent arithmetic oracle. */
    static const uint32_t endpoints[] = {0u, 1u, 262144u, 1947830973u, UINT32_MAX};
    for (size_t from = 0; from < sizeof(endpoints) / sizeof(*endpoints); from++)
        for (size_t to = 0; to < sizeof(endpoints) / sizeof(*endpoints); to++)
            for (uint32_t fraction = 0u; fraction < 65536u; fraction++) {
                int64_t delta = (int64_t) endpoints[to] - endpoints[from];
                uint32_t expected = (uint32_t) ((int64_t) endpoints[from]
                    + delta * fraction / 65536);
                assert(game_audio_curve_interpolate(endpoints[from], endpoints[to],
                    fraction) == expected);
            }
    TilefinchGameAudioMixProbe probe;
    bool probe_okay = tilefinch_game_audio_validation_mix_probe(&probe);
    if (!probe_okay)
        fprintf(stderr, "mixer corpus failed: blocks=%u pcm=%u state=%u\n",
            probe.blocks, probe.pcm_mismatches, probe.state_mismatches);
    assert(probe_okay);
    assert(probe.blocks == 256u && probe.pcm_mismatches == 0u
           && probe.state_mismatches == 0u);
    assert(tilefinch_game_audio_validation_automation_probe(&probe));
    assert(probe.blocks == 256u && probe.pcm_mismatches == 0u
           && probe.state_mismatches == 0u);
    assert(tilefinch_game_audio_validation_constant_probe(&probe));
    assert(probe.blocks == 256u && probe.pcm_mismatches == 0u
           && probe.state_mismatches == 0u);
    assert(tilefinch_game_audio_validation_observer_probe(&probe));
    assert(probe.blocks == 256u && probe.pcm_mismatches == 0u
           && probe.state_mismatches == 0u);
    Budget fast_budget, reference_budget;
    budget_init(&fast_budget, 1024u * 1024u);
    budget_init(&reference_budget, 1024u * 1024u);
    TilefinchGameAudio *fast = tilefinch_game_audio_create(&fast_budget);
    TilefinchGameAudio *reference = tilefinch_game_audio_create(&reference_budget);
    assert(fast != NULL && reference != NULL);
    reference->reference_mix = true;
    prepare_buffers(fast);
    prepare_buffers(reference);
    assert(tilefinch_game_audio_resume(fast));
    assert(tilefinch_game_audio_resume(reference));
    uint32_t handles[4];
    for (size_t at = 0; at < 4u; at++) {
        handles[at] = start_voice(fast, at, at == 1u ? .003 : 0);
        assert(handles[at] == start_voice(reference, at, at == 1u ? .003 : 0));
    }
    /* Silent blocks followed by audible output pin the skipped phase and
       looping-buffer advancement. Also exercise zero-sized output blocks. */
    compare_block(fast, reference, 0);
    for (size_t block = 0; block < 5u; block++)
        compare_block(fast, reference, TILEFINCH_GAME_AUDIO_OUTPUT_FRAMES);
    assert(tilefinch_game_audio_validation_mix_work(false));
    assert(tilefinch_game_audio_validation_mix_measure(true));
    compare_block(fast, reference, TILEFINCH_GAME_AUDIO_OUTPUT_FRAMES);
    assert(!fast->validation_work_counted && !reference->validation_work_counted);
    assert(fast->validation_work.silent == 0 && fast->validation_work.automated == 0);
    assert(tilefinch_game_audio_validation_mix_measure(false));
    assert(tilefinch_game_audio_validation_mix_work(true));
    assert(tilefinch_game_audio_validation_mix_measure(true));
    compare_block(fast, reference, TILEFINCH_GAME_AUDIO_OUTPUT_FRAMES);
    assert(fast->validation_work_counted && reference->validation_work_counted);
    /* The non-looping buffer may have completed already; count only live
       voice samples, not the configured pool capacity. */
    assert(fast->validation_work.silent + fast->validation_work.audible > 0u);
    assert(fast->validation_work.silent + fast->validation_work.audible <= 4u * 512u);
    assert(tilefinch_game_audio_validation_mix_measure(false));
    for (size_t at = 0; at < 3u; at++) {
        assert(tilefinch_game_audio_update_voice(fast, handles[at], .25, .125));
        assert(tilefinch_game_audio_update_voice(reference, handles[at], .25, .125));
    }
    compare_block(fast, reference, TILEFINCH_GAME_AUDIO_OUTPUT_FRAMES);
    /* Equal targets may share the recurrence only while both current gains
       also match. Later equal targets after a panned span must not collapse
       its still-different gains. */
    for (size_t pass = 0; pass < 2u; pass++) {
        for (size_t at = 0; at < 2u; at++) {
            assert(tilefinch_game_audio_cancel_envelope(fast, handles[at]));
            assert(tilefinch_game_audio_cancel_envelope(reference, handles[at]));
            assert(tilefinch_game_audio_update_voice(fast, handles[at], .25, .25));
            assert(tilefinch_game_audio_update_voice(reference, handles[at], .25, .25));
            for (size_t segment = 0; segment < 2u; segment++) {
                double left = segment == 0u ? .3 : .2;
                double right = pass == segment ? left : .1;
                double delay = segment * .003;
                assert(tilefinch_game_audio_schedule_envelope_target(fast,
                    handles[at], left, right, delay, .002));
                assert(tilefinch_game_audio_schedule_envelope_target(reference,
                    handles[at], left, right, delay, .002));
            }
        }
        for (size_t block = 0; block < 10u; block++)
            compare_block(fast, reference, TILEFINCH_GAME_AUDIO_OUTPUT_FRAMES);
    }
    const float gain_curve[] = {0, .8f, 0};
    const float pitch_curve[] = {100, 1700, 320};
    for (size_t block = 0; block < 3000u; block++) {
        uint32_t completed_fast, completed_reference;
        while (tilefinch_game_audio_take_completion(fast, &completed_fast)) {
            assert(tilefinch_game_audio_take_completion(reference, &completed_reference));
            assert(completed_fast == completed_reference);
            size_t slot = (completed_fast & 15u) - 1u;
            handles[slot] = start_voice(fast, slot, (block % 3u) * .004);
            assert(handles[slot] == start_voice(reference, slot, (block % 3u) * .004));
        }
        assert(!tilefinch_game_audio_take_completion(reference, &completed_reference));
        size_t slot = random_word() % 4u;
        uint32_t command = random_word() % 8u;
        double delay = (random_word() % 700u) / 44100.0;
        double gain = (random_word() % 3u) * .25;
        bool a, b;
        switch (command) {
        case 0:
            a = tilefinch_game_audio_update_voice(fast, handles[slot], gain, gain);
            b = tilefinch_game_audio_update_voice(reference, handles[slot], gain, gain);
            assert(a == b);
            break;
        case 1:
            a = tilefinch_game_audio_schedule_curve(fast, handles[slot], false,
                gain_curve, 3u, .7, .3, delay, .009);
            b = tilefinch_game_audio_schedule_curve(reference, handles[slot], false,
                gain_curve, 3u, .7, .3, delay, .009);
            assert(a == b);
            break;
        case 2:
            a = tilefinch_game_audio_schedule_curve(fast, handles[slot], true,
                pitch_curve, 3u, 1, 1, delay, .007);
            b = tilefinch_game_audio_schedule_curve(reference, handles[slot], true,
                pitch_curve, 3u, 1, 1, delay, .007);
            assert(a == b);
            break;
        case 3:
            a = tilefinch_game_audio_schedule_envelope_target(
                fast, handles[slot], gain, 0, delay, .002);
            b = tilefinch_game_audio_schedule_envelope_target(
                reference, handles[slot], gain, 0, delay, .002);
            assert(a == b);
            break;
        case 4:
            a = tilefinch_game_audio_cancel_envelope(fast, handles[slot]);
            b = tilefinch_game_audio_cancel_envelope(reference, handles[slot]);
            assert(a == b);
            break;
        case 5:
            a = tilefinch_game_audio_cancel_pitch_curve(fast, handles[slot]);
            b = tilefinch_game_audio_cancel_pitch_curve(reference, handles[slot]);
            assert(a == b);
            break;
        case 6:
            tilefinch_game_audio_stop(fast, handles[slot], delay);
            tilefinch_game_audio_stop(reference, handles[slot], delay);
            break;
        default:
            a = tilefinch_game_audio_update_oscillator(fast, handles[slot], 617.5);
            b = tilefinch_game_audio_update_oscillator(reference, handles[slot], 617.5);
            assert(a == b);
            break;
        }
        compare_block(fast, reference, random_word() % 513u);
    }
    /* The negative control disables the fast path and must fail these work
       assertions even though its PCM still matches. */
    assert(fast->fast_forwarded_samples > 1024u);
    assert(fast->silent_samples > 1024u);
    assert(fast->constant_path_samples > 1024u);
    assert(reference->constant_path_samples == 0u);
    printf("mixer parity: 3026 blocks; fast-forwarded=%u silent=%u\n",
           fast->fast_forwarded_samples, fast->silent_samples);
    tilefinch_game_audio_destroy(reference);
    tilefinch_game_audio_destroy(fast);
    assert(fast_budget.current == 0u && reference_budget.current == 0u);
    return 0;
}
