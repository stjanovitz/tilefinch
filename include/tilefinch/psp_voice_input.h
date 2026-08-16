#ifndef TILEFINCH_PSP_VOICE_INPUT_H
#define TILEFINCH_PSP_VOICE_INPUT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "tilefinch/budget.h"
#include "tilefinch/voice_model_policy.h"

typedef void (*PspVoiceProgress)(void *user, const char *status);
typedef bool (*PspVoiceCancelRequested)(void *user);

typedef struct {
    void *engine;
    Budget *budget;
    /*
     * A logical reservation made only while experimental voice is enabled.
     * PocketSphinx allocates from newlib rather than Budget, so this prevents
     * page allocations from consuming the bytes the full model and its
     * transient capture/worker state will need later.
     */
    BudgetReservation voice_external;
    size_t voice_reserved_bytes;
    bool enabled;
    bool microphone_initialized;
    bool adaptive_memory;
    VoiceModelTier tier;
    VoiceCacheRows cache_rows;
    uint32_t attempts;
    uint32_t successes;
    uint32_t extra_wide_initializations;
    uint32_t small_initializations;
    uint32_t fallbacks;
    uint32_t pressure_evictions;
    uint32_t timeouts;
    bool disabled_after_timeout;
    uint64_t total_initialization_us;
    uint64_t total_decode_us;
    PspVoiceCancelRequested cancel_requested;
    void *cancel_user;
    char model_root[768];
} PspVoiceInput;

void psp_voice_input_init(
    PspVoiceInput *voice, Budget *budget, const char *model_root);
bool psp_voice_input_set_enabled(PspVoiceInput *voice, bool enabled);
void psp_voice_input_set_adaptive_memory(
    PspVoiceInput *voice, bool enabled);
void psp_voice_input_set_cancel_requested(
    PspVoiceInput *voice, PspVoiceCancelRequested cancel_requested,
    void *cancel_user);

/*
 * Capture at most three seconds from the PSP microphone and decode it with
 * the bundled fixed-point English model. The decoder is created lazily and
 * retained until psp_voice_input_evict() is called.
 */
bool psp_voice_input_transcribe(
    PspVoiceInput *voice, PspVoiceProgress progress, void *progress_user,
    char *output, size_t output_capacity);

void psp_voice_input_evict(PspVoiceInput *voice);
void psp_voice_input_trim(PspVoiceInput *voice);
void psp_voice_input_report(const PspVoiceInput *voice);
void psp_voice_input_shutdown(PspVoiceInput *voice);

#endif
