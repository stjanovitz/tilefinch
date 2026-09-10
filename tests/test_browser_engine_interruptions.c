#include "browser_engine_test_support.h"
#include "tilefinch/psp_ui.h"

/* One owner, immutable incumbent pixels. This exercises the actual chrome
   receiver/compositor, not PSP scheduling or physical scanout timing. */
typedef struct {
    PspUiState ui;
    uint16_t *base, *before, *presented;
    unsigned input, checkpoints, request_checkpoint;
    bool font_work, publication, received, handled, visible;
    uint64_t requested_us, acknowledged_us, visible_us, returned_us;
    uint64_t last_us, max_gap_us;
    char gap_phase[48];
} InterruptionProbe;

static uint64_t interruption_retry_clock(void *opaque)
{
    return *(uint64_t *) opaque;
}

static bool interruption_checkpoint(void *opaque, const char *phase, size_t work)
{
    InterruptionProbe *p = opaque;
    uint64_t now = tilefinch_platform_monotonic_time_us();
    if (p->last_us && now >= p->last_us && now - p->last_us > p->max_gap_us) {
        p->max_gap_us = now - p->last_us;
        snprintf(p->gap_phase, sizeof(p->gap_phase), "%s", phase);
    }
    p->last_us = now;
    if (strcmp(phase, "optional-font-publication") == 0)
        p->publication = work != 0;
    if (p->received || (p->font_work && !p->publication)
        || strncmp(phase, "layout", 6) != 0) return true;
    if (++p->checkpoints < p->request_checkpoint) return true;
    if (!p->requested_us) {
        /* Input arrives just after a safe point, not at its receiver. */
        p->requested_us = now;
        return true;
    }
    p->received = true;
    PspUiInput input = { .analog_x = 128, .analog_y = 128, .elapsed_ms = 16 };
    if (p->input == 0) input.pressed = PSP_UI_BUTTON_MENU;
    else if (p->input == 1) input.analog_x = 255;
    else if (p->input == 2) {
        PspUiToolbarInputState toolbar = {0};
        input.pressed = input.held = PSP_UI_BUTTON_TOOLBAR;
        (void) psp_ui_filter_toolbar_input(&toolbar, &input, true, 1000);
        (void) psp_ui_update_priority(&p->ui, &input);
        input.pressed = input.held = 0;
        (void) psp_ui_filter_toolbar_input(&toolbar, &input, true, 1100);
    }
    if (p->input == 3) {
        /* Scroll is queued until the owner releases its borrowed document.
           Queue admission is not a claim that scroll pixels are visible. */
        p->handled = true;
    } else p->handled = psp_ui_update_priority(&p->ui, &input);
    p->acknowledged_us = tilefinch_platform_monotonic_time_us();
    memcpy(p->presented, p->base, 480u * 272u * sizeof(uint16_t));
    psp_ui_composite(&p->ui, p->presented, 480, 272, 480);
    p->visible = memcmp(p->before, p->presented, 480u * 272u * sizeof(uint16_t)) != 0;
    p->visible_us = tilefinch_platform_monotonic_time_us();
    /* Match the existing optional-font transaction's input-yield contract.
       Required section/image work continues; native chrome is already shown. */
    return !p->font_work;
}

int test_background_interruption_journey(void)
{
    static uint16_t base[480u * 272u], before[480u * 272u], shown[480u * 272u];
    static char html[32768];
    size_t used = (size_t) snprintf(html, sizeof(html),
        "<!doctype html><style>body{margin:8px}p{font:700 18px serif;height:32px}"
        "summary,a{display:block;height:36px}</style><body>"
        "<a href='/next' style='font:700 18px serif'>Next article</a><details id=section open>"
        "<summary id=toggle>History</summary>");
    for (unsigned row = 0; row < 400; row++) {
        int n = snprintf(html + used, sizeof(html) - used,
            "<p>Article history row %u with readable text.</p>", row);
        CHECK(n > 0 && (size_t) n < sizeof(html) - used);
        used += (size_t) n;
    }
    used += (size_t) snprintf(html + used, sizeof(html) - used, "</details><p>End of article.</p></body>");
    for (unsigned work = 0; work < 3; work++) {
        for (unsigned input = 0; input < 4; input++) {
            BrowserDeviceProfile profile;
            browser_device_profile_psp3000(&profile);
            BrowserConfig config;
            browser_config_init(&config, &profile);
            config.memory_limit = 24u * MIB;
            config.javascript.enabled = work == 2;
            config.javascript.document_scripts_enabled = work == 2;
            config.resources.enabled = work == 2;
            config.resources.web_fonts_enabled = false;
            /* Exercise the cancellable transaction even above the default
               optional-publication policy threshold. */
            config.fonts.maximum_publication_work_units = 0;
            CHECK(browser_config_set_font_paths(&config,
                TILEFINCH_TEST_SOURCE_DIR "/fonts/DejaVuSans-Latin.ttf",
                TILEFINCH_TEST_SOURCE_DIR "/fonts/DejaVuSerif-Latin.ttf",
                TILEFINCH_TEST_SOURCE_DIR "/fonts/DejaVuSans-Oblique-Latin.ttf",
                TILEFINCH_TEST_SOURCE_DIR "/fonts/DejaVuSans-Bold-Latin.ttf",
                TILEFINCH_TEST_SOURCE_DIR "/fonts/DejaVuSerif-Bold-Latin.ttf",
                TILEFINCH_TEST_SOURCE_DIR "/fonts/TilefinchSans-Regular.ttf",
                TILEFINCH_TEST_SOURCE_DIR "/fonts/TilefinchSans-Bold.ttf", 1536u * 1024u));
            browser_config_set_staged_font_loading(&config, true);
            char error[256] = {0};
            BrowserEngine *engine = browser_engine_create(&config, error, sizeof(error));
            CHECK(engine != NULL);
            for (unsigned i = 0; i < 8 && !browser_engine_baseline_fonts_ready(engine); i++)
                CHECK(browser_engine_pump_baseline_fonts(engine));
            CHECK(browser_engine_baseline_fonts_ready(engine));
            if (work == 2) {
                CHECK(fetch_trace_replay_begin(TILEFINCH_TEST_SOURCE_DIR
                    "/tests/fixtures/http-interactive-images", error, sizeof(error))
                    && browser_engine_load_url(engine, "https://interactive-images.test/page", true));
            } else CHECK(browser_engine_commit_html(engine,
                "https://interruption.test/article", html, used, true));
            NavigationSession *nav = browser_engine_navigation(engine);
            lxb_dom_node_t *root = lxb_dom_interface_node(nav->page.document.html);
            ControllerAction action = {0};
            if (work == 1) CHECK(browser_engine_focus_node(engine, test_reader_find_id(root, "toggle"))
                && browser_engine_activate(engine, &action));
            if (work == 1) CHECK(nav->page.layout.height < 1000);
            CHECK(browser_engine_focus_move(engine, true) && browser_engine_render_frame(engine, NULL));
            size_t pixels = 0;
            memcpy(base, browser_engine_framebuffer(engine, &pixels), sizeof(base));
            CHECK(pixels == 480u * 272u);
            InterruptionProbe probe = { .base = base, .before = before, .presented = shown,
                .input = input, .request_checkpoint = work == 2 ? 1 : 3 + 16 * input,
                .font_work = work == 0 };
            psp_ui_init(&probe.ui);
            psp_ui_set_page(&probe.ui, "Article", "https://interruption.test/article", true);
            probe.ui.chrome_visible = true;
            probe.ui.analog_cursor_enabled = true;
            /* Compare with the same chrome before input, not a bare page. */
            memcpy(before, base, sizeof(base));
            psp_ui_composite(&probe.ui, before, 480, 272, 480);
            size_t fonts_before = font_set_loaded_bytes(browser_engine_fonts(engine));
            size_t relayouts = nav->incremental_relayouts;
            TilefinchPlatformServices services = { .context = &probe, .cooperate = interruption_checkpoint };
            tilefinch_platform_set_services(&services);
            probe.last_us = tilefinch_platform_monotonic_time_us();
            if (work == 1) {
                CHECK(browser_engine_focus_node(engine, test_reader_find_id(root, "toggle"))
                    && browser_engine_activate(engine, &action));
                CHECK(nav->page.layout.height > 10000);
            } else {
                for (unsigned pump = 0; pump < 512 && !probe.received; pump++) {
                    bool changed = false;
                    if (work == 0) (void) browser_engine_run_idle_work(engine, &changed);
                    else (void) browser_engine_run_deferred_image_work(engine, &changed);
                    (void) interruption_checkpoint(&probe, "pump-return", 0);
                }
            }
            probe.returned_us = tilefinch_platform_monotonic_time_us();
            (void) interruption_checkpoint(&probe, "owner-return", 0);
            tilefinch_platform_set_services(NULL);
            if (!probe.received || !probe.handled || (!probe.visible && input != 3))
                fprintf(stderr, "interruption work=%u input=%u received=%d handled=%d visible=%d checkpoints=%u publication=%d\n",
                    work, input, probe.received, probe.handled, probe.visible, probe.checkpoints, probe.publication);
            CHECK(probe.received && probe.handled && (probe.visible || input == 3) && nav->page.loaded);
            if (work == 0) CHECK(font_set_loaded_bytes(browser_engine_fonts(engine)) == fonts_before
                && nav->incremental_relayouts == relayouts && nav->layout_build_cancelled);
            if (input == 0) {
                CHECK(probe.ui.screen == PSP_UI_SCREEN_MENU);
                PspUiInput down = { .pressed = PSP_UI_BUTTON_DOWN, .analog_x = 128, .analog_y = 128 };
                CHECK(psp_ui_update_priority(&probe.ui, &down) && probe.ui.menu_selection == 1);
            }
            if (input == 3) {
                CHECK(browser_engine_scroll_by(engine, 100));
            }
            CHECK(browser_engine_render_frame(engine, NULL));
            if (input == 3) {
                CHECK(memcmp(base, browser_engine_framebuffer(engine, NULL), sizeof(base)) != 0);
                probe.visible_us = tilefinch_platform_monotonic_time_us();
            }
            printf("background-interruption work=%s input=%u acknowledgement-us=%llu "
                "visible-us=%llu owner-return-us=%llu max-gap-us=%llu phase=%s\n",
                work == 0 ? "font" : work == 1 ? "section" : "image", input,
                (unsigned long long)(probe.acknowledged_us - probe.requested_us),
                (unsigned long long)(probe.visible_us - probe.requested_us),
                (unsigned long long)(probe.returned_us - probe.requested_us),
                (unsigned long long)probe.max_gap_us, probe.gap_phase);
            if (work == 0) {
                bool published = false;
                uint64_t retry_now = tilefinch_platform_monotonic_time_us() + UINT64_C(500000);
                TilefinchPlatformServices retry = { .context = &retry_now,
                    .monotonic_time_us = interruption_retry_clock };
                tilefinch_platform_set_services(&retry);
                for (unsigned pump = 0; pump < 512 && !published; pump++) {
                    bool changed = false;
                    (void) browser_engine_run_idle_work(engine, &changed);
                    published = changed;
                }
                tilefinch_platform_set_services(NULL);
                CHECK(published && nav->page.loaded);
            }
            fetch_trace_end();
            CHECK(browser_engine_shutdown(engine));
            BrowserEngineMetrics final = {0};
            CHECK(browser_engine_metrics(engine, &final) && final.budget_current == 0
                && final.budget_active_allocations == 0);
            browser_engine_destroy(engine);
        }
    }
    return 0;
}
