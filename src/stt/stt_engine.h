#ifndef PSPSTTLAB_STT_ENGINE_H
#define PSPSTTLAB_STT_ENGINE_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define STT_ENGINE_CAPTURE_RATE 22050U
#define STT_ENGINE_MODEL_RATE 16000U
#define STT_ENGINE_DEFAULT_MAX_SECONDS 3U
#define STT_ENGINE_DEFAULT_MAX_CAPTURE_SAMPLES \
    (STT_ENGINE_CAPTURE_RATE * STT_ENGINE_DEFAULT_MAX_SECONDS)
#define STT_ENGINE_DEFAULT_MAX_MODEL_SAMPLES \
    (STT_ENGINE_MODEL_RATE * STT_ENGINE_DEFAULT_MAX_SECONDS)
#define STT_ENGINE_DEFAULT_CHUNK_SAMPLES 2048U
#define STT_ENGINE_TEXT_CAPACITY 256U

typedef struct SttEngine SttEngine;

typedef enum {
    STT_PRESET_QUALITY = 0,
    STT_PRESET_LATENCY_SAFE,
    STT_PRESET_LATENCY_FAST,
    STT_PRESET_LATENCY_MINIMUM
} SttPreset;

typedef enum {
    STT_STATUS_OK = 0,
    STT_STATUS_NO_HYPOTHESIS,
    STT_STATUS_INPUT_REJECTED,
    STT_STATUS_INVALID_ARGUMENT,
    STT_STATUS_OUT_OF_MEMORY,
    STT_STATUS_DECODER_INIT_FAILED,
    STT_STATUS_DECODER_START_FAILED,
    STT_STATUS_DECODER_PROCESS_FAILED,
    STT_STATUS_DECODER_END_FAILED,
    STT_STATUS_RESAMPLE_FAILED,
    STT_STATUS_CANCELLED
} SttStatus;

typedef enum {
    STT_INPUT_NOT_CHECKED = 0,
    STT_INPUT_MODEL_PCM,
    STT_INPUT_ACCEPTED,
    STT_INPUT_EMPTY,
    STT_INPUT_TOO_SHORT,
    STT_INPUT_TOO_QUIET,
    STT_INPUT_TOO_CLIPPED,
    STT_INPUT_TOO_NOISY
} SttInputStatus;

typedef enum {
    STT_PHASE_ENGINE_READY = 0,
    STT_PHASE_INPUT_CONDITIONED,
    STT_PHASE_UTTERANCE_STARTED,
    STT_PHASE_PROCESS_CHUNK,
    STT_PHASE_UTTERANCE_ENDED,
    STT_PHASE_HYPOTHESIS_READY
} SttPhase;

typedef uint64_t (*SttClockMicroseconds)(void *user);
typedef int (*SttCancelCheck)(void *user);
typedef void (*SttObserver)(
    void *user,
    SttPhase phase,
    size_t completed_samples,
    size_t total_samples
);

typedef struct {
    int bestpath;
    int downsample_ratio;
    int topn;
    int max_words_per_frame;
    int max_hmms_per_frame;
    int phone_lookahead_window;
    int forward_flat;
    double forward_flat_beam;
} SttSearchConfig;

typedef struct {
    const char *acoustic_model_path;
    const char *dictionary_path;
    const char *language_model_path;
    const char *log_level;
    size_t process_chunk_samples;
    /* Number of acoustic sendump rows retained in RAM. The fixed English
       model has 384 rows; 384 therefore selects a fully resident table. */
    size_t sendump_cache_rows;
    SttPreset preset;
    SttSearchConfig search;
    SttClockMicroseconds clock_microseconds;
    SttObserver observer;
    void *callback_user;
} SttEngineConfig;

typedef struct {
    uint64_t start_utterance_us;
    uint64_t process_audio_us;
    uint64_t end_utterance_us;
    uint64_t hypothesis_us;
    uint64_t total_decode_us;
} SttTimings;

typedef struct {
    SttStatus status;
    SttInputStatus input_status;
    int32_t score;
    size_t input_samples;
    size_t model_samples;
    SttTimings timings;
    char text[STT_ENGINE_TEXT_CAPACITY];
} SttResult;

/*
 * Initialize a quality-first configuration. Set all three model paths before
 * calling stt_engine_create(). The path strings only need to remain valid for
 * the duration of stt_engine_create().
 */
void stt_engine_config_init(SttEngineConfig *config);

/* Apply a named search preset without changing paths or callbacks. */
void stt_engine_config_set_preset(SttEngineConfig *config, SttPreset preset);

/*
 * Create one resident decoder. An engine is synchronous and must be used by
 * only one thread at a time. Keeping multiple engines resident duplicates the
 * acoustic model and is not recommended on PSP.
 */
SttStatus stt_engine_create(
    SttEngine **output_engine,
    const SttEngineConfig *config
);

void stt_engine_destroy(SttEngine *engine);
uint64_t stt_engine_init_microseconds(const SttEngine *engine);
SttPreset stt_engine_preset(const SttEngine *engine);

/*
 * Decode signed mono PCM16 already sampled at 16 kHz. The input is borrowed
 * for the duration of this call and is not modified.
 */
SttStatus stt_engine_decode_model_pcm(
    SttEngine *engine,
    const int16_t *samples,
    size_t sample_count,
    SttResult *result
);

/*
 * Condition and decode signed mono PCM16 captured at 22.05 kHz. The buffer is
 * modified in place for DC removal and resampling. Resampling shrinks it, so
 * no capacity beyond sample_count is required and ownership stays with the
 * caller.
 */
SttStatus stt_engine_decode_capture_pcm(
    SttEngine *engine,
    int16_t *samples,
    size_t sample_count,
    SttResult *result
);

/* As above, with cooperative cancellation checked before and between bounded
   audio chunks. The callback and user pointer are borrowed only for this
   call, so a retained engine never keeps worker-job state alive. */
SttStatus stt_engine_decode_capture_pcm_cancelable(
    SttEngine *engine,
    int16_t *samples,
    size_t sample_count,
    SttCancelCheck cancel,
    void *cancel_user,
    SttResult *result
);

const char *stt_status_name(SttStatus status);
const char *stt_input_status_name(SttInputStatus status);
const char *stt_preset_name(SttPreset preset);

#ifdef __cplusplus
}
#endif

#endif
