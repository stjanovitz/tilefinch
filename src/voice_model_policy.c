#include "tilefinch/voice_model_policy.h"

#define KIB (1024u)
#define MIB (1024u * KIB)

/*
 * Compact fixed-model PPSSPP measurements with 192 resident sendump rows:
 *   small       4.88 MiB allocator arena
 *   extra-wide  6.91 MiB allocator arena
 *
 * Selection happens after the capture buffer and decoder-worker stack have
 * been allocated. The reservations round those measured arenas up by more
 * than one MiB, and selection requires a further two-MiB margin for
 * unobserved allocator overhead and fragmentation. PocketSphinx still needs
 * a several-MiB contiguous allocation in both tiers.
 */
#define VOICE_SMALL_WORKING_BYTES (6u * MIB)
#define VOICE_EXTRA_WIDE_WORKING_BYTES (8u * MIB)
#define VOICE_SELECTION_MARGIN (2u * MIB)
#define VOICE_SELECTION_LARGEST_FREE (4u * MIB)

/* Once resident, return its working set before the rest of the application
 * reaches an emergency low-water mark. */
#define VOICE_RETAIN_TOTAL_LOW_WATER (3u * MIB)
#define VOICE_RETAIN_LARGEST_LOW_WATER (768u * KIB)

_Static_assert(
    VOICE_CACHE_FULL_ROWS == TILEFINCH_VOICE_SENDUMP_ROWS,
    "voice cache geometry must match the packaged acoustic model");

VoiceModelTier voice_model_policy_select(
    size_t total_free_bytes, size_t largest_free_bytes)
{
    VoiceModelAdmission admission = voice_model_policy_admit(
        total_free_bytes, largest_free_bytes, total_free_bytes, false);
    return admission.tier;
}

VoiceModelTier voice_model_policy_select_available(
    size_t heap_free_bytes, size_t heap_largest_free_bytes,
    size_t budget_remaining_bytes)
{
    return voice_model_policy_admit(
        heap_free_bytes, heap_largest_free_bytes,
        budget_remaining_bytes, false).tier;
}

size_t voice_model_tier_working_bytes_for_cache(
    VoiceModelTier tier, VoiceCacheRows cache_rows)
{
    size_t compact_bytes;
    switch (tier) {
        case VOICE_MODEL_SMALL:
            compact_bytes = VOICE_SMALL_WORKING_BYTES;
            break;
        case VOICE_MODEL_EXTRA_WIDE:
            compact_bytes = VOICE_EXTRA_WIDE_WORKING_BYTES;
            break;
        case VOICE_MODEL_NONE:
        default:
            return 0;
    }
    size_t rows = (size_t) cache_rows;
    if (rows < VOICE_CACHE_COMPACT_ROWS)
        rows = VOICE_CACHE_COMPACT_ROWS;
    if (rows > VOICE_CACHE_FULL_ROWS)
        rows = VOICE_CACHE_FULL_ROWS;
    return compact_bytes
        + (rows - VOICE_CACHE_COMPACT_ROWS)
              * TILEFINCH_VOICE_SENDUMP_ROW_BYTES;
}

static bool voice_model_admission_fits(
    size_t total, size_t largest, VoiceModelTier tier,
    VoiceCacheRows rows, VoiceModelAdmission *admission)
{
    size_t working = voice_model_tier_working_bytes_for_cache(tier, rows);
    if (largest < VOICE_SELECTION_LARGEST_FREE
        || total < working
        || total - working < VOICE_SELECTION_MARGIN)
        return false;
    *admission = (VoiceModelAdmission) {
        .tier = tier,
        .cache_rows = rows,
        .working_bytes = working
    };
    return true;
}

VoiceModelAdmission voice_model_policy_admit(
    size_t heap_free_bytes, size_t heap_largest_free_bytes,
    size_t budget_remaining_bytes, bool adaptive_memory)
{
    VoiceModelAdmission none = {
        .tier = VOICE_MODEL_NONE,
        .cache_rows = VOICE_CACHE_FULL_ROWS,
        .working_bytes = 0
    };
    size_t effective_total = heap_free_bytes < budget_remaining_bytes
        ? heap_free_bytes : budget_remaining_bytes;
    size_t effective_largest =
        heap_largest_free_bytes < effective_total
            ? heap_largest_free_bytes : effective_total;
    static const VoiceModelTier tiers[] = {
        VOICE_MODEL_EXTRA_WIDE, VOICE_MODEL_SMALL
    };
    static const VoiceCacheRows rows[] = {
        VOICE_CACHE_FULL_ROWS,
        VOICE_CACHE_BALANCED_ROWS,
        VOICE_CACHE_COMPACT_ROWS
    };
    size_t row_count = adaptive_memory
        ? sizeof(rows) / sizeof(rows[0]) : 1u;
    for (size_t tier = 0; tier < sizeof(tiers) / sizeof(tiers[0]); tier++) {
        for (size_t cache = 0; cache < row_count; cache++) {
            VoiceModelAdmission admission;
            if (voice_model_admission_fits(
                    effective_total, effective_largest,
                    tiers[tier], rows[cache], &admission))
                return admission;
        }
    }
    return none;
}

const char *voice_model_tier_name(VoiceModelTier tier)
{
    switch (tier) {
        case VOICE_MODEL_SMALL: return "small";
        case VOICE_MODEL_EXTRA_WIDE: return "extra-wide";
        case VOICE_MODEL_NONE:
        default: return "none";
    }
}

size_t voice_model_tier_working_bytes(VoiceModelTier tier)
{
    return voice_model_tier_working_bytes_for_cache(
        tier, VOICE_CACHE_FULL_ROWS);
}

size_t voice_model_selection_margin_bytes(void)
{
    return VOICE_SELECTION_MARGIN;
}

size_t voice_model_largest_allocation_bytes(void)
{
    return VOICE_SELECTION_LARGEST_FREE;
}

bool voice_model_policy_should_evict(
    VoiceModelTier tier, size_t total_free_bytes,
    size_t largest_free_bytes)
{
    return tier != VOICE_MODEL_NONE
        && (total_free_bytes < VOICE_RETAIN_TOTAL_LOW_WATER
            || largest_free_bytes < VOICE_RETAIN_LARGEST_LOW_WATER);
}
