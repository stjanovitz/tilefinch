#include "tilefinch/budget.h"
#include "tilefinch/fetch.h"
#include "tilefinch/layout.h"
#include "tilefinch/navigation.h"
#include "tilefinch/render.h"
#include "tilefinch/script_loader.h"
#include "tilefinch/section_store.h"
#include "tilefinch/style.h"
#include "tilefinch/user_agent.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#ifndef TILEFINCH_TEST_SOURCE_DIR
#define TILEFINCH_TEST_SOURCE_DIR "."
#endif

#define MIB (1024u * 1024u)

static bool replay_begin(void)
{
    char error[256] = {0};
    return fetch_trace_replay_begin(
        TILEFINCH_TEST_SOURCE_DIR "/fixtures/http-stream", error,
        sizeof(error));
}

static bool progressive_preview_replay_begin(void)
{
    char error[256] = {0};
    return fetch_trace_replay_begin(
        TILEFINCH_TEST_SOURCE_DIR "/fixtures/http-progressive-preview",
        error, sizeof(error));
}

static bool progressive_visual_readiness_replay_begin(void)
{
    char error[256] = {0};
    return fetch_trace_replay_begin(
        TILEFINCH_TEST_SOURCE_DIR
            "/fixtures/http-progressive-visual-readiness",
        error, sizeof(error));
}

static bool stylesheet_cache_nonce_replay_begin(bool allowed)
{
    char error[256] = {0};
    return fetch_trace_replay_begin(
        allowed
            ? TILEFINCH_TEST_SOURCE_DIR
                "/fixtures/http-stylesheet-cache-nonce-good"
            : TILEFINCH_TEST_SOURCE_DIR
                "/fixtures/http-stylesheet-cache-nonce-bad",
        error, sizeof(error));
}

static bool stylesheet_cache_metadata_replay_begin(unsigned variant)
{
    const char *directory = variant == 1
        ? "/fixtures/http-stylesheet-cache-metadata-integrity"
        : (variant == 2
              ? "/fixtures/http-stylesheet-cache-metadata-crossorigin"
              : "/fixtures/http-stylesheet-cache-metadata-base");
    char path[512], error[256] = {0};
    int length = snprintf(path, sizeof(path), "%s%s",
                          TILEFINCH_TEST_SOURCE_DIR, directory);
    return length > 0 && (size_t) length < sizeof(path)
        && fetch_trace_replay_begin(path, error, sizeof(error));
}

static bool background_image_continuation_replay_begin(void)
{
    char error[256] = {0};
    return fetch_trace_replay_begin(
        TILEFINCH_TEST_SOURCE_DIR
            "/fixtures/http-background-image-continuation",
        error, sizeof(error));
}

static bool deferred_document_images_replay_begin(void)
{
    char error[256] = {0};
    return fetch_trace_replay_begin(
        TILEFINCH_TEST_SOURCE_DIR
            "/fixtures/http-deferred-document-images",
        error, sizeof(error));
}

static bool streaming_preview_replay_begin(void)
{
    char error[256] = {0};
    return fetch_trace_replay_begin(
        TILEFINCH_TEST_SOURCE_DIR "/fixtures/http-stream-preview",
        error, sizeof(error));
}

static bool streaming_preview_empty_replay_begin(void)
{
    char error[256] = {0};
    return fetch_trace_replay_begin(
        TILEFINCH_TEST_SOURCE_DIR "/fixtures/http-stream-preview-empty",
        error, sizeof(error));
}

static bool resumable_layout_replay_begin(void)
{
    char error[256] = {0};
    return fetch_trace_replay_begin(
        TILEFINCH_TEST_SOURCE_DIR "/fixtures/http-resumable-layout",
        error, sizeof(error));
}

static bool streaming_preview_css_failure_replay_begin(void)
{
    char error[256] = {0};
    return fetch_trace_replay_begin(
        TILEFINCH_TEST_SOURCE_DIR
            "/fixtures/http-stream-preview-css-failure",
        error, sizeof(error));
}

static bool csp_gate_replay_begin(void)
{
    char error[256] = {0};
    return fetch_trace_replay_begin(
        TILEFINCH_TEST_SOURCE_DIR "/fixtures/http-csp-gate",
        error, sizeof(error));
}

static bool frame_policy_replay_begin(void)
{
    char error[256] = {0};
    return fetch_trace_replay_begin(
        TILEFINCH_TEST_SOURCE_DIR "/fixtures/http-frame-policy",
        error, sizeof(error));
}

typedef struct {
    size_t calls;
    size_t rollbacks;
    size_t boxes;
    bool candidate_loaded;
    bool tail_visible;
    bool unresolved_external_visuals;
    bool painted_image;
    bool script_ran_at_paint;
    bool fail_after_paint;
} ProgressivePreviewProbe;

static bool capture_progressive_preview(
    void *opaque, NavigationSession *candidate,
    const LayoutDocument *layout)
{
    ProgressivePreviewProbe *probe = opaque;
    if (probe == NULL || candidate == NULL) return false;
    if (layout == NULL) {
        probe->rollbacks++;
        return true;
    }
    probe->calls++;
    probe->boxes = layout->count;
    probe->candidate_loaded = candidate->page.loaded;
    probe->unresolved_external_visuals =
        layout->unresolved_external_visuals;
    for (size_t i = 0; i < layout->count; i++) {
        if (layout->commands[i].type == DRAW_IMAGE) {
            probe->painted_image = true;
        }
    }
    for (size_t i = 0; i < layout->node_box_count; i++) {
        size_t length = 0;
        if (document_attribute(
                layout->node_boxes[i].node, "data-script-ran", &length)
            != NULL) {
            probe->script_ran_at_paint = true;
        }
        const char *id = document_attribute(
            layout->node_boxes[i].node, "id", &length);
        if (id != NULL && length == 4 && memcmp(id, "tail", 4) == 0) {
            probe->tail_visible = true;
        }
    }
    if (probe->fail_after_paint)
        budget_inject_failure_after(candidate->budget, 0);
    return layout->count != 0;
}

static bool finish_bounded(NavigationLoad *load,
                           const NavigationLoadQuota *quota);

static bool test_progressive_preview_is_bounded_and_transient(void)
{
    Budget budget;
    budget_init(&budget, 16 * MIB);
    bool lexbor_installed = budget_install_lexbor(&budget);
    NavigationSession navigation = {0};
    ProgressivePreviewProbe probe = {0};
    bool ready = lexbor_installed
        && navigation_init(&navigation, &budget, 4)
        && navigation_set_progressive_paint_hook(
               &navigation, capture_progressive_preview, &probe)
        && progressive_preview_replay_begin();
    if (ready) {
        navigation_enable_scripts(&navigation, 2 * MIB, 1000);
        navigation_enable_document_scripts(
            &navigation, 4, 32 * 1024, 16 * 1024, 1000);
        navigation_enable_external_resources(
            &navigation, 2, 32 * 1024, 16 * 1024,
            2, 32 * 1024, 16 * 1024, 64 * 1024, 1000);
    }
    uint64_t generation = ready ? navigation_begin(&navigation) : 0;
    bool loaded = ready && navigation_load_url(
        &navigation, generation, "https://progressive.test/document",
        4096, 1000, 480, NULL, NULL, true);
    bool ok = loaded
        && probe.calls == 1 && probe.boxes != 0 && !probe.candidate_loaded
        && navigation.performance.progressive_layout_attempts == 1
        && navigation.performance.progressive_layout_skips == 0
        && navigation.performance.progressive_layout_failures == 0
        /* The image-incomplete preview is always transient, even when its
           bounded DOM prefix happens to cover this small fixture. The
           authoritative pass must still materialize the inline SVG. */
        && navigation.performance.partial_layouts == 1
        && navigation.performance.partial_paints == 1
        /* Inline SVG remains authoritative-only; the direct priority list
           accepts visible HTML image sources without guessing inherited CSS
           or consuming resource quota out of document order. */
        && navigation.performance.progressive_image_priority_nodes == 0
        && navigation.performance.progressive_image_priority_loaded == 0
        && navigation.page.runtime == NULL
        && probe.boxes < navigation.page.layout.count
        && navigation.performance.first_layout_us != 0
        && navigation.performance.first_paint_us != 0
        && navigation.page.loaded
        && strcmp(navigation.page.document.title,
                  "Progressive preview") == 0;
    if (ready) fetch_trace_end();
    if (lexbor_installed) navigation_destroy(&navigation);
    bool clean = budget.current == 0
        && budget_active_allocations(&budget, NULL) == 0
        && budget_categories_reconcile(&budget);
    if (lexbor_installed) clean = budget_uninstall_lexbor(&budget) && clean;
    return ok && clean;
}

static bool test_failed_candidate_rolls_back_presented_preview(void)
{
    Budget budget;
    budget_init(&budget, 16 * MIB);
    bool installed = budget_install_lexbor(&budget);
    NavigationSession navigation = {0};
    ProgressivePreviewProbe probe = {.fail_after_paint = true};
    bool ready = installed
        && navigation_init(&navigation, &budget, 4)
        && navigation_set_progressive_paint_hook(
               &navigation, capture_progressive_preview, &probe)
        && progressive_preview_replay_begin();
    if (ready) {
        navigation_enable_external_resources(
            &navigation, 2, 32 * 1024, 16 * 1024,
            2, 32 * 1024, 16 * 1024, 64 * 1024, 1000);
    }
    uint64_t generation = ready ? navigation_begin(&navigation) : 0;
    bool loaded = ready && navigation_load_url(
        &navigation, generation, "https://progressive.test/document",
        4096, 1000, 480, NULL, NULL, true);
    budget_clear_failure_injection(&budget);
    bool ok = !loaded && probe.calls == 1 && probe.rollbacks == 1
        && !navigation.page.loaded;
    if (!ok) {
        fprintf(
            stderr,
            "preview-rollback loaded=%d calls=%zu rollbacks=%zu "
            "partial=%zu recorded-rollbacks=%zu page=%d error=\"%s\"\n",
            loaded, probe.calls, probe.rollbacks,
            navigation.performance.partial_paints,
            navigation.performance.progressive_paint_rollbacks,
            navigation.page.loaded, navigation.last_error);
    }
    if (ready) fetch_trace_end();
    if (installed) navigation_destroy(&navigation);
    bool clean = budget.current == 0
        && budget_active_allocations(&budget, NULL) == 0
        && budget_categories_reconcile(&budget);
    if (installed) clean = budget_uninstall_lexbor(&budget) && clean;
    return ok && clean;
}

static bool test_progressive_preview_waits_for_external_visuals(void)
{
    Budget budget;
    budget_init(&budget, 16 * MIB);
    bool lexbor_installed = budget_install_lexbor(&budget);
    NavigationSession navigation = {0};
    ProgressivePreviewProbe probe = {0};
    bool ready = lexbor_installed
        && navigation_init(&navigation, &budget, 4)
        && navigation_set_progressive_paint_hook(
               &navigation, capture_progressive_preview, &probe)
        && progressive_visual_readiness_replay_begin();
    if (ready) {
        navigation_enable_scripts(&navigation, 2 * MIB, 1000);
        navigation_enable_document_scripts(
            &navigation, 4, 32 * 1024, 16 * 1024, 1000);
        navigation_enable_external_resources(
            &navigation, 2, 32 * 1024, 16 * 1024,
            2, 32 * 1024, 16 * 1024, 64 * 1024, 1000);
    }
    uint64_t generation = ready ? navigation_begin(&navigation) : 0;
    bool loaded = ready && navigation_load_url(
        &navigation, generation,
        "https://visual-readiness.test/document",
        4096, 1000, 480, NULL, NULL, true);
    bool ok = loaded
        && probe.calls == 1 && probe.boxes != 0
        && !probe.candidate_loaded
        && !probe.unresolved_external_visuals
        && probe.painted_image
        && navigation.performance.progressive_layout_attempts == 2
        && navigation.performance.progressive_layout_adoptions == 1
        && navigation.performance.progressive_visual_readiness_skips == 1
        && navigation.performance.progressive_image_priority_nodes == 1
        && navigation.performance.progressive_image_priority_loaded == 1
        && navigation.performance.partial_paints == 1
        && navigation.page.images.stats.loaded == 1
        && navigation.page.runtime == NULL
        && navigation.page.loaded
        && strcmp(navigation.page.document.title,
                  "Visual readiness") == 0;
    if (!ok) {
        fprintf(stderr,
                "visual-readiness loaded=%d calls=%zu boxes=%zu "
                "candidate-loaded=%d unresolved=%d image=%d attempts=%zu "
                "adoptions=%zu skips=%zu partial=%zu images=%zu "
                "error=\"%s\"\n",
                loaded, probe.calls, probe.boxes, probe.candidate_loaded,
                probe.unresolved_external_visuals, probe.painted_image,
                navigation.performance.progressive_layout_attempts,
                navigation.performance.progressive_layout_adoptions,
                navigation.performance.progressive_visual_readiness_skips,
                navigation.performance.partial_paints,
                navigation.page.images.stats.loaded,
                navigation.last_error);
    }
    if (ready) fetch_trace_end();
    if (lexbor_installed) navigation_destroy(&navigation);
    bool clean = budget.current == 0
        && budget_active_allocations(&budget, NULL) == 0
        && budget_categories_reconcile(&budget);
    if (lexbor_installed) clean = budget_uninstall_lexbor(&budget) && clean;
    return ok && clean;
}

static bool test_markup_priority_survives_preview_pressure(void)
{
    Budget budget;
    budget_init(&budget, 16 * MIB);
    bool installed = budget_install_lexbor(&budget);
    NavigationSession navigation = {0};
    bool ready = installed && navigation_init(&navigation, &budget, 4)
        && background_image_continuation_replay_begin();
    if (ready) {
        navigation_enable_external_resources(
            &navigation, 2, 32 * 1024, 16 * 1024,
            2, 32 * 1024, 16 * 1024, 64 * 1024, 1000);
        (void) setenv("TILEFINCH_DISABLE_BOUNDED_LAYOUT_PREVIEW", "1", 1);
    }
    uint64_t generation = ready ? navigation_begin(&navigation) : 0;
    bool loaded = ready && navigation_load_url(
        &navigation, generation,
        "https://background-images.test/document",
        4096, 1000, 480, NULL, NULL, true);
    bool ok = loaded && navigation.page.loaded
        && navigation.performance.progressive_layout_attempts == 0
        && navigation.performance.markup_image_priority_nodes == 2
        && navigation.performance.progressive_image_priority_loaded == 2
        && navigation.page.images.stats.loaded == 2;
    if (!ok) {
        fprintf(stderr,
                "markup-priority loaded=%d page=%d attempts=%zu markup=%zu "
                "priority-loaded=%zu images=%zu error=\"%s\"\n",
                loaded, navigation.page.loaded,
                navigation.performance.progressive_layout_attempts,
                navigation.performance.markup_image_priority_nodes,
                navigation.performance.progressive_image_priority_loaded,
                navigation.page.images.stats.loaded, navigation.last_error);
    }
    (void) unsetenv("TILEFINCH_DISABLE_BOUNDED_LAYOUT_PREVIEW");
    if (ready) fetch_trace_end();
    if (installed) navigation_destroy(&navigation);
    bool clean = budget.current == 0
        && budget_active_allocations(&budget, NULL) == 0
        && budget_categories_reconcile(&budget);
    if (installed) clean = budget_uninstall_lexbor(&budget) && clean;
    return ok && clean;
}

static bool test_background_images_continue_after_useful_paint(void)
{
    Budget budget;
    budget_init(&budget, 16 * MIB);
    bool lexbor_installed = budget_install_lexbor(&budget);
    NavigationSession navigation = {0};
    ProgressivePreviewProbe probe = {0};
    bool ready = lexbor_installed
        && navigation_init(&navigation, &budget, 4)
        && navigation_set_progressive_paint_hook(
               &navigation, capture_progressive_preview, &probe)
        && background_image_continuation_replay_begin();
    if (ready) {
        navigation_enable_external_resources(
            &navigation, 2, 32 * 1024, 16 * 1024,
            2, 32 * 1024, 16 * 1024, 64 * 1024, 1000);
        (void) setenv("TILEFINCH_EXPERIMENTAL_BACKGROUND_IMAGES", "1", 1);
    }
    uint64_t generation = ready ? navigation_begin(&navigation) : 0;
    bool loaded = ready && navigation_load_url(
        &navigation, generation,
        "https://background-images.test/document",
        4096, 1000, 480, NULL, NULL, true);
    bool first_frame = loaded && probe.calls == 1 && probe.painted_image
        && navigation.page.images.stats.loaded == 1
        && navigation_background_resources_pending(&navigation);
    bool continued = first_frame
        && navigation_run_background_resources(&navigation);
    bool ok = continued
        && !navigation_background_resources_pending(&navigation)
        && navigation.page.images.stats.loaded == 2
        && navigation.performance.background_image_batches == 1
        && navigation.performance.background_images_loaded == 1
        && navigation.performance.background_image_relayouts == 1
        && navigation.performance.background_image_failures == 0
        && navigation.page.loaded;
    (void) unsetenv("TILEFINCH_EXPERIMENTAL_BACKGROUND_IMAGES");
    if (ready) fetch_trace_end();
    if (lexbor_installed) navigation_destroy(&navigation);
    bool clean = budget.current == 0
        && budget_active_allocations(&budget, NULL) == 0
        && budget_categories_reconcile(&budget);
    if (lexbor_installed) clean = budget_uninstall_lexbor(&budget) && clean;
    return ok && clean;
}

static bool test_background_image_failure_retains_visible_prefix(void)
{
    Budget budget;
    budget_init(&budget, 16 * MIB);
    bool lexbor_installed = budget_install_lexbor(&budget);
    NavigationSession navigation = {0};
    ProgressivePreviewProbe probe = {0};
    bool ready = lexbor_installed
        && navigation_init(&navigation, &budget, 4)
        && navigation_set_progressive_paint_hook(
               &navigation, capture_progressive_preview, &probe)
        && background_image_continuation_replay_begin();
    if (ready) {
        navigation_enable_external_resources(
            &navigation, 2, 32 * 1024, 16 * 1024,
            2, 32 * 1024, 16 * 1024, 64 * 1024, 1000);
        (void) setenv("TILEFINCH_EXPERIMENTAL_BACKGROUND_IMAGES", "1", 1);
    }
    uint64_t generation = ready ? navigation_begin(&navigation) : 0;
    bool loaded = ready && navigation_load_url(
        &navigation, generation,
        "https://background-images.test/document",
        4096, 1000, 480, NULL, NULL, true);
    bool first_frame = loaded
        && navigation_background_resources_pending(&navigation)
        && navigation.page.images.stats.loaded == 1;
    if (first_frame) budget_inject_failure_after(&budget, 0);
    bool continued = first_frame
        && navigation_run_background_resources(&navigation);
    budget_clear_failure_injection(&budget);
    bool ok = first_frame && continued && navigation.page.loaded
        && !navigation_background_resources_pending(&navigation)
        && navigation.performance.background_image_failures == 1
        && navigation.performance.background_image_relayouts == 0
        && navigation.page.images.count == 1
        && navigation.page.images.stats.loaded == 1
        && navigation.page.images.stats.priority_retained_on_failure == 1;
    if (!ok) {
        fprintf(stderr,
                "background-failure ready=%d loaded=%d first=%d "
                "continued=%d page-loaded=%d pending=%d failures=%zu "
                "images=%zu/%zu budget=%zu error=\"%s\"\n",
                ready, loaded, first_frame, continued,
                navigation.page.loaded,
                navigation_background_resources_pending(&navigation),
                navigation.performance.background_image_failures,
                navigation.page.images.stats.loaded,
                navigation.page.images.count, budget.current,
                navigation.last_error);
    }
    (void) unsetenv("TILEFINCH_EXPERIMENTAL_BACKGROUND_IMAGES");
    if (ready) fetch_trace_end();
    if (lexbor_installed) navigation_destroy(&navigation);
    bool clean = budget.current == 0
        && budget_active_allocations(&budget, NULL) == 0
        && budget_categories_reconcile(&budget);
    if (lexbor_installed) clean = budget_uninstall_lexbor(&budget) && clean;
    return ok && clean;
}

static bool test_static_offscreen_images_are_pumped(void)
{
    static const char html[] =
        "<!doctype html><title>Deferred images</title>"
        "<style>html,body{margin:0}img{display:block;width:24px;height:24px}"
        "#tail{margin-top:600px}</style>"
        "<body><img id=hero src=/hero.svg>"
        "<img id=tail src=/tail.svg></body>";
    Budget budget;
    budget_init(&budget, 16 * MIB);
    bool installed = budget_install_lexbor(&budget);
    NavigationSession navigation = {0};
    bool ready = installed && navigation_init(&navigation, &budget, 4)
        && deferred_document_images_replay_begin();
    if (ready) {
        navigation_enable_external_resources(
            &navigation, 2, 32 * 1024, 16 * 1024,
            2, 32 * 1024, 16 * 1024, 64 * 1024, 1000);
    }
    uint64_t generation = ready ? navigation_begin(&navigation) : 0;
    bool committed = ready && navigation_commit_static_html(
        &navigation, generation, "https://deferred-images.test/page",
        html, sizeof(html) - 1u, 480, NULL, NULL, true);
    size_t relayouts_before = navigation.performance.fast_relayouts
        + navigation.performance.full_relayouts;
    bool first_frame = committed && navigation.page.loaded
        && navigation.page.images.stats.loaded == 1
        && navigation.page.deferred_image_count == 1
        && navigation.page.deferred_image_job == NULL
        && navigation_background_resources_pending(&navigation);
    bool admitted = first_frame
        && navigation_run_background_resources(&navigation);
    bool yielded_to_owner = admitted
        && navigation.page.images.stats.loaded == 1
        && navigation.page.deferred_image_job != NULL
        && fetch_scheduler_pending(navigation.page.resource_scheduler) == 1
        && navigation.performance.background_image_batches == 1
        && navigation.performance.fast_relayouts
               + navigation.performance.full_relayouts == relayouts_before;
    size_t pumps = 0;
    while (yielded_to_owner
           && navigation_background_resources_pending(&navigation)
           && pumps++ < 12u) {
        if (!navigation_run_background_resources(&navigation)) break;
    }
    size_t relayouts_after = navigation.performance.fast_relayouts
        + navigation.performance.full_relayouts;
    bool ok = yielded_to_owner && navigation.page.loaded
        && !navigation_background_resources_pending(&navigation)
        && navigation.page.images.stats.loaded == 2
        && navigation.performance.background_images_loaded == 1
        && navigation.performance.background_image_relayouts == 1
        && navigation.performance.background_image_failures == 0
        && relayouts_after == relayouts_before + 1
        && pumps >= 3u;
    if (!ok) {
        fprintf(stderr,
                "deferred-images ready=%d committed=%d first=%d "
                "admitted=%d yielded=%d page=%d pending=%d images=%zu "
                "queue=%zu/%zu job=%d fetch=%zu pumps=%zu batches=%zu "
                "loaded=%zu relayout=%zu failures=%zu error=\"%s\"\n",
                ready, committed, first_frame, admitted, yielded_to_owner,
                navigation.page.loaded,
                navigation_background_resources_pending(&navigation),
                navigation.page.images.stats.loaded,
                navigation.page.deferred_image_cursor,
                navigation.page.deferred_image_count,
                navigation.page.deferred_image_job != NULL,
                fetch_scheduler_pending(
                    navigation.page.resource_scheduler),
                pumps, navigation.performance.background_image_batches,
                navigation.performance.background_images_loaded,
                navigation.performance.background_image_relayouts,
                navigation.performance.background_image_failures,
                navigation.last_error);
    }
    if (ready) fetch_trace_end();
    if (installed) navigation_destroy(&navigation);
    bool clean = budget.current == 0
        && budget_active_allocations(&budget, NULL) == 0
        && budget_categories_reconcile(&budget);
    if (installed) clean = budget_uninstall_lexbor(&budget) && clean;
    return ok && clean;
}

static bool test_web_fonts_begin_after_fallback_layout(void)
{
    static const char html[] =
        "<!doctype html><style>"
        "@font-face{font-family:DeferredFace;"
        "src:url(https://font-background.test/font.ttf) format(truetype)}"
        "body{font-family:DeferredFace,sans-serif}"
        "</style><body>fallback first</body>";
    Budget budget;
    budget_init(&budget, 16 * MIB);
    bool installed = budget_install_lexbor(&budget);
    NavigationSession navigation = {0};
    bool ready = installed && navigation_init(&navigation, &budget, 4);
    if (ready) {
        navigation_enable_web_fonts(
            &navigation, 1, 256 * 1024, 256 * 1024, 1 * MIB, 1000);
    }
    uint64_t generation = ready ? navigation_begin(&navigation) : 0;
    bool committed = ready && navigation_commit_html(
        &navigation, generation, "https://font-background.test/page",
        html, sizeof(html) - 1u, 480, NULL, NULL, true);
    FontFace *face = committed ? stylesheet_web_font_face(
        &navigation.page.stylesheet, 0, false) : NULL;
    size_t relayouts_before = navigation.performance.fast_relayouts
        + navigation.performance.full_relayouts;
    bool fallback_frame = committed && navigation.page.loaded
        && navigation.page.layout.count != 0 && face != NULL && !face->loaded
        && navigation.page.external_fonts.sources_discovered == 1
        && navigation.page.external_fonts.attempted == 0
        && fetch_scheduler_pending(navigation.page.resource_scheduler) == 0
        && navigation_background_resources_pending(&navigation);

    /* The deterministic transport fault must remain unconsumed by commit:
       the first idle slice only enqueues, and the next bounded pump settles
       it without invalidating the usable fallback layout. */
    fetch_inject_failure_once(FETCH_INJECT_TLS);
    bool enqueued = fallback_frame
        && navigation_run_background_resources(&navigation);
    bool pending_after_enqueue = enqueued
        && navigation.page.external_fonts.attempted == 1
        && navigation_background_resources_pending(&navigation);
    bool settled = pending_after_enqueue
        && navigation_run_background_resources(&navigation);
    fetch_inject_failure_once(FETCH_INJECT_NONE);
    size_t relayouts_after = navigation.performance.fast_relayouts
        + navigation.performance.full_relayouts;
    bool ok = settled && navigation.page.loaded && !face->loaded
        && !navigation_background_resources_pending(&navigation)
        && fetch_scheduler_pending(navigation.page.resource_scheduler) == 0
        && navigation.page.external_fonts.failed == 1
        && navigation.performance.background_font_slices == 2
        && navigation.performance.background_fonts_loaded == 0
        && navigation.performance.background_font_relayouts == 0
        && navigation.performance.background_font_failures == 1
        && relayouts_after == relayouts_before;
    if (!ok) {
        fprintf(stderr,
                "font-background ready=%d committed=%d fallback=%d "
                "enqueued=%d pending-after=%d settled=%d page=%d face=%d "
                "sources=%zu attempts=%zu failures=%zu scheduler=%zu "
                "pending=%d slices=%zu loaded=%zu relayouts=%zu "
                "background-failures=%zu layout-relayouts=%zu/%zu "
                "error=\"%s\"\n",
                ready, committed, fallback_frame, enqueued,
                pending_after_enqueue, settled, navigation.page.loaded,
                face != NULL && face->loaded,
                navigation.page.external_fonts.sources_discovered,
                navigation.page.external_fonts.attempted,
                navigation.page.external_fonts.failed,
                fetch_scheduler_pending(
                    navigation.page.resource_scheduler),
                navigation_background_resources_pending(&navigation),
                navigation.performance.background_font_slices,
                navigation.performance.background_fonts_loaded,
                navigation.performance.background_font_relayouts,
                navigation.performance.background_font_failures,
                relayouts_before, relayouts_after, navigation.last_error);
    }
    if (installed) navigation_destroy(&navigation);
    bool clean = budget.current == 0
        && budget_active_allocations(&budget, NULL) == 0
        && budget_categories_reconcile(&budget);
    if (installed) clean = budget_uninstall_lexbor(&budget) && clean;
    return ok && clean;
}

static bool test_streaming_preview_precedes_eof(void)
{
    Budget budget;
    budget_init(&budget, 24 * MIB);
    bool lexbor_installed = budget_install_lexbor(&budget);
    NavigationSession navigation = {0};
    ProgressivePreviewProbe probe = {0};
    bool ready = lexbor_installed
        && navigation_init(&navigation, &budget, 4)
        && navigation_set_progressive_paint_hook(
               &navigation, capture_progressive_preview, &probe)
        && streaming_preview_replay_begin();
    if (ready) {
        navigation_enable_scripts(&navigation, 4 * MIB, 1000);
        navigation_enable_document_scripts(
            &navigation, 4, 32 * 1024, 16 * 1024, 1000);
        navigation_enable_external_resources(
            &navigation, 2, 32 * 1024, 16 * 1024,
            2, 32 * 1024, 16 * 1024, 64 * 1024, 1000);
        navigation_set_stream_delivery(
            &navigation, 64, 0, 0, 0, 0, 0);
    }
    uint64_t generation = ready ? navigation_begin(&navigation) : 0;
    NavigationLoad *load = ready ? navigation_load_begin_url(
        &navigation, generation,
        "https://stream-preview.test/document", 4096, 1000, 480,
        NULL, NULL, true) : NULL;
    NavigationLoadQuota quota = {
        .fetch = {
            .maximum_body_callbacks = 1,
            .maximum_body_bytes = 64,
            .maximum_time_us = 10000
        },
        .maximum_parser_body_bytes = 64,
        .maximum_parser_time_us = 10000
    };
    bool painted_while_pending = false;
    for (size_t i = 0; load != NULL && i < 128; i++) {
        NavigationLoadStatus status = navigation_load_status(load);
        if (status != NAVIGATION_LOAD_PENDING) break;
        status = navigation_load_pump(load, &quota);
        if (probe.calls != 0 && status == NAVIGATION_LOAD_PENDING
            && !navigation.page.loaded) {
            painted_while_pending = true;
        }
    }
    bool loaded = load != NULL
        && finish_bounded(load, &quota);
    lxb_dom_node_t *body = loaded
        ? document_body_node(&navigation.page.document) : NULL;
    size_t script_ran_length = 0;
    const char *script_ran = body == NULL ? NULL : document_attribute(
        body, "data-script-ran", &script_ran_length);
    bool ok = loaded && painted_while_pending
        && probe.calls == 1 && probe.boxes != 0
        && !probe.script_ran_at_paint
        && script_ran != NULL && script_ran_length == 3
        && memcmp(script_ran, "yes", 3) == 0
        && !probe.candidate_loaded && !probe.tail_visible
        && navigation.performance.streaming_preview_checks >= 1
        && navigation.performance.streaming_preview_attempts == 1
        && navigation.performance.streaming_preview_paints == 1
        && navigation.performance.streaming_preview_pre_script_checks == 1
        && navigation.performance.streaming_preview_pre_script_paints == 1
        && navigation.performance.streaming_preview_empty_raster_skips == 0
        && navigation.performance.progressive_layout_attempts == 1
        && navigation.performance.blocking_stylesheet_builds == 1
        && navigation.performance
             .blocking_stylesheet_continuation_fallbacks == 0
        && navigation.performance.streaming_preview_source_bytes < 1531
        && navigation.performance.streaming_preview_node_count
             < navigation.page.document.node_count
        && navigation.performance.partial_layouts == 1
        && navigation.performance.partial_paints == 1
        && navigation.performance.blocking_script_sample_count == 2
        && navigation.performance.parser_script_compile_us > 0
        && navigation.performance.first_paint_us != 0
        && navigation.page.loaded
        && strcmp(navigation.page.document.title,
                  "Streaming preview") == 0;
    if (!ok) {
        fprintf(stderr,
                "stream-preview loaded=%d pending=%d calls=%zu boxes=%zu "
                "candidate-loaded=%d tail=%d script-at-paint=%d "
                "script-final=%.*s checks=%zu attempts=%zu "
                "paints=%zu source=%zu nodes=%zu/%zu partial=%zu/%zu "
                "samples=%zu compile=%llu pre-script=%zu/%zu "
                "style-cont=%zu/%zu visibility=%zu/%zu/%zu "
                "layout-attempts=%zu empty=%zu status=%d error=\"%s\"\n",
                loaded, painted_while_pending, probe.calls, probe.boxes,
                probe.candidate_loaded, probe.tail_visible,
                probe.script_ran_at_paint, (int) script_ran_length,
                script_ran == NULL ? "" : script_ran,
                navigation.performance.streaming_preview_checks,
                navigation.performance.streaming_preview_attempts,
                navigation.performance.streaming_preview_paints,
                navigation.performance.streaming_preview_source_bytes,
                navigation.performance.streaming_preview_node_count,
                navigation.page.document.node_count,
                navigation.performance.partial_layouts,
                navigation.performance.partial_paints,
                navigation.performance.blocking_script_sample_count,
                (unsigned long long)
                    navigation.performance.parser_script_compile_us,
                navigation.performance.streaming_preview_pre_script_paints,
                navigation.performance.streaming_preview_pre_script_checks,
                navigation.performance.blocking_stylesheet_continuations,
                navigation.performance
                    .blocking_stylesheet_continuation_fallbacks,
                navigation.performance.streaming_preview_visibility_skips,
                navigation.performance.streaming_preview_visibility_checks,
                navigation.performance.streaming_preview_visibility_nodes,
                navigation.performance.progressive_layout_attempts,
                navigation.performance.streaming_preview_empty_raster_skips,
                load == NULL ? -1 : (int) navigation_load_status(load),
                navigation.last_error);
        fprintf(stderr,
                "stream-preview-scripts discovered=%zu blocking=%zu "
                "attempted=%zu loaded=%zu failed=%zu skipped=%zu "
                "pressure=%zu bytes=%zu budget=%zu/%zu remaining=%zu\n",
                navigation.script_discovered,
                navigation.script_parser_blocking,
                navigation.script_attempted,
                navigation.script_loaded,
                navigation.script_failed,
                navigation.script_skipped_quota,
                navigation.script_skipped_pressure,
                navigation.script_bytes, budget.current, budget.limit,
                budget_remaining(&budget));
    }
    navigation_load_destroy(load);
    if (ready) fetch_trace_end();
    if (lexbor_installed) navigation_destroy(&navigation);
    bool clean = budget.current == 0
        && budget_active_allocations(&budget, NULL) == 0
        && budget_categories_reconcile(&budget);
    if (lexbor_installed) clean = budget_uninstall_lexbor(&budget) && clean;
    return ok && clean;
}

static bool test_streaming_preview_pressure_preserves_final_commit(void)
{
    Budget budget;
    budget_init(&budget, 16 * MIB);
    bool lexbor_installed = budget_install_lexbor(&budget);
    NavigationSession navigation = {0};
    ProgressivePreviewProbe probe = {0};
    bool ready = lexbor_installed
        && navigation_init(&navigation, &budget, 4)
        && navigation_set_progressive_paint_hook(
               &navigation, capture_progressive_preview, &probe)
        && streaming_preview_replay_begin();
    if (ready) {
        navigation_enable_scripts(&navigation, 2 * MIB, 1000);
        navigation_enable_document_scripts(
            &navigation, 4, 32 * 1024, 16 * 1024, 1000);
        navigation_enable_external_resources(
            &navigation, 2, 32 * 1024, 16 * 1024,
            2, 32 * 1024, 16 * 1024, 64 * 1024, 1000);
        navigation_set_stream_delivery(
            &navigation, 64, 0, 0, 0, 0, 0);
    }
    uint64_t generation = ready ? navigation_begin(&navigation) : 0;
    NavigationLoad *load = ready ? navigation_load_begin_url(
        &navigation, generation,
        "https://stream-preview.test/document", 4096, 1000, 480,
        NULL, NULL, true) : NULL;
    NavigationLoadQuota quota = {
        .fetch = {
            .maximum_body_callbacks = 1,
            .maximum_body_bytes = 64,
            .maximum_time_us = 10000
        },
        .maximum_parser_body_bytes = 64,
        .maximum_parser_time_us = 10000
    };
    bool constrained = false;
    bool pressure_seen = false;
    for (size_t i = 0; load != NULL && i < 256; i++) {
        if (navigation_load_status(load) != NAVIGATION_LOAD_PENDING) break;
        if (!constrained
            && navigation.performance.blocking_stylesheet_builds != 0
            && navigation.performance.streaming_preview_checks == 0) {
            budget.limit = budget.current + 512 * 1024;
            constrained = true;
        }
        (void) navigation_load_pump(load, &quota);
        if (navigation.performance.streaming_preview_pressure_skips != 0) {
            pressure_seen = true;
            budget.limit = 16 * MIB;
        }
    }
    budget.limit = 16 * MIB;
    bool loaded = load != NULL && finish_bounded(load, &quota);
    bool ok = loaded && constrained && pressure_seen
        && probe.calls == 0
        && navigation.performance.streaming_preview_pressure_skips == 1
        && navigation.performance.streaming_preview_paints == 0
        && navigation.performance.partial_paints == 0
        && budget.pressure[BUDGET_PRESSURE_SPECULATION].decisions == 1
        && navigation.page.loaded
        && strcmp(navigation.page.document.title,
                  "Streaming preview") == 0;
    if (!ok) {
        fprintf(stderr,
                "stream-preview-pressure loaded=%d constrained=%d "
                "pressure-seen=%d calls=%zu skips=%zu paints=%zu partial=%zu "
                "decisions=%zu current=%zu status=%d error=\"%s\"\n",
                loaded, constrained, pressure_seen,
                probe.calls,
                navigation.performance.streaming_preview_pressure_skips,
                navigation.performance.streaming_preview_paints,
                navigation.performance.partial_paints,
                budget.pressure[BUDGET_PRESSURE_SPECULATION].decisions,
                budget.current,
                load == NULL ? -1 : (int) navigation_load_status(load),
                navigation.last_error);
    }
    navigation_load_destroy(load);
    if (ready) fetch_trace_end();
    if (lexbor_installed) navigation_destroy(&navigation);
    bool clean = budget.current == 0
        && budget_active_allocations(&budget, NULL) == 0
        && budget_categories_reconcile(&budget);
    if (lexbor_installed) clean = budget_uninstall_lexbor(&budget) && clean;
    return ok && clean;
}

typedef struct {
    uint64_t frame_hash;
    size_t command_count;
    size_t node_box_count;
    size_t stylesheet_rule_count;
    size_t document_node_count;
    int layout_height;
    size_t continuation_count;
    size_t stylesheet_build_count;
} StreamingFinalSnapshot;

static uint64_t hash_frame_pixels(const uint16_t *pixels, size_t count)
{
    uint64_t hash = UINT64_C(1469598103934665603);
    for (size_t i = 0; i < count; i++) {
        hash ^= pixels[i] & 0xffu;
        hash *= UINT64_C(1099511628211);
        hash ^= pixels[i] >> 8;
        hash *= UINT64_C(1099511628211);
    }
    return hash;
}

static bool capture_streaming_final_snapshot(
    bool disable_continuation, StreamingFinalSnapshot *snapshot)
{
    enum { FRAME_WIDTH = 480, FRAME_HEIGHT = 272 };
    if (snapshot == NULL) return false;
    memset(snapshot, 0, sizeof(*snapshot));
    if (disable_continuation) {
        if (setenv("TILEFINCH_DISABLE_STYLESHEET_CONTINUATION", "1", 1)
            != 0) return false;
    } else if (unsetenv("TILEFINCH_DISABLE_STYLESHEET_CONTINUATION") != 0) {
        return false;
    }

    Budget budget;
    budget_init(&budget, 16 * MIB);
    bool lexbor_installed = budget_install_lexbor(&budget);
    NavigationSession navigation = {0};
    ProgressivePreviewProbe probe = {0};
    TileCache cache = {0};
    uint16_t *frame = NULL;
    bool ready = lexbor_installed
        && navigation_init(&navigation, &budget, 4)
        && navigation_set_progressive_paint_hook(
               &navigation, capture_progressive_preview, &probe)
        && streaming_preview_replay_begin();
    if (ready) {
        navigation_enable_scripts(&navigation, 2 * MIB, 1000);
        navigation_enable_document_scripts(
            &navigation, 4, 32 * 1024, 16 * 1024, 1000);
        navigation_enable_external_resources(
            &navigation, 2, 32 * 1024, 16 * 1024,
            2, 32 * 1024, 16 * 1024, 64 * 1024, 1000);
        navigation_set_stream_delivery(
            &navigation, 64, 0, 0, 0, 0, 0);
    }
    uint64_t generation = ready ? navigation_begin(&navigation) : 0;
    NavigationLoad *load = ready ? navigation_load_begin_url(
        &navigation, generation,
        "https://stream-preview.test/document", 4096, 1000,
        FRAME_WIDTH, NULL, NULL, true) : NULL;
    NavigationLoadQuota quota = {
        .fetch = {
            .maximum_body_callbacks = 1,
            .maximum_body_bytes = 64,
            .maximum_time_us = 10000
        },
        .maximum_parser_body_bytes = 64,
        .maximum_parser_time_us = 10000
    };
    for (size_t i = 0; load != NULL && i < 128; i++) {
        NavigationLoadStatus status = navigation_load_status(load);
        if (status != NAVIGATION_LOAD_PENDING) break;
        (void) navigation_load_pump(load, &quota);
    }
    bool loaded = load != NULL && finish_bounded(load, &quota);
    size_t frame_pixels = (size_t) FRAME_WIDTH * FRAME_HEIGHT;
    frame = loaded
        ? budget_malloc(&budget, frame_pixels * sizeof(*frame)) : NULL;
    bool rendered = frame != NULL
        && tile_cache_init(&cache, &budget, &navigation.page.layout, 4)
        && tile_cache_set_frame(&cache, frame, frame_pixels)
        && tile_cache_render_frame(
               &cache, 0, FRAME_WIDTH, FRAME_HEIGHT, NULL);
    if (rendered) {
        snapshot->frame_hash = hash_frame_pixels(frame, frame_pixels);
        snapshot->command_count = navigation.page.layout.count;
        snapshot->node_box_count = navigation.page.layout.node_box_count;
        snapshot->stylesheet_rule_count = navigation.page.stylesheet.count;
        snapshot->document_node_count = navigation.page.document.node_count;
        snapshot->layout_height = navigation.page.layout.height;
        snapshot->continuation_count =
            navigation.performance.blocking_stylesheet_continuations;
        snapshot->stylesheet_build_count =
            navigation.performance.blocking_stylesheet_builds;
    }
    tile_cache_destroy(&cache);
    budget_free(&budget, frame);
    navigation_load_destroy(load);
    if (ready) fetch_trace_end();
    if (lexbor_installed) navigation_destroy(&navigation);
    bool clean = budget.current == 0
        && budget_active_allocations(&budget, NULL) == 0
        && budget_categories_reconcile(&budget);
    if (lexbor_installed) clean = budget_uninstall_lexbor(&budget) && clean;
    return loaded && rendered && probe.calls == 1 && clean;
}

static bool test_streaming_preview_skips_inert_prefix_layout(void)
{
    Budget budget;
    budget_init(&budget, 24 * MIB);
    bool lexbor_installed = budget_install_lexbor(&budget);
    NavigationSession navigation = {0};
    ProgressivePreviewProbe probe = {0};
    bool navigation_initialized = lexbor_installed
        && navigation_init(&navigation, &budget, 4)
        && navigation_set_progressive_paint_hook(
               &navigation, capture_progressive_preview, &probe);
    bool replay_started = navigation_initialized
        && streaming_preview_empty_replay_begin();
    bool ready = replay_started;
    if (ready) {
        navigation_enable_scripts(&navigation, 2 * MIB, 1000);
        navigation_enable_document_scripts(
            &navigation, 4, 32 * 1024, 16 * 1024, 1000);
        navigation_enable_external_resources(
            &navigation, 2, 32 * 1024, 16 * 1024,
            2, 32 * 1024, 16 * 1024, 64 * 1024, 1000);
        navigation_set_stream_delivery(
            &navigation, 64, 0, 0, 0, 0, 0);
    }
    uint64_t generation = ready ? navigation_begin(&navigation) : 0;
    NavigationLoad *load = ready ? navigation_load_begin_url(
        &navigation, generation,
        "https://stream-preview.test/document", 4096, 1000, 480,
        NULL, NULL, true) : NULL;
    NavigationLoadQuota quota = {
        .fetch = {
            .maximum_body_callbacks = 1,
            .maximum_body_bytes = 64,
            .maximum_time_us = 10000
        },
        .maximum_parser_body_bytes = 64,
        .maximum_parser_time_us = 10000
    };
    for (size_t i = 0; load != NULL && i < 128; i++) {
        if (navigation_load_status(load) != NAVIGATION_LOAD_PENDING) break;
        (void) navigation_load_pump(load, &quota);
    }
    bool loaded = load != NULL && finish_bounded(load, &quota);
    bool ok = loaded
        && navigation.performance.streaming_preview_visibility_checks != 0
        && navigation.performance.streaming_preview_visibility_skips != 0
        && navigation.performance.streaming_preview_visibility_nodes != 0
        && navigation.performance.progressive_layout_attempts == 0
        && navigation.performance.streaming_preview_attempts == 0
        && navigation.performance.streaming_preview_paints == 0
        && navigation.page.loaded;
    if (!ok) {
        fprintf(stderr,
                "empty-prefix lexbor=%d init=%d replay=%d generation=%llu "
                "loaded=%d cancelled=%d session-generation=%llu "
                "visibility=%zu/%zu/%zu "
                "layout-attempts=%zu preview=%zu/%zu status=%d error=\"%s\"\n",
                lexbor_installed, navigation_initialized, replay_started,
                (unsigned long long) generation,
                loaded, navigation.cancelled,
                (unsigned long long) navigation.generation,
                navigation.performance.streaming_preview_visibility_skips,
                navigation.performance.streaming_preview_visibility_checks,
                navigation.performance.streaming_preview_visibility_nodes,
                navigation.performance.progressive_layout_attempts,
                navigation.performance.streaming_preview_paints,
                navigation.performance.streaming_preview_attempts,
                load == NULL ? -1 : (int) navigation_load_status(load),
                navigation.last_error);
    }
    navigation_load_destroy(load);
    if (ready) fetch_trace_end();
    if (lexbor_installed) navigation_destroy(&navigation);
    bool clean = budget.current == 0
        && budget_active_allocations(&budget, NULL) == 0
        && budget_categories_reconcile(&budget);
    if (lexbor_installed) clean = budget_uninstall_lexbor(&budget) && clean;
    return ok && clean;
}

static bool test_streaming_stylesheet_continuation_converges(void)
{
    StreamingFinalSnapshot continued = {0};
    StreamingFinalSnapshot rebuilt = {0};
    bool captured = capture_streaming_final_snapshot(false, &continued)
        && capture_streaming_final_snapshot(true, &rebuilt);
    (void) unsetenv("TILEFINCH_DISABLE_STYLESHEET_CONTINUATION");
    bool equivalent = captured
        && continued.continuation_count == 1
        && rebuilt.continuation_count == 0
        && rebuilt.stylesheet_build_count > continued.stylesheet_build_count
        && continued.frame_hash == rebuilt.frame_hash
        && continued.command_count == rebuilt.command_count
        && continued.node_box_count == rebuilt.node_box_count
        && continued.stylesheet_rule_count == rebuilt.stylesheet_rule_count
        && continued.document_node_count == rebuilt.document_node_count
        && continued.layout_height == rebuilt.layout_height;
    if (!equivalent) {
        fprintf(stderr,
                "stream convergence captured=%d hash=%llx/%llx "
                "commands=%zu/%zu boxes=%zu/%zu rules=%zu/%zu "
                "nodes=%zu/%zu height=%d/%d continuation=%zu/%zu "
                "builds=%zu/%zu\n",
                captured,
                (unsigned long long) continued.frame_hash,
                (unsigned long long) rebuilt.frame_hash,
                continued.command_count, rebuilt.command_count,
                continued.node_box_count, rebuilt.node_box_count,
                continued.stylesheet_rule_count,
                rebuilt.stylesheet_rule_count,
                continued.document_node_count, rebuilt.document_node_count,
                continued.layout_height, rebuilt.layout_height,
                continued.continuation_count, rebuilt.continuation_count,
                continued.stylesheet_build_count,
                rebuilt.stylesheet_build_count);
    }
    return equivalent;
}

static bool test_streaming_preview_rejects_incomplete_external_css(void)
{
    Budget budget;
    budget_init(&budget, 16 * MIB);
    bool lexbor_installed = budget_install_lexbor(&budget);
    NavigationSession navigation = {0};
    ProgressivePreviewProbe probe = {0};
    bool ready = lexbor_installed
        && navigation_init(&navigation, &budget, 4)
        && navigation_set_progressive_paint_hook(
               &navigation, capture_progressive_preview, &probe)
        && streaming_preview_css_failure_replay_begin();
    if (ready) {
        navigation_enable_scripts(&navigation, 2 * MIB, 1000);
        navigation_enable_document_scripts(
            &navigation, 4, 32 * 1024, 16 * 1024, 1000);
        navigation_enable_external_resources(
            &navigation, 2, 32 * 1024, 16 * 1024,
            2, 32 * 1024, 16 * 1024, 64 * 1024, 1000);
        navigation_set_stream_delivery(
            &navigation, 64, 0, 0, 0, 0, 0);
    }
    uint64_t generation = ready ? navigation_begin(&navigation) : 0;
    NavigationLoad *load = ready ? navigation_load_begin_url(
        &navigation, generation,
        "https://stream-preview.test/failure-document",
        4096, 1000, 480, NULL, NULL, true) : NULL;
    NavigationLoadQuota quota = {
        .fetch = {
            .maximum_body_callbacks = 1,
            .maximum_body_bytes = 64,
            .maximum_time_us = 10000
        },
        .maximum_parser_body_bytes = 64,
        .maximum_parser_time_us = 10000
    };
    for (size_t i = 0; load != NULL && i < 128; i++) {
        NavigationLoadStatus status = navigation_load_status(load);
        if (status != NAVIGATION_LOAD_PENDING) break;
        (void) navigation_load_pump(load, &quota);
    }
    bool loaded = load != NULL && finish_bounded(load, &quota);
    bool ok = loaded && probe.calls == 0
        && navigation.performance.streaming_preview_style_refresh_attempts
             == 1
        && navigation.performance.streaming_preview_style_refreshes == 0
        && navigation.performance.streaming_preview_style_refresh_failures
             == 1
        && navigation.performance.streaming_preview_paints == 0
        && navigation.performance.partial_paints == 0
        && navigation.page.loaded;
    if (!ok) {
        fprintf(stderr,
                "stream-preview-css-failure loaded=%d calls=%zu "
                "refresh=%zu/%zu failures=%zu paints=%zu partial=%zu "
                "status=%d error=\"%s\"\n",
                loaded, probe.calls,
                navigation.performance.streaming_preview_style_refreshes,
                navigation.performance.streaming_preview_style_refresh_attempts,
                navigation.performance.streaming_preview_style_refresh_failures,
                navigation.performance.streaming_preview_paints,
                navigation.performance.partial_paints,
                load == NULL ? -1 : (int) navigation_load_status(load),
                navigation.last_error);
    }
    navigation_load_destroy(load);
    if (ready) fetch_trace_end();
    if (lexbor_installed) navigation_destroy(&navigation);
    bool clean = budget.current == 0
        && budget_active_allocations(&budget, NULL) == 0
        && budget_categories_reconcile(&budget);
    if (lexbor_installed) clean = budget_uninstall_lexbor(&budget) && clean;
    return ok && clean;
}

static bool test_streaming_preview_cancel_is_transient(void)
{
    Budget budget;
    budget_init(&budget, 16 * MIB);
    bool lexbor_installed = budget_install_lexbor(&budget);
    NavigationSession navigation = {0};
    ProgressivePreviewProbe probe = {0};
    bool ready = lexbor_installed
        && navigation_init(&navigation, &budget, 2)
        && navigation_set_progressive_paint_hook(
               &navigation, capture_progressive_preview, &probe)
        && streaming_preview_replay_begin();
    if (ready) {
        navigation_enable_scripts(&navigation, 2 * MIB, 1000);
        navigation_enable_document_scripts(
            &navigation, 4, 32 * 1024, 16 * 1024, 1000);
        navigation_enable_external_resources(
            &navigation, 2, 32 * 1024, 16 * 1024,
            2, 32 * 1024, 16 * 1024, 64 * 1024, 1000);
        navigation_set_stream_delivery(
            &navigation, 64, 0, 0, 0, 0, 0);
    }
    uint64_t generation = ready ? navigation_begin(&navigation) : 0;
    NavigationLoad *load = ready ? navigation_load_begin_url(
        &navigation, generation,
        "https://stream-preview.test/document", 4096, 1000, 480,
        NULL, NULL, true) : NULL;
    NavigationLoadQuota quota = {
        .fetch = {
            .maximum_body_callbacks = 1,
            .maximum_body_bytes = 64,
            .maximum_time_us = 10000
        },
        .maximum_parser_body_bytes = 64,
        .maximum_parser_time_us = 10000
    };
    for (size_t i = 0; load != NULL && probe.calls == 0 && i < 128; i++) {
        if (navigation_load_pump(load, &quota)
            != NAVIGATION_LOAD_PENDING) break;
    }
    bool previewed = load != NULL && probe.calls == 1
        && navigation_load_status(load) == NAVIGATION_LOAD_PENDING
        && !navigation.page.loaded;
    if (previewed) {
        navigation_load_cancel(load, "test cancellation after preview");
    }
    bool ok = previewed
        && navigation_load_status(load) == NAVIGATION_LOAD_CANCELLED
        && !navigation.page.loaded
        && navigation.history_count == 0
        && navigation.performance.streaming_preview_paints == 1
        && navigation.performance.partial_paints == 1;
    navigation_load_destroy(load);
    if (ready) fetch_trace_end();
    if (lexbor_installed) navigation_destroy(&navigation);
    bool clean = budget.current == 0
        && budget_active_allocations(&budget, NULL) == 0
        && budget_categories_reconcile(&budget);
    if (lexbor_installed) clean = budget_uninstall_lexbor(&budget) && clean;
    return ok && clean;
}

static bool finish_bounded(NavigationLoad *load,
                           const NavigationLoadQuota *quota)
{
    for (size_t i = 0; load != NULL && i < 8; i++) {
        NavigationLoadStatus status = navigation_load_status(load);
        if (status == NAVIGATION_LOAD_SUCCEEDED) return true;
        if (status != NAVIGATION_LOAD_READY_TO_FINISH
            && status != NAVIGATION_LOAD_FINALIZING) return false;
        if (navigation_load_finish(load, quota)) {
            return navigation_load_status(load)
                == NAVIGATION_LOAD_SUCCEEDED;
        }
    }
    return false;
}

static bool critical_ch_replay_begin(void)
{
    char error[256] = {0};
    return fetch_trace_replay_begin(
        TILEFINCH_TEST_SOURCE_DIR "/fixtures/http-critical-ch", error,
        sizeof(error));
}

static bool redirected_critical_ch_replay_begin(void)
{
    char error[256] = {0};
    return fetch_trace_replay_begin(
        TILEFINCH_TEST_SOURCE_DIR "/fixtures/http-critical-ch-redirect", error,
        sizeof(error));
}

static bool remote_isolation_replay_begin(void)
{
    char error[256] = {0};
    return fetch_trace_replay_begin(
        TILEFINCH_TEST_SOURCE_DIR "/fixtures/http-remote-isolation", error,
        sizeof(error));
}

static bool preload_stall_replay_begin(void)
{
    char error[256] = {0};
    return fetch_trace_replay_begin(
        TILEFINCH_TEST_SOURCE_DIR "/fixtures/http-preload-stall", error,
        sizeof(error));
}

static bool script_preload_replay_begin(void)
{
    char error[256] = {0};
    return fetch_trace_replay_begin(
        TILEFINCH_TEST_SOURCE_DIR "/fixtures/http-script-preload", error,
        sizeof(error));
}

static bool dynamic_style_stream_replay_begin(void)
{
    char error[256] = {0};
    return fetch_trace_replay_begin(
        TILEFINCH_TEST_SOURCE_DIR "/fixtures/http-dynamic-style-stream",
        error, sizeof(error));
}

static bool header_cookie_viewport_replay_begin(void)
{
    char error[256] = {0};
    return fetch_trace_replay_begin(
        TILEFINCH_TEST_SOURCE_DIR "/fixtures/http-header-cookie-viewport",
        error, sizeof(error));
}

static bool put_stylesheet_cache(BrowserSession *browser,
                                 const char *document_url, const char *url,
                                 const unsigned char *css, size_t length)
{
    if (browser == NULL || document_url == NULL || url == NULL
        || css == NULL || length == 0) return false;
    unsigned char *copy = budget_malloc(browser->budget, length);
    if (copy == NULL) return false;
    memcpy(copy, css, length);
    BrowserSharedBody *body = browser_shared_body_take(
        browser->budget, copy, length);
    if (body == NULL) {
        budget_free(browser->budget, copy);
        return false;
    }
    TilefinchRequestContext context = {
        .target_url = url, .initiator_url = document_url,
        .top_level_url = document_url, .method = "GET",
        .mode = TILEFINCH_REQUEST_MODE_NO_CORS,
        .credentials = TILEFINCH_CREDENTIALS_INCLUDE,
        .destination = TILEFINCH_DESTINATION_STYLE
    };
    TilefinchResourceGrant grant = {
        .destination = TILEFINCH_DESTINATION_STYLE,
        .mode = TILEFINCH_REQUEST_MODE_NO_CORS,
        .credentials = TILEFINCH_CREDENTIALS_INCLUDE,
        .final_same_origin = tilefinch_url_same_origin(document_url, url),
        .cors_validated = tilefinch_url_same_origin(document_url, url)
    };
    bool stored = browser_session_cache_put_http_shared_resource(
            browser, url, body, NULL, NULL, "text/css", "immutable", NULL,
            0, &context, &grant)
        && browser_session_cache_set_resource_response_provenance(
            browser, url, &context, url, "");
    browser_shared_body_release(body);
    return stored;
}

typedef struct {
    const char *document_url;
    const char *stylesheet_url;
    const char *html;
    const char *css;
    const char *response_referrer_policy;
    const char *stylesheet_request_referrer;
    const char *stylesheet_request_policy;
} ReferrerNavigationReplay;

static uint64_t referrer_navigation_body_hash(
    const char *data, size_t length)
{
    uint64_t value = UINT64_C(14695981039346656037);
    for (size_t i = 0; i < length; i++) {
        value ^= (unsigned char) data[i];
        value *= UINT64_C(1099511628211);
    }
    return value;
}

static bool write_referrer_navigation_replay(
    char directory[128], const ReferrerNavigationReplay *fixture)
{
    if (fixture == NULL || fixture->document_url == NULL
        || fixture->stylesheet_url == NULL || fixture->html == NULL
        || fixture->css == NULL
        || fixture->response_referrer_policy == NULL
        || fixture->stylesheet_request_policy == NULL) return false;
    snprintf(directory, 128, "%s", "/tmp/tilefinch-nav-referrer-XXXXXX");
    if (mkdtemp(directory) == NULL) return false;
    char body_path[192], meta_path[192];
    bool ok = true;
    for (size_t i = 0; ok && i < 2; i++) {
        const char *body_data = i == 0 ? fixture->html : fixture->css;
        size_t body_length = strlen(body_data);
        ok = snprintf(body_path, sizeof(body_path), "%s/%04zu.body",
                      directory, i) > 0
            && snprintf(meta_path, sizeof(meta_path), "%s/%04zu.meta",
                        directory, i) > 0;
        FILE *body = ok ? fopen(body_path, "wb") : NULL;
        bool body_written = body != NULL
            && fwrite(body_data, 1, body_length, body) == body_length;
        bool body_closed = body != NULL && fclose(body) == 0;
        if (!body_written || !body_closed) {
            ok = false;
            break;
        }
        FILE *meta = fopen(meta_path, "wb");
        if (meta == NULL) {
            ok = false;
            break;
        }
        if (i == 0) {
            ok = fprintf(
                meta,
                "psp-http-trace=3\nmethod=GET\nurl=%s\nsuccess=1\n"
                "async-delay-pumps=0\nexternal-cancel=0\n"
                "transport-timeout=0\nerror=\nrequest-body-length=0\n"
                "request-body-hash=cbf29ce484222325\n"
                "request-cookie-bytes=0\nrequest-has-cf-clearance=0\n"
                "request-extra-header-bytes=0\n"
                "request-send-client-hints=0\n"
                "request-send-low-client-hints=1\n"
                "request-sec-fetch-user=1\nrequest-upgrade-insecure=1\n"
                "status=200\nlength=%zu\neffective-url=%s\n"
                "content-type=text/html; charset=utf-8\netag=\n"
                "last-modified=\ncf-mitigated=\naccept-ch=\ncritical-ch=\n"
                "server=fixture\ncf-ray=\nresponse-header-count=2\n"
                "set-cookie-count=0\n"
                "response-header-0=content-type: text/html; charset=utf-8\n"
                "response-header-1=referrer-policy: %s\n",
                fixture->document_url, body_length, fixture->document_url,
                fixture->response_referrer_policy) > 0;
        } else {
            ok = fprintf(
                meta,
                "psp-http-trace=10\ncookie-values=redacted\n"
                "method=GET\nurl=%s\nlogical-request-url=%s\nsuccess=1\n"
                "async-delay-pumps=0\nexternal-cancel=0\n"
                "transport-timeout=0\nredirect-origin-tainted=0\n"
                "error=\nrequest-body-length=0\n"
                "request-body-hash=cbf29ce484222325\n"
                "request-content-type=\nrequest-cookie-bytes=0\n"
                "request-has-cf-clearance=0\n"
                "request-extra-header-bytes=0\n"
                "request-extra-header-shape=\n"
                "request-allow-http-errors=0\n"
                "request-enforce-cors=0\n"
                "request-redirect-same-origin-only=0\n"
                "request-cors-cached-response-validated=0\n"
                "request-if-none-match=\nrequest-if-modified-since=\n"
                "request-referer=%s\nrequest-origin=\n"
                "request-accept=text/css,*/*;q=0.1\n"
                "request-sec-fetch-dest=style\n"
                "request-sec-fetch-mode=no-cors\n"
                "request-sec-fetch-site=cross-site\n"
                "request-send-client-hints=0\n"
                "request-client-hint-tokens=\n"
                "request-client-hint-origin=\n"
                "request-send-low-client-hints=0\n"
                "request-sec-fetch-user=0\nrequest-upgrade-insecure=0\n"
                "request-user-agent=" TILEFINCH_BROWSER_USER_AGENT "\n"
                "request-diagnostic-mobile-safari=0\n"
                "request-credentials=0\n"
                "request-credential-origin=%s\n"
                "request-initiator-url=%s\n"
                "request-referrer-source=%s\n"
                "request-referrer-policy=%s\n"
                "status=200\nlength=%zu\nresponse-body-hash=%016llx\n"
                "effective-url=%s\ncontent-type=text/css\netag=\n"
                "last-modified=\ncf-mitigated=\naccept-ch=\ncritical-ch=\n"
                "server=fixture\ncf-ray=\n"
                "response-referrer-policy-metadata-valid=1\n"
                "response-referrer-policy-present=0\n"
                "response-referrer-policy=\n"
                "response-security-headers-truncated=0\n"
                "response-header-count=2\n"
                "set-cookie-count=0\n"
                "response-header-0=content-type: text/css\n"
                "response-header-1=cache-control: max-age=3600\n",
                fixture->stylesheet_url, fixture->stylesheet_url,
                fixture->stylesheet_request_referrer == NULL
                    ? "" : fixture->stylesheet_request_referrer,
                fixture->document_url, fixture->document_url,
                fixture->document_url, fixture->stylesheet_request_policy,
                body_length,
                (unsigned long long) referrer_navigation_body_hash(
                    body_data, body_length),
                fixture->stylesheet_url) > 0;
        }
        ok = fclose(meta) == 0 && ok;
    }
    char clock_path[192];
    if (ok && snprintf(clock_path, sizeof(clock_path), "%s/trace.meta",
                       directory) > 0) {
        FILE *clock = fopen(clock_path, "wb");
        ok = clock != NULL
            && fprintf(clock,
                       "psp-http-trace-clock=1\norigin-ms=1000\n") > 0
            && fclose(clock) == 0;
    } else {
        ok = false;
    }
    return ok;
}

static void remove_referrer_navigation_replay(const char *directory)
{
    char path[192];
    for (size_t i = 0; i < 2; i++) {
        if (snprintf(path, sizeof(path), "%s/%04zu.body", directory, i)
                > 0) (void) unlink(path);
        if (snprintf(path, sizeof(path), "%s/%04zu.meta", directory, i)
                > 0) (void) unlink(path);
    }
    if (snprintf(path, sizeof(path), "%s/trace.meta", directory) > 0) {
        (void) unlink(path);
    }
    (void) rmdir(directory);
}

static bool module_swap_replay_begin(void)
{
    char error[256] = {0};
    return fetch_trace_replay_begin(
        TILEFINCH_TEST_SOURCE_DIR "/fixtures/http-module-swap",
        error, sizeof(error));
}

static bool critical_side_effect_absent(BrowserSession *browser)
{
    char cookies[4096] = {0};
    return browser_session_cookie_get(
               browser, "https://critical.test/document", cookies,
               sizeof(cookies))
        && strstr(cookies, "parser_side_effect_count=") == NULL;
}

static bool prior_page_at(NavigationSession *navigation, const char *url)
{
    static const char html[] =
        "<!doctype html><title>Prior</title><body>retained</body>";
    uint64_t generation = navigation_begin(navigation);
    return navigation_commit_html(
        navigation, generation, url, html,
        sizeof(html) - 1, 480, NULL, NULL, true);
}

static bool prior_page(NavigationSession *navigation)
{
    return prior_page_at(navigation, "https://prior.test/#old");
}

static lxb_dom_node_t *test_find_id(lxb_dom_node_t *node, const char *id);

/* Navigation tests retain a single executable and shared fixture lifetime;
   these ordered units follow document, transaction, and runtime boundaries. */
#include "suites/navigation_document.inc"
#include "suites/navigation_transactions.inc"
#include "suites/navigation_streaming.inc"
#include "suites/navigation_runtime.inc"
#include "suites/navigation_runner.inc"
