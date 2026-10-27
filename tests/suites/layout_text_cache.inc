/* Retained run measurements (src/layout_text_cache.c): every input of a
   run's measurement is part of its key, a refused or estimated
   measurement is never retained, the optional glyph generation discards
   the table, and the table is bounded and released under pressure. */

#include "tilefinch/glyph_component.h"

typedef struct {
    const FontFace *face;
    FontFamily family;
    const char *text;
    size_t length;
    int size;
    bool synthetic_bold, metric_bold;
    int scale, letter_spacing;
    TextTransformMode transform;
    bool kerning;
} TextMeasureCall;

static int text_measure_reference(const TextMeasureCall *call)
{
    return measured_flow_text_width_fixed(call->face, call->family,
        call->text, call->length, call->size, call->synthetic_bold,
        call->metric_bold, call->scale, call->letter_spacing,
        call->transform, call->kerning);
}

static int text_measure_call(LayoutReuseCache *cache,
                             const TextMeasureCall *call)
{
    return layout_text_measure(cache, call->face, call->family, call->text,
        call->length, call->size, call->synthetic_bold, call->metric_bold,
        call->scale, call->letter_spacing, call->transform, call->kerning);
}

/* One measurement through the cache: expect a hit or a miss, and the
   uncached width either way. */
static bool text_measure_expect(LayoutReuseCache *cache,
                                const TextMeasureCall *call, bool hit)
{
    LayoutReuseStats before, after;
    layout_reuse_cache_stats(cache, &before);
    int width = text_measure_call(cache, call);
    layout_reuse_cache_stats(cache, &after);
    bool was_hit = after.text_measure_hits == before.text_measure_hits + 1u
        && after.text_measure_misses == before.text_measure_misses;
    bool was_miss = after.text_measure_misses == before.text_measure_misses + 1u
        && after.text_measure_hits == before.text_measure_hits;
    int reference = text_measure_reference(call);
    if ((hit ? !was_hit : !was_miss) || width != reference) {
        printf("text measure %.*s: hit=%d expected=%d width=%d reference=%d\n",
               (int) call->length, call->text, was_hit, hit, width,
               reference);
        return false;
    }
    return true;
}

static int test_text_measure_cache_keys(void)
{
    Budget budget;
    budget_init(&budget, 32u * MIB);
    FontSet fonts;
    CHECK(font_set_load(&fonts, &budget, TILEFINCH_TEST_SANS_FONT,
                        TILEFINCH_TEST_SERIF_FONT,
                        TILEFINCH_TEST_SANS_ITALIC_FONT,
                        TILEFINCH_TEST_SANS_BOLD_FONT,
                        TILEFINCH_TEST_SERIF_BOLD_FONT,
                        TILEFINCH_TEST_METRIC_SANS_FONT,
                        TILEFINCH_TEST_METRIC_SANS_BOLD_FONT, 2 * MIB));
    const FontFace *sans = font_set_face(&fonts, FONT_SANS);
    const FontFace *bold = font_set_face_variant(&fonts, FONT_SANS, false,
                                                 true);
    const FontFace *serif = font_set_face(&fonts, FONT_SERIF);
    CHECK(sans != NULL && bold != NULL && serif != NULL && sans != bold
          && sans->serial != 0 && bold->serial != sans->serial);
    LayoutReuseCache *cache = layout_reuse_cache_create(&budget);
    CHECK(cache != NULL);
    size_t retained_before = 0;
    {
        LayoutReuseStats stats;
        layout_reuse_cache_stats(cache, &stats);
        retained_before = stats.retained_bytes;
    }

    const TextMeasureCall base = {
        .face = sans, .family = FONT_SANS, .text = "Wave", .length = 4,
        .size = 13 * 64, .scale = 1, .transform = TEXT_TRANSFORM_NONE,
        .kerning = true
    };
    CHECK(text_measure_expect(cache, &base, false));
    CHECK(text_measure_expect(cache, &base, true));
    /* The same bytes elsewhere in memory are the same run. */
    char copy[] = "xWavex";
    TextMeasureCall moved = base;
    moved.text = copy + 1;
    CHECK(text_measure_expect(cache, &moved, true));

    /* Each input alone must miss, then hit on repetition. */
    TextMeasureCall variants[13];
    size_t count = 0;
    variants[count] = base; variants[count++].text = "Wavf";
    variants[count] = base; variants[count].text = "Wav";
    variants[count++].length = 3;
    variants[count] = base; variants[count].text = "WAVE";
    variants[count++].length = 4;
    variants[count] = base; variants[count++].face = bold;
    variants[count] = base; variants[count++].face = serif;
    variants[count] = base; variants[count++].family = FONT_SYSTEM_SANS;
    variants[count] = base; variants[count++].size = 13 * 64 + 32;
    variants[count] = base; variants[count++].synthetic_bold = true;
    variants[count] = base; variants[count++].metric_bold = true;
    variants[count] = base; variants[count++].letter_spacing = 40;
    variants[count] = base;
    variants[count++].transform = TEXT_TRANSFORM_UPPERCASE;
    variants[count] = base; variants[count++].kerning = false;
    /* Scale only reaches the estimate; it is still a distinct call. */
    variants[count] = base; variants[count++].scale = 2;
    CHECK(count == sizeof(variants) / sizeof(variants[0]));
    for (size_t i = 0; i < count; i++) {
        if (!text_measure_expect(cache, &variants[i], false)) {
            printf("variant %zu did not miss\n", i);
            return 1;
        }
        CHECK(text_measure_expect(cache, &variants[i], true));
    }
    /* Kerning and tracking really change these widths: the key is not
       only distinct but necessary. */
    CHECK(text_measure_reference(&variants[9])
          != text_measure_reference(&base));
    CHECK(text_measure_reference(&variants[11])
          != text_measure_reference(&base));
    CHECK(text_measure_expect(cache, &base, true));

    /* "ahikxw" and "arjtra" share their FNV-1a state, so under any other
       inputs they share a hash and a set: only the bytes tell them apart. */
    TextMeasureCall collide_a = base, collide_b = base;
    collide_a.text = "ahikxw";
    collide_b.text = "arjtra";
    collide_a.length = collide_b.length = 6;
    CHECK(text_measure_expect(cache, &collide_a, false));
    CHECK(text_measure_expect(cache, &collide_b, false));
    CHECK(text_measure_reference(&collide_a)
          != text_measure_reference(&collide_b));
    CHECK(text_measure_expect(cache, &collide_a, true));

    /* A face replaced in place (a reload, a publication) has a new serial:
       the same struct address must not serve the old face's widths. */
    FontFace renewed = *sans;
    renewed.serial = sans->serial | (UINT64_C(1) << 62);
    TextMeasureCall reloaded = base;
    reloaded.face = &renewed;
    CHECK(text_measure_expect(cache, &reloaded, false));

    /* The face could not measure (no implementation): the estimate is
       returned and never retained. */
    FontFace missing = {.loaded = true, .serial = UINT64_C(1) << 61};
    TextMeasureCall estimated = base;
    estimated.face = &missing;
    CHECK(text_measure_expect(cache, &estimated, false));
    CHECK(text_measure_expect(cache, &estimated, false));

    /* Unkeyable runs bypass the table entirely. */
    LayoutReuseStats before, after;
    layout_reuse_cache_stats(cache, &before);
    static const char long_run[] = "Antidisestablishmentarianism-and-more";
    CHECK(sizeof(long_run) - 1u > LAYOUT_TEXT_MEASURE_BYTES);
    TextMeasureCall long_call = base;
    long_call.text = long_run;
    long_call.length = sizeof(long_run) - 1u;
    CHECK(text_measure_call(cache, &long_call)
          == text_measure_reference(&long_call));
    TextMeasureCall wide_spacing = base;
    wide_spacing.letter_spacing = INT16_MAX + 1;
    CHECK(text_measure_call(cache, &wide_spacing)
          == text_measure_reference(&wide_spacing));
    layout_reuse_cache_stats(cache, &after);
    CHECK(after.text_measure_hits == before.text_measure_hits
          && after.text_measure_misses == before.text_measure_misses);

    /* Binding optional glyph packs changes what a codepoint measures as:
       the whole table goes. */
    TilefinchGlyphProvider *provider = tilefinch_glyph_provider_create(&budget);
    CHECK(provider != NULL && font_optional_glyph_provider_install(provider));
    CHECK(text_measure_expect(cache, &base, false));
    CHECK(text_measure_expect(cache, &variants[3], false));
    CHECK(text_measure_expect(cache, &base, true));
    CHECK(font_optional_glyph_provider_uninstall(provider));
    tilefinch_glyph_provider_destroy(provider);
    CHECK(text_measure_expect(cache, &base, false));

    /* Bounded: within a build, many distinct runs replace entries rather
       than grow the table. */
    layout_reuse_cache_stats(cache, &before);
    CHECK(before.text_measure_bytes
              == LAYOUT_TEXT_MEASURE_INITIAL_CAPACITY
                 * sizeof(LayoutTextMeasureEntry)
          && before.retained_bytes
             == retained_before + before.text_measure_bytes);
    char word[16];
    for (unsigned i = 0; i < 4u * LAYOUT_TEXT_MEASURE_CAPACITY_LIMIT; i++) {
        int length = snprintf(word, sizeof(word), "w%u", i);
        TextMeasureCall call = base;
        call.text = word;
        call.length = (size_t) length;
        CHECK(text_measure_call(cache, &call)
              == text_measure_reference(&call));
    }
    layout_reuse_cache_stats(cache, &after);
    CHECK(after.text_measure_bytes == before.text_measure_bytes
          && after.retained_bytes == before.retained_bytes
          && after.text_measure_evictions - before.text_measure_evictions
             >= 4u * LAYOUT_TEXT_MEASURE_CAPACITY_LIMIT
                - LAYOUT_TEXT_MEASURE_INITIAL_CAPACITY);
    /* Between builds, a table whose build evicted grows, keeping its
       entries, and stops at its limit. */
    size_t capacity = LAYOUT_TEXT_MEASURE_INITIAL_CAPACITY;
    for (unsigned build = 0; build < 8u; build++) {
        layout_reuse_cache_prepare(cache, NULL, NULL, NULL, 480);
        if (capacity < LAYOUT_TEXT_MEASURE_CAPACITY_LIMIT) capacity *= 2u;
        layout_reuse_cache_stats(cache, &after);
        CHECK(after.text_measure_bytes
                  == capacity * sizeof(LayoutTextMeasureEntry)
              && after.retained_bytes
                 == retained_before + after.text_measure_bytes);
        CHECK(text_measure_expect(cache, &base, build != 0));
        for (unsigned i = 0; i < 2u * capacity; i++) {
            int length = snprintf(word, sizeof(word), "g%u.%u", build, i);
            TextMeasureCall call = base;
            call.text = word;
            call.length = (size_t) length;
            CHECK(text_measure_call(cache, &call)
                  == text_measure_reference(&call));
        }
        CHECK(text_measure_expect(cache, &base, false));
    }
    CHECK(capacity == LAYOUT_TEXT_MEASURE_CAPACITY_LIMIT);
    /* A build that evicts nothing keeps the table's size. */
    layout_reuse_cache_prepare(cache, NULL, NULL, NULL, 480);
    layout_reuse_cache_prepare(cache, NULL, NULL, NULL, 480);
    layout_reuse_cache_stats(cache, &after);
    CHECK(after.text_measure_bytes
          == LAYOUT_TEXT_MEASURE_CAPACITY_LIMIT
             * sizeof(LayoutTextMeasureEntry));

    /* Pressure: reclaim releases the table with the style payloads, and
       it is not taken again while the headroom is missing. */
    CHECK(layout_reuse_cache_reclaim_styles(cache) >= after.text_measure_bytes);
    layout_reuse_cache_stats(cache, &after);
    CHECK(after.text_measure_bytes == 0
          && after.retained_bytes == retained_before);
    CHECK(text_measure_expect(cache, &base, false));
    CHECK(text_measure_expect(cache, &base, false));
    layout_reuse_cache_stats(cache, &after);
    CHECK(after.text_measure_bytes == 0);
    layout_reuse_cache_destroy(cache);

    /* A budget without the working headroom never admits the table, and
       measuring still answers. */
    cache = layout_reuse_cache_create(&budget);
    CHECK(cache != NULL);
    size_t limit = budget.limit;
    budget.limit = budget.current + LAYOUT_REUSE_STYLE_WORKING_BYTES;
    CHECK(text_measure_expect(cache, &base, false));
    CHECK(text_measure_expect(cache, &base, false));
    layout_reuse_cache_stats(cache, &after);
    CHECK(after.text_measure_bytes == 0);
    budget.limit = limit;
    layout_reuse_cache_destroy(cache);
    font_set_destroy(&fonts);
    CHECK(budget.current == 0);
    return 0;
}

static size_t text_measure_yield_hook(void *opaque, size_t needed_bytes)
{
    (void) needed_bytes;
    return layout_reuse_cache_yield_text_measures(opaque);
}

/* The Budget's reclaim hook (the script heap growing toward its ceiling,
   or any allocation short of room) frees the table, and it stays off for
   the cache's lifetime: a page that needed the room once needs it again. */
static int test_text_measure_cache_yields(void)
{
    Budget budget;
    budget_init(&budget, 32u * MIB);
    FontSet fonts;
    CHECK(font_set_load(&fonts, &budget, TILEFINCH_TEST_SANS_FONT,
                        TILEFINCH_TEST_SERIF_FONT,
                        TILEFINCH_TEST_SANS_ITALIC_FONT,
                        TILEFINCH_TEST_SANS_BOLD_FONT,
                        TILEFINCH_TEST_SERIF_BOLD_FONT,
                        TILEFINCH_TEST_METRIC_SANS_FONT,
                        TILEFINCH_TEST_METRIC_SANS_BOLD_FONT, 2 * MIB));
    LayoutReuseCache *cache = layout_reuse_cache_create(&budget);
    CHECK(cache != NULL);
    const TextMeasureCall base = {
        .face = font_set_face(&fonts, FONT_SANS), .family = FONT_SANS,
        .text = "Wave", .length = 4, .size = 13 * 64, .scale = 1,
        .transform = TEXT_TRANSFORM_NONE, .kerning = true
    };
    CHECK(text_measure_expect(cache, &base, false));
    CHECK(text_measure_expect(cache, &base, true));
    LayoutReuseStats stats;
    layout_reuse_cache_stats(cache, &stats);
    size_t table = stats.text_measure_bytes, retained = stats.retained_bytes;
    CHECK(table != 0);
    budget_set_reclaim_hook(&budget, text_measure_yield_hook, cache);
    size_t current = budget.current;
    /* Ask for more room than is free: only the hook can provide it. */
    CHECK(!budget_make_room(&budget,
                            budget_remaining(&budget) + table + 4096u));
    CHECK(budget.reclaim_calls == 1 && current - budget.current >= table);
    budget_set_reclaim_hook(&budget, NULL, NULL);
    layout_reuse_cache_stats(cache, &stats);
    CHECK(stats.text_measure_bytes == 0
          && stats.retained_bytes == retained - table);
    for (unsigned build = 0; build < 3u; build++) {
        layout_reuse_cache_prepare(cache, NULL, NULL, NULL, 480);
        CHECK(text_measure_expect(cache, &base, false));
        layout_reuse_cache_stats(cache, &stats);
        CHECK(stats.text_measure_bytes == 0);
    }
    CHECK(layout_reuse_cache_yield_text_measures(cache) == 0);
    layout_reuse_cache_destroy(cache);
    font_set_destroy(&fonts);
    CHECK(budget.current == 0);
    return 0;
}

static bool text_cache_commands_identical(const LayoutDocument *a,
                                          const LayoutDocument *b)
{
    if (a->count != b->count || a->height != b->height) {
        printf("text cache: count %zu/%zu height %d/%d\n", a->count, b->count,
               a->height, b->height);
        return false;
    }
    for (size_t i = 0; i < a->count; i++) {
        const DrawCommand *x = &a->commands[i], *y = &b->commands[i];
        if (x->type != y->type || x->x != y->x || x->y != y->y
            || x->width != y->width || x->height != y->height
            || x->color != y->color || x->radius != y->radius
            || x->text != y->text || x->text_length != y->text_length
            || x->font_size != y->font_size || x->scale != y->scale) {
            printf("text cache: command %zu differs: %d,%d,%d,%d r=%d n=%u "
                   "vs %d,%d,%d,%d r=%d n=%u\n", i, x->x, x->y, x->width,
                   x->height, x->radius, (unsigned) x->text_length, y->x,
                   y->y, y->width, y->height, y->radius,
                   (unsigned) y->text_length);
            return false;
        }
    }
    return true;
}

/* Lays `html` out at `width` through `cache` and without any cache, and
   requires the two display lists to be identical. */
static int text_cache_layout_matches(Budget *budget, const FontSet *fonts,
                                     LayoutReuseCache *cache,
                                     const char *html, int width,
                                     LayoutReuseStats *delta)
{
    PocDocument document = {0};
    Stylesheet sheet = {0};
    ViewportContext viewport;
    CHECK(document_parse(&document, budget, html, strlen(html), 23)
          && viewport_context_init(&viewport, width, 272, width, 272)
          && stylesheet_build_context(&sheet, budget, &document, &viewport));
    LayoutReuseStats before, after;
    layout_reuse_cache_stats(cache, &before);
    LayoutDocument cached = {0}, fresh = {0};
    CHECK(layout_build_context_reuse(&cached, budget, &document, &sheet,
                                     fonts, NULL, &viewport, cache));
    layout_reuse_cache_stats(cache, &after);
    CHECK(layout_build_context(&fresh, budget, &document, &sheet,
                               fonts, NULL, &viewport));
    CHECK(text_cache_commands_identical(&cached, &fresh));
    layout_destroy(&cached);
    layout_destroy(&fresh);
    stylesheet_destroy(&sheet);
    document_destroy(&document);
    delta->text_measure_hits = after.text_measure_hits
                               - before.text_measure_hits;
    delta->text_measure_misses = after.text_measure_misses
                                 - before.text_measure_misses;
    return 0;
}

/* The same words in every run, so a wrong key would serve one run's
   widths to another. */
#define TEXT_CACHE_WORDS \
    "Wave AVATAR tolerably fine wording Wave AVATAR tolerably fine " \
    "wording Wave AVATAR tolerably"

static int test_text_measure_cache_layout(void)
{
    Budget budget;
    budget_init(&budget, 32u * MIB);
    CHECK(budget_install_lexbor(&budget));
    FontSet fonts;
    CHECK(font_set_load(&fonts, &budget, TILEFINCH_TEST_SANS_FONT,
                        TILEFINCH_TEST_SERIF_FONT,
                        TILEFINCH_TEST_SANS_ITALIC_FONT,
                        TILEFINCH_TEST_SANS_BOLD_FONT,
                        TILEFINCH_TEST_SERIF_BOLD_FONT,
                        TILEFINCH_TEST_METRIC_SANS_FONT,
                        TILEFINCH_TEST_METRIC_SANS_BOLD_FONT, 2 * MIB));
    static const char first[] =
        "<!doctype html><style>body{margin:0;font-size:13px}"
        "p{margin:0 0 2px}.box{width:150px}"
        ".shrink{display:inline-block}"
        ".clip{white-space:nowrap;overflow:hidden;text-overflow:ellipsis}"
        ".all{word-break:break-all}.any{overflow-wrap:anywhere}"
        ".balance{text-wrap:balance}.pre{white-space:pre}"
        "</style><body>"
        "<p>" TEXT_CACHE_WORDS "</p>"
        "<p class=box>" TEXT_CACHE_WORDS "</p>"
        "<p><span class=shrink>" TEXT_CACHE_WORDS "</span></p>"
        "<table><tr><td>" TEXT_CACHE_WORDS "</td><td>Wave</td></tr></table>"
        "<p class='box clip'>" TEXT_CACHE_WORDS "</p>"
        "<p class='box all'>" TEXT_CACHE_WORDS "</p>"
        "<p class='box any'>WaveAVATARtolerablyfinewordingWave</p>"
        "<p class='box balance'>" TEXT_CACHE_WORDS "</p>"
        "<p class=pre>Wave  AVATAR\ttolerably\nfine</p>"
        "</body>";
    /* The same text with every measurement input changed in turn. */
    static const char second[] =
        "<!doctype html><style>body{margin:0;font-size:13px}"
        "p{margin:0 0 2px}"
        ".spaced{letter-spacing:1.5px}.words{word-spacing:4px}"
        ".upper{text-transform:uppercase}.cap{text-transform:capitalize}"
        ".lower{text-transform:lowercase}"
        ".bold{font-weight:bold}.heavy{font-weight:600}"
        ".italic{font-style:italic}.serif{font-family:serif}"
        ".verdana{font-family:Verdana,sans-serif}"
        ".system{font-family:system-ui,sans-serif}"
        ".size{font-size:17.5px}.small{font-size:9px}"
        ".nokern{font-kerning:none}.pre{white-space:pre-wrap}"
        "</style><body>"
        "<p class=spaced>" TEXT_CACHE_WORDS "</p>"
        "<p class=words>" TEXT_CACHE_WORDS "</p>"
        "<p class=upper>" TEXT_CACHE_WORDS "</p>"
        "<p class=cap>" TEXT_CACHE_WORDS "</p>"
        "<p class=lower>" TEXT_CACHE_WORDS "</p>"
        "<p class=bold>" TEXT_CACHE_WORDS "</p>"
        "<p class=heavy>" TEXT_CACHE_WORDS "</p>"
        "<p class=italic>" TEXT_CACHE_WORDS "</p>"
        "<p class=serif>" TEXT_CACHE_WORDS "</p>"
        "<p class=verdana>" TEXT_CACHE_WORDS "</p>"
        "<p class=system>" TEXT_CACHE_WORDS "</p>"
        "<p class=size>" TEXT_CACHE_WORDS "</p>"
        "<p class=small>" TEXT_CACHE_WORDS "</p>"
        "<p class=nokern>" TEXT_CACHE_WORDS "</p>"
        "<p class=pre>" TEXT_CACHE_WORDS "</p>"
        "<p lang=ar dir=rtl>" TEXT_CACHE_WORDS "</p>"
        "</body>";
    LayoutReuseCache *cache = layout_reuse_cache_create(&budget);
    CHECK(cache != NULL);
    LayoutReuseStats delta;
    /* First build fills the table; the second is served from it. */
    CHECK(text_cache_layout_matches(&budget, &fonts, cache, first, 480,
                                    &delta) == 0);
    CHECK(delta.text_measure_misses > 0);
    CHECK(text_cache_layout_matches(&budget, &fonts, cache, first, 480,
                                    &delta) == 0);
    CHECK(delta.text_measure_hits > 50 && delta.text_measure_misses == 0);
    /* A different available width reuses the widths, not the breaks. */
    CHECK(text_cache_layout_matches(&budget, &fonts, cache, first, 300,
                                    &delta) == 0);
    CHECK(delta.text_measure_hits > 50);
    CHECK(text_cache_layout_matches(&budget, &fonts, cache, first, 97,
                                    &delta) == 0);
    /* Same words, other inputs: each style's runs must miss, never take
       the first document's widths. */
    CHECK(text_cache_layout_matches(&budget, &fonts, cache, second, 480,
                                    &delta) == 0);
    CHECK(delta.text_measure_misses > 0);
    CHECK(text_cache_layout_matches(&budget, &fonts, cache, second, 480,
                                    &delta) == 0);
    CHECK(delta.text_measure_misses == 0);
    layout_reuse_cache_destroy(cache);
    font_set_destroy(&fonts);
    CHECK(budget.current == 0 && budget_uninstall_lexbor(&budget));
    return 0;
}
