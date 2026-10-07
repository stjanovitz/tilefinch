#include "tilefinch/script_admission.h"

/* See include/tilefinch/script_admission.h for the measurements. */
#define SCRIPT_ADMISSION_PEAK_FIXED_BYTES (256u * 1024u)
#define SCRIPT_ADMISSION_FLOOR_FIXED_BYTES (16u * 1024u)
/* Milliseconds per MiB on the PSP (device, 2026-10-06; see the header). */
#define SCRIPT_ADMISSION_RESTORE_MS_PER_MIB 50u
#define SCRIPT_ADMISSION_DISK_READ_MS_PER_MIB 1000u
#define SCRIPT_ADMISSION_RUN_MS_PER_MIB 7100u
#define SCRIPT_ADMISSION_START_MS_PER_MIB 10000u

static size_t saturating_add(size_t left, size_t right)
{
    return left > SIZE_MAX - right ? SIZE_MAX : left + right;
}

size_t script_admission_compile_peak(size_t unit_bytes)
{
    size_t doubled = unit_bytes > SIZE_MAX / 2u ? SIZE_MAX : unit_bytes * 2u;
    return saturating_add(doubled, SCRIPT_ADMISSION_PEAK_FIXED_BYTES);
}

size_t script_admission_compile_floor(size_t unit_bytes)
{
    return saturating_add(unit_bytes, SCRIPT_ADMISSION_FLOOR_FIXED_BYTES);
}

size_t script_admission_work_reserve(size_t source_bytes, size_t multiplier,
                                     size_t ceiling)
{
    size_t scaled = multiplier != 0 && source_bytes > SIZE_MAX / multiplier
        ? SIZE_MAX : source_bytes * multiplier;
    size_t reserve = saturating_add(SCRIPT_ADMISSION_WORK_FLOOR_BYTES, scaled);
    return reserve > ceiling ? ceiling : reserve;
}

size_t script_admission_affordable_bytes(Budget *budget, size_t floor,
                                         bool make_room)
{
    if (budget == NULL) return floor;
    if (make_room) {
        size_t floors = floor > SIZE_MAX / 4u ? SIZE_MAX : floor * 4u;
        size_t tight = saturating_add(
            SCRIPT_ADMISSION_PRESENTATION_RESERVE_BYTES, floors);
        if (budget_remaining(budget) < tight)
            (void) budget_make_room(budget, tight);
    }
    size_t free_bytes = budget_remaining(budget);
    size_t affordable =
        free_bytes > SCRIPT_ADMISSION_PRESENTATION_RESERVE_BYTES
            ? free_bytes - SCRIPT_ADMISSION_PRESENTATION_RESERVE_BYTES
            : 0;
    return affordable < floor ? floor : affordable;
}

static uint32_t milliseconds_for(size_t bytes, uint32_t ms_per_mib)
{
    if ((uint64_t) bytes > UINT64_MAX / ms_per_mib) return UINT32_MAX;
    uint64_t product = (uint64_t) bytes * ms_per_mib;
    uint64_t ms = (product + (1024u * 1024u - 1u)) / (1024u * 1024u);
    return ms > UINT32_MAX ? UINT32_MAX : (uint32_t) ms;
}

uint32_t script_admission_restore_ms(size_t bytes)
{
    return milliseconds_for(bytes, SCRIPT_ADMISSION_RESTORE_MS_PER_MIB);
}

uint32_t script_admission_disk_read_ms(size_t bytes)
{
    return milliseconds_for(bytes, SCRIPT_ADMISSION_DISK_READ_MS_PER_MIB);
}

uint32_t script_admission_run_ms(size_t bytes)
{
    return milliseconds_for(bytes, SCRIPT_ADMISSION_RUN_MS_PER_MIB);
}

uint32_t script_admission_start_ms(size_t bytes)
{
    return milliseconds_for(bytes, SCRIPT_ADMISSION_START_MS_PER_MIB);
}

size_t script_admission_run_memory(size_t bytes)
{
    return bytes > SIZE_MAX / 3u ? SIZE_MAX : bytes * 3u;
}

ScriptHeavyClass script_admission_classify(
    size_t visible_text_bytes, uint32_t start_ms,
    size_t largest_unit_bytes, size_t memory_needed, size_t memory_free)
{
    if (largest_unit_bytes != 0
        && (script_admission_compile_floor(largest_unit_bytes) > memory_free
            || memory_needed > memory_free)) {
        return SCRIPT_HEAVY_CLASS_OVER;
    }
    if (start_ms < SCRIPT_HEAVY_START_MS) return SCRIPT_HEAVY_CLASS_NONE;
    return visible_text_bytes < SCRIPT_HEAVY_SHELL_TEXT_BYTES
        ? SCRIPT_HEAVY_CLASS_SHELL : SCRIPT_HEAVY_CLASS_CONTENT;
}
