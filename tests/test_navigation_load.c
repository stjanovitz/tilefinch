#include "tilefinch/budget.h"
#include "tilefinch/fetch.h"
#include "tilefinch/layout.h"
#include "tilefinch/navigation.h"
#include "tilefinch/platform.h"
#include "tilefinch/render.h"
#include "tilefinch/script_loader.h"
#include "tilefinch/section_store.h"
#include "tilefinch/style.h"
#include "tilefinch/user_agent.h"
#include "../src/image_decode_internal.h"
#include "../src/tilefinch_test_faults.h"

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

static lxb_dom_node_t *test_find_id(lxb_dom_node_t *node, const char *id);
/* Private deterministic seam implemented by navigation.c for this test
   executable; zero always restores the production one-MiB work bound. */
void navigation_test_set_parser_script_stage_work_limit(size_t limit);
void navigation_test_set_parser_script_stage_elapsed_us(uint64_t elapsed_us);
void navigation_test_set_parser_script_stage_time_limit_us(uint64_t limit_us);

static bool replay_begin(void)
{
    char error[256] = {0};
    return fetch_trace_replay_begin(
        TILEFINCH_TEST_SOURCE_DIR "/fixtures/http-stream", error,
        sizeof(error));
}

static bool parser_checkpoint_replay_begin(void)
{
    char error[256] = {0};
    return fetch_trace_replay_begin(
        TILEFINCH_TEST_SOURCE_DIR "/fixtures/http-parser-checkpoint",
        error, sizeof(error));
}

static bool parser_script_circuit_replay_begin(bool work_bound)
{
    char error[256] = {0};
    return fetch_trace_replay_begin(
        work_bound
            ? TILEFINCH_TEST_SOURCE_DIR
                "/fixtures/http-parser-script-work-circuit"
            : TILEFINCH_TEST_SOURCE_DIR
                "/fixtures/http-parser-script-circuit",
        error, sizeof(error));
}

static bool parser_script_transport_failures_replay_begin(void)
{
    char error[256] = {0};
    return fetch_trace_replay_begin(
        TILEFINCH_TEST_SOURCE_DIR
            "/fixtures/http-parser-script-transport-failures",
        error, sizeof(error));
}

static bool parser_script_time_circuit_replay_begin(void)
{
    char error[256] = {0};
    return fetch_trace_replay_begin(
        TILEFINCH_TEST_SOURCE_DIR
            "/fixtures/http-parser-script-time-circuit",
        error, sizeof(error));
}

static bool parser_script_preload_time_replay_begin(void)
{
    char error[256] = {0};
    return fetch_trace_replay_begin(
        TILEFINCH_TEST_SOURCE_DIR
            "/fixtures/http-parser-script-preload-time",
        error, sizeof(error));
}

static bool parser_script_failure_reset_replay_begin(void)
{
    char error[256] = {0};
    return fetch_trace_replay_begin(
        TILEFINCH_TEST_SOURCE_DIR
            "/fixtures/http-parser-script-failure-reset",
        error, sizeof(error));
}

static bool parser_script_work_failure_replay_begin(void)
{
    char error[256] = {0};
    return fetch_trace_replay_begin(
        TILEFINCH_TEST_SOURCE_DIR
            "/fixtures/http-parser-script-work-failure",
        error, sizeof(error));
}

static bool parser_script_policy_refusal_replay_begin(void)
{
    char error[256] = {0};
    return fetch_trace_replay_begin(
        TILEFINCH_TEST_SOURCE_DIR
            "/fixtures/http-parser-script-policy-refusal",
        error, sizeof(error));
}

static bool parser_script_sri_refusal_replay_begin(void)
{
    char error[256] = {0};
    return fetch_trace_replay_begin(
        TILEFINCH_TEST_SOURCE_DIR
            "/fixtures/http-parser-script-sri-refusal",
        error, sizeof(error));
}

static bool parser_script_work_external_replay_begin(void)
{
    char error[256] = {0};
    return fetch_trace_replay_begin(
        TILEFINCH_TEST_SOURCE_DIR
            "/fixtures/http-parser-script-work-external",
        error, sizeof(error));
}

static bool parser_script_file_overflow_replay_begin(void)
{
    char error[256] = {0};
    return fetch_trace_replay_begin(
        TILEFINCH_TEST_SOURCE_DIR
            "/fixtures/http-parser-script-file-overflow",
        error, sizeof(error));
}

static bool parser_script_work_failed_body_replay_begin(void)
{
    char error[256] = {0};
    return fetch_trace_replay_begin(
        TILEFINCH_TEST_SOURCE_DIR
            "/fixtures/http-parser-script-work-failed-body",
        error, sizeof(error));
}

static bool parser_script_mutation_rebind_replay_begin(void)
{
    char error[256] = {0};
    return fetch_trace_replay_begin(
        TILEFINCH_TEST_SOURCE_DIR
            "/fixtures/http-parser-script-mutation-rebind",
        error, sizeof(error));
}

static bool parser_script_mutation_time_replay_begin(void)
{
    char error[256] = {0};
    return fetch_trace_replay_begin(
        TILEFINCH_TEST_SOURCE_DIR
            "/fixtures/http-parser-script-mutation-time",
        error, sizeof(error));
}

static bool parser_script_late_ssr_replay_begin(void)
{
    char error[256] = {0};
    return fetch_trace_replay_begin(
        TILEFINCH_TEST_SOURCE_DIR "/fixtures/http-parser-script-late-ssr",
        error, sizeof(error));
}

static bool resource_only_shed_replay_begin(void)
{
    char error[256] = {0};
    return fetch_trace_replay_begin(
        TILEFINCH_TEST_SOURCE_DIR "/fixtures/http-resource-only-shed",
        error, sizeof(error));
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

static bool canvas_taint_replay_begin(void)
{
    char error[256] = {0};
    /* Response-keyed replay: this test is about canvas policy, not the
       request-header shape of an image fetch, and it must not go stale when
       that shape changes. */
    return fetch_trace_replay_begin_response_keyed(
        TILEFINCH_TEST_SOURCE_DIR "/fixtures/http-canvas-taint",
        error, sizeof(error));
}

static bool deferred_document_images_partial_failure_replay_begin(void)
{
    char error[256] = {0};
    return fetch_trace_replay_begin(
        TILEFINCH_TEST_SOURCE_DIR
            "/fixtures/http-deferred-document-images-partial-failure",
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
    /* Optional resource caches may be the next allocation. Refuse the
       authoritative layout itself, not whichever allocation happens first. */
    if (probe->fail_after_paint)
        tilefinch_test_faults()->cancel_next_layout_cooperate = true;
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
    tilefinch_test_faults()->cancel_next_layout_cooperate = false;
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

static bool test_static_images_retry_after_transient_idle_failure(void)
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
        && navigation.performance.static_image_layout_adoptions == 0
        && navigation.page.images.stats.loaded == 1
        && navigation.page.deferred_image_count == 2
        && navigation.page.deferred_image_job == NULL
        && navigation_background_resources_pending(&navigation);
    if (first_frame) budget_inject_failure_after(&budget, 0);
    bool admitted = first_frame
        && navigation_run_background_resources(&navigation);
    budget_clear_failure_injection(&budget);
    bool retry_parked = admitted
        && navigation.page.images.stats.loaded == 1
        && navigation.page.deferred_image_job == NULL
        && navigation.page.deferred_image_cursor == 0
        && navigation.page.deferred_image_targets[0].retry_count == 1
        && navigation.performance.background_image_rank_scans == 1
        && navigation.performance.background_image_failures == 1
        && navigation.performance.fast_relayouts
               + navigation.performance.full_relayouts == relayouts_before;
    lxb_dom_node_t *retry_node = retry_parked
        ? navigation.page.deferred_image_targets[0].node : NULL;
    bool scroll_kept_retry = retry_parked
        && navigation_set_scroll(&navigation, 600)
        && navigation_run_background_resources(&navigation)
        && navigation.page.deferred_image_cursor == 0
        && navigation.page.deferred_image_targets[0].node == retry_node
        && navigation.page.deferred_image_targets[0].retry_count == 1
        && navigation.performance.background_image_rank_scans == 1;
    size_t pumps = 0;
    while (scroll_kept_retry
           && navigation_background_resources_pending(&navigation)
           && pumps++ < 32u) {
        if (!navigation_run_background_resources(&navigation)) break;
    }
    size_t relayouts_after = navigation.performance.fast_relayouts
        + navigation.performance.full_relayouts;
    bool ok = scroll_kept_retry && navigation.page.loaded
        && !navigation_background_resources_pending(&navigation)
        && navigation.page.images.stats.loaded == 2
        && navigation.performance.background_images_loaded == 1
        && navigation.performance.background_image_relayouts == 1
        && navigation.performance.background_image_failures == 1
        && navigation.performance.background_image_rank_scans == 2
        && relayouts_after == relayouts_before + 1
        && pumps >= 10u;
    if (!ok) {
        fprintf(stderr,
                "deferred-images ready=%d committed=%d first=%d "
                "admitted=%d retry=%d page=%d pending=%d images=%zu "
                "queue=%zu/%zu job=%d fetch=%zu pumps=%zu batches=%zu "
                "loaded=%zu relayout=%zu failures=%zu error=\"%s\"\n",
                ready, committed, first_frame, admitted, retry_parked,
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

static bool test_visible_lazy_static_image_waits_until_idle(void)
{
    static const char html[] =
        "<!doctype html><title>Lazy image</title>"
        "<style>html,body{margin:0}img{display:block;width:24px;height:24px}"
        "</style><body><h1>Results</h1><a href=/play>"
        "<img loading=lazy src=/hero.svg>Play this result</a></body>";
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
    bool ok = committed && navigation.page.loaded
        && navigation.page.images.stats.loaded == 0
        && navigation.page.deferred_image_count == 1
        && navigation.performance.static_image_layout_adoptions == 1
        && navigation.page.deferred_image_cursor == 0
        && navigation_background_resources_pending(&navigation);
    /* Moving the planning layout must preserve final pixels and geometry,
       not merely save a counted build. Compare against a fresh full build. */
    LayoutDocument reference = {0};
    TileCache planned_cache = {0}, reference_cache = {0};
    size_t frame_bytes = 480u * 272u * sizeof(uint16_t);
    uint16_t *planned_frame = budget_malloc(&budget, frame_bytes);
    uint16_t *reference_frame = budget_malloc(&budget, frame_bytes);
    ok = ok && planned_frame != NULL && reference_frame != NULL
        && layout_build_context_reuse(
            &reference, &budget, &navigation.page.document,
            &navigation.page.stylesheet, NULL, &navigation.page.images,
            &navigation.viewport, NULL)
        && reference.height == navigation.page.layout.height
        && reference.link_count == navigation.page.layout.link_count
        && tile_cache_init(&planned_cache, &budget, &navigation.page.layout, 8)
        && tile_cache_init(&reference_cache, &budget, &reference, 8)
        && tile_cache_set_frame(&planned_cache, planned_frame, 480u * 272u)
        && tile_cache_set_frame(&reference_cache, reference_frame, 480u * 272u)
        && tile_cache_render_frame(&planned_cache, 0, 480, 272, NULL)
        && tile_cache_render_frame(&reference_cache, 0, 480, 272, NULL)
        && memcmp(planned_frame, reference_frame, frame_bytes) == 0;
    tile_cache_destroy(&planned_cache);
    tile_cache_destroy(&reference_cache);
    layout_destroy(&reference);
    budget_free(&budget, planned_frame);
    budget_free(&budget, reference_frame);
    if (!ok) {
        fprintf(stderr,
                "visible-lazy ready=%d committed=%d loaded=%zu "
                "queue=%zu/%zu pending=%d error=\"%s\"\n",
                ready, committed, navigation.page.images.stats.loaded,
                navigation.page.deferred_image_cursor,
                navigation.page.deferred_image_count,
                navigation_background_resources_pending(&navigation),
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

static bool test_visible_lazy_pair_overlaps_transport_and_decode(void)
{
    static const char html[] =
        "<!doctype html><title>Lazy image pair</title>"
        "<style>html,body{margin:0}img{display:block;width:24px;height:24px}"
        "</style><body><img id=first loading=lazy src=/hero.svg>"
        "<img id=second loading=lazy src=/tail.svg></body>";
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
    bool admitted = committed
        && navigation.page.deferred_image_count == 2
        && navigation.page.images.stats.loaded == 0
        && navigation_run_background_resources(&navigation)
        && navigation.page.deferred_image_job != NULL
        && navigation.page.deferred_image_batch_count == 2
        && navigation.page.images.stats.loaded == 0;
    size_t first_publish_pumps = 0;
    while (admitted && navigation.page.images.stats.loaded == 0
           && first_publish_pumps++ < 8u) {
        if (!navigation_run_background_resources(&navigation)) break;
    }
    bool first_published = admitted
        && navigation.page.deferred_image_job != NULL
        && navigation.page.deferred_image_batch_count == 2
        && navigation.page.images.stats.loaded == 1
        && navigation.performance.background_images_loaded == 1
        && navigation.performance.background_image_relayouts == 0
        && navigation.page.deferred_image_publication_age != 0;
    lxb_dom_node_t *root = ready
        ? lxb_dom_interface_node(navigation.page.document.html) : NULL;
    lxb_dom_node_t *first = root == NULL
        ? NULL : test_find_id(root, "first");
    lxb_dom_node_t *second = root == NULL
        ? NULL : test_find_id(root, "second");
    first_published = first_published && first != NULL && second != NULL
        && image_resource_available(images_find_node(
               &navigation.page.images, first))
        && !image_resource_available(images_find_node(
               &navigation.page.images, second));
    size_t pumps = 0;
    while (first_published
           && navigation_background_resources_pending(&navigation)
           && pumps++ < 16u) {
        if (!navigation_run_background_resources(&navigation)) break;
    }
    bool ok = first_published
        && !navigation_background_resources_pending(&navigation)
        && navigation.page.images.stats.loaded == 2
        && navigation.performance.background_images_loaded == 2
        && navigation.performance.background_image_relayouts == 1;
    if (ready) fetch_trace_end();
    if (installed) navigation_destroy(&navigation);
    bool clean = budget.current == 0
        && budget_active_allocations(&budget, NULL) == 0
        && budget_categories_reconcile(&budget);
    if (installed) clean = budget_uninstall_lexbor(&budget) && clean;
    return ok && clean;
}

/* Same-origin policy for canvas readback: pixels from a cross-origin image
   must never reach script. Drawing such an image makes the canvas
   origin-unclean, readback and export then throw SecurityError, and the
   taint follows the pixels into any canvas they are drawn onto. A same-origin
   image leaves every readback available. */
static bool test_cross_origin_image_taints_canvas(void)
{
    static const char html[] =
        "<!doctype html><title>Canvas taint</title>"
        "<style>img{display:block;width:2px;height:2px}</style><body>"
        "<img id=same src=/same.svg>"
        "<img id=cross src=https://taint-other.test/cross.svg>"
        "<script>globalThis.pageReady=1</script></body>";
    static const char probe[] =
        "const probe=(id)=>{const c=document.createElement('canvas');"
        "c.width=2;c.height=2;const g=c.getContext('2d');"
        "g.drawImage(document.getElementById(id),0,0);"
        "let read='ok';try{g.getImageData(0,0,1,1)}catch(e){read=e.name}"
        "let url='ok';try{c.toDataURL()}catch(e){url=e.name}"
        "const copy=document.createElement('canvas');copy.width=2;copy.height=2;"
        "const cg=copy.getContext('2d');cg.drawImage(c,0,0);"
        "let copied='ok';try{cg.getImageData(0,0,1,1)}catch(e){copied=e.name}"
        "return read+'/'+url+'/'+copied};"
        "globalThis.pocSummary=probe('same')+'|'+probe('cross')";
    Budget budget;
    budget_init(&budget, 16 * MIB);
    bool installed = budget_install_lexbor(&budget);
    NavigationSession navigation = {0};
    bool ready = installed && navigation_init(&navigation, &budget, 4)
        && canvas_taint_replay_begin();
    if (ready) {
        navigation_enable_scripts(&navigation, 4 * MIB, 1000);
        navigation_enable_document_scripts(
            &navigation, 4, 64 * 1024, 32 * 1024, 1000);
        navigation_enable_external_resources(
            &navigation, 2, 32 * 1024, 16 * 1024,
            2, 32 * 1024, 16 * 1024, 64 * 1024, 1000);
    }
    uint64_t generation = ready ? navigation_begin(&navigation) : 0;
    bool committed = ready && navigation_commit_html(
        &navigation, generation, "https://taint-page.test/page",
        html, sizeof(html) - 1u, 480, NULL, NULL, true);
    size_t pumps = 0;
    while (committed && navigation_background_resources_pending(&navigation)
           && pumps++ < 16u) {
        if (!navigation_run_background_resources(&navigation)) break;
    }
    bool loaded = committed
        && navigation.page.images.stats.loaded == 2
        && navigation.page.runtime != NULL;
    ScriptResult probe_result;
    memset(&probe_result, 0, sizeof(probe_result));
    bool probed = loaded && script_runtime_evaluate_diagnostic(
        navigation.page.runtime, probe, "<canvas-taint>", &probe_result);
    bool ok = probed
        && strcmp(probe_result.summary,
                  "ok/ok/ok|SecurityError/SecurityError/SecurityError") == 0;
    if (!ok) {
        fprintf(stderr,
                "canvas-taint ready=%d committed=%d loaded=%zu runtime=%d "
                "probed=%d summary=\"%s\" error=\"%s\" last=\"%s\"\n",
                ready, committed, navigation.page.images.stats.loaded,
                navigation.page.runtime != NULL, probed,
                probe_result.summary, probe_result.error,
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

static bool test_deferred_jpeg_decode_publishes_on_later_pump(void)
{
    static const unsigned char probe_png[] = {
        0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a,
        0x00, 0x00, 0x00, 0x0d, 0x49, 0x48, 0x44, 0x52,
        0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01,
        0x08, 0x04, 0x00, 0x00, 0x00, 0xb5, 0x1c, 0x0c,
        0x02, 0x00, 0x00, 0x00, 0x0b, 0x49, 0x44, 0x41,
        0x54, 0x78, 0xda, 0x63, 0x64, 0xf8, 0x0f, 0x00,
        0x01, 0x05, 0x01, 0x01, 0x27, 0x18, 0xe3, 0x66,
        0x00, 0x00, 0x00, 0x00, 0x49, 0x45, 0x4e, 0x44,
        0xae, 0x42, 0x60, 0x82
    };
    static const char html[] =
        "<!doctype html><title>Deferred JPEG</title>"
        "<style>img{display:block;width:24px;height:24px}</style>"
        "<img id=thumb loading=lazy src=\"data:image/jpeg;base64,"
        "/9j/4AAQSkZJRgABAgAAAQABAAD//gAQTGF2YzYyLjI4LjEwMgD/2wBDAAgEBAQE"
        "BAUFBQUFBQYGBgYGBgYGBgYGBgYGBgYHBwcICAgHBwcGBgcHCAgICAkJCQgICAgJ"
        "CQoKCgwMCwsODg4RERT/xABMAAEBAAAAAAAAAAAAAAAAAAAABQEBAQAAAAAAAAAA"
        "AAAAAAAAAAYQAQAAAAAAAAAAAAAAAAAAAAARAQAAAAAAAAAAAAAAAAAAAAD/wAAR"
        "CABAAEADASIAAhEAAxEA/9oADAMBAAIRAxEAPwCUAvEOAAAAAAAAAAAAAAAAAAAA"
        "AAAAAAAAAA//2Q==\">";
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
        &navigation, generation, "https://deferred-images.test/jpeg",
        html, sizeof(html) - 1u, 480, NULL, NULL, true);
    bool saw_pending_decode = false;
    bool saw_busy_probe = false;
    size_t pumps = 0;
    while (committed && navigation_background_resources_pending(&navigation)
           && pumps++ < 16u) {
        size_t loaded_before = navigation.page.images.stats.loaded;
        if (!navigation_run_background_resources(&navigation)) break;
        if (navigation.page.deferred_image_job != NULL
            && loaded_before == 0
            && navigation.page.images.stats.loaded == 0) {
            saw_pending_decode = true;
            int width = 0, height = 0, components = 0;
            bool webp = false;
            if (image_decode_probe_info(
                    &budget, probe_png, sizeof(probe_png),
                    &width, &height, &components, &webp)
                == IMAGE_DECODE_PROBE_BUSY) {
                saw_busy_probe = true;
            }
        }
    }
    lxb_dom_node_t *root = ready
        ? lxb_dom_interface_node(navigation.page.document.html) : NULL;
    lxb_dom_node_t *thumb = root == NULL
        ? NULL : test_find_id(root, "thumb");
    const ImageResource *image = thumb == NULL ? NULL
        : images_find_node(&navigation.page.images, thumb);
    int probe_width = 0, probe_height = 0, probe_components = 0;
    bool probe_webp = false;
    bool probe_retried = image_decode_probe_info(
        &budget, probe_png, sizeof(probe_png), &probe_width, &probe_height,
        &probe_components, &probe_webp) == IMAGE_DECODE_PROBE_SUPPORTED;
    bool ok = committed && saw_pending_decode && saw_busy_probe && probe_retried
        && !navigation_background_resources_pending(&navigation)
        && navigation.page.images.stats.loaded == 1
        && navigation.page.images.stats.downsampled == 1
        && image != NULL && image->pixels != NULL
        && image->source_width == 64 && image->source_height == 64
        && image->width > 0 && image->width <= 24
        && image->height > 0 && image->height <= 24;
    if (!ok) {
        fprintf(stderr,
                "deferred-jpeg ready=%d committed=%d pending=%d busy=%d "
                "retried=%d pumps=%zu "
                "loaded=%zu downsampled=%zu image=%d pixels=%d "
                "source=%dx%d target=%dx%d skipped=%zu error=\"%s\"\n",
                ready, committed, saw_pending_decode, saw_busy_probe,
                probe_retried, pumps,
                navigation.page.images.stats.loaded,
                navigation.page.images.stats.downsampled,
                image != NULL, image != NULL && image->pixels != NULL,
                image == NULL ? 0 : image->source_width,
                image == NULL ? 0 : image->source_height,
                image == NULL ? 0 : image->width,
                image == NULL ? 0 : image->height,
                navigation.page.images.stats.skipped_limit,
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

static bool test_visible_lazy_queue_continues_after_first_pair(void)
{
    static const char html[] =
        "<!doctype html><title>Lazy image queue</title>"
        "<style>html,body{margin:0}img{display:block;width:24px;height:24px}"
        "</style><body><img id=first loading=lazy src=/hero.svg>"
        "<img id=second loading=lazy src=/tail.svg>"
        "<img id=third loading=lazy src=/hero.svg>"
        "<img id=fourth loading=lazy src=/tail.svg></body>";
    Budget budget;
    budget_init(&budget, 16 * MIB);
    bool installed = budget_install_lexbor(&budget);
    NavigationSession navigation = {0};
    bool ready = installed && navigation_init(&navigation, &budget, 4)
        && deferred_document_images_replay_begin();
    if (ready) {
        navigation_enable_external_resources(
            &navigation, 4, 64 * 1024, 16 * 1024,
            4, 64 * 1024, 16 * 1024, 128 * 1024, 1000);
    }
    uint64_t generation = ready ? navigation_begin(&navigation) : 0;
    bool committed = ready && navigation_commit_static_html(
        &navigation, generation, "https://deferred-images.test/page",
        html, sizeof(html) - 1u, 480, NULL, NULL, true);
    bool first_pair = committed
        && navigation.page.deferred_image_count == 4
        && navigation_run_background_resources(&navigation)
        && navigation.page.deferred_image_batch_count == 2;
    size_t first_publish_pumps = 0;
    while (first_pair && navigation.page.images.stats.loaded == 0
           && first_publish_pumps++ < 8u) {
        if (!navigation_run_background_resources(&navigation)) break;
    }
    first_pair = first_pair && navigation.page.images.stats.loaded == 1;
    size_t pumps = 0;
    while (first_pair
           && navigation_background_resources_pending(&navigation)
           && pumps++ < 24u) {
        if (!navigation_run_background_resources(&navigation)) break;
    }
    lxb_dom_node_t *root = ready
        ? lxb_dom_interface_node(navigation.page.document.html) : NULL;
    bool ok = first_pair
        && !navigation_background_resources_pending(&navigation)
        && image_resource_available(images_find_node(
               &navigation.page.images, test_find_id(root, "first")))
        && image_resource_available(images_find_node(
               &navigation.page.images, test_find_id(root, "second")))
        && image_resource_available(images_find_node(
               &navigation.page.images, test_find_id(root, "third")))
        && image_resource_available(images_find_node(
               &navigation.page.images, test_find_id(root, "fourth")));
    if (ready) fetch_trace_end();
    if (installed) navigation_destroy(&navigation);
    bool clean = budget.current == 0
        && budget_active_allocations(&budget, NULL) == 0
        && budget_categories_reconcile(&budget);
    if (installed) clean = budget_uninstall_lexbor(&budget) && clean;
    return ok && clean;
}

static bool test_focused_deferred_image_follows_active_pair(void)
{
    static const char html[] =
        "<!doctype html><title>Focused lazy image</title>"
        "<style>html,body{margin:0}img{display:block;width:24px;height:24px}"
        "</style><body><img id=first loading=lazy src=/hero.svg>"
        "<img id=second loading=lazy src=/tail.svg>"
        "<img id=third loading=lazy src=/hero.svg>"
        "<img id=fourth loading=lazy src=/tail.svg></body>";
    Budget budget;
    budget_init(&budget, 16 * MIB);
    bool installed = budget_install_lexbor(&budget);
    NavigationSession navigation = {0};
    bool ready = installed && navigation_init(&navigation, &budget, 4)
        && deferred_document_images_replay_begin();
    if (ready) {
        navigation_enable_external_resources(
            &navigation, 4, 64 * 1024, 16 * 1024,
            4, 64 * 1024, 16 * 1024, 128 * 1024, 1000);
    }
    uint64_t generation = ready ? navigation_begin(&navigation) : 0;
    bool committed = ready && navigation_commit_static_html(
        &navigation, generation, "https://deferred-images.test/page",
        html, sizeof(html) - 1u, 480, NULL, NULL, true);
    bool first_published = committed
        && navigation.page.deferred_image_count == 4
        && navigation_run_background_resources(&navigation)
        && navigation.page.deferred_image_job != NULL
        && navigation.page.deferred_image_batch_count == 2;
    size_t first_publish_pumps = 0;
    while (first_published && navigation.page.images.stats.loaded == 0
           && first_publish_pumps++ < 8u) {
        if (!navigation_run_background_resources(&navigation)) break;
    }
    first_published = first_published
        && navigation.page.images.stats.loaded == 1
        && navigation.page.deferred_image_job != NULL;
    lxb_dom_node_t *second = first_published
        ? test_find_id(
              lxb_dom_interface_node(navigation.page.document.html), "second")
        : NULL;
    lxb_dom_node_t *third = first_published
        ? test_find_id(
              lxb_dom_interface_node(navigation.page.document.html), "third")
        : NULL;
    lxb_dom_node_t *fourth = first_published
        ? test_find_id(
              lxb_dom_interface_node(navigation.page.document.html), "fourth")
        : NULL;
    lxb_dom_node_t *first = first_published
        ? test_find_id(
              lxb_dom_interface_node(navigation.page.document.html), "first")
        : NULL;
    bool prioritized = first_published && second != NULL && third != NULL
        && fourth != NULL && first != NULL
        && navigation_prioritize_deferred_image(&navigation, fourth);
    bool reprioritized = prioritized
        && navigation_prioritize_deferred_image(&navigation, third);
    bool ok = reprioritized
        && navigation.page.deferred_image_job != NULL
        && navigation.page.deferred_image_batch_count == 2
        && navigation.page.deferred_image_cursor == 0
        && navigation.page.deferred_image_targets[0].node == first
        && navigation.page.deferred_image_targets[1].node == second
        && navigation.page.deferred_image_targets[2].node == third
        && navigation.page.deferred_image_targets[3].node == fourth
        && navigation.page.images.stats.loaded == 1
        && image_resource_available(images_find_node(
               &navigation.page.images, first));
    if (ready) fetch_trace_end();
    if (installed) navigation_destroy(&navigation);
    bool clean = budget.current == 0
        && budget_active_allocations(&budget, NULL) == 0
        && budget_categories_reconcile(&budget);
    if (installed) clean = budget_uninstall_lexbor(&budget) && clean;
    return ok && clean;
}

static bool test_deferred_image_focus_rotation_preserves_order(void)
{
    static const char html[] =
        "<!doctype html><title>Stable focused order</title>"
        "<style>html,body{margin:0}img{display:block;width:24px;height:24px}"
        "</style><body><img id=first loading=lazy src=/one.svg>"
        "<img id=second loading=lazy src=/two.svg>"
        "<img id=third loading=lazy src=/three.svg>"
        "<img id=fourth loading=lazy src=/four.svg></body>";
    Budget budget;
    budget_init(&budget, 16 * MIB);
    bool installed = budget_install_lexbor(&budget);
    NavigationSession navigation = {0};
    bool ready = installed && navigation_init(&navigation, &budget, 4);
    if (ready) {
        navigation_enable_external_resources(
            &navigation, 4, 64 * 1024, 16 * 1024,
            4, 64 * 1024, 16 * 1024, 128 * 1024, 1000);
    }
    uint64_t generation = ready ? navigation_begin(&navigation) : 0;
    bool committed = ready && navigation_commit_static_html(
        &navigation, generation, "https://stable-order.test/page",
        html, sizeof(html) - 1u, 480, NULL, NULL, true);
    lxb_dom_node_t *root = committed
        ? lxb_dom_interface_node(navigation.page.document.html) : NULL;
    lxb_dom_node_t *first = root == NULL
        ? NULL : test_find_id(root, "first");
    lxb_dom_node_t *second = root == NULL
        ? NULL : test_find_id(root, "second");
    lxb_dom_node_t *third = root == NULL
        ? NULL : test_find_id(root, "third");
    lxb_dom_node_t *fourth = root == NULL
        ? NULL : test_find_id(root, "fourth");
    bool initial = committed && first != NULL && second != NULL
        && third != NULL && fourth != NULL
        && navigation.page.deferred_image_count == 4
        && navigation.page.deferred_image_cursor == 0
        && navigation.page.deferred_image_targets[0].node == first
        && navigation.page.deferred_image_targets[1].node == second
        && navigation.page.deferred_image_targets[2].node == third
        && navigation.page.deferred_image_targets[3].node == fourth;
    bool promoted_second = initial
        && navigation_prioritize_deferred_image(&navigation, second)
        && navigation.page.deferred_image_targets[0].node == second
        && navigation.page.deferred_image_targets[1].node == first
        && navigation.page.deferred_image_targets[2].node == third
        && navigation.page.deferred_image_targets[3].node == fourth;
    /* Model the singular queue transition after item 2 commits. The image
       loader's completion mechanics are covered separately; this test pins
       only the ordering reducer so no transport timing can obscure it. */
    if (promoted_second) navigation.page.deferred_image_cursor = 1;
    bool promoted_third = promoted_second
        && navigation_prioritize_deferred_image(&navigation, third);
    bool ok = promoted_third
        && navigation.page.deferred_image_cursor == 1
        && navigation.page.deferred_image_targets[0].node == second
        && navigation.page.deferred_image_targets[1].node == third
        && navigation.page.deferred_image_targets[2].node == first
        && navigation.page.deferred_image_targets[3].node == fourth;
    if (installed) navigation_destroy(&navigation);
    bool clean = budget.current == 0
        && budget_active_allocations(&budget, NULL) == 0
        && budget_categories_reconcile(&budget);
    if (installed) clean = budget_uninstall_lexbor(&budget) && clean;
    return ok && clean;
}

static bool test_visible_lazy_pair_retries_only_failed_suffix(void)
{
    static const char html[] =
        "<!doctype html><title>Lazy image partial failure</title>"
        "<style>html,body{margin:0}img{display:block;width:24px;height:24px}"
        "</style><body><img loading=lazy src=/hero.svg>"
        "<img loading=lazy src=/tail.svg></body>";
    Budget budget;
    budget_init(&budget, 16 * MIB);
    bool installed = budget_install_lexbor(&budget);
    NavigationSession navigation = {0};
    bool ready = installed && navigation_init(&navigation, &budget, 4)
        && deferred_document_images_partial_failure_replay_begin();
    if (ready) {
        navigation_enable_external_resources(
            &navigation, 2, 32 * 1024, 16 * 1024,
            2, 32 * 1024, 16 * 1024, 64 * 1024, 1000);
    }
    uint64_t generation = ready ? navigation_begin(&navigation) : 0;
    bool committed = ready && navigation_commit_static_html(
        &navigation, generation, "https://deferred-images.test/page",
        html, sizeof(html) - 1u, 480, NULL, NULL, true);
    bool first_published = false;
    bool suffix_failed = false;
    size_t pumps = 0;
    while (committed && navigation_background_resources_pending(&navigation)
           && pumps++ < 24u) {
        if (!navigation_run_background_resources(&navigation)) break;
        if (navigation.page.images.stats.loaded == 1
            && navigation.page.deferred_image_cursor == 0) {
            first_published = true;
        }
        if (navigation.page.deferred_image_cursor == 1
            && navigation.page.deferred_image_targets != NULL
            && navigation.page.deferred_image_targets[1].retry_count == 1) {
            suffix_failed = true;
            break;
        }
    }
    bool ok = first_published && suffix_failed
        && navigation.page.images.stats.loaded == 1
        && image_resource_available(images_find_node(
               &navigation.page.images,
               navigation.page.deferred_image_targets[0].node))
        && !image_resource_available(images_find_node(
               &navigation.page.images,
               navigation.page.deferred_image_targets[1].node));
    if (ready) fetch_trace_end();
    if (installed) navigation_destroy(&navigation);
    bool clean = budget.current == 0
        && budget_active_allocations(&budget, NULL) == 0
        && budget_categories_reconcile(&budget);
    if (installed) clean = budget_uninstall_lexbor(&budget) && clean;
    return ok && clean;
}

static bool test_failed_visible_image_retries_after_first_paint(void)
{
    static const char html[] =
        "<!doctype html><title>Retry visible image</title>"
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
        fetch_inject_failure_once(FETCH_INJECT_TLS);
    }
    uint64_t generation = ready ? navigation_begin(&navigation) : 0;
    bool committed = ready && navigation_commit_static_html(
        &navigation, generation, "https://deferred-images.test/page",
        html, sizeof(html) - 1u, 480, NULL, NULL, true);
    fetch_inject_failure_once(FETCH_INJECT_NONE);
    bool first_frame = committed && navigation.page.loaded
        && navigation.page.images.stats.loaded == 0
        && navigation.page.images.stats.failed == 1
        && navigation.page.deferred_image_count == 2
        && navigation_background_resources_pending(&navigation);
    size_t pumps = 0;
    while (first_frame
           && navigation_background_resources_pending(&navigation)
           && pumps++ < 40u) {
        if (!navigation_run_background_resources(&navigation)) break;
    }
    bool ok = first_frame && navigation.page.loaded
        && !navigation_background_resources_pending(&navigation)
        && navigation.page.images.stats.loaded == 2
        && navigation.performance.background_images_loaded == 2
        && navigation.performance.background_image_relayouts == 2
        && navigation.performance.background_image_failures == 0;
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

static size_t parser_checkpoint_reason_count(
    const NavigationPerformance *performance,
    NavigationParserCheckpointTestFault fault)
{
    if (performance == NULL) return 0;
    switch (fault) {
    case NAVIGATION_TEST_PARSER_CHECKPOINT_METADATA:
        return performance->parser_checkpoint_metadata_refusals;
    case NAVIGATION_TEST_PARSER_CHECKPOINT_FINGERPRINT:
        return performance->parser_checkpoint_fingerprint_refusals;
    case NAVIGATION_TEST_PARSER_CHECKPOINT_STYLESHEET:
        return performance->parser_checkpoint_stylesheet_refusals;
    case NAVIGATION_TEST_PARSER_CHECKPOINT_MUTATION:
        return performance->parser_checkpoint_mutation_refusals;
    case NAVIGATION_TEST_PARSER_CHECKPOINT_NONE:
    case NAVIGATION_TEST_PARSER_FEED_HARD_FAILURE:
    default:
        return 0;
    }
}

static bool test_parser_checkpoint_refusals_preserve_server_dom(void)
{
    static const NavigationParserCheckpointTestFault faults[] = {
        NAVIGATION_TEST_PARSER_CHECKPOINT_METADATA,
        NAVIGATION_TEST_PARSER_CHECKPOINT_FINGERPRINT,
        NAVIGATION_TEST_PARSER_CHECKPOINT_STYLESHEET,
        NAVIGATION_TEST_PARSER_CHECKPOINT_MUTATION
    };
    bool all_ok = true;
    for (size_t fault_index = 0;
         fault_index < sizeof(faults) / sizeof(faults[0]); fault_index++) {
        Budget budget;
        budget_init(&budget, 16 * MIB);
        bool installed = budget_install_lexbor(&budget);
        BrowserSession browser = {0};
        NavigationSession navigation = {0};
        bool browser_ready = installed
            && browser_session_init(&browser, &budget, 64 * 1024);
        bool ready = browser_ready
            && navigation_init(&navigation, &budget, 2)
            && parser_checkpoint_replay_begin();
        if (ready) {
            navigation_attach_browser_session(&navigation, &browser);
            navigation_enable_scripts(&navigation, 4 * MIB, 1000);
            navigation_enable_document_scripts(
                &navigation, 4, 64 * 1024, 64 * 1024, 1000);
            navigation_set_stream_delivery(
                &navigation, 37, 0, 0, 0, 0, 0);
            navigation_test_refuse_next_parser_checkpoint(
                faults[fault_index]);
        }
        uint64_t generation = ready ? navigation_begin(&navigation) : 0;
        bool loaded = ready && navigation_load_url(
            &navigation, generation, "https://checkpoint.test/document",
            4096, 1000, 480, NULL, NULL, true);
        lxb_dom_node_t *content = loaded ? test_find_id(
            lxb_dom_interface_node(navigation.page.document.html),
            "content") : NULL;
        lxb_dom_node_t *tail = loaded ? test_find_id(
            lxb_dom_interface_node(navigation.page.document.html),
            "tail") : NULL;
        lxb_dom_node_t *body = loaded
            ? document_body_node(&navigation.page.document) : NULL;
        size_t script_ran_length = 0;
        const char *script_ran = body == NULL ? NULL : document_attribute(
            body, "data-script-ran", &script_ran_length);
        bool ok = loaded && navigation.page.loaded
            && content != NULL && tail != NULL && script_ran == NULL
            && navigation.page.runtime == NULL
            && navigation.last_error[0] == '\0'
            && navigation.performance.optional_work_sheds == 1
            && navigation.performance.parser_checkpoint_soft_refusals == 1
            && parser_checkpoint_reason_count(
                   &navigation.performance, faults[fault_index]) == 1
            && strstr(navigation.page.script_result.summary,
                      "JavaScript stopped") != NULL;
        if (!ok) {
            fprintf(stderr,
                    "parser checkpoint fault=%d ready=%d loaded=%d page=%d "
                    "content=%p tail=%p script=%.*s runtime=%p sheds=%zu "
                    "soft=%zu reason=%zu summary=\"%s\" error=\"%s\"\n",
                    (int) faults[fault_index], ready ? 1 : 0,
                    loaded ? 1 : 0, navigation.page.loaded ? 1 : 0,
                    (void *) content, (void *) tail,
                    (int) script_ran_length,
                    script_ran == NULL ? "" : script_ran,
                    (void *) navigation.page.runtime,
                    navigation.performance.optional_work_sheds,
                    navigation.performance.parser_checkpoint_soft_refusals,
                    parser_checkpoint_reason_count(
                        &navigation.performance, faults[fault_index]),
                    navigation.page.script_result.summary,
                    navigation.last_error);
        }
        if (ready) fetch_trace_end();
        if (installed) navigation_destroy(&navigation);
        if (browser_ready) browser_session_destroy(&browser);
        bool clean = budget.current == 0
            && budget_active_allocations(&budget, NULL) == 0
            && budget_categories_reconcile(&budget);
        if (installed) clean = budget_uninstall_lexbor(&budget) && clean;
        all_ok = all_ok && ok && clean;
    }

    Budget budget;
    budget_init(&budget, 16 * MIB);
    bool installed = budget_install_lexbor(&budget);
    NavigationSession navigation = {0};
    bool ready = installed && navigation_init(&navigation, &budget, 2)
        && parser_checkpoint_replay_begin();
    if (ready) {
        navigation_set_stream_delivery(&navigation, 37, 0, 0, 0, 0, 0);
        navigation_test_refuse_next_parser_checkpoint(
            NAVIGATION_TEST_PARSER_FEED_HARD_FAILURE);
    }
    uint64_t generation = ready ? navigation_begin(&navigation) : 0;
    bool loaded = ready && navigation_load_url(
        &navigation, generation, "https://checkpoint.test/document",
        4096, 1000, 480, NULL, NULL, true);
    bool hard_ok = ready && !loaded && !navigation.page.loaded
        && navigation.performance.parser_checkpoint_soft_refusals == 0
        && strstr(navigation.last_error,
                  "HTML parser rejected staged response body") != NULL;
    if (!hard_ok) {
        fprintf(stderr,
                "hard parser checkpoint ready=%d loaded=%d page=%d soft=%zu "
                "error=\"%s\"\n",
                ready ? 1 : 0, loaded ? 1 : 0,
                navigation.page.loaded ? 1 : 0,
                navigation.performance.parser_checkpoint_soft_refusals,
                navigation.last_error);
    }
    if (ready) fetch_trace_end();
    if (installed) navigation_destroy(&navigation);
    bool clean = budget.current == 0
        && budget_active_allocations(&budget, NULL) == 0
        && budget_categories_reconcile(&budget);
    if (installed) clean = budget_uninstall_lexbor(&budget) && clean;
    return all_ok && hard_ok && clean;
}

static bool test_parser_script_stage_circuit_preserves_actions(void)
{
    bool all_ok = true;
    for (size_t work_case = 0; work_case < 3u; work_case++) {
        Budget budget;
        budget_init(&budget, 16 * MIB);
        bool installed = budget_install_lexbor(&budget);
        BrowserSession browser = {0};
        NavigationSession navigation = {0};
        bool browser_ready = installed
            && browser_session_init(&browser, &budget, 64 * 1024);
        bool ready = browser_ready
            && navigation_init(&navigation, &budget, 2)
            && parser_script_circuit_replay_begin(work_case == 1u);
        if (ready) {
            navigation_attach_browser_session(&navigation, &browser);
            navigation_enable_scripts(&navigation, 4 * MIB, 1000);
            navigation_enable_document_scripts(
                &navigation, 8, work_case == 1u ? 512u : 64 * 1024u,
                work_case == 1u ? 512u : 64 * 1024u, 1000);
            navigation_set_stream_delivery(
                &navigation, 43, 0, 0, 0, 0, 0);
        }
        uint64_t generation = ready ? navigation_begin(&navigation) : 0;
        const char *url = work_case == 1u
            ? "https://script-work-circuit.test/document"
            : "https://script-circuit.test/document";
        if (work_case == 1u) {
            navigation_test_set_parser_script_stage_work_limit(256u);
        } else if (work_case == 2u) {
            navigation_test_set_parser_script_stage_elapsed_us(8000000u);
        }
        bool loaded = ready && navigation_load_url(
            &navigation, generation, url, 4096, 1000, 480,
            NULL, NULL, true);
        FetchTraceReplayStats replay_stats = {0};
        bool replay_stats_ready = ready
            && fetch_trace_replay_stats(&replay_stats);
        navigation_test_set_parser_script_stage_work_limit(0u);
        navigation_test_set_parser_script_stage_elapsed_us(0u);
        lxb_dom_node_t *root = loaded
            ? lxb_dom_interface_node(navigation.page.document.html) : NULL;
        lxb_dom_node_t *body = loaded
            ? document_body_node(&navigation.page.document) : NULL;
        lxb_dom_node_t *link = root == NULL
            ? NULL : test_find_id(root, "useful-link");
        lxb_dom_node_t *form = root == NULL
            ? NULL : test_find_id(root, "usable-form");
        size_t first_length = 0, second_length = 0, later_length = 0;
        const char *first = body == NULL ? NULL : document_attribute(
            body, work_case == 1u ? "data-first-ran" : "data-fourth-ran",
            &first_length);
        const char *second = body == NULL || work_case != 1u ? NULL
            : document_attribute(body, "data-second-ran", &second_length);
        const char *later = body == NULL ? NULL : document_attribute(
            body, work_case == 1u ? "data-third-ran" : "data-fifth-ran",
            &later_length);
        bool execution_state = work_case == 1u
            ? first != NULL && first_length == 3u
                /* The second script would cross the cumulative source cap,
                   so it is refused before compilation rather than allowing
                   one whole-file overshoot. */
                && second == NULL && second_length == 0u
                && later == NULL && navigation.script_failed == 0u
            : first == NULL && later == NULL
                && navigation.script_failed >= (work_case == 0u ? 3u : 1u);
        const char *expected_summary = work_case == 0u
            ? "repeated failures" : work_case == 1u
                ? "bounded parser work" : "parser time limit";
        bool ok = loaded && navigation.page.loaded
            && link != NULL && form != NULL
            && navigation.page.layout.link_count != 0u
            && navigation.page.layout.control_count != 0u
            && execution_state
            && navigation.page.runtime == NULL
            && navigation.page.script_degradation_observed
            && navigation.script_skipped_pressure != 0u
            && navigation.last_error[0] == '\0'
            && navigation.performance.optional_work_sheds == 1u
            && navigation.performance.parser_checkpoint_soft_refusals == 1u
            && navigation.performance.parser_checkpoint_script_refusals == 1u
            && navigation.performance.parser_script_stage_breakers == 1u
            && navigation.performance.parser_script_stage_work != 0u
            && navigation.performance.parser_script_stage_skipped != 0u
            /* The external script after the third failure has no replay
               record.  A mere soft failure could otherwise hide an
               accidental fetch, so pin the request ledger as well. */
            && (work_case != 0u
                || (replay_stats_ready
                    && replay_stats.request_count == 1u
                    && replay_stats.matched_request_count == 1u
                    && replay_stats.unmatched_request_count == 0u))
            && (work_case == 0u
                    ? navigation.performance
                          .parser_script_stage_failure_breakers == 1u
                        && navigation.performance
                               .parser_script_stage_work_breakers == 0u
                        && strstr(navigation.page.script_result.error,
                                  "first expected failure") != NULL
                    : work_case == 1u
                        ? navigation.performance
                          .parser_script_stage_failure_breakers == 0u
                        && navigation.performance
                               .parser_script_stage_work_breakers == 1u
                        : navigation.performance
                              .parser_script_stage_time_breakers == 1u
                            && strstr(navigation.page.script_result.error,
                                      "first expected failure") != NULL)
            && strstr(navigation.page.script_result.summary,
                      "Limited page:") != NULL
            && strstr(navigation.page.script_result.summary,
                      expected_summary) != NULL;
        if (!ok) {
            fprintf(stderr,
                    "parser script circuit work=%zu ready=%d loaded=%d "
                    "page=%d link=%p form=%p links=%zu controls=%zu "
                    "first=%.*s second=%.*s later=%.*s failed=%zu "
                    "skipped=%zu runtime=%p degraded=%d sheds=%zu soft=%zu "
                    "script-refusals=%zu stage=%zu/%zu/%zu reasons=%zu/%zu/%zu "
                    "summary=\"%s\" script-error=\"%s\" error=\"%s\"\n",
                    work_case, ready ? 1 : 0, loaded ? 1 : 0,
                    navigation.page.loaded ? 1 : 0, (void *) link,
                    (void *) form, navigation.page.layout.link_count,
                    navigation.page.layout.control_count,
                    (int) first_length, first == NULL ? "" : first,
                    (int) second_length, second == NULL ? "" : second,
                    (int) later_length, later == NULL ? "" : later,
                    navigation.script_failed,
                    navigation.script_skipped_pressure,
                    (void *) navigation.page.runtime,
                    navigation.page.script_degradation_observed ? 1 : 0,
                    navigation.performance.optional_work_sheds,
                    navigation.performance.parser_checkpoint_soft_refusals,
                    navigation.performance.parser_checkpoint_script_refusals,
                    navigation.performance.parser_script_stage_work,
                    navigation.performance.parser_script_stage_skipped,
                    navigation.performance.parser_script_stage_breakers,
                    navigation.performance
                        .parser_script_stage_failure_breakers,
                    navigation.performance.parser_script_stage_time_breakers,
                    navigation.performance.parser_script_stage_work_breakers,
                    navigation.page.script_result.summary,
                    navigation.page.script_result.error,
                    navigation.last_error);
        }
        if (ready) fetch_trace_end();
        if (installed) navigation_destroy(&navigation);
        if (browser_ready) browser_session_destroy(&browser);
        bool clean = budget.current == 0
            && budget_active_allocations(&budget, NULL) == 0
            && budget_categories_reconcile(&budget);
        if (installed) clean = budget_uninstall_lexbor(&budget) && clean;
        all_ok = all_ok && ok && clean;
    }

    /* CSP/policy refusals remain ordinary failed-script diagnostics, but
       author policy is not an execution failure. Three refused scripts must
       therefore leave the realm alive for a later nonce-authorized script. */
    {
        Budget budget;
        budget_init(&budget, 16 * MIB);
        bool installed = budget_install_lexbor(&budget);
        BrowserSession browser = {0};
        NavigationSession navigation = {0};
        bool browser_ready = installed
            && browser_session_init(&browser, &budget, 64 * 1024);
        bool ready = browser_ready
            && navigation_init(&navigation, &budget, 2)
            && parser_script_policy_refusal_replay_begin();
        if (ready) {
            navigation_attach_browser_session(&navigation, &browser);
            navigation_enable_scripts(&navigation, 4 * MIB, 1000);
            navigation_enable_document_scripts(
                &navigation, 8, 64 * 1024u, 64 * 1024u, 1000);
            navigation_set_stream_delivery(
                &navigation, 43, 0, 0, 0, 0, 0);
        }
        uint64_t generation = ready ? navigation_begin(&navigation) : 0;
        bool loaded = ready && navigation_load_url(
            &navigation, generation,
            "https://script-policy-refusal.test/document",
            4096, 1000, 480, NULL, NULL, true);
        lxb_dom_node_t *root = loaded
            ? lxb_dom_interface_node(navigation.page.document.html) : NULL;
        lxb_dom_node_t *body = loaded
            ? document_body_node(&navigation.page.document) : NULL;
        size_t allowed_length = 0, blocked_length = 0;
        const char *allowed = body == NULL ? NULL : document_attribute(
            body, "data-allowed-ran", &allowed_length);
        const char *blocked = body == NULL ? NULL : document_attribute(
            body, "data-blocked-one", &blocked_length);
        FetchTraceReplayStats replay_stats = {0};
        bool replay_stats_ready = ready
            && fetch_trace_replay_stats(&replay_stats);
        bool policy_ok = loaded && navigation.page.loaded
            && root != NULL && test_find_id(root, "useful-link") != NULL
            && allowed != NULL && allowed_length == 3u
            && blocked == NULL && blocked_length == 0u
            && navigation.script_failed >= 3u
            && navigation.page.runtime != NULL
            && navigation.page.script_degradation_observed
            && navigation.performance.parser_script_stage_breakers == 0u
            && navigation.performance
                   .parser_script_stage_failure_breakers == 0u
            && navigation.performance.optional_work_sheds == 0u
            && navigation.last_error[0] == '\0'
            && replay_stats_ready && replay_stats.request_count == 1u
            && replay_stats.matched_request_count == 1u
            && replay_stats.unmatched_request_count == 0u;
        if (!policy_ok) {
            fprintf(stderr,
                    "parser policy attribution ready=%d loaded=%d page=%d "
                    "allowed=%.*s blocked=%.*s failed=%zu runtime=%p "
                    "degraded=%d breaker=%zu/%zu sheds=%zu "
                    "requests=%zu/%zu/%zu error=\"%s\"\n",
                    ready ? 1 : 0, loaded ? 1 : 0,
                    navigation.page.loaded ? 1 : 0,
                    (int) allowed_length, allowed == NULL ? "" : allowed,
                    (int) blocked_length, blocked == NULL ? "" : blocked,
                    navigation.script_failed, (void *) navigation.page.runtime,
                    navigation.page.script_degradation_observed ? 1 : 0,
                    navigation.performance.parser_script_stage_breakers,
                    navigation.performance
                        .parser_script_stage_failure_breakers,
                    navigation.performance.optional_work_sheds,
                    replay_stats.request_count,
                    replay_stats.matched_request_count,
                    replay_stats.unmatched_request_count,
                    navigation.last_error);
        }
        if (ready) fetch_trace_end();
        if (installed) navigation_destroy(&navigation);
        if (browser_ready) browser_session_destroy(&browser);
        bool clean = budget.current == 0
            && budget_active_allocations(&budget, NULL) == 0
            && budget_categories_reconcile(&budget);
        if (installed) clean = budget_uninstall_lexbor(&budget) && clean;
        all_ok = all_ok && policy_ok && clean;
    }

    /* Response-stage integrity mismatches are policy refusals too. They may
       diagnose each element as failed, but must not consume the consecutive
       author-execution failure budget and suppress a later valid script. */
    {
        Budget budget;
        budget_init(&budget, 16 * MIB);
        bool installed = budget_install_lexbor(&budget);
        BrowserSession browser = {0};
        NavigationSession navigation = {0};
        bool browser_ready = installed
            && browser_session_init(&browser, &budget, 64 * 1024);
        bool ready = browser_ready
            && navigation_init(&navigation, &budget, 2)
            && parser_script_sri_refusal_replay_begin();
        if (ready) {
            navigation_attach_browser_session(&navigation, &browser);
            navigation_enable_scripts(&navigation, 4 * MIB, 1000);
            navigation_enable_document_scripts(
                &navigation, 8, 64 * 1024u, 64 * 1024u, 1000);
            navigation_set_stream_delivery(
                &navigation, 43, 0, 0, 0, 0, 0);
        }
        uint64_t generation = ready ? navigation_begin(&navigation) : 0;
        bool loaded = ready && navigation_load_url(
            &navigation, generation,
            "https://script-sri-refusal.test/document",
            4096, 1000, 480, NULL, NULL, true);
        FetchTraceReplayStats replay_stats = {0};
        bool replay_stats_ready = ready
            && fetch_trace_replay_stats(&replay_stats);
        bool policy_ok = loaded && navigation.page.loaded
            && strcmp(navigation.page.script_result.summary,
                      "SCRIPT-PRELOAD-OK") == 0
            && navigation.script_failed >= 3u
            && navigation.script_loaded == 1u
            && navigation.page.runtime != NULL
            && navigation.page.script_degradation_observed
            && navigation.performance.parser_script_stage_breakers == 0u
            && navigation.performance
                   .parser_script_stage_failure_breakers == 0u
            && navigation.performance.optional_work_sheds == 0u
            && navigation.last_error[0] == '\0'
            && replay_stats_ready && replay_stats.request_count == 5u
            && replay_stats.matched_request_count == 5u
            && replay_stats.unmatched_request_count == 0u;
        if (!policy_ok) {
            fprintf(stderr,
                    "parser SRI attribution ready=%d loaded=%d page=%d "
                    "summary=\"%s\" loaded-scripts=%zu failed=%zu runtime=%p "
                    "degraded=%d breaker=%zu/%zu sheds=%zu "
                    "requests=%zu/%zu/%zu error=\"%s\"\n",
                    ready ? 1 : 0, loaded ? 1 : 0,
                    navigation.page.loaded ? 1 : 0,
                    navigation.page.script_result.summary,
                    navigation.script_loaded, navigation.script_failed,
                    (void *) navigation.page.runtime,
                    navigation.page.script_degradation_observed ? 1 : 0,
                    navigation.performance.parser_script_stage_breakers,
                    navigation.performance
                        .parser_script_stage_failure_breakers,
                    navigation.performance.optional_work_sheds,
                    replay_stats.request_count,
                    replay_stats.matched_request_count,
                    replay_stats.unmatched_request_count,
                    navigation.last_error);
        }
        if (ready) fetch_trace_end();
        if (installed) navigation_destroy(&navigation);
        if (browser_ready) browser_session_destroy(&browser);
        bool clean = budget.current == 0
            && budget_active_allocations(&budget, NULL) == 0
            && budget_categories_reconcile(&budget);
        if (installed) clean = budget_uninstall_lexbor(&budget) && clean;
        all_ok = all_ok && policy_ok && clean;
    }

    /* An external body which is larger than the caller's narrowed remaining
       parser-work allowance is a typed exhaustion, whether it arrives from a
       raw-scanner preload/cache or directly from replay transport. It opens
       the work circuit before the following inline script can run. */
    {
        Budget budget;
        budget_init(&budget, 16 * MIB);
        bool installed = budget_install_lexbor(&budget);
        BrowserSession browser = {0};
        NavigationSession navigation = {0};
        bool browser_ready = installed
            && browser_session_init(&browser, &budget, 64 * 1024);
        bool ready = browser_ready
            && navigation_init(&navigation, &budget, 2)
            && parser_script_work_external_replay_begin();
        if (ready) {
            navigation_attach_browser_session(&navigation, &browser);
            navigation_enable_scripts(&navigation, 4 * MIB, 1000);
            navigation_enable_document_scripts(
                &navigation, 8, 512u, 512u, 1000);
            navigation_set_stream_delivery(
                &navigation, 43, 0, 0, 0, 0, 0);
        }
        uint64_t generation = ready ? navigation_begin(&navigation) : 0;
        navigation_test_set_parser_script_stage_work_limit(180u);
        bool loaded = ready && navigation_load_url(
            &navigation, generation,
            "https://script-work-external.test/document",
            4096, 1000, 480, NULL, NULL, true);
        navigation_test_set_parser_script_stage_work_limit(0u);
        lxb_dom_node_t *root = loaded
            ? lxb_dom_interface_node(navigation.page.document.html) : NULL;
        lxb_dom_node_t *body = loaded
            ? document_body_node(&navigation.page.document) : NULL;
        size_t first_length = 0, external_length = 0, tail_length = 0;
        const char *first = body == NULL ? NULL : document_attribute(
            body, "data-first-ran", &first_length);
        const char *external = body == NULL ? NULL : document_attribute(
            body, "data-external-ran", &external_length);
        const char *tail = body == NULL ? NULL : document_attribute(
            body, "data-tail-ran", &tail_length);
        FetchTraceReplayStats replay_stats = {0};
        bool replay_stats_ready = ready
            && fetch_trace_replay_stats(&replay_stats);
        bool work_ok = loaded && navigation.page.loaded
            && root != NULL && test_find_id(root, "useful-link") != NULL
            && first != NULL && first_length == 3u
            && external == NULL && external_length == 0u
            && tail == NULL && tail_length == 0u
            && navigation.page.runtime == NULL
            && navigation.page.script_degradation_observed
            && navigation.script_skipped_quota != 0u
            && navigation.performance.parser_script_stage_work == 135u
            && navigation.performance.parser_script_stage_breakers == 1u
            && navigation.performance.parser_script_stage_work_breakers == 1u
            && navigation.performance
                   .parser_script_stage_failure_breakers == 0u
            && navigation.performance.optional_work_sheds == 1u
            && navigation.last_error[0] == '\0'
            && strstr(navigation.page.script_result.summary,
                      "bounded parser work") != NULL
            && replay_stats_ready && replay_stats.request_count == 2u
            && replay_stats.matched_request_count == 2u
            && replay_stats.unmatched_request_count == 0u;
        if (!work_ok) {
            fprintf(stderr,
                    "parser external work ready=%d loaded=%d page=%d "
                    "first=%.*s external=%.*s tail=%.*s runtime=%p "
                    "degraded=%d skipped=%zu work=%zu breaker=%zu/%zu/%zu "
                    "sheds=%zu requests=%zu/%zu/%zu summary=\"%s\" "
                    "error=\"%s\"\n",
                    ready ? 1 : 0, loaded ? 1 : 0,
                    navigation.page.loaded ? 1 : 0,
                    (int) first_length, first == NULL ? "" : first,
                    (int) external_length,
                    external == NULL ? "" : external,
                    (int) tail_length, tail == NULL ? "" : tail,
                    (void *) navigation.page.runtime,
                    navigation.page.script_degradation_observed ? 1 : 0,
                    navigation.script_skipped_quota,
                    navigation.performance.parser_script_stage_work,
                    navigation.performance.parser_script_stage_breakers,
                    navigation.performance.parser_script_stage_work_breakers,
                    navigation.performance
                        .parser_script_stage_failure_breakers,
                    navigation.performance.optional_work_sheds,
                    replay_stats.request_count,
                    replay_stats.matched_request_count,
                    replay_stats.unmatched_request_count,
                    navigation.page.script_result.summary,
                    navigation.last_error);
        }
        if (ready) fetch_trace_end();
        if (installed) navigation_destroy(&navigation);
        if (browser_ready) browser_session_destroy(&browser);
        bool clean = budget.current == 0
            && budget_active_allocations(&budget, NULL) == 0
            && budget_categories_reconcile(&budget);
        if (installed) clean = budget_uninstall_lexbor(&budget) && clean;
        all_ok = all_ok && work_ok && clean;
    }

    /* A response which exceeds the ordinary per-file ceiling skips that
       element only. It is not evidence that the navigation-wide parser-work
       budget is exhausted, so a later small script must still execute. */
    {
        Budget budget;
        budget_init(&budget, 16 * MIB);
        bool installed = budget_install_lexbor(&budget);
        BrowserSession browser = {0};
        NavigationSession navigation = {0};
        bool browser_ready = installed
            && browser_session_init(&browser, &budget, 64 * 1024);
        bool ready = browser_ready
            && navigation_init(&navigation, &budget, 2)
            && parser_script_file_overflow_replay_begin();
        if (ready) {
            navigation_attach_browser_session(&navigation, &browser);
            navigation_enable_scripts(&navigation, 4 * MIB, 1000);
            navigation_enable_document_scripts(
                &navigation, 8, 512u, 64u, 1000);
            navigation_set_stream_delivery(
                &navigation, 43, 0, 0, 0, 0, 0);
        }
        uint64_t generation = ready ? navigation_begin(&navigation) : 0;
        /* Equality is deliberately not cumulative narrowing: the first body
           exceeds the ordinary 64-byte file cap, while the following inline
           script still fits in the untouched 64-byte stage allowance. */
        navigation_test_set_parser_script_stage_work_limit(64u);
        bool loaded = ready && navigation_load_url(
            &navigation, generation,
            "https://script-file-overflow.test/document",
            4096, 1000, 480, NULL, NULL, true);
        navigation_test_set_parser_script_stage_work_limit(0u);
        lxb_dom_node_t *body = loaded
            ? document_body_node(&navigation.page.document) : NULL;
        size_t large_length = 0, small_length = 0;
        const char *large = body == NULL ? NULL : document_attribute(
            body, "data-large-ran", &large_length);
        const char *small = body == NULL ? NULL : document_attribute(
            body, "data-small-ran", &small_length);
        FetchTraceReplayStats replay_stats = {0};
        bool replay_stats_ready = ready
            && fetch_trace_replay_stats(&replay_stats);
        bool overflow_ok = loaded && navigation.page.loaded
            && large == NULL && large_length == 0u
            && small != NULL && small_length == 3u
            && navigation.page.runtime != NULL
            && navigation.script_skipped_quota != 0u
            && navigation.performance.parser_script_stage_breakers == 0u
            && navigation.performance.parser_script_stage_work_breakers == 0u
            && navigation.performance.optional_work_sheds == 0u
            && navigation.last_error[0] == '\0'
            && replay_stats_ready && replay_stats.request_count == 2u
            && replay_stats.matched_request_count == 2u
            && replay_stats.unmatched_request_count == 0u;
        if (!overflow_ok) {
            fprintf(stderr,
                    "parser file overflow ready=%d loaded=%d page=%d "
                    "large=%.*s small=%.*s runtime=%p skipped=%zu "
                    "breaker=%zu/%zu sheds=%zu requests=%zu/%zu/%zu "
                    "summary=\"%s\" error=\"%s\"\n",
                    ready ? 1 : 0, loaded ? 1 : 0,
                    navigation.page.loaded ? 1 : 0,
                    (int) large_length, large == NULL ? "" : large,
                    (int) small_length, small == NULL ? "" : small,
                    (void *) navigation.page.runtime,
                    navigation.script_skipped_quota,
                    navigation.performance.parser_script_stage_breakers,
                    navigation.performance.parser_script_stage_work_breakers,
                    navigation.performance.optional_work_sheds,
                    replay_stats.request_count,
                    replay_stats.matched_request_count,
                    replay_stats.unmatched_request_count,
                    navigation.page.script_result.summary,
                    navigation.last_error);
        }
        if (ready) fetch_trace_end();
        if (installed) navigation_destroy(&navigation);
        if (browser_ready) browser_session_destroy(&browser);
        bool clean = budget.current == 0
            && budget_active_allocations(&budget, NULL) == 0
            && budget_categories_reconcile(&budget);
        if (installed) clean = budget_uninstall_lexbor(&budget) && clean;
        all_ok = all_ok && overflow_ok && clean;
    }

    /* A normal external-script failure near the cumulative work ceiling is
       not evidence that the response itself exhausted that ceiling. The
       missing replay record makes the request fail deterministically; the
       page may still shed optional scripting, but it must not report a work
       breaker or charge bytes it never retained. */
    {
        Budget budget;
        budget_init(&budget, 16 * MIB);
        bool installed = budget_install_lexbor(&budget);
        BrowserSession browser = {0};
        NavigationSession navigation = {0};
        bool browser_ready = installed
            && browser_session_init(&browser, &budget, 64 * 1024);
        bool ready = browser_ready
            && navigation_init(&navigation, &budget, 2)
            && parser_script_work_failure_replay_begin();
        if (ready) {
            navigation_attach_browser_session(&navigation, &browser);
            navigation_enable_scripts(&navigation, 4 * MIB, 1000);
            navigation_enable_document_scripts(
                &navigation, 8, 512u, 512u, 1000);
            navigation_set_stream_delivery(
                &navigation, 43, 0, 0, 0, 0, 0);
        }
        uint64_t generation = ready ? navigation_begin(&navigation) : 0;
        navigation_test_set_parser_script_stage_work_limit(180u);
        bool loaded = ready && navigation_load_url(
            &navigation, generation,
            "https://script-work-failure.test/document",
            4096, 1000, 480, NULL, NULL, true);
        navigation_test_set_parser_script_stage_work_limit(0u);
        lxb_dom_node_t *root = loaded
            ? lxb_dom_interface_node(navigation.page.document.html) : NULL;
        lxb_dom_node_t *body = loaded
            ? document_body_node(&navigation.page.document) : NULL;
        size_t first_length = 0;
        const char *first = body == NULL ? NULL : document_attribute(
            body, "data-first-ran", &first_length);
        FetchTraceReplayStats replay_stats = {0};
        bool replay_stats_ready = ready
            && fetch_trace_replay_stats(&replay_stats);
        bool attributed = loaded && navigation.page.loaded
            && root != NULL && test_find_id(root, "useful-link") != NULL
            && first != NULL && first_length == 3u
            && navigation.performance.parser_script_stage_work == 135u
            && navigation.performance.parser_script_stage_breakers == 0u
            && navigation.performance.parser_script_stage_work_breakers == 0u
            && navigation.performance.optional_work_sheds == 0u
            && navigation.last_error[0] == '\0'
            && replay_stats_ready && replay_stats.request_count == 2u
            && replay_stats.matched_request_count == 1u
            && replay_stats.unmatched_request_count == 1u;
        if (!attributed) {
            fprintf(stderr,
                    "parser work attribution ready=%d loaded=%d first=%.*s "
                    "work=%zu breaker=%zu/%zu sheds=%zu requests=%zu/%zu/%zu "
                    "error=\"%s\"\n",
                    ready ? 1 : 0, loaded ? 1 : 0,
                    (int) first_length, first == NULL ? "" : first,
                    navigation.performance.parser_script_stage_work,
                    navigation.performance.parser_script_stage_breakers,
                    navigation.performance.parser_script_stage_work_breakers,
                    navigation.performance.optional_work_sheds,
                    replay_stats.request_count,
                    replay_stats.matched_request_count,
                    replay_stats.unmatched_request_count,
                    navigation.last_error);
        }
        if (ready) fetch_trace_end();
        if (installed) navigation_destroy(&navigation);
        if (browser_ready) browser_session_destroy(&browser);
        bool clean = budget.current == 0
            && budget_active_allocations(&budget, NULL) == 0
            && budget_categories_reconcile(&budget);
        if (installed) clean = budget_uninstall_lexbor(&budget) && clean;
        all_ok = all_ok && attributed && clean;
    }

    /* A healthy, small parser hydration remains live and receives the same
       lifecycle treatment as before the cumulative breaker. */
    Budget budget;
    budget_init(&budget, 16 * MIB);
    bool installed = budget_install_lexbor(&budget);
    BrowserSession browser = {0};
    NavigationSession navigation = {0};
    bool browser_ready = installed
        && browser_session_init(&browser, &budget, 64 * 1024);
    bool ready = browser_ready && navigation_init(&navigation, &budget, 2)
        && parser_checkpoint_replay_begin();
    if (ready) {
        navigation_attach_browser_session(&navigation, &browser);
        navigation_enable_scripts(&navigation, 4 * MIB, 1000);
        navigation_enable_document_scripts(
            &navigation, 4, 64 * 1024u, 64 * 1024u, 1000);
        navigation_set_stream_delivery(&navigation, 37, 0, 0, 0, 0, 0);
    }
    uint64_t generation = ready ? navigation_begin(&navigation) : 0;
    /* The fixture's one 97-byte script sits one byte below this deterministic
       bound, pinning that exhaustion is cumulative rather than anticipatory. */
    navigation_test_set_parser_script_stage_work_limit(98u);
    bool loaded = ready && navigation_load_url(
        &navigation, generation, "https://checkpoint.test/document",
        4096, 1000, 480, NULL, NULL, true);
    navigation_test_set_parser_script_stage_work_limit(0u);
    lxb_dom_node_t *body = loaded
        ? document_body_node(&navigation.page.document) : NULL;
    size_t ran_length = 0;
    const char *ran = body == NULL ? NULL : document_attribute(
        body, "data-script-ran", &ran_length);
    bool healthy = loaded && navigation.page.loaded && ran != NULL
        && ran_length == 3u && navigation.page.runtime != NULL
        && !navigation.page.script_degradation_observed
        && navigation.performance.optional_work_sheds == 0u
        && navigation.performance.parser_checkpoint_soft_refusals == 0u
        && navigation.performance.parser_script_stage_work == 97u
        && navigation.performance.blocking_script_sample_count == 1u
        && navigation.performance.blocking_script_samples[0].host_callback_us
               != 0u
        && navigation.performance.parser_script_stage_us
               == navigation.performance.parser_script_execute_us
        && navigation.performance.parser_script_stage_breakers == 0u
        && strcmp(navigation.page.script_result.summary,
                  "checkpoint-script-ran") == 0;
    if (!healthy) {
        fprintf(stderr,
                "healthy parser hydration ready=%d loaded=%d page=%d "
                "ran=%.*s runtime=%p degraded=%d sheds=%zu soft=%zu "
                "stage-us=%llu execute-us=%llu callbacks=%zu/%llu "
                "summary=\"%s\" error=\"%s\"\n",
                ready ? 1 : 0, loaded ? 1 : 0,
                navigation.page.loaded ? 1 : 0,
                (int) ran_length, ran == NULL ? "" : ran,
                (void *) navigation.page.runtime,
                navigation.page.script_degradation_observed ? 1 : 0,
                navigation.performance.optional_work_sheds,
                navigation.performance.parser_checkpoint_soft_refusals,
                (unsigned long long)
                    navigation.performance.parser_script_stage_us,
                (unsigned long long)
                    navigation.performance.parser_script_execute_us,
                navigation.performance.blocking_script_sample_count,
                (unsigned long long)
                    navigation.performance.blocking_script_samples[0]
                        .host_callback_us,
                navigation.page.script_result.summary,
                navigation.last_error);
    }
    if (ready) fetch_trace_end();
    if (installed) navigation_destroy(&navigation);
    if (browser_ready) browser_session_destroy(&browser);
    bool clean = budget.current == 0
        && budget_active_allocations(&budget, NULL) == 0
        && budget_categories_reconcile(&budget);
    if (installed) clean = budget_uninstall_lexbor(&budget) && clean;
    return all_ok && healthy && clean;
}

static bool test_parser_transport_failures_do_not_retire_realm(void)
{
    Budget budget;
    budget_init(&budget, 16 * MIB);
    bool installed = budget_install_lexbor(&budget);
    BrowserSession browser = {0};
    NavigationSession navigation = {0};
    bool browser_ready = installed
        && browser_session_init(&browser, &budget, 64 * 1024);
    bool ready = browser_ready
        && navigation_init(&navigation, &budget, 2)
        && parser_script_transport_failures_replay_begin();
    if (ready) {
        navigation_attach_browser_session(&navigation, &browser);
        navigation_enable_scripts(&navigation, 4 * MIB, 1000);
        navigation_enable_document_scripts(
            &navigation, 8, 64 * 1024u, 64 * 1024u, 1000);
        navigation_set_stream_delivery(
            &navigation, 37, 0, 0, 0, 0, 0);
    }
    uint64_t generation = ready ? navigation_begin(&navigation) : 0;
    bool loaded = ready && navigation_load_url(
        &navigation, generation,
        "https://script-transport-failures.test/document",
        4096, 1000, 480, NULL, NULL, true);
    bool ok = loaded && navigation.page.loaded
        && strcmp(navigation.page.script_result.summary,
                  "SCRIPT-PRELOAD-OK") == 0
        && navigation.page.runtime != NULL
        && navigation.performance.parser_script_stage_breakers == 0u
        && navigation.performance.parser_script_stage_failure_breakers == 0u
        && navigation.last_error[0] == '\0';
    if (!ok) {
        fprintf(stderr,
                "transport failures ready=%d loaded=%d page=%d evaluated=%zu "
                "runtime=%p failed=%zu breakers=%zu/%zu summary=\"%s\" "
                "error=\"%s\"\n",
                ready ? 1 : 0, loaded ? 1 : 0,
                navigation.page.loaded ? 1 : 0,
                navigation.page.script_result.scripts_evaluated,
                (void *) navigation.page.runtime, navigation.script_failed,
                navigation.performance.parser_script_stage_breakers,
                navigation.performance.parser_script_stage_failure_breakers,
                navigation.page.script_result.summary,
                navigation.last_error);
    }
    if (ready) fetch_trace_end();
    if (installed) navigation_destroy(&navigation);
    if (browser_ready) browser_session_destroy(&browser);
    bool clean = budget.current == 0
        && budget_active_allocations(&budget, NULL) == 0
        && budget_categories_reconcile(&budget);
    if (installed) clean = budget_uninstall_lexbor(&budget) && clean;
    return ok && clean;
}

static bool test_parser_script_stage_watchdog_and_failure_reset(void)
{
    Budget budget;
    budget_init(&budget, 16 * MIB);
    bool installed = budget_install_lexbor(&budget);
    BrowserSession browser = {0};
    NavigationSession navigation = {0};
    bool browser_ready = installed
        && browser_session_init(&browser, &budget, 64 * 1024);
    bool ready = browser_ready
        && navigation_init(&navigation, &budget, 2)
        && parser_script_time_circuit_replay_begin();
    if (ready) {
        navigation_attach_browser_session(&navigation, &browser);
        navigation_enable_scripts(&navigation, 4 * MIB, 1500);
        navigation_enable_document_scripts(
            &navigation, 8, 64 * 1024u, 64 * 1024u, 1500);
        navigation_set_stream_delivery(
            &navigation, 43, 0, 0, 0, 0, 0);
    }
    uint64_t generation = ready ? navigation_begin(&navigation) : 0;
    navigation_test_set_parser_script_stage_time_limit_us(25000u);
    /* The watchdog stops the first hostile script near the deadline, but the
       measured callback can land a few microseconds below it on a fast host.
       Attribute the configured stage slice deterministically so that the
       breaker opens on that script rather than after a later script runs. */
    navigation_test_set_parser_script_stage_elapsed_us(25000u);
    uint64_t started_us = tilefinch_platform_monotonic_time_us();
    bool loaded = ready && navigation_load_url(
        &navigation, generation,
        "https://script-time-circuit.test/document",
        4096, 1500, 480, NULL, NULL, true);
    uint64_t finished_us = tilefinch_platform_monotonic_time_us();
    navigation_test_set_parser_script_stage_time_limit_us(0u);
    navigation_test_set_parser_script_stage_elapsed_us(0u);
    uint64_t wall_us = finished_us >= started_us
        ? finished_us - started_us : UINT64_MAX;
    lxb_dom_node_t *root = loaded
        ? lxb_dom_interface_node(navigation.page.document.html) : NULL;
    lxb_dom_node_t *body = loaded
        ? document_body_node(&navigation.page.document) : NULL;
    size_t later_length = 0;
    const char *later = body == NULL ? NULL : document_attribute(
        body, "data-tail-ran", &later_length);
    bool watchdog_ok = loaded && navigation.page.loaded
        && root != NULL && test_find_id(root, "useful-link") != NULL
        && later == NULL && later_length == 0u
        && navigation.page.runtime == NULL
        && navigation.page.script_degradation_observed
        && navigation.performance.parser_script_stage_time_breakers == 1u
        && navigation.performance.parser_script_stage_breakers == 1u
        && navigation.performance.parser_script_stage_skipped != 0u
        && navigation.last_error[0] == '\0'
        /* A broken cumulative cap waits for the configured 1.5-second
           per-script watchdog. Leave ample sanitizer/CI variance while
           proving that path cannot be reached. */
        && wall_us < UINT64_C(750000);
    if (!watchdog_ok) {
        fprintf(stderr,
                "parser watchdog ready=%d loaded=%d page=%d wall=%llu "
                "stage-us=%llu breakers=%zu/%zu skipped=%zu later=%.*s "
                "runtime=%p summary=\"%s\" error=\"%s\"\n",
                ready ? 1 : 0, loaded ? 1 : 0,
                navigation.page.loaded ? 1 : 0,
                (unsigned long long) wall_us,
                (unsigned long long)
                    navigation.performance.parser_script_stage_us,
                navigation.performance.parser_script_stage_breakers,
                navigation.performance.parser_script_stage_time_breakers,
                navigation.performance.parser_script_stage_skipped,
                (int) later_length, later == NULL ? "" : later,
                (void *) navigation.page.runtime,
                navigation.page.script_result.summary,
                navigation.last_error);
    }
    if (ready) fetch_trace_end();
    if (installed) navigation_destroy(&navigation);
    if (browser_ready) browser_session_destroy(&browser);
    bool clean = budget.current == 0
        && budget_active_allocations(&budget, NULL) == 0
        && budget_categories_reconcile(&budget);
    if (installed) clean = budget_uninstall_lexbor(&budget) && clean;

    /* A raw-scanner preload is settled before the ordinary script fetch.
       Transport wait is not author evaluation time and must not consume the
       parser execution breaker; the following healthy script still runs. */
    budget_init(&budget, 16 * MIB);
    installed = budget_install_lexbor(&budget);
    memset(&browser, 0, sizeof(browser));
    memset(&navigation, 0, sizeof(navigation));
    browser_ready = installed
        && browser_session_init(&browser, &budget, 64 * 1024);
    ready = browser_ready && navigation_init(&navigation, &budget, 2)
        && parser_script_preload_time_replay_begin();
    if (ready) {
        navigation_attach_browser_session(&navigation, &browser);
        navigation_enable_scripts(&navigation, 4 * MIB, 1500);
        navigation_enable_document_scripts(
            &navigation, 8, 64 * 1024u, 64 * 1024u, 1500);
        navigation_set_stream_delivery(
            &navigation, 43, 0, 0, 0, 0, 0);
    }
    generation = ready ? navigation_begin(&navigation) : 0;
    navigation_test_set_parser_script_stage_time_limit_us(25000u);
    started_us = tilefinch_platform_monotonic_time_us();
    loaded = ready && navigation_load_url(
        &navigation, generation,
        "https://script-preload-time.test/document",
        4096, 1500, 480, NULL, NULL, true);
    finished_us = tilefinch_platform_monotonic_time_us();
    navigation_test_set_parser_script_stage_time_limit_us(0u);
    wall_us = finished_us >= started_us
        ? finished_us - started_us : UINT64_MAX;
    root = loaded
        ? lxb_dom_interface_node(navigation.page.document.html) : NULL;
    body = loaded ? document_body_node(&navigation.page.document) : NULL;
    later_length = 0;
    later = body == NULL ? NULL : document_attribute(
        body, "data-later-ran", &later_length);
    bool preload_ok = loaded && navigation.page.loaded
        && root != NULL && test_find_id(root, "useful-link") != NULL
        && later != NULL && later_length == 3u
        && navigation.page.runtime != NULL
        && navigation.performance.parser_script_stage_time_breakers == 0u
        && navigation.performance.parser_script_stage_breakers == 0u
        && navigation.preloads_deferred != 0u
        && navigation.last_error[0] == '\0'
        && wall_us < UINT64_C(750000);
    if (!preload_ok) {
        fprintf(stderr,
                "parser preload watchdog ready=%d loaded=%d wall=%llu "
                "breakers=%zu/%zu deferred=%zu later=%.*s runtime=%p "
                "summary=\"%s\" error=\"%s\"\n",
                ready ? 1 : 0, loaded ? 1 : 0,
                (unsigned long long) wall_us,
                navigation.performance.parser_script_stage_breakers,
                navigation.performance.parser_script_stage_time_breakers,
                navigation.preloads_deferred,
                (int) later_length, later == NULL ? "" : later,
                (void *) navigation.page.runtime,
                navigation.page.script_result.summary,
                navigation.last_error);
    }
    if (ready) fetch_trace_end();
    if (installed) navigation_destroy(&navigation);
    if (browser_ready) browser_session_destroy(&browser);
    bool preload_clean = budget.current == 0
        && budget_active_allocations(&budget, NULL) == 0
        && budget_categories_reconcile(&budget);
    if (installed)
        preload_clean = budget_uninstall_lexbor(&budget) && preload_clean;

    budget_init(&budget, 16 * MIB);
    installed = budget_install_lexbor(&budget);
    memset(&browser, 0, sizeof(browser));
    memset(&navigation, 0, sizeof(navigation));
    browser_ready = installed
        && browser_session_init(&browser, &budget, 64 * 1024);
    ready = browser_ready && navigation_init(&navigation, &budget, 2)
        && parser_script_failure_reset_replay_begin();
    if (ready) {
        navigation_attach_browser_session(&navigation, &browser);
        navigation_enable_scripts(&navigation, 4 * MIB, 1000);
        navigation_enable_document_scripts(
            &navigation, 8, 64 * 1024u, 64 * 1024u, 1000);
        navigation_set_stream_delivery(
            &navigation, 43, 0, 0, 0, 0, 0);
    }
    generation = ready ? navigation_begin(&navigation) : 0;
    loaded = ready && navigation_load_url(
        &navigation, generation,
        "https://script-failure-reset.test/document",
        4096, 1000, 480, NULL, NULL, true);
    body = loaded ? document_body_node(&navigation.page.document) : NULL;
    size_t reset_length = 0, tail_length = 0;
    const char *reset = body == NULL ? NULL : document_attribute(
        body, "data-reset-ran", &reset_length);
    const char *tail = body == NULL ? NULL : document_attribute(
        body, "data-tail-ran", &tail_length);
    bool reset_ok = loaded && navigation.page.loaded
        && reset != NULL && reset_length == 3u
        && tail != NULL && tail_length == 3u
        && navigation.page.runtime != NULL
        && navigation.script_failed == 4u
        && navigation.performance.parser_script_stage_breakers == 0u
        && navigation.performance.parser_script_stage_failure_breakers == 0u
        && navigation.performance.optional_work_sheds == 0u
        && navigation.last_error[0] == '\0';
    if (!reset_ok) {
        fprintf(stderr,
                "parser failure reset ready=%d loaded=%d reset=%.*s "
                "tail=%.*s runtime=%p failed=%zu breaker=%zu/%zu "
                "degraded=%d error=\"%s\"\n",
                ready ? 1 : 0, loaded ? 1 : 0,
                (int) reset_length, reset == NULL ? "" : reset,
                (int) tail_length, tail == NULL ? "" : tail,
                (void *) navigation.page.runtime, navigation.script_failed,
                navigation.performance.parser_script_stage_breakers,
                navigation.performance.parser_script_stage_failure_breakers,
                navigation.page.script_degradation_observed ? 1 : 0,
                navigation.last_error);
    }
    if (ready) fetch_trace_end();
    if (installed) navigation_destroy(&navigation);
    if (browser_ready) browser_session_destroy(&browser);
    bool reset_clean = budget.current == 0
        && budget_active_allocations(&budget, NULL) == 0
        && budget_categories_reconcile(&budget);
    if (installed)
        reset_clean = budget_uninstall_lexbor(&budget) && reset_clean;
    return watchdog_ok && clean && preload_ok && preload_clean
        && reset_ok && reset_clean;
}

static bool test_parser_script_mutation_rebinds_live_node(void)
{
    Budget budget;
    budget_init(&budget, 16 * MIB);
    bool installed = budget_install_lexbor(&budget);
    BrowserSession browser = {0};
    NavigationSession navigation = {0};
    bool browser_ready = installed
        && browser_session_init(&browser, &budget, 64 * 1024);
    bool ready = browser_ready && navigation_init(&navigation, &budget, 2)
        && parser_script_mutation_rebind_replay_begin();
    if (ready) {
        navigation_attach_browser_session(&navigation, &browser);
        navigation_enable_scripts(&navigation, 4 * MIB, 1000);
        navigation_enable_document_scripts(
            &navigation, 8, 64 * 1024u, 64 * 1024u, 1000);
        navigation_set_stream_delivery(&navigation, 37, 0, 0, 0, 0, 0);
    }
    uint64_t generation = ready ? navigation_begin(&navigation) : 0;
    bool loaded = ready && navigation_load_url(
        &navigation, generation,
        "https://script-mutation-rebind.test/document",
        4096, 1000, 480, NULL, NULL, true);
    lxb_dom_node_t *root = loaded
        ? lxb_dom_interface_node(navigation.page.document.html) : NULL;
    lxb_dom_node_t *body = loaded
        ? document_body_node(&navigation.page.document) : NULL;
    size_t removed_length = 0, retyped_length = 0, tail_length = 0;
    size_t victim_ran_length = 0, retyped_ran_length = 0;
    const char *removed = body == NULL ? NULL : document_attribute(
        body, "data-removed", &removed_length);
    const char *retyped = body == NULL ? NULL : document_attribute(
        body, "data-retyped", &retyped_length);
    const char *tail = body == NULL ? NULL : document_attribute(
        body, "data-tail-ran", &tail_length);
    const char *victim_ran = body == NULL ? NULL : document_attribute(
        body, "data-victim-ran", &victim_ran_length);
    const char *retyped_ran = body == NULL ? NULL : document_attribute(
        body, "data-retyped-ran", &retyped_ran_length);
    lxb_dom_node_t *retyped_node = root == NULL
        ? NULL : test_find_id(root, "retyped");
    size_t type_length = 0;
    const char *type = retyped_node == NULL ? NULL : document_attribute(
        retyped_node, "type", &type_length);
    bool ok = loaded && navigation.page.loaded && root != NULL
        && test_find_id(root, "useful-link") != NULL
        && test_find_id(root, "victim") == NULL
        && removed != NULL && removed_length == 3u
        && retyped != NULL && retyped_length == 3u
        && retyped_node != NULL && type != NULL
        && type_length == sizeof("application/json") - 1u
        && memcmp(type, "application/json", type_length) == 0
        && victim_ran == NULL && victim_ran_length == 0u
        && retyped_ran == NULL && retyped_ran_length == 0u
        && tail != NULL && tail_length == 3u
        && navigation.page.runtime != NULL
        && !navigation.page.script_degradation_observed
        && navigation.last_error[0] == '\0';
    if (!ok) {
        fprintf(stderr,
                "parser mutation rebind ready=%d loaded=%d page=%d "
                "victim=%p removed=%.*s retyped=%.*s type=%.*s "
                "victim-ran=%.*s retyped-ran=%.*s tail=%.*s runtime=%p "
                "degraded=%d summary=\"%s\" error=\"%s\"\n",
                ready ? 1 : 0, loaded ? 1 : 0,
                navigation.page.loaded ? 1 : 0,
                root == NULL ? NULL : (void *) test_find_id(root, "victim"),
                (int) removed_length, removed == NULL ? "" : removed,
                (int) retyped_length, retyped == NULL ? "" : retyped,
                (int) type_length, type == NULL ? "" : type,
                (int) victim_ran_length,
                victim_ran == NULL ? "" : victim_ran,
                (int) retyped_ran_length,
                retyped_ran == NULL ? "" : retyped_ran,
                (int) tail_length, tail == NULL ? "" : tail,
                (void *) navigation.page.runtime,
                navigation.page.script_degradation_observed ? 1 : 0,
                navigation.page.script_result.summary,
                navigation.last_error);
    }
    if (ready) fetch_trace_end();
    if (installed) navigation_destroy(&navigation);
    if (browser_ready) browser_session_destroy(&browser);
    bool clean = budget.current == 0
        && budget_active_allocations(&budget, NULL) == 0
        && budget_categories_reconcile(&budget);
    if (installed) clean = budget_uninstall_lexbor(&budget) && clean;
    return ok && clean;
}

static bool test_failed_external_parser_source_counts_as_work(void)
{
    Budget budget;
    budget_init(&budget, 16 * MIB);
    bool installed = budget_install_lexbor(&budget);
    BrowserSession browser = {0};
    NavigationSession navigation = {0};
    bool browser_ready = installed
        && browser_session_init(&browser, &budget, 64 * 1024);
    bool ready = browser_ready && navigation_init(&navigation, &budget, 2)
        && parser_script_work_failed_body_replay_begin();
    if (ready) {
        navigation_attach_browser_session(&navigation, &browser);
        navigation_enable_scripts(&navigation, 4 * MIB, 1000);
        navigation_enable_document_scripts(
            &navigation, 8, 512u, 512u, 1000);
        navigation_set_stream_delivery(&navigation, 37, 0, 0, 0, 0, 0);
    }
    uint64_t generation = ready ? navigation_begin(&navigation) : 0;
    navigation_test_set_parser_script_stage_work_limit(190u);
    bool loaded = ready && navigation_load_url(
        &navigation, generation,
        "https://script-work-failed-body.test/document",
        4096, 1000, 480, NULL, NULL, true);
    navigation_test_set_parser_script_stage_work_limit(0u);
    lxb_dom_node_t *root = loaded
        ? lxb_dom_interface_node(navigation.page.document.html) : NULL;
    lxb_dom_node_t *body = loaded
        ? document_body_node(&navigation.page.document) : NULL;
    size_t first_length = 0, tail_length = 0;
    const char *first = body == NULL ? NULL : document_attribute(
        body, "data-first-ran", &first_length);
    const char *tail = body == NULL ? NULL : document_attribute(
        body, "data-tail-ran", &tail_length);
    bool ok = loaded && navigation.page.loaded && root != NULL
        && test_find_id(root, "useful-link") != NULL
        && first != NULL && first_length == 3u
        && tail == NULL && tail_length == 0u
        && navigation.performance.parser_script_stage_work == 154u
        && navigation.performance.parser_script_stage_breakers == 1u
        && navigation.performance.parser_script_stage_work_breakers == 1u
        && navigation.page.script_degradation_observed
        && navigation.page.runtime == NULL
        && navigation.last_error[0] == '\0';
    if (!ok) {
        fprintf(stderr,
                "failed parser source work ready=%d loaded=%d page=%d "
                "first=%.*s tail=%.*s work=%zu breakers=%zu/%zu "
                "runtime=%p degraded=%d summary=\"%s\" error=\"%s\"\n",
                ready ? 1 : 0, loaded ? 1 : 0,
                navigation.page.loaded ? 1 : 0,
                (int) first_length, first == NULL ? "" : first,
                (int) tail_length, tail == NULL ? "" : tail,
                navigation.performance.parser_script_stage_work,
                navigation.performance.parser_script_stage_breakers,
                navigation.performance.parser_script_stage_work_breakers,
                (void *) navigation.page.runtime,
                navigation.page.script_degradation_observed ? 1 : 0,
                navigation.page.script_result.summary,
                navigation.last_error);
    }
    if (ready) fetch_trace_end();
    if (installed) navigation_destroy(&navigation);
    if (browser_ready) browser_session_destroy(&browser);
    bool clean = budget.current == 0
        && budget_active_allocations(&budget, NULL) == 0
        && budget_categories_reconcile(&budget);
    if (installed) clean = budget_uninstall_lexbor(&budget) && clean;
    return ok && clean;
}

static bool test_parser_mutation_checkpoint_uses_stage_deadline(void)
{
    Budget budget;
    budget_init(&budget, 16 * MIB);
    bool installed = budget_install_lexbor(&budget);
    BrowserSession browser = {0};
    NavigationSession navigation = {0};
    bool browser_ready = installed
        && browser_session_init(&browser, &budget, 64 * 1024);
    bool ready = browser_ready && navigation_init(&navigation, &budget, 2)
        && parser_script_mutation_time_replay_begin();
    if (ready) {
        navigation_attach_browser_session(&navigation, &browser);
        navigation_enable_scripts(&navigation, 4 * MIB, 1500);
        navigation_enable_document_scripts(
            &navigation, 8, 64 * 1024u, 64 * 1024u, 1500);
        navigation_set_stream_delivery(&navigation, 37, 0, 0, 0, 0, 0);
    }
    uint64_t generation = ready ? navigation_begin(&navigation) : 0;
    navigation_test_set_parser_script_stage_time_limit_us(25000u);
    uint64_t started_us = tilefinch_platform_monotonic_time_us();
    bool loaded = ready && navigation_load_url(
        &navigation, generation,
        "https://script-mutation-time.test/document",
        4096, 1500, 480, NULL, NULL, true);
    uint64_t finished_us = tilefinch_platform_monotonic_time_us();
    navigation_test_set_parser_script_stage_time_limit_us(0u);
    uint64_t wall_us = finished_us >= started_us
        ? finished_us - started_us : UINT64_MAX;
    lxb_dom_node_t *root = loaded
        ? lxb_dom_interface_node(navigation.page.document.html) : NULL;
    lxb_dom_node_t *body = loaded
        ? document_body_node(&navigation.page.document) : NULL;
    size_t victim_length = 0, tail_length = 0;
    const char *victim = body == NULL ? NULL : document_attribute(
        body, "data-victim-ran", &victim_length);
    const char *tail = body == NULL ? NULL : document_attribute(
        body, "data-tail-ran", &tail_length);
    bool ok = loaded && navigation.page.loaded && root != NULL
        && test_find_id(root, "useful-link") != NULL
        && victim == NULL && victim_length == 0u
        && tail == NULL && tail_length == 0u
        && navigation.performance.parser_script_stage_time_breakers == 1u
        && navigation.performance.parser_script_stage_breakers == 1u
        && navigation.page.script_degradation_observed
        && navigation.page.runtime == NULL
        && navigation.last_error[0] == '\0'
        && wall_us < UINT64_C(750000);
    if (!ok) {
        fprintf(stderr,
                "parser mutation deadline ready=%d loaded=%d page=%d "
                "wall=%llu victim=%.*s tail=%.*s stage-us=%llu "
                "breakers=%zu/%zu runtime=%p degraded=%d summary=\"%s\" "
                "error=\"%s\"\n",
                ready ? 1 : 0, loaded ? 1 : 0,
                navigation.page.loaded ? 1 : 0,
                (unsigned long long) wall_us,
                (int) victim_length, victim == NULL ? "" : victim,
                (int) tail_length, tail == NULL ? "" : tail,
                (unsigned long long)
                    navigation.performance.parser_script_stage_us,
                navigation.performance.parser_script_stage_breakers,
                navigation.performance.parser_script_stage_time_breakers,
                (void *) navigation.page.runtime,
                navigation.page.script_degradation_observed ? 1 : 0,
                navigation.page.script_result.summary,
                navigation.last_error);
    }
    if (ready) fetch_trace_end();
    if (installed) navigation_destroy(&navigation);
    if (browser_ready) browser_session_destroy(&browser);
    bool clean = budget.current == 0
        && budget_active_allocations(&budget, NULL) == 0
        && budget_categories_reconcile(&budget);
    if (installed) clean = budget_uninstall_lexbor(&budget) && clean;
    return ok && clean;
}

static bool test_late_server_actions_refresh_hydration_snapshot(void)
{
    Budget budget;
    budget_init(&budget, 16 * MIB);
    bool installed = budget_install_lexbor(&budget);
    BrowserSession browser = {0};
    NavigationSession navigation = {0};
    bool browser_ready = installed
        && browser_session_init(&browser, &budget, 64 * 1024);
    bool ready = browser_ready && navigation_init(&navigation, &budget, 2)
        && parser_script_late_ssr_replay_begin();
    if (ready) {
        navigation_attach_browser_session(&navigation, &browser);
        navigation_enable_scripts(&navigation, 4 * MIB, 1000);
        navigation_enable_document_scripts(
            &navigation, 8, 64 * 1024u, 64 * 1024u, 1000);
        navigation_set_stream_delivery(&navigation, 37, 0, 0, 0, 0, 0);
    }
    uint64_t generation = ready ? navigation_begin(&navigation) : 0;
    bool loaded = ready && navigation_load_url(
        &navigation, generation, "https://script-late-ssr.test/document",
        4096, 1000, 480, NULL, NULL, true);
    lxb_dom_node_t *root = loaded
        ? lxb_dom_interface_node(navigation.page.document.html) : NULL;
    lxb_dom_node_t *early = root == NULL
        ? NULL : test_find_id(root, "early-content");
    lxb_dom_node_t *link = root == NULL
        ? NULL : test_find_id(root, "late-link");
    lxb_dom_node_t *loading = root == NULL
        ? NULL : test_find_id(root, "loading");
    /* The late link adds only one element, one text node, and two text bytes:
       both old coarse refresh thresholds (8 nodes / 256 bytes) remain unmet. */
    bool ok = loaded && navigation.page.loaded && early != NULL
        && link != NULL && loading == NULL
        && navigation.page.layout.link_count != 0u
        && navigation.script_failed >= 1u
        && navigation.page.script_degradation_observed
        && navigation.last_error[0] == '\0';
    if (!ok) {
        fprintf(stderr,
                "late SSR snapshot ready=%d loaded=%d page=%d early=%p "
                "link=%p loading=%p links=%zu controls=%zu "
                "failed=%zu runtime=%p degraded=%d summary=\"%s\" "
                "error=\"%s\"\n",
                ready ? 1 : 0, loaded ? 1 : 0,
                navigation.page.loaded ? 1 : 0, (void *) early,
                (void *) link, (void *) loading,
                navigation.page.layout.link_count,
                navigation.page.layout.control_count,
                navigation.script_failed, (void *) navigation.page.runtime,
                navigation.page.script_degradation_observed ? 1 : 0,
                navigation.page.script_result.summary,
                navigation.last_error);
    }
    if (ready) fetch_trace_end();
    if (installed) navigation_destroy(&navigation);
    if (browser_ready) browser_session_destroy(&browser);
    bool clean = budget.current == 0
        && budget_active_allocations(&budget, NULL) == 0
        && budget_categories_reconcile(&budget);
    if (installed) clean = budget_uninstall_lexbor(&budget) && clean;
    return ok && clean;
}

static bool test_resource_scheduler_refusal_is_not_script_degradation(void)
{
    Budget budget;
    budget_init(&budget, 16 * MIB);
    bool installed = budget_install_lexbor(&budget);
    BrowserSession browser = {0};
    NavigationSession navigation = {0};
    bool browser_ready = installed
        && browser_session_init(&browser, &budget, 64 * 1024);
    bool ready = browser_ready
        && navigation_init(&navigation, &budget, 2)
        && resource_only_shed_replay_begin();
    if (ready) {
        navigation_attach_browser_session(&navigation, &browser);
        navigation_enable_external_resources(
            &navigation, 2, 32 * 1024, 16 * 1024,
            2, 32 * 1024, 16 * 1024, 64 * 1024, 1000);
        navigation_set_reader_candidate_mode(&navigation, true);
        navigation_test_refuse_next_stream_scheduler_creation();
    }
    uint64_t generation = ready ? navigation_begin(&navigation) : 0;
    bool loaded = ready && navigation_load_url(
        &navigation, generation, "https://resource-only.test/document",
        4096, 1000, 480, NULL, NULL, true);
    bool ok = loaded && navigation.page.loaded
        && navigation.performance.optional_work_sheds == 1u
        && navigation.page.runtime == NULL
        && !navigation.scripts_enabled
        && !navigation.document_scripts_enabled
        && !navigation.page.script_degradation_observed
        && navigation_layout_is_visually_blank(&navigation.page.layout)
        && navigation.page.reader_analysis.prepared
        && navigation.page.reader_analysis.kind == READER_PAGE_ARTICLE;
    if (!ok) {
        fprintf(stderr,
                "resource-only shed ready=%d loaded=%d page=%d sheds=%zu "
                "runtime=%p scripts=%d/%d degraded=%d blank=%d "
                "reader=%d/%d error=\"%s\"\n",
                ready, loaded, navigation.page.loaded,
                navigation.performance.optional_work_sheds,
                (void *) navigation.page.runtime, navigation.scripts_enabled,
                navigation.document_scripts_enabled,
                navigation.page.script_degradation_observed,
                navigation_layout_is_visually_blank(&navigation.page.layout),
                navigation.page.reader_analysis.prepared,
                (int) navigation.page.reader_analysis.kind,
                navigation.last_error);
    }
    if (ready) fetch_trace_end();
    if (installed) navigation_destroy(&navigation);
    if (browser_ready) browser_session_destroy(&browser);
    bool clean = budget.current == 0
        && budget_active_allocations(&budget, NULL) == 0
        && budget_categories_reconcile(&budget);
    if (installed) clean = budget_uninstall_lexbor(&budget) && clean;
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

static bool viewport_after_stylesheet_replay_begin(void)
{
    char error[256] = {0};
    return fetch_trace_replay_begin(
        TILEFINCH_TEST_SOURCE_DIR "/fixtures/http-viewport-after-stylesheet",
        error, sizeof(error));
}

static uint32_t layout_text_color(const LayoutDocument *layout,
                                  const char *text)
{
    size_t length = strlen(text);
    for (size_t i = 0; layout != NULL && i < layout->count; i++) {
        const DrawCommand *command = &layout->commands[i];
        if (command->type == DRAW_TEXT && command->text_length == length
            && memcmp(command->text, text, length) == 0) {
            return command->color;
        }
    }
    return UINT32_MAX;
}

/* A parser-blocking script before the first stylesheet compiles the
   streaming sheet against the legacy layout width; a <meta name=viewport>
   parsed later moves the viewport to the device width.  The committed
   page must answer media queries for the committed viewport rather than
   continue the stale prefix. */
static bool test_streaming_stylesheet_follows_late_viewport(void)
{
    Budget budget;
    budget_init(&budget, 16 * MIB);
    bool lexbor_installed = budget_install_lexbor(&budget);
    NavigationSession navigation = {0};
    bool ready = lexbor_installed
        && navigation_init(&navigation, &budget, 4)
        && viewport_after_stylesheet_replay_begin();
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
        "https://viewport-order.test/document",
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
    uint32_t color = loaded
        ? layout_text_color(&navigation.page.layout, "viewport")
        : UINT32_MAX;
    bool ok = loaded && navigation.page.loaded
        && navigation.viewport.css_width == 480
        && navigation.page.stylesheet.viewport_width == 480
        && navigation.performance.blocking_stylesheet_viewport_rebuilds == 1
        && navigation.performance.blocking_script_sample_count >= 1
        && color == 0x112233u
        && strcmp(navigation.page.script_result.summary, "viewport:480") == 0;
    if (!ok) {
        fprintf(stderr,
                "viewport-after-stylesheet loaded=%d page=%d viewport=%d "
                "sheet=%dx%d rebuilds=%zu samples=%zu color=%06x "
                "summary=\"%s\" status=%d error=\"%s\"\n",
                loaded, navigation.page.loaded,
                navigation.viewport.css_width,
                navigation.page.stylesheet.viewport_width,
                navigation.page.stylesheet.viewport_height,
                navigation.performance.blocking_stylesheet_viewport_rebuilds,
                navigation.performance.blocking_script_sample_count,
                (unsigned) color, navigation.page.script_result.summary,
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

static bool persisted_client_hint_replay_begin(void)
{
    char error[256] = {0};
    return fetch_trace_replay_begin(
        TILEFINCH_TEST_SOURCE_DIR "/fixtures/http-client-hint-persisted",
        error, sizeof(error));
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

/* Navigation tests retain a single executable and shared fixture lifetime;
   these ordered units follow document, transaction, and runtime boundaries. */
#include "suites/navigation_document.inc"
#include "suites/navigation_transactions.inc"
#include "suites/navigation_streaming.inc"
#include "suites/navigation_runtime.inc"
#include "suites/navigation_runner.inc"
