#include "stt_engine.h"

#include <pocketsphinx.h>
#include <stdlib.h>
#include <string.h>

#include "audio_gate.h"
#include "resampler.h"

struct SttEngine {
    ps_decoder_t *decoder;
    size_t process_chunk_samples;
    SttPreset preset;
    SttClockMicroseconds clock_microseconds;
    SttObserver observer;
    void *callback_user;
    uint64_t init_microseconds;
};

static uint64_t engine_now(const SttEngine *engine)
{
    if (!engine || !engine->clock_microseconds)
        return 0;
    return engine->clock_microseconds(engine->callback_user);
}

static uint64_t engine_elapsed(const SttEngine *engine, uint64_t started)
{
    uint64_t ended;
    if (!engine || !engine->clock_microseconds)
        return 0;
    ended = engine_now(engine);
    return ended >= started ? ended - started : 0;
}

static void observe(
    SttEngine *engine,
    SttPhase phase,
    size_t completed_samples,
    size_t total_samples
)
{
    if (engine && engine->observer) {
        engine->observer(
            engine->callback_user,
            phase,
            completed_samples,
            total_samples
        );
    }
}

static void result_init(SttResult *result)
{
    if (!result)
        return;
    memset(result, 0, sizeof(*result));
    result->status = STT_STATUS_INVALID_ARGUMENT;
    result->input_status = STT_INPUT_NOT_CHECKED;
}

static int config_is_valid(const SttEngineConfig *config)
{
    return config
        && config->acoustic_model_path
        && config->acoustic_model_path[0]
        && config->dictionary_path
        && config->dictionary_path[0]
        && config->language_model_path
        && config->language_model_path[0]
        && config->process_chunk_samples > 0
        && config->search.downsample_ratio > 0
        && config->search.topn > 0;
}

static SttInputStatus map_capture_status(CaptureStatus status)
{
    switch (status) {
    case CAPTURE_OK:
        return STT_INPUT_ACCEPTED;
    case CAPTURE_EMPTY:
        return STT_INPUT_EMPTY;
    case CAPTURE_TOO_SHORT:
        return STT_INPUT_TOO_SHORT;
    case CAPTURE_TOO_QUIET:
        return STT_INPUT_TOO_QUIET;
    case CAPTURE_TOO_CLIPPED:
        return STT_INPUT_TOO_CLIPPED;
    case CAPTURE_TOO_NOISY:
        return STT_INPUT_TOO_NOISY;
    default:
        return STT_INPUT_NOT_CHECKED;
    }
}

void stt_engine_config_init(SttEngineConfig *config)
{
    if (!config)
        return;
    memset(config, 0, sizeof(*config));
    config->log_level = "ERROR";
    config->process_chunk_samples = STT_ENGINE_DEFAULT_CHUNK_SAMPLES;
    config->sendump_cache_rows = 384;
    stt_engine_config_set_preset(config, STT_PRESET_QUALITY);
}

void stt_engine_config_set_preset(SttEngineConfig *config, SttPreset preset)
{
    if (!config)
        return;
    config->preset = preset;
    config->search.bestpath = 1;
    config->search.downsample_ratio = 1;
    config->search.topn = 4;
    config->search.max_words_per_frame = 10;
    config->search.max_hmms_per_frame = 8000;
    config->search.phone_lookahead_window = 5;
    config->search.forward_flat = 1;
    config->search.forward_flat_beam = 1e-64;
    switch (preset) {
    case STT_PRESET_LATENCY_SAFE:
        config->search.forward_flat_beam = 1e-48;
        break;
    case STT_PRESET_LATENCY_FAST:
        config->search.downsample_ratio = 2;
        config->search.max_words_per_frame = 8;
        config->search.max_hmms_per_frame = 5000;
        break;
    case STT_PRESET_LATENCY_MINIMUM:
        config->search.bestpath = 0;
        config->search.downsample_ratio = 2;
        config->search.topn = 2;
        config->search.max_words_per_frame = 5;
        config->search.max_hmms_per_frame = 3000;
        break;
    case STT_PRESET_QUALITY:
    default:
        config->preset = STT_PRESET_QUALITY;
        break;
    }
}

SttStatus stt_engine_create(
    SttEngine **output_engine,
    const SttEngineConfig *config
)
{
    SttEngine *engine;
    ps_config_t *decoder_config;
    uint64_t started;

    if (output_engine)
        *output_engine = NULL;
    if (!output_engine || !config_is_valid(config))
        return STT_STATUS_INVALID_ARGUMENT;

    engine = calloc(1, sizeof(*engine));
    if (!engine)
        return STT_STATUS_OUT_OF_MEMORY;
    engine->process_chunk_samples = config->process_chunk_samples;
    engine->preset = config->preset;
    engine->clock_microseconds = config->clock_microseconds;
    engine->observer = config->observer;
    engine->callback_user = config->callback_user;

    decoder_config = ps_config_init(NULL);
    if (!decoder_config) {
        free(engine);
        return STT_STATUS_OUT_OF_MEMORY;
    }
    ps_config_set_str(
        decoder_config,
        "hmm",
        config->acoustic_model_path
    );
    ps_config_set_str(decoder_config, "dict", config->dictionary_path);
    ps_config_set_str(decoder_config, "lm", config->language_model_path);
    ps_config_set_int(decoder_config, "samprate", STT_ENGINE_MODEL_RATE);
    ps_config_set_bool(decoder_config, "mmap", 0);
    ps_config_set_int(
        decoder_config, "tilefinch_sendump_cache_rows",
        (long) config->sendump_cache_rows);
    ps_config_set_str(
        decoder_config,
        "loglevel",
        config->log_level ? config->log_level : "ERROR"
    );
    ps_config_set_bool(
        decoder_config,
        "bestpath",
        config->search.bestpath
    );
    ps_config_set_int(
        decoder_config,
        "ds",
        config->search.downsample_ratio
    );
    ps_config_set_int(decoder_config, "topn", config->search.topn);
    ps_config_set_int(
        decoder_config,
        "maxwpf",
        config->search.max_words_per_frame
    );
    ps_config_set_int(
        decoder_config,
        "maxhmmpf",
        config->search.max_hmms_per_frame
    );
    ps_config_set_int(
        decoder_config,
        "pl_window",
        config->search.phone_lookahead_window
    );
    ps_config_set_bool(
        decoder_config,
        "fwdflat",
        config->search.forward_flat
    );
    ps_config_set_float(
        decoder_config,
        "fwdflatbeam",
        config->search.forward_flat_beam
    );

    started = engine_now(engine);
    engine->decoder = ps_init(decoder_config);
    engine->init_microseconds = engine_elapsed(engine, started);
    ps_config_free(decoder_config);
    if (!engine->decoder) {
        free(engine);
        return STT_STATUS_DECODER_INIT_FAILED;
    }
    observe(engine, STT_PHASE_ENGINE_READY, 0, 0);
    *output_engine = engine;
    return STT_STATUS_OK;
}

void stt_engine_destroy(SttEngine *engine)
{
    if (!engine)
        return;
    if (engine->decoder)
        ps_free(engine->decoder);
    memset(engine, 0, sizeof(*engine));
    free(engine);
}

uint64_t stt_engine_init_microseconds(const SttEngine *engine)
{
    return engine ? engine->init_microseconds : 0;
}

SttPreset stt_engine_preset(const SttEngine *engine)
{
    return engine ? engine->preset : STT_PRESET_QUALITY;
}

static SttStatus stt_engine_decode_model_pcm_cancelable(
    SttEngine *engine,
    const int16_t *samples,
    size_t sample_count,
    SttCancelCheck cancel,
    void *cancel_user,
    SttResult *result
)
{
    size_t offset;
    uint64_t total_started;
    uint64_t phase_started;
    const char *hypothesis;
    int32 score = 0;

    result_init(result);
    if (!engine || !engine->decoder || !samples || !sample_count || !result)
        return STT_STATUS_INVALID_ARGUMENT;

    result->input_status = STT_INPUT_MODEL_PCM;
    result->input_samples = sample_count;
    result->model_samples = sample_count;
    total_started = engine_now(engine);
    if (cancel && cancel(cancel_user)) {
        result->status = STT_STATUS_CANCELLED;
        return result->status;
    }

    phase_started = engine_now(engine);
    if (ps_start_utt(engine->decoder) < 0) {
        result->status = STT_STATUS_DECODER_START_FAILED;
        return result->status;
    }
    result->timings.start_utterance_us = engine_elapsed(engine, phase_started);
    observe(engine, STT_PHASE_UTTERANCE_STARTED, 0, sample_count);

    phase_started = engine_now(engine);
    for (offset = 0; offset < sample_count; offset += engine->process_chunk_samples) {
        if (cancel && cancel(cancel_user)) {
            (void) ps_end_utt(engine->decoder);
            result->status = STT_STATUS_CANCELLED;
            return result->status;
        }
        size_t available = sample_count - offset;
        size_t count = available < engine->process_chunk_samples
            ? available
            : engine->process_chunk_samples;
        if (ps_process_raw(engine->decoder, samples + offset, count, 0, 0) < 0) {
            ps_end_utt(engine->decoder);
            result->status = STT_STATUS_DECODER_PROCESS_FAILED;
            return result->status;
        }
        observe(
            engine,
            STT_PHASE_PROCESS_CHUNK,
            offset + count,
            sample_count
        );
    }
    result->timings.process_audio_us = engine_elapsed(engine, phase_started);
    if (cancel && cancel(cancel_user)) {
        (void) ps_end_utt(engine->decoder);
        result->status = STT_STATUS_CANCELLED;
        return result->status;
    }

    phase_started = engine_now(engine);
    if (ps_end_utt(engine->decoder) < 0) {
        result->status = STT_STATUS_DECODER_END_FAILED;
        return result->status;
    }
    result->timings.end_utterance_us = engine_elapsed(engine, phase_started);
    observe(
        engine,
        STT_PHASE_UTTERANCE_ENDED,
        sample_count,
        sample_count
    );
    if (cancel && cancel(cancel_user)) {
        result->status = STT_STATUS_CANCELLED;
        return result->status;
    }

    phase_started = engine_now(engine);
    hypothesis = ps_get_hyp(engine->decoder, &score);
    result->timings.hypothesis_us = engine_elapsed(engine, phase_started);
    result->timings.total_decode_us = engine_elapsed(engine, total_started);
    result->score = (int32_t)score;
    if (hypothesis) {
        strncpy(result->text, hypothesis, sizeof(result->text) - 1);
        result->text[sizeof(result->text) - 1] = '\0';
    }
    observe(
        engine,
        STT_PHASE_HYPOTHESIS_READY,
        sample_count,
        sample_count
    );
    result->status = result->text[0]
        ? STT_STATUS_OK
        : STT_STATUS_NO_HYPOTHESIS;
    return result->status;
}

SttStatus stt_engine_decode_model_pcm(
    SttEngine *engine,
    const int16_t *samples,
    size_t sample_count,
    SttResult *result
)
{
    return stt_engine_decode_model_pcm_cancelable(
        engine, samples, sample_count, NULL, NULL, result);
}

SttStatus stt_engine_decode_capture_pcm_cancelable(
    SttEngine *engine,
    int16_t *samples,
    size_t sample_count,
    SttCancelCheck cancel,
    void *cancel_user,
    SttResult *result
)
{
    CaptureStatus capture_status;
    size_t model_samples;
    SttResult decoded;

    result_init(result);
    if (!engine || !result || (!samples && sample_count))
        return STT_STATUS_INVALID_ARGUMENT;
    if (!sample_count) {
        result->status = STT_STATUS_INPUT_REJECTED;
        result->input_status = STT_INPUT_EMPTY;
        return result->status;
    }
    if (cancel && cancel(cancel_user)) {
        result->status = STT_STATUS_CANCELLED;
        return result->status;
    }
    result->input_samples = sample_count;
    capture_status = inspect_and_condition_capture(
        samples,
        sample_count,
        STT_ENGINE_CAPTURE_RATE
    );
    result->input_status = map_capture_status(capture_status);
    if (capture_status != CAPTURE_OK) {
        result->status = STT_STATUS_INPUT_REJECTED;
        return result->status;
    }
    if (cancel && cancel(cancel_user)) {
        result->status = STT_STATUS_CANCELLED;
        return result->status;
    }

    model_samples = resample_22050_to_16000_inplace(samples, sample_count);
    if (!model_samples) {
        result->status = STT_STATUS_RESAMPLE_FAILED;
        return result->status;
    }
    if (cancel && cancel(cancel_user)) {
        result->status = STT_STATUS_CANCELLED;
        return result->status;
    }
    observe(
        engine,
        STT_PHASE_INPUT_CONDITIONED,
        model_samples,
        sample_count
    );
    stt_engine_decode_model_pcm_cancelable(
        engine, samples, model_samples, cancel, cancel_user, &decoded);
    *result = decoded;
    result->input_status = STT_INPUT_ACCEPTED;
    result->input_samples = sample_count;
    result->model_samples = model_samples;
    return result->status;
}

SttStatus stt_engine_decode_capture_pcm(
    SttEngine *engine,
    int16_t *samples,
    size_t sample_count,
    SttResult *result
)
{
    return stt_engine_decode_capture_pcm_cancelable(
        engine, samples, sample_count, NULL, NULL, result);
}

const char *stt_status_name(SttStatus status)
{
    switch (status) {
    case STT_STATUS_OK:
        return "ok";
    case STT_STATUS_NO_HYPOTHESIS:
        return "no_hypothesis";
    case STT_STATUS_INPUT_REJECTED:
        return "input_rejected";
    case STT_STATUS_INVALID_ARGUMENT:
        return "invalid_argument";
    case STT_STATUS_OUT_OF_MEMORY:
        return "out_of_memory";
    case STT_STATUS_DECODER_INIT_FAILED:
        return "decoder_init_failed";
    case STT_STATUS_DECODER_START_FAILED:
        return "decoder_start_failed";
    case STT_STATUS_DECODER_PROCESS_FAILED:
        return "decoder_process_failed";
    case STT_STATUS_DECODER_END_FAILED:
        return "decoder_end_failed";
    case STT_STATUS_RESAMPLE_FAILED:
        return "resample_failed";
    case STT_STATUS_CANCELLED:
        return "cancelled";
    default:
        return "unknown";
    }
}

const char *stt_input_status_name(SttInputStatus status)
{
    switch (status) {
    case STT_INPUT_NOT_CHECKED:
        return "not_checked";
    case STT_INPUT_MODEL_PCM:
        return "model_pcm";
    case STT_INPUT_ACCEPTED:
        return "ok";
    case STT_INPUT_EMPTY:
        return "empty";
    case STT_INPUT_TOO_SHORT:
        return "too_short";
    case STT_INPUT_TOO_QUIET:
        return "too_quiet";
    case STT_INPUT_TOO_CLIPPED:
        return "too_clipped";
    case STT_INPUT_TOO_NOISY:
        return "too_noisy";
    default:
        return "unknown";
    }
}

const char *stt_preset_name(SttPreset preset)
{
    switch (preset) {
    case STT_PRESET_QUALITY:
        return "quality";
    case STT_PRESET_LATENCY_SAFE:
        return "latency-safe";
    case STT_PRESET_LATENCY_FAST:
        return "latency-fast";
    case STT_PRESET_LATENCY_MINIMUM:
        return "latency-minimum";
    default:
        return "unknown";
    }
}
