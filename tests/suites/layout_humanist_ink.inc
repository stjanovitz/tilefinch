/* Repository-owned typography, independent of any retained site markup. */
#include "../../src/humanist_ink_bounds.inc"
#include "../../src/render/humanist_ink_membership.inc"

static int test_humanist_ink_fitting(void)
{
    puts("test: humanist cold glyph fitting preserves metrics and ownership");
    /* Guard both missing candidate bits and unnecessary native-key splits.
       These are the exact cold metadata and warm normalization functions. */
    for (unsigned profile = 0; profile <= 4; profile++) {
        for (unsigned codepoint = 0; codepoint <= 127; codepoint++) {
            unsigned expected = humanist_ink_bound(profile, codepoint)
                ? profile : 0;
            CHECK(glyph_cache_ink_profile(profile, codepoint) == expected);
        }
    }
    Budget budget;
    budget_init(&budget, 16 * MIB);
    budget_install_lexbor(&budget);
    FontSet fonts;
    CHECK(font_set_load(&fonts, &budget, TILEFINCH_TEST_SANS_FONT,
                        TILEFINCH_TEST_SERIF_FONT, TILEFINCH_TEST_SANS_ITALIC_FONT,
                        TILEFINCH_TEST_SANS_BOLD_FONT,
                        TILEFINCH_TEST_SERIF_BOLD_FONT,
                        TILEFINCH_TEST_METRIC_SANS_FONT,
                        TILEFINCH_TEST_METRIC_SANS_BOLD_FONT, 2 * MIB));
    FontFace malformed = fonts.sans;
    malformed.budget = NULL;
    FontGlyph rejected;
    CHECK(!font_glyph_load_at_size(&malformed, 'J', 939, false, &rejected)
          && rejected.pixels == NULL && rejected.budget == NULL);
    size_t saved_limit = budget.limit;
    budget.limit = budget.current;
    CHECK(!font_glyph_load_at_size(&fonts.sans, 'J', 939, false, &rejected)
          && rejected.pixels == NULL && rejected.budget == NULL
          && rejected.width == 0 && rejected.height == 0);
    budget.limit = saved_limit;
    /* Each raster stage uses the owner-thread Budget. Refusal must not be
       confused with an empty glyph or yield a cacheable partial bitmap. */
    static const struct { unsigned codepoint; int size; unsigned profile; }
        refusal_cases[] = {{'J', 939, 0}, {'J', 939, 1}, {'W', 8192, 0}};
    for (size_t c = 0; c < sizeof(refusal_cases) / sizeof(refusal_cases[0]); c++) {
        FontGlyph probe;
        CHECK(font_glyph_load_at_size_profile(
            &fonts.sans, refusal_cases[c].codepoint, refusal_cases[c].size,
            false, refusal_cases[c].profile, &probe));
        if (refusal_cases[c].codepoint == 'W') CHECK(probe.width > 64);
        font_glyph_destroy(&fonts.sans, &probe);
        size_t settled_glyph = budget.current;
        size_t refused = 0;
        for (size_t failure = 0; failure < 16; failure++) {
            size_t failures_before = budget.failure_count;
            budget_inject_failure_after(&budget, failure);
            bool loaded = font_glyph_load_at_size_profile(
                &fonts.sans, refusal_cases[c].codepoint, refusal_cases[c].size,
                false, refusal_cases[c].profile, &probe);
            budget_clear_failure_injection(&budget);
            if (budget.failure_count != failures_before) {
                FontGlyph empty;
                memset(&empty, 0, sizeof(empty));
                CHECK(!loaded && memcmp(&probe, &empty, sizeof(probe)) == 0);
                refused++;
            } else {
                CHECK(loaded);
            }
            font_glyph_destroy(&fonts.sans, &probe);
            CHECK(budget.current == settled_glyph);
        }
        CHECK(refused >= 6);
    }
    static const char page[] =
        "<!doctype html><style>html,body{margin:0;background:white}"
        "div{font:14.666667px Verdana,sans-serif;line-height:24px}"
        ".native{font-family:sans-serif}.bold{font-weight:700}"
        ".italic{font-style:italic}.synthetic{font-weight:600}"
        "</style><div>Jasper Java</div><div class=native>Jasper Java</div>"
        "<div class=bold>Jasper Java</div><div class=italic>Jasper Java</div>"
        "<div class=synthetic>Jasper Java</div>"
        "<div class='bold italic'>Jasper Java</div>";
    PocDocument document = {0};
    Stylesheet sheet = {0};
    LayoutDocument layout = {0};
    CHECK(document_parse(&document, &budget, page, sizeof(page) - 1, 32));
    CHECK(stylesheet_build(&sheet, &budget, &document, 480));
    CHECK(layout_build(&layout, &budget, &document, &sheet,
                       &fonts, NULL, 480));
    uint64_t fingerprint = layout.text_fingerprint;
    int widths[12] = {0};
    size_t text_count = 0;
    for (size_t i = 0; i < layout.count; i++) {
        const DrawCommand *command = &layout.commands[i];
        if (command->type != DRAW_TEXT) continue;
        CHECK(text_count < 12);
        widths[text_count++] = command->width;
    }
    CHECK(text_count == 12);
    static const int expected_widths[] = {
        48, 34, 46, 32, 54, 39, 48, 34, 50, 36, 57, 40
    };
    CHECK(fingerprint == UINT64_C(13452059951604937083)
          && memcmp(widths, expected_widths, sizeof(widths)) == 0);
    printf("humanist-layout fingerprint=%llu widths=",
           (unsigned long long) fingerprint);
    for (size_t i = 0; i < text_count; i++) printf("%s%d", i ? "," : "", widths[i]);
    putchar('\n');
    size_t settled = budget.current;
    uint16_t *frame = budget_calloc(&budget, 480u * 192u, sizeof(*frame));
    CHECK(frame != NULL);
    TileCache cache;
    CHECK(tile_cache_init(&cache, &budget, &layout, 8)
          && tile_cache_set_frame(&cache, frame, 480u * 192u)
          && tile_cache_render_frame(&cache, 0, 480, 192, NULL));
    bool raw_regular = false, fitted_regular = false;
    bool fitted_bold = false, fitted_italic = false;
    size_t shared_native_a = 0;
    FontGlyph raw = {0};
    CHECK(font_glyph_load_at_size(&fonts.sans, 'J', 939, false, &raw));
    for (size_t i = 0; i < cache.glyph_cache_capacity; i++) {
        const GlyphCacheEntry *entry = &cache.glyph_cache[i];
        shared_native_a += entry->valid && entry->codepoint == 'a'
            && entry->face == &fonts.sans && !entry->bold;
        if (!entry->valid || entry->codepoint != 'J') continue;
        if (entry->face == &fonts.sans && !entry->bold) {
            int right = entry->glyph.x_offset + entry->glyph.width;
            raw_regular |= right == raw.x_offset + raw.width + 1;
            fitted_regular |= right >= 7 && right <= 8;
            CHECK(entry->glyph.advance_fixed == raw.advance_fixed);
        } else if (entry->face == &fonts.sans_bold && !entry->bold) {
            fitted_bold |= entry->glyph.x_offset + entry->glyph.width >= 8;
        } else if (entry->face == &fonts.sans_italic && !entry->bold) {
            fitted_italic |= entry->glyph.x_offset + entry->glyph.width >= 8;
        }
    }
    font_glyph_destroy(&fonts.sans, &raw);
    /* The pre-fix renderer has only the raw J and fails this assertion. */
    CHECK(raw_regular && fitted_regular && fitted_bold && fitted_italic
          && shared_native_a == 1);
    FontGlyph raw_synthetic = {0}, raw_bold_italic = {0};
    CHECK(font_glyph_load_at_size(&fonts.sans, 'J', 939, true,
                                  &raw_synthetic)
          && font_glyph_load_at_size(&fonts.sans_italic, 'J', 939, true,
                                     &raw_bold_italic));
    bool synthetic_unchanged = false, bold_italic_unchanged = false;
    for (size_t i = 0; i < cache.glyph_cache_capacity; i++) {
        const GlyphCacheEntry *entry = &cache.glyph_cache[i];
        if (!entry->valid || entry->codepoint != 'J' || !entry->bold) continue;
        const FontGlyph *expected = entry->face == &fonts.sans
            ? &raw_synthetic : &raw_bold_italic;
        CHECK(entry->glyph.x_offset == expected->x_offset - 1
              && entry->glyph.width == expected->width + 2
              && entry->glyph.advance_fixed == expected->advance_fixed);
        synthetic_unchanged |= entry->face == &fonts.sans;
        bold_italic_unchanged |= entry->face == &fonts.sans_italic;
    }
    CHECK(synthetic_unchanged && bold_italic_unchanged);
    font_glyph_destroy(&fonts.sans, &raw_synthetic);
    font_glyph_destroy(&fonts.sans_italic, &raw_bold_italic);
    size_t misses = cache.glyph_cache_misses;
    size_t allocations = budget.allocation_count;
    CHECK(tile_cache_render_frame(&cache, 0, 480, 192, NULL)
          && cache.glyph_cache_misses == misses
          && budget.allocation_count == allocations
          && layout.text_fingerprint == fingerprint);
    size_t at = 0;
    for (size_t i = 0; i < layout.count; i++) {
        if (layout.commands[i].type == DRAW_TEXT)
            CHECK(layout.commands[i].width == widths[at++]);
    }
    tile_cache_cancel_frame_work(&cache);
    tile_cache_cancel_idle_work(&cache);
    tile_cache_destroy(&cache);
    budget_free(&budget, frame);
    CHECK(budget.current == settled);
    TilefinchCancellation cancelled;
    tilefinch_cancellation_init(&cancelled);
    tilefinch_cancellation_request(&cancelled);
    CHECK(tile_cache_init(&cache, &budget, &layout, 4)
          && tile_cache_prepare_frame_bounded_cancelable(
                 &cache, 0, 480, 192, UINT64_MAX / 2u, 1, &cancelled)
                 == RENDER_FRAME_WORK_CANCELLED
          && !tile_cache_frame_work_pending(&cache));
    tile_cache_destroy(&cache);
    CHECK(budget.current == settled);
    CHECK(tile_cache_init(&cache, &budget, &layout, 4)
          && tile_cache_prepare_frame_bounded(
                 &cache, 0, 480, 192, UINT64_MAX / 2u, 1)
                 == RENDER_FRAME_WORK_PENDING
          && cache.glyph_cache_misses > 0);
    CHECK(tile_cache_prepare_frame_bounded_cancelable(
              &cache, 0, 480, 192, UINT64_MAX / 2u, 1, &cancelled)
              == RENDER_FRAME_WORK_CANCELLED
          && !tile_cache_frame_work_pending(&cache));
    tile_cache_destroy(&cache);
    CHECK(budget.current == settled);
    /* Cancellation can leave completed cold glyphs resident until teardown;
       refusal can stop any of the rasterizer's bounded allocations. */
    bool saw_glyph_refusal_path = false;
    for (size_t failure = 0; failure < 16; failure++) {
        CHECK(tile_cache_init(&cache, &budget, &layout, 4));
        budget_inject_failure_after(&budget, failure);
        (void) tile_cache_prepare_frame_bounded(
            &cache, 0, 480, 192, UINT64_MAX / 2u, 1);
        saw_glyph_refusal_path |= cache.glyph_cache_misses > 0
            && budget.injected_failure_count > 0;
        budget_clear_failure_injection(&budget);
        tile_cache_cancel_frame_work(&cache);
        tile_cache_cancel_idle_work(&cache);
        tile_cache_destroy(&cache);
        CHECK(budget.current == settled);
    }
    CHECK(saw_glyph_refusal_path);
    layout_destroy(&layout);
    FontSet synthetic_fonts = fonts;
    synthetic_fonts.sans_bold.loaded = false;
    synthetic_fonts.sans_italic.loaded = false;
    static const char synthetic_page[] =
        "<!doctype html><style>div{font:700 italic 14.666667px Verdana;"
        "line-height:24px}</style><div>Jasper Java</div>";
    PocDocument synthetic_document;
    Stylesheet synthetic_sheet;
    CHECK(document_parse(&synthetic_document, &budget, synthetic_page,
                          sizeof(synthetic_page) - 1, 32)
          && stylesheet_build(&synthetic_sheet, &budget,
                               &synthetic_document, 480)
          && layout_build(&layout, &budget, &synthetic_document,
                           &synthetic_sheet, &synthetic_fonts, NULL, 480)
          && tile_cache_init(&cache, &budget, &layout, 4));
    frame = budget_calloc(&budget, 480u * 64u, sizeof(*frame));
    CHECK(frame != NULL
          && tile_cache_set_frame(&cache, frame, 480u * 64u)
          && tile_cache_render_frame(&cache, 0, 480, 64, NULL)
          && font_glyph_load_at_size(&fonts.sans, 'J', 939, true,
                                     &raw_synthetic));
    synthetic_unchanged = false;
    for (size_t i = 0; i < cache.glyph_cache_capacity; i++) {
        const GlyphCacheEntry *entry = &cache.glyph_cache[i];
        if (!entry->valid || entry->codepoint != 'J') continue;
        CHECK(entry->glyph.x_offset == raw_synthetic.x_offset - 1
              && entry->glyph.width == raw_synthetic.width + 2
              && entry->glyph.advance_fixed == raw_synthetic.advance_fixed);
        synthetic_unchanged = true;
    }
    CHECK(synthetic_unchanged);
    font_glyph_destroy(&fonts.sans, &raw_synthetic);
    tile_cache_destroy(&cache);
    budget_free(&budget, frame);
    layout_destroy(&layout);
    stylesheet_destroy(&synthetic_sheet);
    document_destroy(&synthetic_document);
    stylesheet_destroy(&sheet);
    document_destroy(&document);
    font_set_destroy(&fonts);
    CHECK(budget.current == 0);
    return 0;
}
