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
   Contextually shaped runs in an inline flow never reach here. Intrinsic
   sizing retains the shaped width of a whole Arabic word under its own
   key bit (layout_text_measure_find_shaped): a word bounded by white
   space joins only within itself, so its forms do not depend on the text
   around it (layout_bidi_intrinsic_shaped_width says which words qualify).

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
    TEXT_MEASURE_KERNING = 1u << 2,
    TEXT_MEASURE_SHAPED = 1u << 3
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

typedef struct {
    const FontFace *face;
    const char *text;
    size_t length;
    uint32_t hash;
    int32_t font_size_fixed;
    int16_t letter_spacing;
    int16_t scale;
    uint8_t metric_family;
    uint8_t transform;
    uint8_t flags;
} TextMeasureKey;

/* False when the run or an argument does not fit an entry. */
static bool text_measure_key(
    TextMeasureKey *key, const FontFace *face, FontFamily metric_family,
    const char *text, size_t length, int font_size_fixed,
    bool synthetic_bold, bool metric_bold, int scale, int letter_spacing,
    TextTransformMode transform, bool kerning, bool shaped)
{
    if (face == NULL || face->serial == 0 || text == NULL
        || length == 0 || length > LAYOUT_TEXT_MEASURE_BYTES
        || letter_spacing < INT16_MIN || letter_spacing > INT16_MAX
        || scale < INT16_MIN || scale > INT16_MAX
        || (unsigned) metric_family > UINT8_MAX) return false;
    uint8_t flags = (uint8_t) ((synthetic_bold ? TEXT_MEASURE_SYNTHETIC_BOLD : 0u)
        | (metric_bold ? TEXT_MEASURE_METRIC_BOLD : 0u)
        | (kerning ? TEXT_MEASURE_KERNING : 0u)
        | (shaped ? TEXT_MEASURE_SHAPED : 0u));
    uint32_t parameters[5] = {
        (uint32_t) face->serial, (uint32_t) (face->serial >> 32),
        (uint32_t) font_size_fixed,
        (uint32_t) (uint16_t) letter_spacing << 16 | (uint16_t) scale,
        (uint32_t) metric_family << 16 | (uint32_t) transform << 8 | flags
    };
    uint32_t hash = style_fnv1a(STYLE_FNV1A_SEED, text, length, false);
    hash = style_fnv1a(hash, parameters, sizeof(parameters), false);
    *key = (TextMeasureKey) {
        .face = face, .text = text, .length = length,
        .hash = hash == 0 ? 1u : hash,
        .font_size_fixed = font_size_fixed,
        .letter_spacing = (int16_t) letter_spacing,
        .scale = (int16_t) scale,
        .metric_family = (uint8_t) metric_family,
        .transform = (uint8_t) transform,
        .flags = flags
    };
    return true;
}

static bool text_measure_find(LayoutReuseCache *cache,
                              const TextMeasureKey *key, int *width)
{
    uint64_t generation = font_optional_glyph_generation();
    if (cache->text_measures != NULL
        && generation != cache->text_measure_generation) {
        layout_text_measure_clear(cache);
        cache->text_measure_generation = generation;
    }
    if (cache->text_measures != NULL) {
        LayoutTextMeasureEntry *set = layout_text_measure_set(
            cache->text_measures, cache->text_measure_capacity, key->hash);
        for (size_t way = 0; way < LAYOUT_TEXT_MEASURE_WAYS; way++) {
            LayoutTextMeasureEntry *entry = &set[way];
            if (entry->hash == key->hash && entry->length == key->length
                && entry->face_serial == key->face->serial
                && entry->font_size_fixed == key->font_size_fixed
                && entry->letter_spacing == key->letter_spacing
                && entry->scale == key->scale
                && entry->metric_family == key->metric_family
                && entry->transform == key->transform
                && entry->flags == key->flags
                && memcmp(entry->text, key->text, key->length) == 0) {
                entry->stamp = ++cache->text_measure_clock;
                cache->stats.text_measure_hits++;
                if ((key->flags & TEXT_MEASURE_SHAPED) != 0)
                    cache->stats.text_measure_shaped_hits++;
                *width = entry->width;
                return true;
            }
        }
    }
    cache->stats.text_measure_misses++;
    return false;
}

static void text_measure_store(LayoutReuseCache *cache,
                               const TextMeasureKey *key, int width,
                               uint64_t generation)
{
    /* Measuring can allocate (a face's glyph state), and an allocation can
       run the Budget's reclaim hook, which may have yielded the table:
       find the set again. */
    if (!layout_text_measure_admit(cache, generation)) return;
    LayoutTextMeasureEntry *set = layout_text_measure_set(
        cache->text_measures, cache->text_measure_capacity, key->hash);
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
    victim->face_serial = key->face->serial;
    victim->hash = key->hash;
    victim->stamp = ++cache->text_measure_clock;
    victim->width = width;
    victim->font_size_fixed = key->font_size_fixed;
    victim->letter_spacing = key->letter_spacing;
    victim->scale = key->scale;
    victim->length = (uint8_t) key->length;
    victim->metric_family = key->metric_family;
    victim->transform = key->transform;
    victim->flags = key->flags;
    memcpy(victim->text, key->text, key->length);
}

int layout_text_measure(LayoutReuseCache *cache, const FontFace *face,
                        FontFamily metric_family, const char *text,
                        size_t length, int font_size_fixed,
                        bool synthetic_bold, bool metric_bold, int scale,
                        int letter_spacing, TextTransformMode transform,
                        bool kerning)
{
    TextMeasureKey key;
    if (cache == NULL || !text_measure_key(
            &key, face, metric_family, text, length, font_size_fixed,
            synthetic_bold, metric_bold, scale, letter_spacing, transform,
            kerning, false)) {
        return measured_flow_text_width_fixed(
            face, metric_family, text, length, font_size_fixed,
            synthetic_bold, metric_bold, scale, letter_spacing, transform,
            kerning);
    }
    uint64_t generation = font_optional_glyph_generation();
    int width = 0;
    if (text_measure_find(cache, &key, &width)) return width;
    bool exact = false;
    width = measured_flow_text_width_fixed_checked(
        face, metric_family, text, length, font_size_fixed, synthetic_bold,
        metric_bold, scale, letter_spacing, transform, kerning, &exact);
    if (exact) text_measure_store(cache, &key, width, generation);
    return width;
}

bool layout_text_measure_find_shaped(
    LayoutReuseCache *cache, const FontFace *face, FontFamily metric_family,
    const char *text, size_t length, int font_size_fixed,
    bool synthetic_bold, bool metric_bold, int letter_spacing,
    TextTransformMode transform, bool kerning, int *width_fixed)
{
    TextMeasureKey key;
    return cache != NULL && width_fixed != NULL
        && text_measure_key(&key, face, metric_family, text, length,
                            font_size_fixed, synthetic_bold, metric_bold, 0,
                            letter_spacing, transform, kerning, true)
        && text_measure_find(cache, &key, width_fixed);
}

void layout_text_measure_store_shaped(
    LayoutReuseCache *cache, const FontFace *face, FontFamily metric_family,
    const char *text, size_t length, int font_size_fixed,
    bool synthetic_bold, bool metric_bold, int letter_spacing,
    TextTransformMode transform, bool kerning, int width_fixed,
    uint64_t generation)
{
    TextMeasureKey key;
    if (cache != NULL
        && text_measure_key(&key, face, metric_family, text, length,
                            font_size_fixed, synthetic_bold, metric_bold, 0,
                            letter_spacing, transform, kerning, true))
        text_measure_store(cache, &key, width_fixed, generation);
}
