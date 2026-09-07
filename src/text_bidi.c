#include "tilefinch/text_bidi.h"

#include <limits.h>
#include <string.h>

#include <SheenBidi/SheenBidi.h>
/* A static allocator avoids one unbudgeted allocator-object allocation. The
   browser is single-threaded and bidi calls are never nested. */
#include <API/SBAllocator.h>

#include "tilefinch/font.h"

#define TEXT_BIDI_SCRATCH_BYTES (12u * 1024u)
#define TEXT_BIDI_SCRATCH_ALIGNMENT _Alignof(max_align_t)

_Static_assert((TEXT_BIDI_SCRATCH_ALIGNMENT
                & (TEXT_BIDI_SCRATCH_ALIGNMENT - 1u)) == 0,
               "bidi scratch alignment must be a power of two");

typedef struct {
    uint16_t codepoint;
    uint16_t isolated;
    uint16_t final;
    uint16_t initial;
    uint16_t medial;
    uint8_t joining;
} ArabicShapeEntry;

#include "generated/arabic_shaping_data.inc"

typedef struct {
    Budget *budget;
    unsigned char *scratch;
    size_t scratch_used;
    size_t scratch_capacity;
} BidiAllocatorContext;

struct TextBidiParagraph {
    Budget *budget;
    SBAlgorithmRef algorithm;
    SBParagraphRef paragraphs[TEXT_BIDI_SEGMENT_LIMIT];
    size_t paragraph_count;
    TextBidiUnit *units;
    uint32_t *codepoints;
    size_t count;
    size_t allocation_size;
    BidiAllocatorContext allocator;
};

static BidiAllocatorContext *active_allocator;

static void *bidi_allocate(SBUInteger size, void *opaque)
{
    BidiAllocatorContext *context = opaque;
    if (context == NULL || context != active_allocator || size == 0
        || size > SIZE_MAX) return NULL;
    return budget_malloc_category(
        context->budget, BUDGET_CATEGORY_LAYOUT, (size_t) size);
}

static void *bidi_reallocate(void *pointer, SBUInteger size, void *opaque)
{
    (void) pointer;
    (void) size;
    (void) opaque;
    /* The non-experimental paragraph API never reallocates. Fail closed if a
       future upstream change starts depending on list growth. */
    return NULL;
}

static void bidi_deallocate(void *pointer, void *opaque)
{
    BidiAllocatorContext *context = opaque;
    if (context != NULL && context == active_allocator)
        budget_free(context->budget, pointer);
}

static void *bidi_allocate_scratch(SBUInteger size, void *opaque)
{
    BidiAllocatorContext *context = opaque;
    if (context == NULL || context != active_allocator || size == 0
        || context->scratch_used > context->scratch_capacity
        || size > context->scratch_capacity - context->scratch_used) {
        return NULL;
    }
    if ((size_t) size > SIZE_MAX - (TEXT_BIDI_SCRATCH_ALIGNMENT - 1u))
        return NULL;
    size_t aligned = ((size_t) size + TEXT_BIDI_SCRATCH_ALIGNMENT - 1u)
                     & ~(TEXT_BIDI_SCRATCH_ALIGNMENT - 1u);
    if (aligned > context->scratch_capacity - context->scratch_used)
        return NULL;
    void *result = context->scratch + context->scratch_used;
    context->scratch_used += aligned;
    return result;
}

static void bidi_reset_scratch(void *opaque)
{
    BidiAllocatorContext *context = opaque;
    if (context != NULL && context == active_allocator)
        context->scratch_used = 0;
}

static SBAllocator bidi_allocator = SBAllocatorMake(
    bidi_allocate, bidi_reallocate, bidi_deallocate,
    bidi_allocate_scratch, bidi_reset_scratch, NULL);

static SBAllocatorRef bidi_allocator_enter(BidiAllocatorContext *context)
{
    if (context == NULL || active_allocator != NULL) return NULL;
    SBAllocatorRef previous = SBAllocatorGetDefault();
    active_allocator = context;
    bidi_allocator._info = context;
    SBAllocatorSetDefault(&bidi_allocator);
    return previous;
}

static void bidi_allocator_leave(SBAllocatorRef previous)
{
    SBAllocatorSetDefault(previous);
    bidi_allocator._info = NULL;
    active_allocator = NULL;
}

static bool bidi_control(unsigned codepoint)
{
    return codepoint == 0x061cu || codepoint == 0x200eu
        || codepoint == 0x200fu
        || (codepoint >= 0x202au && codepoint <= 0x202eu)
        || (codepoint >= 0x2066u && codepoint <= 0x2069u);
}

static bool strong_rtl(unsigned codepoint)
{
    return (codepoint >= 0x0590u && codepoint <= 0x08ffu)
        || (codepoint >= 0xfb1du && codepoint <= 0xfdffu)
        || (codepoint >= 0xfe70u && codepoint <= 0xfeffu)
        || (codepoint >= 0x10800u && codepoint <= 0x10fffu)
        || (codepoint >= 0x1e800u && codepoint <= 0x1eeffu);
}

/* UAX #9 rule L3 reorders combining sequences as units. Keep the same
   bounded extender vocabulary as inline wrapping, including variation
   selectors and ZWJ emoji sequences, so bidi does not split a grapheme that
   the ordinary text path already treats atomically. */
static bool bidi_cluster_extender(unsigned codepoint)
{
    return (codepoint >= 0x0300u && codepoint <= 0x036fu)
        || (codepoint >= 0x0591u && codepoint <= 0x05c7u)
        || (codepoint >= 0x0610u && codepoint <= 0x061au)
        || (codepoint >= 0x064bu && codepoint <= 0x065fu)
        || (codepoint >= 0x06d6u && codepoint <= 0x06edu)
        || (codepoint >= 0x08d3u && codepoint <= 0x0903u)
        || (codepoint >= 0x1ab0u && codepoint <= 0x1affu)
        || (codepoint >= 0x1dc0u && codepoint <= 0x1dffu)
        || (codepoint >= 0xfe00u && codepoint <= 0xfe0fu)
        || (codepoint >= 0xfe20u && codepoint <= 0xfe2fu)
        || (codepoint >= 0xe0100u && codepoint <= 0xe01efu)
        || codepoint == 0x200du;
}

static size_t bidi_cluster_end(const TextBidiParagraph *paragraph,
                               size_t at, size_t end)
{
    size_t cursor = at + 1u;
    bool join_next = false;
    while (cursor < end) {
        uint32_t codepoint = paragraph->units[cursor].codepoint;
        if (!join_next && !bidi_cluster_extender(codepoint)) break;
        cursor++;
        join_next = codepoint == 0x200du;
    }
    return cursor;
}

bool text_bidi_maybe_needed(const char *text, size_t length)
{
    if (text == NULL) return false;
    for (size_t at = 0; at < length;) {
        unsigned codepoint = 0;
        size_t used = font_utf8_next(text + at, length - at, &codepoint);
        if (used == 0) {
            at++;
            continue;
        }
        if (strong_rtl(codepoint) || bidi_control(codepoint)) return true;
        at += used;
    }
    return false;
}

static const ArabicShapeEntry *arabic_shape_entry(uint32_t codepoint)
{
    size_t low = 0;
    size_t high = sizeof(arabic_shape_entries)
                  / sizeof(arabic_shape_entries[0]);
    while (low < high) {
        size_t middle = low + (high - low) / 2u;
        uint32_t candidate = arabic_shape_entries[middle].codepoint;
        if (candidate < codepoint) low = middle + 1u;
        else high = middle;
    }
    return low < sizeof(arabic_shape_entries)
                     / sizeof(arabic_shape_entries[0])
               && arabic_shape_entries[low].codepoint == codepoint
        ? &arabic_shape_entries[low] : NULL;
}

enum {
    ARABIC_JOIN_NONE,
    ARABIC_JOIN_RIGHT,
    ARABIC_JOIN_LEFT,
    ARABIC_JOIN_DUAL,
    ARABIC_JOIN_CAUSING,
    ARABIC_JOIN_TRANSPARENT
};

static bool arabic_connects_previous(uint8_t joining)
{
    return joining == ARABIC_JOIN_RIGHT || joining == ARABIC_JOIN_DUAL
        || joining == ARABIC_JOIN_CAUSING;
}

static bool arabic_connects_next(uint8_t joining)
{
    return joining == ARABIC_JOIN_LEFT || joining == ARABIC_JOIN_DUAL
        || joining == ARABIC_JOIN_CAUSING;
}

static const ArabicShapeEntry *arabic_joining_neighbor(
    const TextBidiParagraph *paragraph, size_t at, int direction)
{
    static const ArabicShapeEntry join_causing = {
        .codepoint = 0x200du,
        .joining = ARABIC_JOIN_CAUSING
    };
    size_t cursor = at;
    while ((direction < 0 && cursor != 0)
           || (direction > 0 && cursor + 1u < paragraph->count)) {
        cursor = direction < 0 ? cursor - 1u : cursor + 1u;
        if (paragraph->units[cursor].level != paragraph->units[at].level)
            return NULL;
        /* ZWJ requests joining while ZWNJ deliberately stops it. They live
           outside the generated Arabic-family table, but participate in
           ArabicShaping.txt's joining algorithm. */
        if (paragraph->units[cursor].codepoint == 0x200du)
            return &join_causing;
        if (paragraph->units[cursor].codepoint == 0x200cu)
            return NULL;
        const ArabicShapeEntry *entry = arabic_shape_entry(
            paragraph->units[cursor].codepoint);
        if (entry != NULL && entry->joining == ARABIC_JOIN_TRANSPARENT)
            continue;
        return entry;
    }
    return NULL;
}

static void arabic_shape(TextBidiParagraph *paragraph)
{
    for (size_t at = 0; at < paragraph->count; at++) {
        TextBidiUnit *unit = &paragraph->units[at];
        const ArabicShapeEntry *entry = arabic_shape_entry(unit->codepoint);
        if (entry == NULL || entry->joining == ARABIC_JOIN_TRANSPARENT
            || entry->joining == ARABIC_JOIN_NONE) continue;
        const ArabicShapeEntry *before = arabic_joining_neighbor(
            paragraph, at, -1);
        const ArabicShapeEntry *after = arabic_joining_neighbor(
            paragraph, at, 1);
        bool joins_before = before != NULL
            && arabic_connects_next(before->joining)
            && arabic_connects_previous(entry->joining);
        bool joins_after = after != NULL
            && arabic_connects_next(entry->joining)
            && arabic_connects_previous(after->joining);
        uint32_t shaped = joins_before && joins_after ? entry->medial
            : (joins_before ? entry->final
               : (joins_after ? entry->initial : entry->isolated));
        if (shaped != 0) unit->shaped_codepoint = shaped;
    }
}

TextBidiParagraph *text_bidi_paragraph_create(
    Budget *budget, const char *text, size_t length, TextBidiBase base,
    TextBidiStatus *status)
{
    if (status != NULL) *status = TEXT_BIDI_MALFORMED;
    if (budget == NULL || text == NULL) return NULL;
    if (!text_bidi_maybe_needed(text, length)
        && base != TEXT_BIDI_BASE_RTL
        && base != TEXT_BIDI_BASE_AUTO_RTL) {
        if (status != NULL) *status = TEXT_BIDI_NOT_NEEDED;
        return NULL;
    }
    if (length == 0 || length > TEXT_BIDI_PARAGRAPH_BYTE_LIMIT) {
        if (status != NULL) *status = TEXT_BIDI_LIMIT_EXCEEDED;
        return NULL;
    }
    size_t count = 0;
    for (size_t at = 0; at < length;) {
        unsigned codepoint = 0;
        size_t used = font_utf8_next(text + at, length - at, &codepoint);
        if (used == 0 || used > UINT8_MAX) return NULL;
        if (++count > TEXT_BIDI_CODEPOINT_LIMIT) {
            if (status != NULL) *status = TEXT_BIDI_LIMIT_EXCEEDED;
            return NULL;
        }
        at += used;
    }
    size_t units_bytes = count * sizeof(TextBidiUnit);
    size_t codepoints_bytes = count * sizeof(uint32_t);
    size_t allocation_size = sizeof(TextBidiParagraph);
    const size_t scratch_padding = TEXT_BIDI_SCRATCH_ALIGNMENT - 1u;
    if (units_bytes > SIZE_MAX - allocation_size
        || codepoints_bytes > SIZE_MAX - allocation_size - units_bytes
        || TEXT_BIDI_SCRATCH_BYTES
               > SIZE_MAX - allocation_size - units_bytes - codepoints_bytes
                 - scratch_padding) {
        if (status != NULL) *status = TEXT_BIDI_LIMIT_EXCEEDED;
        return NULL;
    }
    allocation_size += units_bytes + codepoints_bytes + scratch_padding
                       + TEXT_BIDI_SCRATCH_BYTES;
    TextBidiParagraph *result = budget_malloc_category(
        budget, BUDGET_CATEGORY_LAYOUT, allocation_size);
    if (result == NULL) {
        if (status != NULL) *status = TEXT_BIDI_OUT_OF_MEMORY;
        return NULL;
    }
    memset(result, 0, sizeof(*result));
    result->budget = budget;
    result->count = count;
    result->allocation_size = allocation_size;
    result->units = (TextBidiUnit *) (result + 1);
    result->codepoints = (uint32_t *) (
        (unsigned char *) result->units + units_bytes);
    uintptr_t scratch_unaligned = (uintptr_t) (
        (unsigned char *) result->codepoints + codepoints_bytes);
    uintptr_t scratch_aligned = (
        scratch_unaligned + TEXT_BIDI_SCRATCH_ALIGNMENT - 1u)
        & ~(uintptr_t) (TEXT_BIDI_SCRATCH_ALIGNMENT - 1u);
    result->allocator = (BidiAllocatorContext) {
        .budget = budget,
        .scratch = (unsigned char *) scratch_aligned,
        .scratch_capacity = TEXT_BIDI_SCRATCH_BYTES
    };
    size_t logical = 0;
    for (size_t at = 0; at < length && logical < count; logical++) {
        unsigned codepoint = 0;
        size_t used = font_utf8_next(text + at, length - at, &codepoint);
        result->codepoints[logical] = codepoint;
        result->units[logical] = (TextBidiUnit) {
            .codepoint = codepoint,
            .shaped_codepoint = codepoint,
            .byte_offset = (uint32_t) at,
            .byte_length = (uint8_t) used,
            .cluster_extender = bidi_cluster_extender(codepoint)
        };
        at += used;
    }

    SBAllocatorRef previous = bidi_allocator_enter(&result->allocator);
    if (previous == NULL && active_allocator != &result->allocator) {
        budget_free(budget, result);
        if (status != NULL) *status = TEXT_BIDI_OUT_OF_MEMORY;
        return NULL;
    }
    SBCodepointSequence sequence = {
        .stringEncoding = SBStringEncodingUTF32,
        .stringBuffer = result->codepoints,
        .stringLength = count
    };
    result->algorithm = SBAlgorithmCreate(&sequence);
    SBLevel level = base == TEXT_BIDI_BASE_LTR ? 0
        : (base == TEXT_BIDI_BASE_RTL ? 1
           : (base == TEXT_BIDI_BASE_AUTO_RTL
              ? SBLevelDefaultRTL : SBLevelDefaultLTR));
    size_t resolved = 0;
    while (result->algorithm != NULL && resolved < count
           && result->paragraph_count < TEXT_BIDI_SEGMENT_LIMIT) {
        SBParagraphRef segment = SBAlgorithmCreateParagraph(
            result->algorithm, resolved, count - resolved, level);
        if (segment == NULL) break;
        result->paragraphs[result->paragraph_count++] = segment;
        size_t segment_count = SBParagraphGetLength(segment);
        if (SBParagraphGetOffset(segment) != resolved || segment_count == 0
            || segment_count > count - resolved) break;
        const SBLevel *levels = SBParagraphGetLevelsPtr(segment);
        for (size_t local = 0; local < segment_count; local++) {
            size_t at = resolved + local;
            result->units[at].level = levels[local];
            if ((levels[local] & 1u) != 0u) {
                SBCodepoint mirrored = SBCodepointGetMirror(
                    result->units[at].codepoint);
                if (mirrored != 0)
                    result->units[at].shaped_codepoint = mirrored;
            }
        }
        resolved += segment_count;
    }
    bool segment_limit = resolved < count
        && result->paragraph_count == TEXT_BIDI_SEGMENT_LIMIT;
    if (resolved == count) arabic_shape(result);
    bidi_allocator_leave(previous);
    if (resolved != count) {
        text_bidi_paragraph_destroy(result);
        if (status != NULL) *status = segment_limit
            ? TEXT_BIDI_LIMIT_EXCEEDED : TEXT_BIDI_OUT_OF_MEMORY;
        return NULL;
    }
    if (status != NULL) *status = TEXT_BIDI_OK;
    return result;
}

void text_bidi_paragraph_destroy(TextBidiParagraph *paragraph)
{
    if (paragraph == NULL) return;
    SBAllocatorRef previous = bidi_allocator_enter(&paragraph->allocator);
    if (previous == NULL && active_allocator != &paragraph->allocator) return;
    for (size_t i = 0; i < paragraph->paragraph_count; i++)
        SBParagraphRelease(paragraph->paragraphs[i]);
    if (paragraph->algorithm != NULL)
        SBAlgorithmRelease(paragraph->algorithm);
    bidi_allocator_leave(previous);
    budget_free(paragraph->budget, paragraph);
}

size_t text_bidi_paragraph_count(const TextBidiParagraph *paragraph)
{
    return paragraph == NULL ? 0 : paragraph->count;
}

unsigned text_bidi_paragraph_level(const TextBidiParagraph *paragraph)
{
    return paragraph == NULL || paragraph->paragraph_count == 0 ? 0
        : SBParagraphGetBaseLevel(paragraph->paragraphs[0]);
}

const TextBidiUnit *text_bidi_paragraph_units(
    const TextBidiParagraph *paragraph)
{
    return paragraph == NULL ? NULL : paragraph->units;
}

bool text_bidi_line_visual_order(
    TextBidiParagraph *paragraph, size_t line_start, size_t line_count,
    uint16_t *visual_to_logical, size_t capacity, size_t *run_count)
{
    if (run_count != NULL) *run_count = 0;
    if (paragraph == NULL || visual_to_logical == NULL || line_count == 0
        || line_start > paragraph->count
        || line_count > paragraph->count - line_start
        || line_count > capacity || paragraph->count > UINT16_MAX) {
        return false;
    }
    SBAllocatorRef previous = bidi_allocator_enter(&paragraph->allocator);
    if (previous == NULL && active_allocator != &paragraph->allocator)
        return false;
    size_t output = 0;
    size_t runs = 0;
    bool okay = true;
    for (size_t segment = 0; segment < paragraph->paragraph_count; segment++) {
        SBParagraphRef part = paragraph->paragraphs[segment];
        size_t start = SBParagraphGetOffset(part);
        size_t end = start + SBParagraphGetLength(part);
        if (end <= line_start) continue;
        if (start >= line_start + line_count) break;
        if (start < line_start) start = line_start;
        if (end > line_start + line_count) end = line_start + line_count;
        SBLineRef line = SBParagraphCreateLine(part, start, end - start);
        if (line == NULL) { okay = false; break; }
        size_t part_runs = SBLineGetRunCount(line);
        if (part_runs > TEXT_BIDI_RUN_LIMIT - runs) {
            SBLineRelease(line);
            runs = TEXT_BIDI_RUN_LIMIT + 1u;
            okay = false;
            break;
        }
        runs += part_runs;
        const SBRun *run_data = SBLineGetRunsPtr(line);
        for (size_t run = 0; run < part_runs; run++) {
            size_t start = run_data[run].offset;
            size_t count = run_data[run].length;
            if (start < line_start || count > line_count
                || start - line_start > line_count - count
                || count > line_count - output) {
                okay = false;
                break;
            }
            if ((run_data[run].level & 1u) != 0u) {
                size_t destination = output + count;
                size_t at = start;
                size_t end = start + count;
                while (at < end) {
                    size_t cluster_end = bidi_cluster_end(
                        paragraph, at, end);
                    size_t cluster_count = cluster_end - at;
                    destination -= cluster_count;
                    for (size_t item = 0; item < cluster_count; item++) {
                        visual_to_logical[destination + item] =
                            (uint16_t) (at + item);
                    }
                    at = cluster_end;
                }
                output += count;
            } else {
                for (size_t at = 0; at < count; at++)
                    visual_to_logical[output++] = (uint16_t) (start + at);
            }
        }
        SBLineRelease(line);
        if (!okay) break;
    }
    bidi_allocator_leave(previous);
    if (run_count != NULL) *run_count = runs;
    return okay && output == line_count && runs <= TEXT_BIDI_RUN_LIMIT;
}
