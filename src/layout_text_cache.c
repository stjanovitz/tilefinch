/* Retained run measurements. A relayout re-measures every word of the
   document even when no text, font or style changed (a focus move, an
   image arrival). measured_flow_text_width_fixed is a function of its
   arguments, the face they name and the optional glyph packs bound to the
   font layer, so its result for an identical call can be reused:

   - the key holds every argument (the run's bytes, the face by lifetime
     serial, metric family, 26.6 size, synthetic and metric bold, scale,
     letter spacing, text transform and kerning) and an entry is used only
     when all of them compare equal, bytes included;
   - a change of the optional glyph generation discards the table;
   - a run the face could not measure (the per-character estimate stood
     in for a refused size or variation change) is not retained.

   What the run's measurement does not take is not part of the key: the
   available width, white-space, word spacing and the line position act
   on these widths in flow_text and are evaluated on every relayout.
   Contextually shaped runs (layout_bidi) never reach here.

   The table is set-associative, charged to the layout category, admitted
   only with the style payloads' working headroom, grown (to a fixed
   limit) only with their growth reserve, and released with them under
   pressure. */
#include "layout_internal.h"

#include "fnv1a.h"

#include <string.h>

enum {
    TEXT_MEASURE_SYNTHETIC_BOLD = 1u << 0,
    TEXT_MEASURE_METRIC_BOLD = 1u << 1,
    TEXT_MEASURE_KERNING = 1u << 2
};

static size_t layout_text_measure_bytes(size_t capacity)
{
    return capacity * sizeof(LayoutTextMeasureEntry);
}

static LayoutTextMeasureEntry *layout_text_measure_set(
    LayoutTextMeasureEntry *table, size_t capacity, uint32_t hash)
{
    size_t sets = capacity / LAYOUT_TEXT_MEASURE_WAYS;
    return &table[(hash & (sets - 1u)) * LAYOUT_TEXT_MEASURE_WAYS];
}

static void layout_text_measure_clear(LayoutReuseCache *cache)
{
    memset(cache->text_measures, 0,
           layout_text_measure_bytes(cache->text_measure_capacity));
    cache->text_measure_clock = 0;
}

size_t layout_text_measure_release(LayoutReuseCache *cache)
{
    if (cache == NULL || cache->text_measures == NULL) return 0;
    size_t bytes = layout_text_measure_bytes(cache->text_measure_capacity);
    budget_free(cache->budget, cache->text_measures);
    cache->text_measures = NULL;
    cache->text_measure_capacity = 0;
    cache->text_measure_clock = 0;
    cache->text_measure_build_evictions = 0;
    cache->stats.retained_bytes -= bytes;
    cache->stats.text_measure_bytes = 0;
    return bytes;
}

static bool layout_text_measure_admit(LayoutReuseCache *cache,
                                      uint64_t generation)
{
    if (cache->text_measures != NULL) return true;
    if (cache->text_measure_refused || cache->text_measure_yielded
        || cache->style_pressure_trimmed)
        return false;
    size_t bytes = layout_text_measure_bytes(
        LAYOUT_TEXT_MEASURE_INITIAL_CAPACITY);
    /* Optional, like the style payloads: never take the headroom the
       required flow needs. */
    if (budget_pressure_required(cache->budget, bytes,
            LAYOUT_REUSE_STYLE_WORKING_BYTES
                + LAYOUT_REUSE_STYLE_RESERVE_BYTES)) return false;
    cache->text_measures = budget_calloc(cache->budget,
        LAYOUT_TEXT_MEASURE_INITIAL_CAPACITY, sizeof(LayoutTextMeasureEntry));
    if (cache->text_measures == NULL) {
        cache->text_measure_refused = true;
        return false;
    }
    cache->text_measure_capacity = LAYOUT_TEXT_MEASURE_INITIAL_CAPACITY;
    cache->text_measure_generation = generation;
    cache->text_measure_clock = 0;
    cache->text_measure_build_evictions = 0;
    cache->stats.retained_bytes += bytes;
    cache->stats.text_measure_bytes = bytes;
    return true;
}

size_t layout_reuse_cache_yield_text_measures(LayoutReuseCache *cache)
{
    if (cache == NULL) return 0;
    cache->text_measure_yielded = true;
    return layout_text_measure_release(cache);
}

void layout_text_measure_prepare(LayoutReuseCache *cache)
{
    if (cache == NULL) return;
    cache->text_measure_refused = false;
    uint32_t evicted = cache->text_measure_build_evictions;
    cache->text_measure_build_evictions = 0;
    size_t capacity = cache->text_measure_capacity;
    /* A walk with more distinct runs than the table holds evicts, in
       document order, what the next walk needs first. */
    if (cache->text_measures == NULL
        || capacity >= LAYOUT_TEXT_MEASURE_CAPACITY_LIMIT
        || evicted <= capacity / 8u) return;
    size_t grown = capacity * 2u;
    if (budget_pressure_required(cache->budget,
                                 layout_text_measure_bytes(grown),
                                 LAYOUT_REUSE_STYLE_GROWTH_RESERVE_BYTES))
        return;
    LayoutTextMeasureEntry *table = budget_calloc(
        cache->budget, grown, sizeof(LayoutTextMeasureEntry));
    if (table == NULL) return;
    /* The allocation can run the Budget's reclaim hook, which may have
       yielded the table being grown. */
    if (cache->text_measures == NULL) {
        budget_free(cache->budget, table);
        return;
    }
    LayoutTextMeasureEntry *old = cache->text_measures;
    for (size_t i = 0; i < capacity; i++) {
        if (old[i].hash == 0) continue;
        LayoutTextMeasureEntry *set =
            layout_text_measure_set(table, grown, old[i].hash);
        for (size_t way = 0; way < LAYOUT_TEXT_MEASURE_WAYS; way++) {
            if (set[way].hash != 0) continue;
            set[way] = old[i];
            break;
        }
    }
    budget_free(cache->budget, old);
    cache->text_measures = table;
    cache->text_measure_capacity = (uint32_t) grown;
    cache->stats.retained_bytes += layout_text_measure_bytes(grown)
                                   - layout_text_measure_bytes(capacity);
    cache->stats.text_measure_bytes = layout_text_measure_bytes(grown);
}

int layout_text_measure(LayoutReuseCache *cache, const FontFace *face,
                        FontFamily metric_family, const char *text,
                        size_t length, int font_size_fixed,
                        bool synthetic_bold, bool metric_bold, int scale,
                        int letter_spacing, TextTransformMode transform,
                        bool kerning)
{
    if (cache == NULL || face == NULL || face->serial == 0 || text == NULL
        || length == 0 || length > LAYOUT_TEXT_MEASURE_BYTES
        || letter_spacing < INT16_MIN || letter_spacing > INT16_MAX
        || scale < INT16_MIN || scale > INT16_MAX
        || (unsigned) metric_family > UINT8_MAX) {
        return measured_flow_text_width_fixed(
            face, metric_family, text, length, font_size_fixed,
            synthetic_bold, metric_bold, scale, letter_spacing, transform,
            kerning);
    }
    uint64_t generation = font_optional_glyph_generation();
    if (cache->text_measures != NULL
        && generation != cache->text_measure_generation) {
        layout_text_measure_clear(cache);
        cache->text_measure_generation = generation;
    }
    uint8_t flags = (uint8_t) ((synthetic_bold ? TEXT_MEASURE_SYNTHETIC_BOLD : 0u)
        | (metric_bold ? TEXT_MEASURE_METRIC_BOLD : 0u)
        | (kerning ? TEXT_MEASURE_KERNING : 0u));
    uint32_t parameters[5] = {
        (uint32_t) face->serial, (uint32_t) (face->serial >> 32),
        (uint32_t) font_size_fixed,
        (uint32_t) (uint16_t) letter_spacing << 16 | (uint16_t) scale,
        (uint32_t) metric_family << 16 | (uint32_t) transform << 8 | flags
    };
    uint32_t hash = style_fnv1a(STYLE_FNV1A_SEED, text, length, false);
    hash = style_fnv1a(hash, parameters, sizeof(parameters), false);
    if (hash == 0) hash = 1;
    LayoutTextMeasureEntry *set = NULL;
    if (cache->text_measures != NULL) {
        set = layout_text_measure_set(cache->text_measures,
                                      cache->text_measure_capacity, hash);
        for (size_t way = 0; way < LAYOUT_TEXT_MEASURE_WAYS; way++) {
            LayoutTextMeasureEntry *entry = &set[way];
            if (entry->hash == hash && entry->length == length
                && entry->face_serial == face->serial
                && entry->font_size_fixed == font_size_fixed
                && entry->letter_spacing == letter_spacing
                && entry->scale == scale
                && entry->metric_family == (uint8_t) metric_family
                && entry->transform == transform
                && entry->flags == flags
                && memcmp(entry->text, text, length) == 0) {
                entry->stamp = ++cache->text_measure_clock;
                cache->stats.text_measure_hits++;
                return entry->width;
            }
        }
    }
    cache->stats.text_measure_misses++;
    bool exact = false;
    int width = measured_flow_text_width_fixed_checked(
        face, metric_family, text, length, font_size_fixed, synthetic_bold,
        metric_bold, scale, letter_spacing, transform, kerning, &exact);
    if (!exact) return width;
    /* Measuring can allocate (a face's glyph state), and an allocation can
       run the Budget's reclaim hook, which may have yielded the table:
       find the set again. */
    if (!layout_text_measure_admit(cache, generation)) return width;
    set = layout_text_measure_set(cache->text_measures,
                                  cache->text_measure_capacity, hash);
    /* The clock orders entries for replacement; restart it, and the table,
       before it wraps. */
    if (cache->text_measure_clock == UINT32_MAX)
        layout_text_measure_clear(cache);
    LayoutTextMeasureEntry *victim = &set[0];
    for (size_t way = 0; way < LAYOUT_TEXT_MEASURE_WAYS; way++) {
        if (set[way].hash == 0) {
            victim = &set[way];
            break;
        }
        if (set[way].stamp < victim->stamp) victim = &set[way];
    }
    if (victim->hash != 0) {
        cache->stats.text_measure_evictions++;
        cache->text_measure_build_evictions++;
    }
    victim->face_serial = face->serial;
    victim->hash = hash;
    victim->stamp = ++cache->text_measure_clock;
    victim->width = width;
    victim->font_size_fixed = font_size_fixed;
    victim->letter_spacing = (int16_t) letter_spacing;
    victim->scale = (int16_t) scale;
    victim->length = (uint8_t) length;
    victim->metric_family = (uint8_t) metric_family;
    victim->transform = transform;
    victim->flags = flags;
    memcpy(victim->text, text, length);
    return width;
}
