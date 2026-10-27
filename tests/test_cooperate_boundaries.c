/* What an engine cooperate checkpoint means reaches the frontend as
   TilefinchCooperateBoundary flags, not as a convention on phase names. */
#include "browser_engine_test_support.h"
#include "tilefinch/budget.h"
#include "tilefinch/layout.h"
#include "tilefinch/render.h"
#include "tilefinch/resources.h"

typedef struct {
    unsigned calls;
    unsigned legacy_calls;
    unsigned last_boundary;
    char last_phase[48];
} BoundaryRecorder;

static bool boundary_record(void *opaque, const char *phase,
                            size_t completed, unsigned boundary)
{
    (void) completed;
    BoundaryRecorder *recorder = opaque;
    recorder->calls++;
    recorder->last_boundary = boundary;
    snprintf(recorder->last_phase, sizeof(recorder->last_phase), "%s", phase);
    return true;
}

static bool boundary_record_legacy(void *opaque, const char *phase,
                                   size_t completed)
{
    (void) phase;
    (void) completed;
    ((BoundaryRecorder *) opaque)->legacy_calls++;
    return true;
}

/* The boundary flags a checkpoint passes reach a cooperate_at frontend
   unchanged, and a frontend that installs only cooperate still gets every
   checkpoint. */
int test_cooperate_boundary_dispatch(void)
{
    BoundaryRecorder recorder = {0};
    TilefinchPlatformServices services = {
        .context = &recorder, .cooperate = boundary_record_legacy,
        .cooperate_at = boundary_record
    };
    tilefinch_platform_set_services(&services);
    CHECK(tilefinch_platform_cooperate_at(
        "anything", 3, TILEFINCH_COOPERATE_PAGE_TASK));
    CHECK(recorder.calls == 1 && recorder.legacy_calls == 0
        && recorder.last_boundary == TILEFINCH_COOPERATE_PAGE_TASK
        && strcmp(recorder.last_phase, "anything") == 0);
    CHECK(tilefinch_platform_cooperate_at(NULL, 0, 0));
    CHECK(recorder.calls == 2 && recorder.last_boundary == 0
        && strcmp(recorder.last_phase, "engine") == 0);
    services.cooperate_at = NULL;
    tilefinch_platform_set_services(&services);
    CHECK(tilefinch_platform_cooperate_at(
        "anything", 3, TILEFINCH_COOPERATE_PAGE_TASK));
    CHECK(recorder.calls == 2 && recorder.legacy_calls == 1);
    tilefinch_platform_set_services(NULL);
    return 0;
}

/* A plain checkpoint means nothing beyond pacing: the phase name is for
   logs, so naming a checkpoint like a semantic one gives it no meaning. */
int test_cooperate_boundary_phase_names(void)
{
    static const char *const names[] = {
        "page-task-boundary", "render-input-boundary",
        "image-admission-boundary", "page-focus-feedback",
        "page-scroll-servable", "optional-image-publication",
        "optional-layout-completion", "render-compose", "render-prepare",
        "style-prepare", "layout"
    };
    BoundaryRecorder recorder = {0};
    TilefinchPlatformServices services = {
        .context = &recorder, .cooperate_at = boundary_record
    };
    tilefinch_platform_set_services(&services);
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
        recorder.last_boundary = ~0u;
        CHECK(tilefinch_platform_cooperate(names[i], 1)
            && recorder.last_boundary == 0u);
    }
    tilefinch_platform_set_services(NULL);
    return 0;
}

/* The flags each conventional phase name stands for, as the frontend read
   them before flags existed. A checkpoint that carries the name must carry
   at least these. */
static unsigned conventional_boundary(const char *phase, bool *known)
{
    static const struct {
        const char *phase;
        unsigned boundary;
    } names[] = {
        {"page-task-boundary", TILEFINCH_COOPERATE_PAGE_TASK},
        {"render-input-boundary", TILEFINCH_COOPERATE_RENDER_INPUT
            | TILEFINCH_COOPERATE_RENDER_SHELL_BORROWED},
        {"image-admission-boundary", TILEFINCH_COOPERATE_IMAGE_ADMISSION},
        {"page-focus-feedback", TILEFINCH_COOPERATE_FOCUS_FEEDBACK},
        {"page-scroll-servable", TILEFINCH_COOPERATE_SCROLL_SERVABLE},
        {"optional-font-publication",
            TILEFINCH_COOPERATE_OPTIONAL_PUBLICATION},
        {"optional-image-publication",
            TILEFINCH_COOPERATE_OPTIONAL_PUBLICATION
            | TILEFINCH_COOPERATE_IMAGE_PUBLICATION},
        {"optional-layout-completion",
            TILEFINCH_COOPERATE_LAYOUT_COMPLETION},
        {"optional-layout-extension-awaited",
            TILEFINCH_COOPERATE_LAYOUT_COMPLETION
            | TILEFINCH_COOPERATE_EXTENSION_AWAITED},
        {"render-compose", TILEFINCH_COOPERATE_RENDER_COMPOSE
            | TILEFINCH_COOPERATE_RENDER_SHELL_BORROWED},
        {"render-prepare", TILEFINCH_COOPERATE_RENDER_SHELL_BORROWED},
        {"style-prepare", TILEFINCH_COOPERATE_RENDER_SHELL_BORROWED},
    };
    *known = false;
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++)
        if (strcmp(phase, names[i].phase) == 0) {
            *known = true;
            return names[i].boundary;
        }
    return 0u;
}

/* Every boundary flag, each of which the journey below must reach. */
#define COOPERATE_ALL_BOUNDARIES \
    (TILEFINCH_COOPERATE_PAGE_TASK | TILEFINCH_COOPERATE_RENDER_INPUT \
     | TILEFINCH_COOPERATE_IMAGE_ADMISSION | TILEFINCH_COOPERATE_FOCUS_FEEDBACK \
     | TILEFINCH_COOPERATE_SCROLL_SERVABLE \
     | TILEFINCH_COOPERATE_OPTIONAL_PUBLICATION \
     | TILEFINCH_COOPERATE_IMAGE_PUBLICATION \
     | TILEFINCH_COOPERATE_LAYOUT_COMPLETION \
     | TILEFINCH_COOPERATE_EXTENSION_AWAITED \
     | TILEFINCH_COOPERATE_RENDER_COMPOSE \
     | TILEFINCH_COOPERATE_RENDER_SHELL_BORROWED)

typedef struct {
    unsigned checkpoints;
    unsigned seen;
    unsigned mismatches;
    char mismatch[64];
    bool rendering;
    unsigned raster_decodes;
    unsigned raster_decodes_borrowed;
    char unborrowed_decode[48];
} BoundaryJourneyProbe;

static bool boundary_journey_probe(void *opaque, const char *phase,
                                   size_t completed, unsigned boundary)
{
    (void) completed;
    BoundaryJourneyProbe *probe = opaque;
    probe->checkpoints++;
    probe->seen |= boundary;
    bool known = false;
    unsigned expected = conventional_boundary(phase, &known);
    /* A checkpoint that means something beyond a borrowed render shell
       (which any decoder or layout step under raster may carry) must use
       a name from the table: a renamed or new semantic site fails here
       rather than escaping the name check. */
    bool unknown = !known
        && (boundary & ~TILEFINCH_COOPERATE_RENDER_SHELL_BORROWED) != 0;
    if (((boundary & expected) != expected || unknown)
        && probe->mismatches++ == 0)
        snprintf(probe->mismatch, sizeof(probe->mismatch), "%s 0x%x%s",
                 phase, boundary, unknown ? " (unknown name)" : "");
    bool decode = strstr(phase, "decode") != NULL
        && strcmp(phase, "image-decoder-release") != 0;
    if (probe->rendering && decode) {
        probe->raster_decodes++;
        if ((boundary & TILEFINCH_COOPERATE_RENDER_SHELL_BORROWED) != 0)
            probe->raster_decodes_borrowed++;
        else
            snprintf(probe->unborrowed_decode,
                     sizeof(probe->unborrowed_decode), "%s", phase);
    }
    return true;
}

/* A page with images, deferred image work, frames, a bounded render job
   and focus moves. */
static int boundary_journey_images(BoundaryJourneyProbe *probe, bool scripts)
{
    BrowserDeviceProfile profile;
    browser_device_profile_psp3000(&profile);
    BrowserConfig config;
    browser_config_init(&config, &profile);
    config.memory_limit = 24u * MIB;
    config.javascript.enabled = scripts;
    config.javascript.document_scripts_enabled = scripts;
    config.resources.enabled = true;
    config.resources.web_fonts_enabled = false;
    config.resources.maximum_images = 12;
    char error[256] = {0};
    BrowserEngine *engine = browser_engine_create(&config, error, sizeof(error));
    CHECK(engine != NULL && fetch_trace_replay_begin(
        TILEFINCH_TEST_SOURCE_DIR "/tests/fixtures/http-interactive-images",
        error, sizeof(error)));
    TilefinchPlatformServices services = {
        .context = probe, .cooperate_at = boundary_journey_probe
    };
    tilefinch_platform_set_services(&services);
    CHECK(browser_engine_load_url(
        engine, "https://interactive-images.test/page", true));
    NavigationSession *nav = browser_engine_navigation(engine);
    for (unsigned pump = 0; nav->page.image_continuation_pending && pump < 512; pump++) {
        bool changed = false;
        (void) browser_engine_run_deferred_image_work(engine, &changed);
    }
    for (unsigned turn = 0; turn < 64; turn++) {
        bool changed = false;
        if (!browser_engine_run_idle_work(engine, &changed)) break;
    }
    if (!scripts) {
        /* A scriptless page publishes a loaded visual batch as optional
           work (a provisional page's discovered backgrounds and masks);
           start that publication on these decoded images. */
        CHECK(nav->page.runtime == NULL);
        nav->page.visual_image_publication_pending = true;
        CHECK(navigation_run_background_resources(nav)
                  != NAVIGATION_BACKGROUND_WORK_HARD_FAILURE
              && !nav->page.visual_image_publication_pending);
    }
    probe->rendering = true;
    CHECK(browser_engine_render_frame(engine, NULL));
    probe->rendering = false;
    for (unsigned move = 0; move < 4; move++) {
        (void) browser_engine_focus_move(engine, true);
        probe->rendering = true;
        CHECK(browser_engine_render_frame(engine, NULL));
        probe->rendering = false;
    }
    /* A frame built in bounded slices stops between raster units. */
    CHECK(browser_engine_refresh_render_shell(engine));
    BrowserRenderJobStatus status = BROWSER_RENDER_JOB_PENDING;
    probe->rendering = true;
    for (unsigned i = 0; i < 128u && status == BROWSER_RENDER_JOB_PENDING; i++)
        status = browser_engine_render_frame_bounded(engine, 8000u, 1u);
    probe->rendering = false;
    CHECK(status == BROWSER_RENDER_JOB_COMPLETE);
    tilefinch_platform_set_services(NULL);
    fetch_trace_end();
    CHECK(browser_engine_shutdown(engine));
    browser_engine_destroy(engine);
    return 0;
}

/* A large static page commits provisionally: the rest of its layout is
   completed at idle (awaited when the reader waits at its bottom), and a
   later relayout keeps the committed page servable to scrolling. */
static int boundary_journey_provisional(BoundaryJourneyProbe *probe)
{
    BrowserDeviceProfile profile;
    browser_device_profile_psp3000(&profile);
    BrowserConfig config;
    browser_config_init(&config, &profile);
    config.memory_limit = 16u * MIB;
    config.session_cache_limit = 256u * 1024u;
    config.maximum_document_bytes = 1u * MIB;
    config.tile_capacity = 4;
    config.resources.enabled = true;
    config.javascript.enabled = false;
    char error[256] = {0};
    BrowserEngine *engine = browser_engine_create(&config, error, sizeof(error));
    CHECK(engine != NULL && fetch_trace_replay_begin(
        TILEFINCH_TEST_SOURCE_DIR "/fixtures/http-provisional-commit",
        error, sizeof(error)));
    TilefinchPlatformServices services = {
        .context = probe, .cooperate_at = boundary_journey_probe
    };
    tilefinch_platform_set_services(&services);
    CHECK(browser_engine_begin_navigation_url(
        engine, "https://provisional-commit.test/document",
        256u * 1024u, 1000, true));
    BrowserNavigationJobStatus job = BROWSER_NAVIGATION_JOB_PENDING;
    for (size_t pumps = 0; pumps < 4096 && job == BROWSER_NAVIGATION_JOB_PENDING;
         pumps++)
        job = browser_engine_pump_navigation(engine, NULL);
    NavigationSession *navigation = browser_engine_navigation(engine);
    CHECK(job == BROWSER_NAVIGATION_JOB_SUCCEEDED
          && navigation->page.layout_provisional);
    CHECK(navigation_set_scroll(navigation, navigation->page.layout.height
                                - navigation->viewport.css_height)
          && navigation_layout_completion_awaited(navigation));
    for (size_t turns = 0;
         turns < 128 && navigation_layout_completion_pending(navigation);
         turns++) {
        bool changed = false;
        (void) browser_engine_run_idle_work(engine, &changed);
    }
    CHECK(!navigation->page.layout_provisional);
    uint64_t threshold = navigation->relayout_preview_threshold_us;
    navigation->relayout_preview_threshold_us = 0;
    CHECK(navigation_relayout(navigation));
    navigation->relayout_preview_threshold_us = threshold;
    tilefinch_platform_set_services(NULL);
    fetch_trace_end();
    CHECK(browser_engine_shutdown(engine));
    browser_engine_destroy(engine);
    return 0;
}

/* Page tasks run between task boundaries. */
static int boundary_journey_page_tasks(BoundaryJourneyProbe *probe)
{
    BrowserDeviceProfile profile;
    browser_device_profile_psp3000(&profile);
    BrowserConfig config;
    browser_config_init(&config, &profile);
    config.javascript.enabled = true;
    config.javascript.document_scripts_enabled = true;
    static const char page[] = "<!doctype html><p id=t>wait</p><script>"
        "setTimeout(function () { t.textContent = 'one'; }, 0);"
        "setTimeout(function () { t.textContent = 'two'; }, 0);</script>";
    char error[256] = {0};
    BrowserEngine *engine = browser_engine_create(&config, error, sizeof(error));
    CHECK(engine != NULL && browser_engine_commit_html(
        engine, "https://tasks.test/", page, sizeof(page) - 1u, true));
    TilefinchPlatformServices services = {
        .context = probe, .cooperate_at = boundary_journey_probe
    };
    tilefinch_platform_set_services(&services);
    for (unsigned turn = 0; turn < 4; turn++)
        (void) browser_engine_advance_runtime(engine, 16u, 4u, NULL);
    tilefinch_platform_set_services(NULL);
    CHECK(browser_engine_shutdown(engine));
    browser_engine_destroy(engine);
    return 0;
}

/* Staged optional faces are published at idle. */
static int boundary_journey_fonts(BoundaryJourneyProbe *probe)
{
    BrowserDeviceProfile profile;
    browser_device_profile_psp3000(&profile);
    BrowserConfig config;
    browser_config_init(&config, &profile);
    config.memory_limit = 24u * MIB;
    config.session_cache_limit = 256u * 1024u;
    config.maximum_document_bytes = 64u * 1024u;
    config.tile_capacity = 2;
    static const char *const faces[] = {
        "DejaVuSans-Latin.ttf", "DejaVuSerif-Latin.ttf",
        "DejaVuSans-Oblique-Latin.ttf", "DejaVuSans-Bold-Latin.ttf",
        "DejaVuSerif-Bold-Latin.ttf", "TilefinchSans-Regular.ttf",
        "TilefinchSans-Bold.ttf"
    };
    char paths[7][512];
    for (size_t i = 0; i < 7; i++)
        snprintf(paths[i], sizeof(paths[i]), "%s/fonts/%s",
                 TILEFINCH_TEST_SOURCE_DIR, faces[i]);
    CHECK(browser_config_set_font_paths(
              &config, paths[0], paths[1], paths[2], paths[3], paths[4],
              paths[5], paths[6], 1536u * 1024u));
    browser_config_set_staged_font_loading(&config, true);
    static const char page[] =
        "<!doctype html><p style='font:22px serif'>Serif face</p>"
        "<p style='font:italic 16px sans-serif'>Italic face</p>"
        "<p style='font:bold 16px sans-serif'>Bold face</p>";
    char error[256] = {0};
    BrowserEngine *engine = browser_engine_create(&config, error, sizeof(error));
    CHECK(engine != NULL);
    for (size_t i = 0; i < 4 && !browser_engine_baseline_fonts_ready(engine); i++)
        CHECK(browser_engine_pump_baseline_fonts(engine));
    CHECK(browser_engine_baseline_fonts_ready(engine)
          && browser_engine_commit_html(
              engine, "https://font.test/", page, sizeof(page) - 1u, true));
    TilefinchPlatformServices services = {
        .context = probe, .cooperate_at = boundary_journey_probe
    };
    tilefinch_platform_set_services(&services);
    for (size_t i = 0; i < 64 && (probe->seen
             & TILEFINCH_COOPERATE_OPTIONAL_PUBLICATION) == 0; i++) {
        bool changed = false;
        (void) browser_engine_run_idle_work(engine, &changed);
    }
    tilefinch_platform_set_services(NULL);
    CHECK(browser_engine_shutdown(engine));
    browser_engine_destroy(engine);
    return 0;
}

/* Journeys that together reach every checkpoint the engine flags: each
   carries the flags its name stands for, no flagged checkpoint has a name
   outside the table, and every flag is reached, so a site that drops its
   flags or is renamed fails here even where the name map has no entry. */
int test_cooperate_boundaries_reach_frontend(void)
{
    BoundaryJourneyProbe probe = {0};
    CHECK(boundary_journey_images(&probe, true) == 0);
    unsigned images = probe.seen;
    CHECK(boundary_journey_images(&probe, false) == 0);
    unsigned scriptless = probe.seen & ~images;
    CHECK(boundary_journey_provisional(&probe) == 0);
    CHECK(boundary_journey_page_tasks(&probe) == 0);
    CHECK(boundary_journey_fonts(&probe) == 0);
    fprintf(stderr, "cooperate boundaries: checkpoints=%u seen=0x%x "
            "(images 0x%x, scriptless adds 0x%x) missing=0x%x "
            "raster-decodes=%u borrowed=%u\n", probe.checkpoints, probe.seen,
            images, scriptless, COOPERATE_ALL_BOUNDARIES & ~probe.seen,
            probe.raster_decodes, probe.raster_decodes_borrowed);
    if (probe.mismatches != 0)
        fprintf(stderr, "first mismatch: %s\n", probe.mismatch);
    CHECK(probe.mismatches == 0);
    CHECK((probe.seen & COOPERATE_ALL_BOUNDARIES) == COOPERATE_ALL_BOUNDARIES);
    return 0;
}

static uint64_t boundary_fast_clock_us(void *opaque)
{
    /* Every read is a fresh slice, so decoders reach their checkpoints. */
    uint64_t *now = opaque;
    *now += UINT64_C(10000);
    return *now;
}

static uint64_t boundary_fast_clock_ns(void *opaque)
{
    return boundary_fast_clock_us(opaque) * UINT64_C(1000);
}

typedef struct {
    uint64_t now_us;
    BoundaryJourneyProbe probe;
} BoundaryRasterProbe;

static bool boundary_raster_probe(void *opaque, const char *phase,
                                  size_t completed, unsigned boundary)
{
    return boundary_journey_probe(
        &((BoundaryRasterProbe *) opaque)->probe, phase, completed, boundary);
}

/* Image decoders cooperate under their own names ("svg-decode",
   "image-jpeg-decode"...). When raster decodes an image it still holds
   encoded, the render shell is borrowed: a frontend must not reenter it
   from that checkpoint, so the checkpoint carries RENDER_SHELL_BORROWED
   whatever it is called. */
int test_raster_decode_checkpoints_borrow_render_shell(void)
{
    char svg[8192];
    size_t used = (size_t) snprintf(svg, sizeof(svg),
        "<svg xmlns='http://www.w3.org/2000/svg' width='64' height='64'>");
    for (unsigned i = 0; i < 96 && used < sizeof(svg) - 128; i++)
        used += (size_t) snprintf(svg + used, sizeof(svg) - used,
            "<path d='M%u %u l3 0 l0 3 l-3 0 z' fill='#%02x%02x40'/>",
            (i * 7u) % 60u, (i * 5u) % 60u, i * 2u, 255u - i * 2u);
    used += (size_t) snprintf(svg + used, sizeof(svg) - used, "</svg>");
    if (used >= sizeof(svg)) return 1;
    Budget budget;
    budget_init(&budget, 8u * MIB);
    ImageResource image = {
        .encoded = (unsigned char *) svg, .encoded_length = used,
        .source_width = 64, .source_height = 64, .width = 64, .height = 64,
        .retarget_flags = IMAGE_RETARGET_VECTOR
    };
    DrawCommand commands[] = {
        {.image = &image, .x = 0, .y = 0, .width = 32, .height = 32,
         .opacity_scale = 256, .type = DRAW_IMAGE,
         .image_fit = LAYOUT_IMAGE_FIT_STRETCH}
    };
    LayoutDocument layout = {
        .budget = &budget, .commands = commands, .count = 1,
        .width = 32, .scroll_width = 32, .height = 32,
        .page_background = 0xffffff
    };
    BoundaryRasterProbe raster = { .now_us = UINT64_C(1000000) };
    TilefinchPlatformServices services = {
        .context = &raster, .cooperate_at = boundary_raster_probe,
        .monotonic_time_us = boundary_fast_clock_us,
        .monotonic_time_ns = boundary_fast_clock_ns
    };
    TileCache cache;
    uint16_t *frame = budget_malloc(&budget, 32u * 32u * sizeof(*frame));
    CHECK(frame != NULL && tile_cache_init(&cache, &budget, &layout, 1)
        && tile_cache_set_frame(&cache, frame, 32u * 32u));
    tilefinch_platform_set_services(&services);
    raster.probe.rendering = true;
    bool rendered = tile_cache_render_frame(&cache, 0, 32, 32, NULL);
    raster.probe.rendering = false;
    tilefinch_platform_set_services(NULL);
    size_t decodes = cache.decoded_image_builds + cache.scaled_image_builds;
    tile_cache_destroy(&cache);
    budget_free(&budget, frame);
    fprintf(stderr, "raster decodes=%zu checkpoints=%u borrowed=%u%s%s\n",
            decodes, raster.probe.raster_decodes,
            raster.probe.raster_decodes_borrowed,
            raster.probe.unborrowed_decode[0] != '\0'
                ? " first-unborrowed=" : "",
            raster.probe.unborrowed_decode);
    CHECK(rendered && decodes != 0);
    CHECK(raster.probe.raster_decodes != 0);
    CHECK(raster.probe.raster_decodes_borrowed == raster.probe.raster_decodes);
    CHECK(budget.current == 0);
    return 0;
}
