#ifndef TILEFINCH_VOICE_MODEL_POLICY_H
#define TILEFINCH_VOICE_MODEL_POLICY_H

#include <stdbool.h>
#include <stddef.h>

typedef enum {
    VOICE_MODEL_NONE = 0,
    VOICE_MODEL_SMALL,
    VOICE_MODEL_EXTRA_WIDE
} VoiceModelTier;

typedef enum {
    VOICE_CACHE_COMPACT_ROWS = 192,
    VOICE_CACHE_BALANCED_ROWS = 256,
    VOICE_CACHE_FULL_ROWS = 384
} VoiceCacheRows;

typedef struct {
    VoiceModelTier tier;
    VoiceCacheRows cache_rows;
    size_t working_bytes;
} VoiceModelAdmission;

/*
 * Select the largest offline recognizer tier that leaves the experimentally
 * established decoder headroom in both total and contiguous PSP heap.
 */
VoiceModelTier voice_model_policy_select(
    size_t total_free_bytes, size_t largest_free_bytes);

/*
 * Apply both allocator availability and the browser's remaining external
 * reservation budget before selecting a tier.
 */
VoiceModelTier voice_model_policy_select_available(
    size_t heap_free_bytes, size_t heap_largest_free_bytes,
    size_t budget_remaining_bytes);

/*
 * Select model and acoustic-table residency together. With adaptive_memory
 * false, only the complete 384-row table is considered. Adaptive mode keeps
 * the same model arithmetic and may select 256 or 192 resident rows when
 * that is what fits.
 */
VoiceModelAdmission voice_model_policy_admit(
    size_t heap_free_bytes, size_t heap_largest_free_bytes,
    size_t budget_remaining_bytes, bool adaptive_memory);

const char *voice_model_tier_name(VoiceModelTier tier);
size_t voice_model_tier_working_bytes(VoiceModelTier tier);
size_t voice_model_tier_working_bytes_for_cache(
    VoiceModelTier tier, VoiceCacheRows cache_rows);
size_t voice_model_selection_margin_bytes(void);
size_t voice_model_largest_allocation_bytes(void);

/*
 * A resident decoder is outside the browser's tracked page heap. Evict it
 * before physical heap pressure becomes severe enough to destabilize later
 * browser allocations.
 */
bool voice_model_policy_should_evict(
    VoiceModelTier tier, size_t total_free_bytes,
    size_t largest_free_bytes);

#endif
