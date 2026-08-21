#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>
#include <errno.h>
#include <limits.h>
#include <unistd.h>

#include <lexbor/dom/interface.h>
#include <lexbor/dom/interfaces/attr.h>
#include <lexbor/dom/interfaces/comment.h>
#include <lexbor/dom/interfaces/element.h>
#include <lexbor/html/interfaces/element.h>
#include <lexbor/html/interfaces/template_element.h>
#include <lexbor/html/serialize.h>

#include "tilefinch/browser_engine.h"
#include "tilefinch/budget.h"
#include "tilefinch/content_blocker.h"
#include "tilefinch/controller.h"
#include "tilefinch/document_backing.h"
#include "tilefinch/fetch.h"
#include "tilefinch/font.h"
#include "tilefinch/host_media.h"
#include "tilefinch/navigation.h"
#include "tilefinch/platform.h"
#include "tilefinch/psp_ui.h"
#include "tilefinch/render.h"
#include "tilefinch/request_context.h"
#include "tilefinch/section_pager.h"
#include "tilefinch/section_router.h"
#include "tilefinch/script_loader.h"
#include "tilefinch/session.h"
#include "tilefinch/youtube_resolver.h"

#define MIB (1024u * 1024u)
#define KIB (1024u)
#define EXPERIMENTAL_SECTION_BLOCK_BYTES (64u * KIB - 1)
#define EXPERIMENTAL_SECTION_MAX_BYTES (512u * KIB)
#define EXPERIMENTAL_REMOTE_INNER_HTML_LIMIT (64u * KIB)
#define EXPERIMENTAL_REMOTE_MUTATION_LIMIT 32
#define EXPERIMENTAL_REMOTE_ATTRIBUTE_MUTATION_LIMIT 64
#define EXPERIMENTAL_SCRIPT_FRONTIER_LIMIT 256
#define VISUAL_EVIDENCE_MAX_FRAMES 64
#define VISUAL_EVIDENCE_LINE_LIMIT 32768
#define VISUAL_EVIDENCE_TEXT_LIMIT 4096
#define TEXT_METRICS_RUN_LIMIT 4096
#define TEXT_METRICS_TEXT_LIMIT 1024

#ifndef TILEFINCH_SANS_FONT
#define TILEFINCH_SANS_FONT ""
#endif
#ifndef TILEFINCH_SERIF_FONT
#define TILEFINCH_SERIF_FONT ""
#endif
#ifndef TILEFINCH_SANS_ITALIC_FONT
#define TILEFINCH_SANS_ITALIC_FONT ""
#endif
#ifndef TILEFINCH_SANS_BOLD_FONT
#define TILEFINCH_SANS_BOLD_FONT ""
#endif
#ifndef TILEFINCH_SERIF_BOLD_FONT
#define TILEFINCH_SERIF_BOLD_FONT ""
#endif
#ifndef TILEFINCH_METRIC_SANS_FONT
#define TILEFINCH_METRIC_SANS_FONT ""
#endif
#ifndef TILEFINCH_METRIC_SANS_BOLD_FONT
#define TILEFINCH_METRIC_SANS_BOLD_FONT ""
#endif

typedef struct {
    uint64_t last_us;
    uint64_t total_us;
    uint64_t max_us;
} ExperimentalPhaseTiming;

typedef struct {
    int scroll_y[VISUAL_EVIDENCE_MAX_FRAMES];
    size_t frame_count;
    bool overflow;
} VisualEvidenceFrames;

typedef struct {
    char *name;
    size_t name_length;
    char *value;
    size_t value_length;
    bool removed;
} ExperimentalRemoteAttributeMutation;

typedef struct {
    bool used;
    char stable_key[193];
    size_t stable_key_length;
    size_t section_index;
    bool content_written;
    ScriptRemoteNodeWriteKind content_kind;
    char *content;
    size_t content_length;
    ExperimentalRemoteAttributeMutation *attributes;
    size_t attribute_count;
    size_t attribute_capacity;
} ExperimentalRemoteMutation;

typedef struct {
    SectionPager pager;
    DocumentBacking backing;
    CompressedSectionStore *store;
    SectionStoreStreamBuilder *builder;
    FetchResult *response;
    Budget *budget;
    BrowserEngine *engine;
    BrowserSession *browser_session;
    NavigationSession *navigation;
    const char *fixture_source;
    size_t fixture_length;
    size_t maximum_download_bytes;
    size_t history_sections[64];
    bool history_section_known[64];
    char locator[NAVIGATION_URL_LIMIT];
    unsigned char *executed_sections;
    size_t executed_section_bytes;
    size_t script_frontier_sections[EXPERIMENTAL_SCRIPT_FRONTIER_LIMIT];
    size_t script_frontier_ordinals[EXPERIMENTAL_SCRIPT_FRONTIER_LIMIT];
    size_t script_frontier_node_ordinals[EXPERIMENTAL_SCRIPT_FRONTIER_LIMIT];
    bool script_frontier_body_visible[EXPERIMENTAL_SCRIPT_FRONTIER_LIMIT];
    size_t script_frontier_count;
    size_t script_visible_section;
    size_t script_visible_ordinal;
    size_t script_visible_node_ordinal;
    bool script_body_visible;
    bool script_frontier_active;
    bool script_frontier_failed;
    uint64_t pending_swap_started_ns;
    uint64_t ready_total_us;
    uint64_t ready_max_us;
    uint64_t first_tile_total_us;
    uint64_t first_tile_max_us;
    ExperimentalPhaseTiming state_save_timing;
    ExperimentalPhaseTiming pager_timing;
    ExperimentalPhaseTiming commit_timing;
    ExperimentalPhaseTiming content_timing;
    ExperimentalPhaseTiming restore_timing;
    ExperimentalPhaseTiming scroll_timing;
    size_t timing_swaps;
    size_t first_tile_samples;
    size_t swaps;
    size_t semantic_queries;
    size_t semantic_sections;
    size_t semantic_sections_skipped;
    size_t semantic_matches;
    size_t semantic_failures;
    uint64_t semantic_total_us;
    uint64_t semantic_max_us;
    uint64_t initial_load_started_ns;
    uint64_t transfer_complete_us;
    uint64_t provisional_commit_us;
    uint64_t first_paint_ready_us;
    uint64_t index_complete_us;
    uint64_t final_ready_us;
    size_t initial_section;
    size_t initial_tile_capacity;
    size_t index_progress_callbacks;
    uint64_t initial_generation;
    const FontSet *initial_fonts;
    bool provisional_attempted;
    bool provisional_painted;
    bool provisional_presented;
    size_t remote_reads;
    size_t remote_read_bytes;
    size_t remote_read_failures;
    size_t remote_geometry_reads;
    size_t remote_geometry_layouts;
    size_t remote_geometry_cache_hits;
    size_t remote_geometry_failures;
    bool remote_geometry_cache_valid;
    char remote_geometry_cache_key[193];
    size_t remote_geometry_cache_key_length;
    size_t remote_geometry_cache_section;
    size_t remote_geometry_cache_current_section;
    size_t remote_geometry_cache_writes;
    size_t remote_geometry_cache_relayouts;
    size_t remote_geometry_cache_loads;
    int remote_geometry_cache_scroll;
    ScriptRemoteNodeReadResult remote_geometry_cache_result;
    ExperimentalRemoteMutation
        remote_mutations[EXPERIMENTAL_REMOTE_MUTATION_LIMIT];
    size_t remote_mutation_count;
    size_t remote_mutation_head;
    size_t remote_writes;
    size_t remote_write_bytes;
    size_t remote_write_failures;
    size_t remote_mutation_evictions;
    bool body_source_replaced;
    bool first_tile_pending;
    bool active;
} ExperimentalSectionContext;


/* Private frontend implementation seams. These remain one translation unit
   so the refactor cannot change callback visibility or PSP code generation. */
#include "interactive/experimental.inc"
#include "interactive/platform_support.inc"
#include "interactive/diagnostics.inc"
#include "interactive/media.inc"
#include "interactive/commands.inc"

int main(int argc, char **argv)
{
    const char *fixture = NULL;
    const char *url = NULL;
    const char *typed = NULL;
    const char *focus_id = NULL, *click_selector = NULL;
    const char *post_click_selector = NULL;
    const char *output = "interactive.ppm";
    const char *probe_script = NULL;
    const char *visual_state_marker = NULL;
    const char *user_css = NULL;
    const char *commands_path = NULL;
    const char *text_metrics_path = NULL;
    const char *first_present_output = NULL;
    const char *media_file = NULL;
    const char *loop_output_dir = "interactive-frames";
    const char *capture_http = NULL, *replay_http = NULL;
    const char *capture_http_from_top_level = NULL;
    const char *content_blocker_name = "off";
    const char *content_blocker_list = NULL;
    size_t capture_http_top_level_ordinal = 0;
    size_t replay_http_options = 0;
    const char *psp_profile = NULL;
    unsigned long long deterministic_replay_seed = 0;
    bool deterministic_replay_requested = false;
    bool response_keyed_replay = false;
    bool visual_evidence = false;
    bool limit_explicit = false;
    bool adaptive_resources = false;
    bool adaptive_resources_explicit = false;
    bool pace_real_time = false;
    size_t ticks = 0, tick_ms = 16, focus_next = 0, limit_mb = 24;
    size_t max_download_kb = 4096;
    size_t interaction_ticks = 32;
    int requested_scroll_y = 0;
    bool scroll_y_requested = false, scroll_bottom = false;
    int viewport_css_width = 0, viewport_css_height = 0;
    const char *blocked_origin_hosts[64];
    size_t blocked_origin_count = 0;
    bool post_click_from_top = false;
    size_t script_timeout_ms = 10000;
    size_t script_heap_mb = 4, script_total_mb = 1;
    size_t script_file_kb = 384, script_count = 32;
    bool script_heap_explicit = false, script_total_explicit = false;
    /* Fidelity-run overrides: host-side reference scoring wants to see
       everything the engine can render, so the PSP-shaped image budgets
       and the wall-clock resource stage deadline are adjustable. Zero
       means "keep the profile default". */
    size_t image_count_override = 0, image_total_kb_override = 0;
    size_t image_file_kb_override = 0, image_decoded_mb_override = 0;
    size_t font_attempts_override = 0, font_total_kb_override = 0;
    size_t font_file_kb_override = 0, font_backend_kb_override = 0;
    size_t resource_timeout_ms = 15000;
    size_t session_cache_kb_override = 0;
    bool script_file_explicit = false, script_count_explicit = false;
    size_t reloads = 0;
    bool fetch_scripts = false, activate = false, follow_action = false;
    bool trace_frames = false, trace_page = false;
    bool diagnostic_frame_safari = false;
    bool interactive_loop = false;
    bool loop_capture_frames = true;
    bool platform_sim = false;
    bool external_resources = true;
    bool forced_dark = false;
    bool progressive_first_paint = true;
    bool hide_cookie_banners = false;
    bool wpt_test_rendered = false;
    bool low_memory_navigation = false;
    bool experimental_compressed_sections = false;
    bool experimental_section_explicit = false;
    size_t experimental_section = 0;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--fetch-scripts") == 0) fetch_scripts = true;
        else if (strcmp(argv[i], "--pace-real-time") == 0) {
            pace_real_time = true;
        }
        else if (strcmp(argv[i], "--trace-frames") == 0) trace_frames = true;
        else if (strcmp(argv[i], "--trace-page") == 0) trace_page = true;
        else if (strcmp(argv[i], "--diagnostic-frame-safari") == 0) {
            diagnostic_frame_safari = true;
        }
        else if (strcmp(argv[i], "--activate") == 0) activate = true;
        else if (strcmp(argv[i], "--follow-action") == 0) follow_action = true;
        else if (strcmp(argv[i], "--scroll-bottom") == 0) scroll_bottom = true;
        else if (strcmp(argv[i], "--interactive") == 0) interactive_loop = true;
        else if (strcmp(argv[i], "--no-loop-capture") == 0) {
            loop_capture_frames = false;
        }
        else if (strcmp(argv[i], "--visual-evidence") == 0) {
            visual_evidence = true;
        }
        else if (strcmp(argv[i], "--platform-sim") == 0) {
            platform_sim = true;
        }
        else if (strcmp(argv[i], "--low-memory-navigation") == 0) {
            low_memory_navigation = true;
        }
        else if (strcmp(argv[i], "--experimental-compressed-sections") == 0) {
            experimental_compressed_sections = true;
        }
        else if (strcmp(argv[i], "--no-external-resources") == 0) {
            external_resources = false;
        }
        else if (strcmp(argv[i], "--forced-dark") == 0) {
            forced_dark = true;
        }
        else if (strcmp(argv[i], "--no-progressive-first-paint") == 0) {
            progressive_first_paint = false;
        }
        else if (strcmp(argv[i], "--wpt-test-rendered") == 0) {
            wpt_test_rendered = true;
        }
        else if (strcmp(argv[i], "--hide-cookie-banners") == 0) {
            hide_cookie_banners = true;
        }
        else if (strcmp(argv[i], "--adaptive-resources") == 0) {
            adaptive_resources = true;
            adaptive_resources_explicit = true;
        }
        else if (strcmp(argv[i], "--no-adaptive-resources") == 0) {
            adaptive_resources = false;
            adaptive_resources_explicit = true;
        }
        else if (strcmp(argv[i], "--post-click-from-top") == 0) {
            post_click_from_top = true;
        }
        else if (strcmp(argv[i], "--capture-http-from-top-level") == 0) {
            if (i + 2 >= argc) { usage(argv[0]); return 2; }
            char *end = NULL;
            errno = 0;
            const char *ordinal = argv[++i];
            unsigned long long parsed = strtoull(ordinal, &end, 10);
            if (ordinal[0] == '-' || end == ordinal || *end != '\0'
                || errno == ERANGE || parsed == 0 || parsed > SIZE_MAX) {
                usage(argv[0]); return 2;
            }
            capture_http_top_level_ordinal = (size_t) parsed;
            capture_http_from_top_level = argv[++i];
        }
        else if (i + 1 >= argc) { usage(argv[0]); return 2; }
        else if (strcmp(argv[i], "--fixture") == 0) fixture = argv[++i];
        else if (strcmp(argv[i], "--url") == 0) url = argv[++i];
        else if (strcmp(argv[i], "--media-file") == 0
                 || strcmp(argv[i], "--media-url") == 0)
            media_file = argv[++i];
        else if (strcmp(argv[i], "--ticks") == 0) ticks = strtoul(argv[++i], NULL, 10);
        else if (strcmp(argv[i], "--tick-ms") == 0) tick_ms = strtoul(argv[++i], NULL, 10);
        else if (strcmp(argv[i], "--focus-next") == 0) focus_next = strtoul(argv[++i], NULL, 10);
        else if (strcmp(argv[i], "--reload") == 0) reloads = strtoul(argv[++i], NULL, 10);
        else if (strcmp(argv[i], "--type") == 0) typed = argv[++i];
        else if (strcmp(argv[i], "--focus-id") == 0) focus_id = argv[++i];
        else if (strcmp(argv[i], "--click-selector") == 0) {
            click_selector = argv[++i];
        }
        else if (strcmp(argv[i], "--post-click-selector") == 0) {
            post_click_selector = argv[++i];
        }
        else if (strcmp(argv[i], "--interaction-ticks") == 0) {
            interaction_ticks = strtoul(argv[++i], NULL, 10);
        }
        else if (strcmp(argv[i], "--output") == 0) output = argv[++i];
        else if (strcmp(argv[i], "--first-present-output") == 0) {
            first_present_output = argv[++i];
            platform_sim = true;
        }
        else if (strcmp(argv[i], "--user-css") == 0) user_css = argv[++i];
        else if (strcmp(argv[i], "--commands") == 0) {
            commands_path = argv[++i];
        }
        else if (strcmp(argv[i], "--dump-text-metrics") == 0) {
            text_metrics_path = argv[++i];
        }
        else if (strcmp(argv[i], "--loop-output-dir") == 0) {
            loop_output_dir = argv[++i];
        }
        else if (strcmp(argv[i], "--scroll-y") == 0) {
            requested_scroll_y = (int) strtol(argv[++i], NULL, 10);
            scroll_y_requested = true;
        }
        else if (strcmp(argv[i], "--probe-script") == 0) {
            probe_script = argv[++i];
        }
        else if (strcmp(argv[i], "--block-origin") == 0) {
            if (blocked_origin_count
                < sizeof(blocked_origin_hosts)
                  / sizeof(blocked_origin_hosts[0])) {
                blocked_origin_hosts[blocked_origin_count++] = argv[++i];
            } else {
                ++i;
            }
        }
        else if (strcmp(argv[i], "--content-blocker") == 0) {
            content_blocker_name = argv[++i];
        }
        else if (strcmp(argv[i], "--content-blocker-list") == 0) {
            content_blocker_list = argv[++i];
        }
        else if (strcmp(argv[i], "--viewport-css-width") == 0) {
            viewport_css_width = (int) strtol(argv[++i], NULL, 10);
        }
        else if (strcmp(argv[i], "--viewport-css-height") == 0) {
            viewport_css_height = (int) strtol(argv[++i], NULL, 10);
        }
        else if (strcmp(argv[i], "--visual-state-marker") == 0) {
            visual_state_marker = argv[++i];
        }
        else if (strcmp(argv[i], "--capture-http") == 0) {
            capture_http = argv[++i];
        }
        else if (strcmp(argv[i], "--replay-http") == 0) {
            replay_http = argv[++i];
            replay_http_options++;
        }
        else if (strcmp(argv[i], "--replay-http-response-keyed") == 0) {
            replay_http = argv[++i];
            response_keyed_replay = true;
            replay_http_options++;
        }
        else if (strcmp(argv[i], "--deterministic-replay-seed") == 0) {
            deterministic_replay_seed = strtoull(argv[++i], NULL, 0);
            deterministic_replay_requested = true;
        }
        else if (strcmp(argv[i], "--limit-mb") == 0) {
            limit_mb = strtoul(argv[++i], NULL, 10);
            limit_explicit = true;
        }
        else if (strcmp(argv[i], "--psp-profile") == 0) {
            psp_profile = argv[++i];
        }
        else if (strcmp(argv[i], "--max-download-kb") == 0) {
            max_download_kb = strtoul(argv[++i], NULL, 10);
        }
        else if (strcmp(argv[i], "--script-timeout-ms") == 0) {
            script_timeout_ms = strtoul(argv[++i], NULL, 10);
        }
        else if (strcmp(argv[i], "--image-count") == 0) {
            image_count_override = strtoul(argv[++i], NULL, 10);
        }
        else if (strcmp(argv[i], "--image-total-kb") == 0) {
            image_total_kb_override = strtoul(argv[++i], NULL, 10);
        }
        else if (strcmp(argv[i], "--image-file-kb") == 0) {
            image_file_kb_override = strtoul(argv[++i], NULL, 10);
        }
        else if (strcmp(argv[i], "--image-decoded-mb") == 0) {
            image_decoded_mb_override = strtoul(argv[++i], NULL, 10);
        }
        else if (strcmp(argv[i], "--font-attempts") == 0) {
            font_attempts_override = strtoul(argv[++i], NULL, 10);
        }
        else if (strcmp(argv[i], "--font-total-kb") == 0) {
            font_total_kb_override = strtoul(argv[++i], NULL, 10);
        }
        else if (strcmp(argv[i], "--font-file-kb") == 0) {
            font_file_kb_override = strtoul(argv[++i], NULL, 10);
        }
        else if (strcmp(argv[i], "--font-backend-kb") == 0) {
            font_backend_kb_override = strtoul(argv[++i], NULL, 10);
        }
        else if (strcmp(argv[i], "--resource-timeout-ms") == 0) {
            resource_timeout_ms = strtoul(argv[++i], NULL, 10);
        }
        else if (strcmp(argv[i], "--session-cache-kb") == 0) {
            session_cache_kb_override = strtoul(argv[++i], NULL, 10);
        }
        else if (strcmp(argv[i], "--script-heap-mb") == 0) {
            script_heap_mb = strtoul(argv[++i], NULL, 10);
            script_heap_explicit = true;
        }
        else if (strcmp(argv[i], "--script-total-mb") == 0) {
            script_total_mb = strtoul(argv[++i], NULL, 10);
            script_total_explicit = true;
        }
        else if (strcmp(argv[i], "--script-file-kb") == 0) {
            script_file_kb = strtoul(argv[++i], NULL, 10);
            script_file_explicit = true;
        }
        else if (strcmp(argv[i], "--script-count") == 0) {
            script_count = strtoul(argv[++i], NULL, 10);
            script_count_explicit = true;
        }
        else if (strcmp(argv[i], "--experimental-section") == 0) {
            char *end = NULL;
            errno = 0;
            unsigned long long parsed = strtoull(argv[++i], &end, 10);
            if (argv[i][0] == '-' || end == argv[i] || *end != '\0'
                || errno == ERANGE || parsed > SIZE_MAX) {
                usage(argv[0]); return 2;
            }
            experimental_section = (size_t) parsed;
            experimental_compressed_sections = true;
            experimental_section_explicit = true;
        }
        else { usage(argv[0]); return 2; }
    }
    if (psp_profile != NULL) {
        bool strict = strcmp(psp_profile, "strict") == 0;
        bool realistic = strcmp(psp_profile, "realistic") == 0;
        if (!strict && !realistic) { usage(argv[0]); return 2; }
        if (!limit_explicit) limit_mb = strict ? 16 : 24;
        if (!script_heap_explicit) script_heap_mb = strict ? 4 : 5;
        if (!script_total_explicit) script_total_mb = strict ? 1 : 2;
        if (!adaptive_resources_explicit) adaptive_resources = true;
    }
    if ((fixture == NULL) == (url == NULL) || limit_mb < 4 || limit_mb > 512
        || ticks > 20000 || interaction_ticks > 20000
        || max_download_kb < 1 || max_download_kb > 65536
        || tick_ms > 60000 || focus_next > 10000
        || script_timeout_ms < 1 || script_timeout_ms > 300000
        || script_heap_mb < 1 || script_heap_mb > 256
        || script_total_mb < 1 || script_total_mb > 128
        || script_file_kb < 1 || script_file_kb > 8192
        || script_count < 1 || script_count > 256
        || reloads > 20 || (reloads != 0 && url == NULL)
        || (experimental_compressed_sections && reloads != 0)
        || replay_http_options > 1
        || ((capture_http != NULL) + (replay_http != NULL)
            + (capture_http_from_top_level != NULL) > 1)
        || (interactive_loop && commands_path != NULL)
        || (deterministic_replay_requested
            && replay_http == NULL && capture_http == NULL
            && capture_http_from_top_level == NULL)
        || (visual_state_marker != NULL
            && (visual_state_marker[0] == '\0'
                || strlen(visual_state_marker) > 4096))
        || (visual_evidence
            && (visual_state_marker == NULL || commands_path == NULL
                || !deterministic_replay_requested
                || !response_keyed_replay || !loop_capture_frames))
        || (probe_script != NULL && fixture == NULL)) {
        usage(argv[0]); return 2;
    }

    bool strict_pressure = adaptive_resources && limit_mb <= 16;
    size_t session_cache_limit = 1024 * KIB;
    size_t history_capacity = 16;
    size_t tile_capacity = 8;
    size_t stylesheet_count = 6, stylesheet_bytes = 768 * KIB;
    size_t stylesheet_file_bytes = 256 * KIB;
    size_t image_count = 24, image_bytes = 1536 * KIB;
    size_t image_file_bytes = 512 * KIB;
    size_t decoded_image_bytes = 3 * MIB;
    /* A larger page budget should buy broader, still-bounded web-app
       coverage without requiring per-site launch flags. Modern bundles can
       split a modest CSS byte total across many links, so byte/file ceilings
       remain the primary bound while the count only caps request fan-out.
       Preserve every explicit script policy and the tighter 24 MiB/PSP
       defaults; the execution profile may clamp file limits further. */
    if (limit_mb >= 18) {
        stylesheet_count = 8;
        stylesheet_bytes = 1 * MIB;
        stylesheet_file_bytes = 384 * KIB;
    }
    if (limit_mb >= 24) {
        stylesheet_count = 24;
        stylesheet_bytes = 2112 * KIB;
        stylesheet_file_bytes = 768 * KIB;
    }
    if (limit_mb >= 32 && psp_profile == NULL) {
        stylesheet_count = 24;
        stylesheet_bytes = 2112 * KIB;
        stylesheet_file_bytes = 768 * KIB;
        if (!script_heap_explicit) script_heap_mb = 8;
        if (!script_total_explicit) script_total_mb = 4;
        if (!script_file_explicit) script_file_kb = 1536;
        /* Modern module graphs commonly contain many small files.  Let the
           aggregate and per-file byte ceilings remain the primary memory
           bounds instead of rejecting an otherwise bounded graph by count. */
        if (!script_count_explicit) script_count = 128;
    }
    if (strict_pressure) {
        session_cache_limit = 512 * KIB;
        history_capacity = 8;
        tile_capacity = 4;
        stylesheet_count = 4;
        stylesheet_bytes = 512 * KIB;
        stylesheet_file_bytes = 192 * KIB;
        image_count = 12;
        image_bytes = 768 * KIB;
        image_file_bytes = 256 * KIB;
        decoded_image_bytes = 1536 * KIB;
    }
    if (session_cache_kb_override != 0) {
        session_cache_limit = session_cache_kb_override * KIB;
    }

    size_t script_pressure_avoided = 0;
    if (strict_pressure && fetch_scripts && url != NULL) {
        size_t old_script_total = script_total_mb * MIB;
        if (script_count > 16) script_count = 16;
        if (script_total_mb > 0) script_total_mb = 1;
        if (script_file_kb > 256) script_file_kb = 256;
        size_t new_script_total = script_total_mb * MIB;
        script_pressure_avoided = old_script_total > new_script_total
            ? old_script_total - new_script_total : 0;
    }
    printf("runtime-profile=%s limit-mb=%zu adaptive=%s history=%zu "
           "session-cache=%zu tiles=%zu navigation=%s\n",
           psp_profile == NULL ? "custom" : psp_profile, limit_mb,
           adaptive_resources ? "yes" : "no", history_capacity,
           session_cache_limit, tile_capacity,
           low_memory_navigation ? "low-memory" : "transactional");
    HostPlatformSimulator platform_simulator = {
        .first_frame_output = first_present_output
    };
    if (platform_sim) {
        TilefinchPlatformServices services = {
            .context = &platform_simulator,
            .read_asset = host_platform_read_asset,
            .poll_input = host_platform_poll_input,
            .present_rgb565 = host_platform_present
        };
        host_platform_simulator = &platform_simulator;
        tilefinch_platform_set_services(&services);
    }

    BrowserDeviceProfile device_profile;
    browser_device_profile_psp3000(&device_profile);
    BrowserConfig *engine_config = calloc(1, sizeof(*engine_config));
    if (engine_config == NULL) {
        fprintf(stderr, "browser configuration reservation failed\n");
        if (platform_sim) {
            tilefinch_platform_set_services(NULL);
            host_platform_simulator = NULL;
        }
        return 1;
    }
    browser_config_init(engine_config, &device_profile);
    engine_config->memory_limit = limit_mb * MIB;
    engine_config->history_capacity = history_capacity;
    engine_config->session_cache_limit = session_cache_limit;
    engine_config->maximum_document_bytes = max_download_kb * KIB;
    engine_config->navigation_timeout_ms = 30000;
    engine_config->navigation_replacement_mode = low_memory_navigation
        ? NAVIGATION_REPLACEMENT_LOW_MEMORY
        : NAVIGATION_REPLACEMENT_TRANSACTIONAL;
    if (viewport_css_width > 0 && viewport_css_height > 0) {
        engine_config->declared_css_width = viewport_css_width;
        engine_config->declared_css_height = viewport_css_height;
    }
    engine_config->tile_capacity = tile_capacity;
    engine_config->javascript.enabled = true;
    engine_config->javascript.document_scripts_enabled =
        fetch_scripts && url != NULL;
    engine_config->javascript.heap_limit = script_heap_mb * MIB;
    engine_config->javascript.runtime_timeout_ms =
        (unsigned) script_timeout_ms;
    engine_config->javascript.maximum_scripts = script_count;
    engine_config->javascript.maximum_total_bytes = script_total_mb * MIB;
    engine_config->javascript.maximum_file_bytes = script_file_kb * KIB;
    engine_config->javascript.network_timeout_ms = 15000;
    ScriptExecutionProfile execution_profile = SCRIPT_EXECUTION_PROFILE_LAB;
    if (psp_profile != NULL) {
        execution_profile = strcmp(psp_profile, "strict") == 0
            ? SCRIPT_EXECUTION_PROFILE_PSP_STRICT
            : SCRIPT_EXECUTION_PROFILE_PSP_REALISTIC;
    }
    if (!script_execution_policy_for_profile(
            execution_profile,
            &engine_config->javascript.execution_policy)) {
        fprintf(stderr, "PSP script execution policy setup failed\n");
        free(engine_config);
        if (platform_sim) {
            tilefinch_platform_set_services(NULL);
            host_platform_simulator = NULL;
        }
        return 1;
    }
    size_t maximum_compile_bytes = engine_config->javascript.execution_policy
                                       .maximum_host_compile_source_bytes;
    if (maximum_compile_bytes != 0
        && engine_config->javascript.maximum_file_bytes
               > maximum_compile_bytes) {
        engine_config->javascript.maximum_file_bytes = maximum_compile_bytes;
    }
    engine_config->resources.enabled = external_resources;
    engine_config->progressive_first_paint = progressive_first_paint;
    engine_config->resources.maximum_stylesheets = stylesheet_count;
    engine_config->resources.maximum_stylesheet_bytes = stylesheet_bytes;
    engine_config->resources.maximum_stylesheet_file_bytes =
        stylesheet_file_bytes;
    if (image_count_override != 0) image_count = image_count_override;
    if (image_total_kb_override != 0) {
        image_bytes = image_total_kb_override * KIB;
    }
    if (image_file_kb_override != 0) {
        image_file_bytes = image_file_kb_override * KIB;
    }
    if (image_decoded_mb_override != 0) {
        decoded_image_bytes = image_decoded_mb_override * MIB;
    }
    engine_config->resources.maximum_images = image_count;
    engine_config->resources.maximum_image_bytes = image_bytes;
    engine_config->resources.maximum_image_file_bytes = image_file_bytes;
    engine_config->resources.maximum_decoded_image_bytes = decoded_image_bytes;
    /* Page fonts default to PSP-shaped ceilings (96 KiB/file) that reject the
       ~280 KiB WOFF / ~570 KiB TTF web faces real sites ship.  Reference
       scoring wants everything the engine can render, so let the harness raise
       the font budgets the same way it raises the image budgets; zero keeps the
       profile default. */
    if (font_attempts_override != 0) {
        engine_config->resources.maximum_font_attempts = font_attempts_override;
    }
    if (font_total_kb_override != 0) {
        engine_config->resources.maximum_font_bytes =
            font_total_kb_override * KIB;
    }
    if (font_file_kb_override != 0) {
        engine_config->resources.maximum_font_file_bytes =
            font_file_kb_override * KIB;
    }
    if (font_backend_kb_override != 0) {
        engine_config->resources.maximum_font_face_backend_bytes =
            font_backend_kb_override * KIB;
    }
    engine_config->resources.timeout_ms = (long) resource_timeout_ms;
    printf("runtime-policy scripts=%zu source-bytes=%zu file-bytes=%zu "
           "heap-bytes=%zu stylesheets=%zu css-bytes=%zu "
           "css-file-bytes=%zu\n",
           engine_config->javascript.maximum_scripts,
           engine_config->javascript.maximum_total_bytes,
           engine_config->javascript.maximum_file_bytes,
           engine_config->javascript.heap_limit,
           engine_config->resources.maximum_stylesheets,
           engine_config->resources.maximum_stylesheet_bytes,
           engine_config->resources.maximum_stylesheet_file_bytes);
    bool configured_fonts = TILEFINCH_SANS_FONT[0] != '\0'
        || TILEFINCH_SERIF_FONT[0] != '\0'
        || TILEFINCH_SANS_ITALIC_FONT[0] != '\0'
        || TILEFINCH_SANS_BOLD_FONT[0] != '\0'
        || TILEFINCH_SERIF_BOLD_FONT[0] != '\0'
        || TILEFINCH_METRIC_SANS_FONT[0] != '\0'
        || TILEFINCH_METRIC_SANS_BOLD_FONT[0] != '\0';
    if (configured_fonts
        && !browser_config_set_font_paths(
               engine_config, TILEFINCH_SANS_FONT, TILEFINCH_SERIF_FONT,
               TILEFINCH_SANS_ITALIC_FONT, TILEFINCH_SANS_BOLD_FONT,
               TILEFINCH_SERIF_BOLD_FONT, TILEFINCH_METRIC_SANS_FONT,
               TILEFINCH_METRIC_SANS_BOLD_FONT, 1536 * KIB)) {
        fprintf(stderr, "browser font configuration failed\n");
        free(engine_config);
        if (platform_sim) {
            tilefinch_platform_set_services(NULL);
            host_platform_simulator = NULL;
        }
        return 1;
    }
    char engine_error[256] = {0};
    BrowserEngine *engine = browser_engine_create(
        engine_config, engine_error, sizeof(engine_error));
    free(engine_config);
    if (engine == NULL) {
        fprintf(stderr, "browser engine setup failed: %s\n", engine_error);
        if (platform_sim) {
            tilefinch_platform_set_services(NULL);
            host_platform_simulator = NULL;
        }
        return 1;
    }
    if (forced_dark && !browser_engine_set_forced_dark(engine, true)) {
        fprintf(stderr, "forced-dark setup failed\n");
        browser_engine_destroy(engine);
        if (platform_sim) {
            tilefinch_platform_set_services(NULL);
            host_platform_simulator = NULL;
        }
        return 1;
    }
    ContentBlockerMode content_blocker_mode = CONTENT_BLOCKER_OFF;
    if (strcmp(content_blocker_name, "basic") == 0) {
        content_blocker_mode = CONTENT_BLOCKER_BASIC;
    } else if (strcmp(content_blocker_name, "custom") == 0) {
        content_blocker_mode = CONTENT_BLOCKER_CUSTOM;
    } else if (strcmp(content_blocker_name, "off") != 0) {
        fprintf(stderr, "unknown content-blocker mode: %s\n",
                content_blocker_name);
        browser_engine_destroy(engine);
        if (platform_sim) {
            tilefinch_platform_set_services(NULL);
            host_platform_simulator = NULL;
        }
        return 2;
    }
    if (!browser_engine_content_blocker_configure(
            engine, content_blocker_mode, content_blocker_list)) {
        fprintf(stderr, "content-blocker configuration failed\n");
        browser_engine_destroy(engine);
        if (platform_sim) {
            tilefinch_platform_set_services(NULL);
            host_platform_simulator = NULL;
        }
        return 1;
    }
    Budget *budget = browser_engine_budget(engine);
    if (budget == NULL) {
        browser_engine_destroy(engine);
        if (platform_sim) {
            tilefinch_platform_set_services(NULL);
            host_platform_simulator = NULL;
        }
        return 1;
    }
    if (strict_pressure) {
        budget_record_pressure(budget, BUDGET_PRESSURE_CACHE, 512 * KIB, 0);
        budget_record_pressure(
            budget, BUDGET_PRESSURE_HISTORY,
            (16 - history_capacity) * sizeof(NavigationEntry), 0);
        budget_record_pressure(
            budget, BUDGET_PRESSURE_TILE,
            (8 - tile_capacity) * sizeof(RenderTile), 0);
        if (external_resources && url != NULL) {
            budget_record_pressure(
                budget, BUDGET_PRESSURE_STYLESHEET, 256 * KIB, 0);
            budget_record_pressure(
                budget, BUDGET_PRESSURE_IMAGE, 2304 * KIB, 0);
        }
        if (fetch_scripts && url != NULL) {
            budget_record_pressure(
                budget, BUDGET_PRESSURE_JAVASCRIPT,
                script_pressure_avoided, 0);
        }
    }
    InteractiveApplicationContext *application = budget_calloc_category(
        budget, BUDGET_CATEGORY_NAVIGATION, 1, sizeof(*application));
    if (application == NULL) {
        fprintf(stderr, "interactive application context reservation failed\n");
        browser_engine_destroy(engine);
        if (platform_sim) {
            tilefinch_platform_set_services(NULL);
            host_platform_simulator = NULL;
        }
        return 1;
    }
#define session (*application->engine_session)
#define navigation (*application->engine_navigation)
#define cache (*application->engine_render)
#define controller (*application->engine_controller)
#define fonts (*application->engine_fonts)
#define section_store (application->section_store)
#define section_stream_builder (application->section_stream_builder)
#define experimental_router (application->experimental_router)
#define experimental (application->experimental)
#define section_fetch (application->section_fetch)
    char *input = NULL;
    uint16_t *frame = NULL;
    FILE *command_stream = NULL;
    InteractiveMediaSession media;
    application->engine = engine;
    application->engine_session = browser_engine_session(engine);
    application->engine_navigation = browser_engine_navigation(engine);
    application->engine_fonts = browser_engine_fonts(engine);
    interactive_media_init(
        &media, media_file == NULL, pace_real_time,
        font_set_face(application->engine_fonts, FONT_SANS));
    bool navigation_ready = application->engine_navigation != NULL;
    bool fonts_ready = application->engine_fonts != NULL;
    bool success = false;

    if (application->engine_session == NULL || !navigation_ready) {
        fprintf(stderr, "browser engine accessors are unavailable\n");
        goto cleanup;
    }
    fetch_set_blocked_origins(blocked_origin_hosts, blocked_origin_count);
    budget_mark_phase(budget, "session");
    budget_mark_phase(budget, "fonts");
    budget_mark_phase(budget, "navigation");

    char trace_error[256] = {0};
    script_runtime_configure_deterministic_replay(
        deterministic_replay_requested, deterministic_replay_seed);
    if ((capture_http != NULL
         && !fetch_trace_capture_begin(capture_http, trace_error,
                                       sizeof(trace_error)))
        || (capture_http_from_top_level != NULL
            && !fetch_trace_capture_arm_top_level(
                   capture_http_from_top_level,
                   capture_http_top_level_ordinal,
                   trace_error, sizeof(trace_error)))
        || (replay_http != NULL
            && !(response_keyed_replay
                   ? fetch_trace_replay_begin_response_keyed(
                         replay_http, trace_error, sizeof(trace_error))
                   : fetch_trace_replay_begin(
                         replay_http, trace_error, sizeof(trace_error))))) {
        fprintf(stderr, "HTTP trace setup failed: %s\n", trace_error);
        goto cleanup;
    }
    if (replay_http != NULL
        && !fetch_trace_replay_seed_session(
               &session, trace_error, sizeof(trace_error))) {
        fprintf(stderr, "HTTP replay cookie seed failed: %s\n", trace_error);
        goto cleanup;
    }

    navigation_enable_frame_capability_trace(&navigation, trace_frames);
    navigation_enable_page_capability_trace(&navigation, trace_page);
    navigation_enable_diagnostic_frame_safari(
        &navigation, diagnostic_frame_safari);
    experimental.store = &section_store;
    experimental.builder = &section_stream_builder;
    experimental.response = &section_fetch;
    experimental.budget = budget;
    experimental.engine = engine;
    experimental.browser_session = &session;
    experimental_bind_navigation(&experimental, &navigation);
    experimental.maximum_download_bytes = max_download_kb * KIB;
    experimental.initial_section = experimental_section;
    experimental.initial_tile_capacity = tile_capacity;
    experimental.initial_fonts = fonts_ready ? &fonts : NULL;
    if (experimental_compressed_sections) {
        experimental.initial_load_started_ns =
            tilefinch_platform_monotonic_time_ns();
        snprintf(experimental.locator, sizeof(experimental.locator), "%s",
                 url != NULL ? url : "https://fixture.test/");
    }
    uint64_t generation = experimental_compressed_sections
        ? navigation_begin(&navigation) : 0;
    experimental.initial_generation = generation;
    if (url != NULL && experimental_compressed_sections) {
        if (!section_route_stream_begin(
                &experimental_router, &section_stream_builder,
                &section_store, budget,
                EXPERIMENTAL_SECTION_BLOCK_BYTES,
                EXPERIMENTAL_SECTION_MAX_BYTES,
                experimental_section_explicit)) goto cleanup;
        section_route_stream_set_progress(
            &experimental_router, experimental_index_progress,
            &experimental);
        FetchStreamMetrics stream_metrics = {0};
        FetchStreamOptions stream = {
            .on_body = section_route_stream_body,
            .opaque = &experimental_router,
            .chunk_bytes = 16384
        };
        TilefinchRequestContext section_context = {
            .target_url = url,
            .top_level_url = url,
            .method = "GET",
            .mode = TILEFINCH_REQUEST_MODE_NAVIGATE,
            .credentials = TILEFINCH_CREDENTIALS_INCLUDE,
            .destination = TILEFINCH_DESTINATION_DOCUMENT,
            .top_level_navigation = true,
            .user_activated = true
        };
        char section_cookies[4096] = {0};
        (void) browser_session_cookie_header_context(
            &session, &section_context,
            section_cookies, sizeof(section_cookies));
        FetchRequest section_request = {
            .method = "GET", .cookie = section_cookies,
            .allow_http_errors = true,
            .send_low_client_hints = true,
            .accept = "text/html,application/xhtml+xml,application/xml;q=0.9,*/*;q=0.8",
            .sec_fetch_dest = tilefinch_request_fetch_destination(
                &section_context),
            .sec_fetch_mode = tilefinch_request_fetch_mode(&section_context),
            .sec_fetch_site = tilefinch_request_fetch_site(&section_context),
            .sec_fetch_user = section_context.user_activated,
            .upgrade_insecure_requests = true,
            .credentials = FETCH_CREDENTIALS_INCLUDE,
            .credential_origin = url,
            .initiator_url = url,
            .cookie_session = &session,
            .cookie_context = &section_context
        };
        bool fetched = fetch_request_stream_cancelable(
            budget, url, &section_request, max_download_kb * KIB, 30000,
            NULL, NULL, &stream, &stream_metrics, &section_fetch);
        experimental.transfer_complete_us =
            experimental_initial_elapsed_us(&experimental);
        bool replay_basic_request = false;
        if (!fetched && replay_http != NULL
            && strstr(section_fetch.error, "does not match") != NULL) {
            section_route_stream_abort(&experimental_router);
            fetch_result_destroy(&section_fetch);
            memset(&stream_metrics, 0, sizeof(stream_metrics));
            /* A replay mismatch consumes the attempted trace record. Restart
               the deterministic trace before retrying its recorded request
               shape; live fetches never enter this compatibility path. */
            fetch_trace_end();
            bool replay_restarted = response_keyed_replay
                ? fetch_trace_replay_begin_response_keyed(
                      replay_http, trace_error, sizeof(trace_error))
                : fetch_trace_replay_begin(
                      replay_http, trace_error, sizeof(trace_error));
            if (!replay_restarted) {
                fprintf(stderr, "HTTP replay restart failed: %s\n",
                        trace_error);
                goto cleanup;
            }
            if (!fetch_trace_replay_seed_session(
                    &session, trace_error, sizeof(trace_error))) {
                fprintf(stderr, "HTTP replay cookie seed restart failed: %s\n",
                        trace_error);
                goto cleanup;
            }
            if (!section_route_stream_begin(
                    &experimental_router, &section_stream_builder,
                    &section_store, budget,
                    EXPERIMENTAL_SECTION_BLOCK_BYTES,
                    EXPERIMENTAL_SECTION_MAX_BYTES,
                    experimental_section_explicit)) goto cleanup;
            section_route_stream_set_progress(
                &experimental_router, experimental_index_progress,
                &experimental);
            fetched = fetch_request_stream_cancelable(
                budget, url, NULL, max_download_kb * KIB, 30000,
                NULL, NULL, &stream, &stream_metrics, &section_fetch);
            experimental.transfer_complete_us =
                experimental_initial_elapsed_us(&experimental);
            replay_basic_request = fetched;
        }
        if (!fetched || !section_route_stream_finish(&experimental_router)) {
            goto cleanup;
        }
        if (experimental_router.sectioned
            && !experimental_section_explicit
            && section_store.section_count == 1
            && section_store.source_length
                 <= experimental_router.byte_threshold) {
            char *whole_document = NULL;
            size_t whole_length = 0;
            if (!section_store_extract_source(
                    &section_store, &whole_document, &whole_length)) {
                goto cleanup;
            }
            section_store_destroy(&section_store);
            experimental_router.buffer = (unsigned char *) whole_document;
            experimental_router.length = whole_length;
            experimental_router.capacity = whole_length + 1;
            experimental_router.sectioned = false;
            printf("experimental-adaptive fallback=single-section\n");
        }
        snprintf(experimental.locator, sizeof(experimental.locator), "%s",
                 section_fetch.effective_url[0] != '\0'
                   ? section_fetch.effective_url : url);
        navigation.last_http_status = section_fetch.status_code;
        snprintf(navigation.last_cf_mitigated,
                 sizeof(navigation.last_cf_mitigated), "%s",
                 section_fetch.cf_mitigated);
        snprintf(navigation.last_server, sizeof(navigation.last_server), "%s",
                 section_fetch.server);
        for (size_t i = 0; i < section_fetch.set_cookie_count; i++) {
            TilefinchRequestContext response_context = section_context;
            response_context.target_url = fetch_set_cookie_url(
                &section_fetch, i, experimental.locator);
            (void) browser_session_cookie_set_http_context(
                &session, &response_context,
                section_fetch.set_cookies[i]);
        }
        if (!experimental_router.sectioned) {
            printf("experimental-adaptive mode=full-document source=%zu "
                   "threshold=%zu elements=%zu attributes=%zu depth=%zu "
                   "scan-us=%llu\n",
                   experimental_router.length,
                   experimental_router.byte_threshold,
                   experimental_router.element_count,
                   experimental_router.attribute_count,
                   experimental_router.maximum_depth,
                   (unsigned long long) experimental_router.scan_us);
            bool committed = browser_engine_commit_html(
                engine, experimental.locator,
                (const char *) experimental_router.buffer,
                experimental_router.length, true);
            budget_free(budget, experimental_router.buffer);
            experimental_router.buffer = NULL;
            if (!committed) goto cleanup;
        } else {
            if (experimental_section >= section_store.section_count
                || !section_pager_init(
                       &experimental.pager, &section_store,
                       experimental_section, 1)
                || !experimental_script_state_init(&experimental)) {
                goto cleanup;
            }
            experimental.active = true;
            if (!experimental_install_navigation_callbacks(
                    &navigation, &experimental)
                || !experimental_preflight_external_layout(
                    &experimental, &navigation, &generation,
                    fonts_ready ? &fonts : NULL)) {
                goto cleanup;
            }
            navigation.defer_document_script_pipeline =
                navigation.document_scripts_enabled;
            bool initial_committed = browser_engine_commit_section_html(
                engine, &experimental.backing, NULL,
                experimental.locator, experimental.pager.current_html,
                experimental.pager.current_length, false, true);
            navigation.defer_document_script_pipeline = false;
            if (!initial_committed
                || (navigation.document_scripts_enabled
                    ? !experimental_execute_all_document_scripts(
                          &experimental, &navigation)
                    : !experimental_mark_section_scripts_executed(
                          &experimental,
                          experimental.pager.current_section))) {
                goto cleanup;
            }
            experimental.final_ready_us =
                experimental_initial_elapsed_us(&experimental);
            section_pager_record_height(&experimental.pager,
                                        navigation.page.layout.height);
            printf("experimental-pager-input source=%zu stored=%zu "
                   "sections=%zu anchors=%zu selected=%zu chunks=%zu "
                   "peak-buffer=%zu request=%s\n",
                   section_store.source_length, section_store.stored_length,
                   section_store.section_count, section_store.anchor_count,
                   experimental.pager.current_section,
                   stream_metrics.chunks_received,
                   stream_metrics.peak_buffered_bytes,
                   replay_basic_request ? "recorded-basic" : "navigation");
            printf("experimental-adaptive mode=sections threshold=%zu "
                   "elements=%zu attributes=%zu depth=%zu scan-us=%llu "
                   "replay-us=%llu\n",
                   experimental_router.byte_threshold,
                   experimental_router.element_count,
                   experimental_router.attribute_count,
                   experimental_router.maximum_depth,
                   (unsigned long long) experimental_router.scan_us,
                   (unsigned long long) experimental_router.replay_us);
            experimental_print_store_timing(&section_store);
        }
    } else if (url != NULL) {
        if (!browser_engine_load_url(engine, url, true)) goto cleanup;
    } else {
        size_t input_length = 0;
        input = read_file(budget, fixture, &input_length);
        const char *committed_html = input;
        size_t committed_length = input_length;
        if (input != NULL && experimental_compressed_sections) {
            experimental.fixture_source = input;
            experimental.fixture_length = input_length;
            SectionRouteStream classifier = {
                .budget = budget,
                .byte_threshold = section_route_stream_threshold(budget)
            };
            section_route_stream_scan(
                &classifier, (const unsigned char *) input, input_length);
            bool use_sections = experimental_section_explicit
                || section_route_stream_prefers_sections(
                       &classifier, input_length);
            experimental.transfer_complete_us =
                experimental_initial_elapsed_us(&experimental);
            bool section_store_ready = false;
            if (use_sections && section_store_stream_begin(
                    &section_stream_builder, &section_store, budget,
                    EXPERIMENTAL_SECTION_BLOCK_BYTES,
                    EXPERIMENTAL_SECTION_MAX_BYTES)) {
                section_store_stream_set_progress(
                    &section_stream_builder, experimental_index_progress,
                    &experimental);
                section_store_ready = section_store_stream_append(
                        &section_stream_builder,
                        (const unsigned char *) input, input_length)
                    && section_store_stream_finish(&section_stream_builder);
            }
            if (use_sections && !section_store_ready
                && section_stream_builder.active) {
                section_store_stream_abort(&section_stream_builder);
            }
            if (!use_sections) {
                printf("experimental-adaptive mode=full-document source=%zu "
                       "threshold=%zu elements=%zu attributes=%zu depth=%zu "
                       "fixture=yes\n",
                       input_length, classifier.byte_threshold,
                       classifier.element_count, classifier.attribute_count,
                       classifier.maximum_depth);
            } else if (!section_store_ready
                || experimental_section >= section_store.section_count
                || !section_pager_init(&experimental.pager, &section_store,
                                       experimental_section, 1)
                || !experimental_script_state_init(&experimental)) {
                goto cleanup;
            }
            if (use_sections) {
                printf("experimental-adaptive mode=sections source=%zu "
                       "threshold=%zu elements=%zu attributes=%zu depth=%zu "
                       "fixture=yes\n",
                       input_length, classifier.byte_threshold,
                       classifier.element_count, classifier.attribute_count,
                       classifier.maximum_depth);
                snprintf(experimental.locator, sizeof(experimental.locator),
                         "%s", "https://fixture.test/");
                experimental.active = true;
                if (!experimental_install_navigation_callbacks(
                        &navigation, &experimental)
                    || !navigation_set_runtime_section_identity(
                        &navigation, experimental.pager.current_section)) {
                    goto cleanup;
                }
                committed_html = experimental.pager.current_html;
                committed_length = experimental.pager.current_length;
            }
        }
        if (experimental.active) {
            if (!experimental_preflight_external_layout(
                    &experimental, &navigation, &generation,
                    fonts_ready ? &fonts : NULL)
                || !navigation_set_runtime_section_identity(
                       &navigation,
                       experimental.pager.current_section)) goto cleanup;
            committed_html = experimental.pager.current_html;
            committed_length = experimental.pager.current_length;
        }
        if (experimental.active) {
            navigation.defer_document_script_pipeline =
                navigation.document_scripts_enabled;
        }
        bool fixture_committed = input != NULL
            && (experimental.active
                ? browser_engine_commit_section_html(
                      engine, &experimental.backing, NULL,
                      "https://fixture.test/", committed_html,
                      committed_length, false, true)
                : browser_engine_commit_html(
                      engine, "https://fixture.test/", committed_html,
                      committed_length, true));
        navigation.defer_document_script_pipeline = false;
        if (!fixture_committed
            || (experimental.active
                && (navigation.document_scripts_enabled
                    ? !experimental_execute_all_document_scripts(
                          &experimental, &navigation)
                    : !experimental_mark_section_scripts_executed(
                          &experimental,
                          experimental.pager.current_section)))) goto cleanup;
        if (experimental.active) {
            experimental.final_ready_us =
                experimental_initial_elapsed_us(&experimental);
            section_pager_record_height(&experimental.pager,
                                        navigation.page.layout.height);
            printf("experimental-pager-input source=%zu stored=%zu sections=%zu "
                   "anchors=%zu "
                   "selected=%zu fixture=yes\n",
                   section_store.source_length, section_store.stored_length,
                   section_store.section_count, section_store.anchor_count,
                   experimental.pager.current_section);
            experimental_print_store_timing(&section_store);
        }
        if (!experimental.active) {
            budget_free(budget, input);
            input = NULL;
        }
    }
    if (experimental.active && navigation.history_index < 64) {
        experimental.history_sections[navigation.history_index] =
            experimental.pager.current_section;
        experimental.history_section_known[navigation.history_index] = true;
    }

    ExternalScriptMetrics script_metrics = {0};
    for (size_t cycle = 0; cycle <= reloads; cycle++) {
        if (cycle != 0) {
            if (!browser_engine_load_url(engine, url, true)) {
                goto cleanup;
            }
        }
        if (fetch_scripts && url != NULL) {
            script_metrics.discovered += navigation.script_discovered;
            script_metrics.attempted += navigation.script_attempted;
            script_metrics.loaded += navigation.script_loaded;
            script_metrics.failed += navigation.script_failed;
            script_metrics.skipped_cross_origin +=
                navigation.script_skipped_cross_origin;
            script_metrics.skipped_module +=
                navigation.script_skipped_module;
            script_metrics.skipped_nomodule +=
                navigation.script_skipped_nomodule;
            script_metrics.skipped_quota +=
                navigation.script_skipped_quota;
            script_metrics.skipped_pressure +=
                navigation.script_skipped_pressure;
            script_metrics.pressure_collections +=
                navigation.script_pressure_collections;
            script_metrics.pressure_reclaimed_bytes +=
                navigation.script_pressure_reclaimed_bytes;
            script_metrics.pressure_capped_requests +=
                navigation.script_pressure_capped_requests;
            script_metrics.bytes += navigation.script_bytes;
            script_metrics.cache_hits += navigation.script_cache_hits;
            script_metrics.parser_blocking +=
                navigation.script_parser_blocking;
            script_metrics.deferred += navigation.script_deferred;
            script_metrics.asynchronous += navigation.script_asynchronous;
            script_metrics.modules += navigation.script_modules;
            script_metrics.module_map_hits +=
                navigation.script_module_map_hits;
            script_metrics.inline_data_fast_paths +=
                navigation.script_inline_data_fast_paths;
            script_metrics.inline_data_fast_path_bytes +=
                navigation.script_inline_data_fast_path_bytes;
            script_metrics.inline_data_quota_exemptions +=
                navigation.script_inline_data_quota_exemptions;
            script_metrics.cost_class_rejections +=
                navigation.script_cost_class_rejections;
            script_metrics.watchdog_classification_misses +=
                navigation.script_watchdog_classification_misses;
            script_metrics.watchdog_classification_miss_bytes +=
                navigation.script_watchdog_classification_miss_bytes;
            script_metrics.watchdog_classification_miss_loops +=
                navigation.script_watchdog_classification_miss_loops;
            script_metrics.watchdog_classification_miss_flags |=
                navigation.script_watchdog_classification_miss_flags;
        }
    }
    size_t engine_shell_relayouts = navigation.incremental_relayouts;
    if (probe_script != NULL) {
        size_t probe_length = 0;
        char *probe_source = read_file(budget, probe_script, &probe_length);
        probe_source = instrument_probe_source(budget, probe_source,
                                               &probe_length);
        lxb_dom_node_t *probe_node = find_external_script(
            lxb_dom_interface_node(navigation.page.document.html));
        static const char trace_setup[] =
            "(()=>{const seen=new Map(),missing=new Set();"
            "globalThis.__tilefinchCapabilityTrace={seen,missing};"
            "globalThis.__tilefinchTraceObject=(target,label)=>new Proxy(target,{"
            "get(object,key,receiver){if(typeof key!=='symbol'){const name=label+'.'+String(key);seen.set(name,(seen.get(name)||0)+1);if(!(key in object))missing.add(name);}return Reflect.get(object,key,receiver);},"
            "set(object,key,value,receiver){if(typeof key!=='symbol'){const name=label+'.'+String(key)+'=';seen.set(name,(seen.get(name)||0)+1);if(!(key in object))missing.add(name);}return Reflect.set(object,key,value,receiver);},"
            "construct(target,args,newTarget){seen.set(label+'.construct',(seen.get(label+'.construct')||0)+1);return globalThis.__tilefinchTraceObject(Reflect.construct(target,args,newTarget),label+'.instance');}});"
            "globalThis.__tilefinchGlobalProxy=__tilefinchTraceObject(globalThis,'global');"
            "globalThis.document=__tilefinchTraceObject(document,'document');"
            "globalThis.navigator=__tilefinchTraceObject(navigator,'navigator');"
            "globalThis.performance=__tilefinchTraceObject(performance,'performance');"
            "globalThis.crypto=__tilefinchTraceObject(crypto,'crypto');"
            "globalThis.location=__tilefinchTraceObject(location,'location');"
            "for(const name of ['XMLHttpRequest','PerformanceObserver','Blob','URL'])if(typeof globalThis[name]==='function')globalThis[name]=__tilefinchTraceObject(globalThis[name],name);"
            "document.location=location;})();";
        bool probe_ok = probe_source != NULL && probe_node != NULL
            && navigation_evaluate_external_script(
                &navigation, probe_node, trace_setup,
                sizeof(trace_setup) - 1,
                "https://fixture.test/capability-trace.js")
            && navigation_evaluate_external_script(
                &navigation, probe_node, probe_source, probe_length,
                "https://fixture.test/orchestrator.js");
        if (probe_ok) {
            static const char replay_load[] =
                "window.dispatchEvent(new Event('load'));"
                "globalThis.pocSummary='decoder-s='+typeof s+"
                "',decoder-G='+typeof G+"
                "',worker='+typeof Worker+',blob='+typeof Blob+"
                "',wasm='+typeof WebAssembly+',screen='+typeof screen+"
                "',canvas='+typeof CanvasRenderingContext2D;";
            probe_ok = navigation_evaluate_external_script(
                &navigation, probe_node, replay_load,
                sizeof(replay_load) - 1,
                "https://fixture.test/probe-load.js");
        }
        printf("script-probe status=%s bytes=%zu error=\"%s\"\n",
               probe_ok ? "completed" : "failed", probe_length,
               navigation.page.script_result.error);
        budget_free(budget, probe_source);
    }
    if (wpt_test_rendered && navigation.page.runtime != NULL) {
        static const char rendered_event[] =
            "document.documentElement.dispatchEvent("
            "new Event('TestRendered'));";
        if (!script_runtime_evaluate_diagnostic(
                navigation.page.runtime, rendered_event,
                "tilefinch:wpt-test-rendered",
                &navigation.page.script_result)
            || (script_runtime_consume_relayout(navigation.page.runtime)
                && !navigation_relayout(&navigation))) {
            goto cleanup;
        }
    }
    /* Hibernation feasibility spike (host lab only): fork at a chosen
       tick; the child sleeps through a wall-clock gap, then resumes the
       identical remaining tick sequence writing frames to its own
       directory.  Bit-identical child frames against an unforked control
       run prove the quiesced state is temporally self-contained. */
    long hibernate_spike_tick = -1;
#if !defined(__PSP__)
    {
        const char *spike = getenv("TILEFINCH_HIBERNATE_SPIKE_TICK");
        if (spike != NULL) hibernate_spike_tick = atol(spike);
    }
#endif
    for (size_t i = 0; i < ticks; i++) {
#if !defined(__PSP__)
        if (hibernate_spike_tick >= 0 && i == (size_t) hibernate_spike_tick) {
            const char *resume_dir = getenv("TILEFINCH_HIBERNATE_RESUME_DIR");
            pid_t spike_pid = resume_dir == NULL ? -1 : fork();
            if (spike_pid == 0) {
                char log_path[1024];
                snprintf(log_path, sizeof(log_path), "%s.log", resume_dir);
                freopen(log_path, "w", stdout);
                freopen(log_path, "a", stderr);
                sleep(3);
                loop_output_dir = resume_dir;
                fprintf(stderr, "hibernate-spike resumed tick=%zu\n", i);
            } else if (spike_pid > 0) {
                fprintf(stderr, "hibernate-spike forked child=%d tick=%zu\n",
                        (int) spike_pid, i);
            }
        }
#endif
        if (pace_real_time && tick_ms != 0) {
            struct timespec delay = {
                .tv_sec = (time_t) (tick_ms / 1000u),
                .tv_nsec = (long) ((tick_ms % 1000u) * 1000000u)
            };
            while (nanosleep(&delay, &delay) != 0 && errno == EINTR) {}
        }
        if (!browser_engine_advance_runtime(
                engine, (unsigned) tick_ms, 8, NULL)) {
            goto cleanup;
        }
        if (!navigation_run_background_resources(&navigation)) {
            goto cleanup;
        }
        /* WPT reftests are captured from the settled result, including local
           web fonts.  The shared transport worker waits in real time while
           the lab normally advances synthetic ticks as fast as possible;
           without this bounded yield, the whole tick budget can elapse
           before the worker gets one poll and an Ahem-dependent reference
           is compared using fallback glyphs.  This is host evidence pacing,
           not part of the PSP frame loop. */
        if (wpt_test_rendered
            && navigation_background_resources_pending(&navigation)) {
#if !defined(__PSP__)
            struct timespec worker_yield = {
                .tv_sec = 0,
                .tv_nsec = 1000000L
            };
            while (nanosleep(&worker_yield, &worker_yield) != 0
                   && errno == EINTR) {}
#else
            fetch_background_transport_wait(1u);
#endif
        }
    }
    /* Reproduction hook for the device late-rebuild collapse: force N
       resource-driven full stylesheet rebuilds after the trace records are
       consumed, printing the resulting rule count and page height each
       time.  A correct rebuild preserves the external CSS the incumbent
       stylesheet already applied even when replay cannot re-serve it. */
    {
        const char *force_rebuilds = getenv("TILEFINCH_FORCE_REBUILDS");
        long rebuilds = force_rebuilds == NULL ? 0 : atol(force_rebuilds);
        for (long r = 0; r < rebuilds; r++) {
            navigation.style_rebuild_required = true;
            bool ok = navigation_relayout(&navigation);
            printf("forced-rebuild %ld ok=%d css-rules=%zu height=%d "
                   "css-loaded=%zu/%zu\n",
                   r, ok, navigation.page.stylesheet.count,
                   navigation.page.layout.height,
                   navigation.page.external_stylesheets.loaded,
                   navigation.page.external_stylesheets.discovered);
        }
    }
    if (getenv("TILEFINCH_DUMP_BUDGET") != NULL) {
        budget_report_categories(budget, "post-ticks", stdout);
        budget_report_active(budget, BUDGET_CATEGORY_RESOURCE,
                             32 * 1024, stdout);
    }
    if (navigation.page.runtime != NULL) {
        script_runtime_report_memory(navigation.page.runtime, stdout);
        if (getenv("TILEFINCH_TRACE_JS_ROOTS") != NULL) {
            script_runtime_report_roots(navigation.page.runtime, stdout,
                                        "post-ticks", "top", 0);
            for (size_t i = 0; i < navigation.page.frame_count; i++) {
                NavigationFrame *frame = &navigation.page.frames[i];
                if (frame->runtime != NULL) {
                    script_runtime_report_roots(
                        frame->runtime, stdout, "post-ticks", "child",
                        frame->parent_handle);
                }
            }
        }
        printf("script-modules compiled=%zu compile-us=%llu\n",
               navigation.page.script_result.module_compile_count,
               navigation.page.script_result.module_compile_us);
    }
    /* Post-run page evaluation for lab debugging: run the given script in
       the page context after the tick loop.  Combine with
       TILEFINCH_TRACE_CONSOLE to read its console output. */
    const char *probe_eval_path = getenv("TILEFINCH_PROBE_EVAL_FILE");
    if (probe_eval_path != NULL) {
        size_t eval_length = 0;
        char *eval_source = read_file(budget, probe_eval_path, &eval_length);
        lxb_dom_node_t *eval_node = find_external_script(
            lxb_dom_interface_node(navigation.page.document.html));
        bool eval_ok = eval_source != NULL && eval_node != NULL
            && navigation_evaluate_external_script(
                &navigation, eval_node, eval_source, eval_length,
                "https://fixture.test/debug-eval.js");
        printf("probe-eval status=%s error=\"%s\"\n",
               eval_ok ? "ok" : "failed",
               navigation.page.script_result.error);
        budget_free(budget, eval_source);
    }
    if (getenv("TILEFINCH_TRACE_DOM") != NULL) {
        size_t remaining = 1024;
        dump_probe_dom(lxb_dom_interface_node(navigation.page.document.html),
                       0, &remaining);
    }
    if (trace_frames
        && !navigation_collect_frame_capability_trace(&navigation)) {
        goto cleanup;
    }
    if (trace_page) {
        (void) navigation_collect_page_capability_trace(&navigation);
    }
    if (probe_script != NULL) {
        lxb_dom_node_t *probe_node = find_external_script(
            lxb_dom_interface_node(navigation.page.document.html));
        static const char trace_report[] =
            "{const frame=document.querySelector('iframe');globalThis.pocSummary='missing='+[...__tilefinchCapabilityTrace.missing].filter(name=>!/^global[.][A-Za-z]+[0-9]=?$/.test(name)&&name!=='global.runProgram=').sort().join(',')+'|navigator='+[...__tilefinchCapabilityTrace.seen.keys()].filter(name=>/^navigator[.]/.test(name)).sort().join(',')+'|iframe='+(frame?String(frame.src):'none');}";
        if (probe_node != NULL) {
            (void) navigation_evaluate_external_script(
                &navigation, probe_node, trace_report,
                sizeof(trace_report) - 1,
                "https://fixture.test/capability-report.js");
        }
        const char *probe_text = document_body_text(
            &navigation.page.document);
        if (probe_text == NULL) probe_text = "";
        size_t probe_text_length = navigation.page.document.body_text == NULL
                                   ? 0
                                   : navigation.page.document.body_text_length;
        size_t shown = probe_text_length < 512 ? probe_text_length : 512;
        printf("script-probe body-text-bytes=%zu text=\"%.*s\"\n",
               probe_text_length, (int) shown, probe_text);
        size_t remaining = 160;
        dump_probe_dom(lxb_dom_interface_node(navigation.page.document.html),
                       0, &remaining);
    }
    application->engine_controller = browser_engine_controller(engine);
    if (application->engine_controller == NULL) {
        if (!browser_engine_refresh_shell(engine)) goto cleanup;
        application->engine_controller = browser_engine_controller(engine);
    }
    if (application->engine_controller == NULL) goto cleanup;
    bool initial_remote_swap = false;
    if (!experimental_process_remote_elements(
            experimental_compressed_sections ? &experimental : NULL,
            &navigation, &controller, user_css,
            &initial_remote_swap)) goto cleanup;
    if (focus_id != NULL) {
        lxb_dom_node_t *focused = find_element_id(
            lxb_dom_interface_node(navigation.page.document.html), focus_id);
        if (!browser_engine_focus_node(engine, focused)) goto cleanup;
    }
    for (size_t i = 0; i < focus_next; i++) {
        if (!browser_engine_focus_move(engine, true)) goto cleanup;
    }
    if (typed != NULL
        && !browser_engine_insert_text(engine, typed, strlen(typed))) {
        goto cleanup;
    }
    if (typed != NULL) {
        for (size_t i = 0; i < interaction_ticks; i++) {
            if (!browser_engine_advance_runtime(
                    engine, (unsigned) tick_ms, 8, NULL)) goto cleanup;
        }
        trace_interaction_state(&navigation, "after-input");
    }
    if (click_selector != NULL) {
        if (!navigation_dispatch_event(&navigation, click_selector, "click")) {
            goto cleanup;
        }
        for (size_t i = 0; i < interaction_ticks; i++) {
            if (!browser_engine_advance_runtime(
                    engine, (unsigned) tick_ms, 8, NULL)) goto cleanup;
        }
        trace_interaction_state(&navigation, "after-click");
    }
    if (post_click_selector != NULL) {
        if (post_click_from_top
            && !browser_engine_scroll_by(engine, -2147483647)) {
            goto cleanup;
        }
        if (!navigation_dispatch_event(&navigation, post_click_selector,
                                       "click")) goto cleanup;
        for (size_t i = 0; i < interaction_ticks; i++) {
            if (!browser_engine_advance_runtime(
                    engine, (unsigned) tick_ms, 8, NULL)) goto cleanup;
        }
        trace_interaction_state(&navigation, "after-post-click");
    }
    ControllerAction action = {0};
    if (activate && !browser_engine_activate(engine, &action)) goto cleanup;
    if (follow_action
        && !browser_engine_execute_action(
               engine, &action, 4 * MIB, 30000)) goto cleanup;
    bool post_interaction_remote_swap = false;
    if (!experimental_process_remote_elements(
            experimental_compressed_sections ? &experimental : NULL,
            &navigation, &controller, user_css,
            &post_interaction_remote_swap)) goto cleanup;
    if (getenv("TILEFINCH_TRACE_FINAL_DOM") != NULL) {
        size_t remaining = 240;
        dump_probe_dom(lxb_dom_interface_node(navigation.page.document.html),
                       0, &remaining);
    }
    const NavigationEntry *pre_style_entry = navigation_current(&navigation);
    int pre_style_maximum = viewport_max_scroll_css(
        &navigation.viewport, navigation.page.layout.height);
    bool pre_style_at_bottom = pre_style_entry != NULL
        && pre_style_entry->scroll_y >= pre_style_maximum;
    if (hide_cookie_banners && user_css != NULL) {
        fprintf(stderr, "cookie-banner and file user CSS cannot be combined\n");
        goto cleanup;
    }
    if (hide_cookie_banners) {
        char cookie_css[8192];
        size_t cookie_css_length = 0;
        if (!content_blocker_cookie_banner_css(
                cookie_css, sizeof(cookie_css), &cookie_css_length)
            || !navigation_set_user_css(
                &navigation, cookie_css, cookie_css_length)
            || !navigation_relayout(&navigation)) {
            fprintf(stderr, "interactive cookie-banner CSS failed\n");
            goto cleanup;
        }
    } else if (!apply_user_css(&navigation, user_css)) {
        fprintf(stderr, "interactive user stylesheet failed: %s\n",
                user_css == NULL ? "" : user_css);
        goto cleanup;
    }
    const NavigationEntry *styled_entry = navigation_current(&navigation);
    if (styled_entry != NULL) {
        int maximum_scroll = viewport_max_scroll_css(
            &navigation.viewport, navigation.page.layout.height);
        navigation_set_scroll(
            &navigation, pre_style_at_bottom ? maximum_scroll
              : (styled_entry->scroll_y < maximum_scroll
                   ? styled_entry->scroll_y : maximum_scroll));
    }
#ifdef TILEFINCH_HAVE_HOST_MEDIA
    if (media_file != NULL) {
        char media_error[256] = {0};
        if (!interactive_media_open(
                &media, budget, &navigation, media_file, false,
                media_error, sizeof(media_error))) {
            fprintf(stderr, "media setup failed: %s\n", media_error);
            goto cleanup;
        }
        interactive_media_print_status(&media);
    } else if (!interactive_media_sync_route(
                   &media, budget, &navigation)) {
        goto cleanup;
    }
#else
    if (media_file != NULL) {
        fprintf(stderr,
                "media setup failed: this build has no FFmpeg host backend\n");
        goto cleanup;
    }
#endif
    if (scroll_bottom && !browser_engine_scroll_to_edge(engine, true)) {
        goto cleanup;
    }
    if (scroll_y_requested
        && !browser_engine_scroll_by(engine, requested_scroll_y)) {
        goto cleanup;
    }

    VisualEvidenceFrames visual_frames = {0};
    size_t frame_pixels = 0;
    if (navigation.incremental_relayouts != engine_shell_relayouts
        && !browser_engine_refresh_render_shell(engine)) goto cleanup;
    application->engine_render = browser_engine_render_shell(engine);
    frame = browser_engine_framebuffer(engine, &frame_pixels);
    if (application->engine_render == NULL || frame == NULL
        || frame_pixels != 480u * 272u) goto cleanup;
    if (interactive_loop || commands_path != NULL) {
        command_stream = interactive_loop ? stdin : fopen(commands_path, "r");
        if (command_stream == NULL
            || !run_command_loop(command_stream, interactive_loop,
                                 loop_output_dir, engine, budget, &navigation,
                                 &controller, &cache, frame, frame_pixels,
                                 &media,
                                 user_css, loop_capture_frames,
                                 pace_real_time,
                                 experimental_compressed_sections
                                   ? &experimental : NULL,
                                 visual_evidence ? &visual_frames : NULL)) {
                goto cleanup;
            }
    }
    bool loop_mode = interactive_loop || commands_path != NULL;
    if ((loop_mode
         && !loop_render(
                engine, &cache, &navigation, &controller, &media,
                output, NULL))
        || (!loop_mode
            && !loop_render(
                engine, &cache, &navigation, &controller, &media,
                platform_sim ? NULL : output, NULL))) goto cleanup;
    if (!dump_text_metrics(text_metrics_path, &navigation.page.layout)) {
        fprintf(stderr, "could not write text metrics: %s\n",
                text_metrics_path == NULL ? "" : text_metrics_path);
        goto cleanup;
    }
    budget_mark_phase(budget, "render");

    printf("interactive status=ok title=\"%s\" height=%d scroll-y=%d links=%zu controls=%zu "
           "ticks=%zu callbacks=%zu pending=%zu relayouts=%zu\n",
           navigation.page.document.title, navigation.page.layout.height,
           navigation_current(&navigation) == NULL ? 0
             : navigation_current(&navigation)->scroll_y,
           navigation.page.layout.link_count,
           navigation.page.layout.control_count,
           navigation.page.script_result.runtime_ticks,
           navigation.page.script_result.timer_callbacks_run,
           navigation.page.script_result.pending_tasks,
           navigation.incremental_relayouts);
    ScriptDeterministicReplayDiagnostics replay_diagnostics = {0};
    bool replay_diagnostics_available = false;
    if (deterministic_replay_requested) {
        replay_diagnostics_available =
            script_runtime_deterministic_replay_diagnostics(
                navigation.page.runtime, &replay_diagnostics);
        if (replay_diagnostics_available) {
            const char *replay_seed_source = replay_http != NULL
                    && replay_diagnostics.seed
                           == replay_diagnostics.clock_origin_ms
                ? SCRIPT_DETERMINISTIC_REPLAY_SEED_SOURCE
                : SCRIPT_DETERMINISTIC_CONFIGURED_SEED_SOURCE;
            printf("deterministic-replay enabled=yes seed=%llu "
                   "seed-source=%s rng=%s clock=%s host-elapsed-ms=%llu "
                   "wall-elapsed-ms=%llu monotonic-elapsed-ms=%llu "
                   "wall-observations=%llu monotonic-observations=%llu "
                   "monotonic-samples=%llu "
                   "clock-sources=%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,"
                   "%llu,%llu,%llu,%llu "
                   "performance-entries=%s intl=%s\n",
                   (unsigned long long) replay_diagnostics.seed,
                   replay_seed_source,
                   SCRIPT_DETERMINISTIC_ENTROPY_CONTRACT,
                   SCRIPT_DETERMINISTIC_CLOCK_CONTRACT,
                   (unsigned long long)
                       replay_diagnostics.host_elapsed_ms,
                   (unsigned long long)
                       replay_diagnostics.wall_elapsed_ms,
                   (unsigned long long)
                       replay_diagnostics.monotonic_elapsed_ms,
                   (unsigned long long)
                       replay_diagnostics.wall_observations,
                   (unsigned long long)
                       replay_diagnostics.monotonic_observations,
                   (unsigned long long)
                       replay_diagnostics.monotonic_samples,
                   (unsigned long long) replay_diagnostics.sources.date_now,
                   (unsigned long long)
                       replay_diagnostics.sources.date_function,
                   (unsigned long long)
                       replay_diagnostics.sources.date_constructor,
                   (unsigned long long)
                       replay_diagnostics.sources.performance_now,
                   (unsigned long long)
                       replay_diagnostics.sources.performance_mark,
                   (unsigned long long)
                       replay_diagnostics.sources.performance_measure,
                   (unsigned long long)
                       replay_diagnostics.sources.animation_timeline,
                   (unsigned long long) replay_diagnostics.sources
                       .idle_deadline_time_remaining,
                   (unsigned long long)
                       replay_diagnostics.sources.animation_frame,
                   (unsigned long long)
                       replay_diagnostics.sources.event_timestamp,
                   (unsigned long long)
                       replay_diagnostics.sources.intersection_observer,
                   (unsigned long long)
                       replay_diagnostics.sources.idle_callback_start,
                   SCRIPT_DETERMINISTIC_PERFORMANCE_ENTRIES,
                   SCRIPT_DETERMINISTIC_INTL_SURFACE);
        } else {
            printf("deterministic-replay enabled=yes diagnostics=unavailable\n");
        }
    }
    printf("document-memory nodes=%zu elements=%zu text-nodes=%zu "
           "attributes=%zu attribute-bytes=%zu body-text=%zu\n",
           navigation.page.document.node_count,
           navigation.page.document.element_count,
           navigation.page.document.text_node_count,
           navigation.page.document.attribute_count,
           navigation.page.document.attribute_value_bytes,
           navigation.page.document.text_bytes);
    const LayoutDocument *retained_layout = &navigation.page.layout;
    size_t retained_layout_bytes =
        retained_layout->capacity * sizeof(*retained_layout->commands)
        + retained_layout->link_capacity * sizeof(*retained_layout->links)
        + retained_layout->control_capacity
            * sizeof(*retained_layout->controls)
        + retained_layout->sticky_capacity
            * sizeof(*retained_layout->sticky_ranges)
        + retained_layout->fixed_capacity
            * sizeof(*retained_layout->fixed_ranges)
        + retained_layout->node_box_capacity
            * sizeof(*retained_layout->node_boxes)
        + retained_layout->paint_order_count
            * sizeof(*retained_layout->paint_order)
        + retained_layout->count * sizeof(*retained_layout->command_flags)
        + (retained_layout->spatial_band_count + 1)
            * sizeof(*retained_layout->spatial_band_offsets)
        + retained_layout->spatial_band_order_count
            * sizeof(*retained_layout->spatial_band_orders)
        + retained_layout->spatial_global_count
            * sizeof(*retained_layout->spatial_global_orders)
        + retained_layout->overflow_order_count
            * sizeof(*retained_layout->overflow_orders);
    printf("layout-retained bytes=%zu command-size=%zu commands=%zu/%zu "
           "node-box-size=%zu node-boxes=%zu/%zu links=%zu/%zu "
           "controls=%zu/%zu\n",
           retained_layout_bytes, sizeof(*retained_layout->commands),
           retained_layout->count, retained_layout->capacity,
           sizeof(*retained_layout->node_boxes),
           retained_layout->node_box_count,
           retained_layout->node_box_capacity,
           retained_layout->link_count, retained_layout->link_capacity,
           retained_layout->control_count,
           retained_layout->control_capacity);
    printf("layout-responsiveness work=%zu yields=%zu max-slice-us=%llu "
           "max-slice-work=%zu\n",
           retained_layout->layout_work_units,
           retained_layout->cooperative_yields,
           (unsigned long long) retained_layout->max_work_slice_us,
           retained_layout->max_work_slice_units);
    printf("resources stylesheets=%zu/%zu css-rules=%zu "
           "css-imports=%zu/%zu "
           "css-import-skips=%zu/%zu css-bytes=%zu css-cache-hits=%zu "
           "css-fragments=%zu/%zu/%zu css-fragment-rules=%zu "
           "css-fragment-bytes=%zu css-ir=%zu/%zu/%zu "
           "css-ir-operations=%zu css-ir-bytes=%zu "
           "css-retained-hits=%zu css-retries=%zu css-transient-failures=%zu "
           "css-terminal-failures=%zu css-retry-suppressed=%zu "
           "css-final-retry-grants=%zu "
           "css-pressure-skips=%zu css-theme-skips=%zu css-fallbacks=%zu "
           "css-pressure-serializations=%zu "
           "css-batches=%zu css-first-batch=%zu css-deadline=%s/%zu css-elapsed-ms=%llu "
           "images=%zu/%zu image-cache-hits=%zu "
           "image-decoded-cache-hits=%zu "
           "image-backgrounds=%zu image-rewrites=%zu image-encoded=%zu "
           "image-decoded=%zu image-downsampled=%zu "
           "image-source-peak=%zu image-target-peak=%zu\n",
           navigation.page.external_stylesheets.loaded,
           navigation.page.external_stylesheets.discovered,
           navigation.page.external_stylesheets.rules_added,
           navigation.page.external_stylesheets.imports_loaded,
           navigation.page.external_stylesheets.imports_discovered,
           navigation.page.external_stylesheets.imports_skipped_conditions,
           navigation.page.external_stylesheets.imports_skipped_depth,
           navigation.page.external_stylesheets.bytes,
           navigation.page.external_stylesheets.cache_hits,
           navigation.page.external_stylesheets.compiled_fragment_hits,
           navigation.page.external_stylesheets.compiled_fragment_misses,
           navigation.page.external_stylesheets.compiled_fragment_stores,
           navigation.page.external_stylesheets
             .compiled_fragment_rules_reused,
           navigation.page.external_stylesheets.compiled_fragment_bytes,
           navigation.page.external_stylesheets.parsed_ir_hits,
           navigation.page.external_stylesheets.parsed_ir_misses,
           navigation.page.external_stylesheets.parsed_ir_stores,
           navigation.page.external_stylesheets.parsed_ir_operations_reused,
           navigation.page.external_stylesheets.parsed_ir_bytes,
           navigation.page.external_stylesheets.retained_body_hits,
           navigation.page.external_stylesheets.transient_retries,
           navigation.page.external_stylesheets.transient_failures,
           navigation.page.external_stylesheets.terminal_failures,
           navigation.page.external_stylesheets.retry_suppressed,
           navigation.page.external_stylesheets.final_retry_grants,
           navigation.page.external_stylesheets.skipped_pressure,
           navigation.page.external_stylesheets.skipped_alternate_theme,
           navigation.performance.stylesheet_pressure_fallbacks,
           navigation.page.external_stylesheets.pressure_serializations,
           navigation.page.external_stylesheets.batches,
           navigation.page.external_stylesheets.first_batch_loaded,
           navigation.page.external_stylesheets.deadline_exceeded
             ? "exceeded" : "within",
           navigation.page.external_stylesheets.deadline_cancelled,
           (unsigned long long)
             navigation.page.external_stylesheets.elapsed_ms,
           navigation.page.images.stats.loaded,
           navigation.page.images.stats.discovered,
           navigation.page.images.stats.cache_hits,
           navigation.page.images.stats.decoded_cache_hits,
           navigation.page.images.stats.backgrounds_loaded,
           navigation.page.images.stats.compatible_format_rewrites,
           navigation.page.images.stats.encoded_bytes,
           navigation.page.images.stats.decoded_bytes,
           navigation.page.images.stats.downsampled,
           navigation.page.images.stats.largest_source_decode_bytes,
           navigation.page.images.stats.largest_target_decode_bytes);
    printf("image-network fetch-failures=%zu/%zu/%zu/%zu/%zu/%zu "
           "progress=%zu/%zu/%zu stalled-polls=%zu "
           "no-progress-cancelled=%zu origin-cooldown=%zu/%zu "
           "max-no-progress-ms=%llu max-request-ms=%llu\n",
           navigation.page.images.stats.fetch_failures_http_4xx,
           navigation.page.images.stats.fetch_failures_http_5xx,
           navigation.page.images.stats.fetch_failures_timeout,
           navigation.page.images.stats.fetch_failures_cancelled,
           navigation.page.images.stats.fetch_failures_quota,
           navigation.page.images.stats.fetch_failures_transport,
           navigation.page.images.stats.progress_samples,
           navigation.page.images.stats.progress_events,
           navigation.page.images.stats.progress_bytes,
           navigation.page.images.stats.stalled_polls,
           navigation.page.images.stats.no_progress_cancelled,
           navigation.page.images.stats.no_progress_origin_cooldowns,
           navigation.page.images.stats.no_progress_origin_skipped,
           (unsigned long long)
             navigation.page.images.stats.maximum_no_progress_ms,
           (unsigned long long)
             navigation.page.images.stats.maximum_request_ms);
    printf("web-fonts declarations=%zu sources=%zu attempted=%zu loaded=%zu "
           "failed=%zu unsupported=%zu skipped=%zu duplicates=%zu "
           "cache-hits=%zu encoded=%zu retained=%zu deadline=%s/%zu "
           "elapsed-ms=%llu pending=%s\n",
           navigation.page.external_fonts.declarations_discovered,
           navigation.page.external_fonts.sources_discovered,
           navigation.page.external_fonts.attempted,
           navigation.page.external_fonts.loaded_faces,
           navigation.page.external_fonts.failed,
           navigation.page.external_fonts.unsupported,
           navigation.page.external_fonts.skipped_limit,
           navigation.page.external_fonts.duplicate_sources,
           navigation.page.external_fonts.cache_hits,
           navigation.page.external_fonts.encoded_bytes,
           navigation.page.external_fonts.retained_encoded_bytes,
           navigation.page.external_fonts.deadline_exceeded
             ? "exceeded" : "within",
           navigation.page.external_fonts.deadline_cancelled,
           (unsigned long long) navigation.page.external_fonts.elapsed_ms,
           fonts_external_loader_pending(
               &navigation.page.external_font_loader) ? "yes" : "no");
    printf("resource-responsiveness css-work=%zu css-yields=%zu "
           "css-max-slice-us=%llu css-max-slice-work=%zu image-work=%zu "
           "image-yields=%zu image-max-slice-us=%llu "
           "image-max-slice-work=%zu\n",
           navigation.page.external_stylesheets.work_units,
           navigation.page.external_stylesheets.cooperative_yields,
           (unsigned long long)
             navigation.page.external_stylesheets.max_slice_us,
           navigation.page.external_stylesheets.max_slice_work_units,
           navigation.page.images.stats.work_units,
           navigation.page.images.stats.cooperative_yields,
           (unsigned long long) navigation.page.images.stats.max_slice_us,
           navigation.page.images.stats.max_slice_work_units);
    printf("image-attribution-us traversal=%llu styles=%llu admission=%llu "
           "node-styles=%llu pseudo-styles=%llu style-cache=%zu/%zu "
           "resolve=%llu cache=%llu context=%llu enqueue=%llu "
           "scheduler=%llu finish=%llu drain=%llu\n",
           (unsigned long long)
             navigation.page.images.stats.traversal_us,
           (unsigned long long)
             navigation.page.images.stats.style_resolve_us,
           (unsigned long long)
             navigation.page.images.stats.admission_us,
           (unsigned long long)
             navigation.page.images.stats.node_style_resolve_us,
           (unsigned long long)
             navigation.page.images.stats.pseudo_style_resolve_us,
           navigation.page.images.stats.node_style_cache_hits,
           navigation.page.images.stats.node_style_cache_misses,
           (unsigned long long)
             navigation.page.images.stats.admission_resolve_us,
           (unsigned long long)
             navigation.page.images.stats.admission_cache_us,
           (unsigned long long)
             navigation.page.images.stats.admission_context_us,
           (unsigned long long)
             navigation.page.images.stats.admission_enqueue_us,
           (unsigned long long)
             navigation.page.images.stats.scheduler_us,
           (unsigned long long) navigation.page.images.stats.finish_us,
           (unsigned long long) navigation.page.images.stats.drain_us);
    printf("stylesheet rules=%zu important-rules=%zu layers=%zu variables=%zu scoped-variables=%zu "
           "generated-text=%zu/%zu deferred-bytes=%zu\n",
           navigation.page.stylesheet.count,
           navigation.page.stylesheet.important_rule_count,
           navigation.page.stylesheet.layer_count,
           navigation.page.stylesheet.variable_count,
           navigation.page.stylesheet.custom_rule_count,
           navigation.page.stylesheet.generated_text_count,
           navigation.page.stylesheet.generated_text_bytes,
           navigation.page.stylesheet.deferred_bytes);
    printf("deferred-program instructions=%zu/%zu bytes=%zu "
           "executions=%llu fallbacks=%llu executed-instructions=%llu\n",
           navigation.page.stylesheet.deferred_instruction_count,
           navigation.page.stylesheet.deferred_instruction_capacity,
           navigation.page.stylesheet.deferred_program_bytes,
           (unsigned long long)
             navigation.page.stylesheet.deferred_program_executions,
           (unsigned long long)
             navigation.page.stylesheet.deferred_program_fallbacks,
           (unsigned long long)
             navigation.page.stylesheet.deferred_program_instructions);
    const Stylesheet *diagnostic_sheet = &navigation.page.stylesheet;
    printf("stylesheet-diagnostics declarations=%llu supported=%llu "
           "rejected=%llu deferred=%llu custom-drops=%llu "
           "selector-drops=%llu unknown-media=%llu supports-false=%llu "
           "rejected-properties=",
           (unsigned long long) diagnostic_sheet->diagnostic_declarations,
           (unsigned long long)
             diagnostic_sheet->diagnostic_supported_declarations,
           (unsigned long long)
             diagnostic_sheet->diagnostic_rejected_declarations,
           (unsigned long long)
             diagnostic_sheet->diagnostic_deferred_declarations,
           (unsigned long long)
             diagnostic_sheet->diagnostic_custom_property_drops,
           (unsigned long long) diagnostic_sheet->diagnostic_selector_drops,
           (unsigned long long)
             diagnostic_sheet->diagnostic_unknown_media_features,
           (unsigned long long)
             diagnostic_sheet->diagnostic_supports_false_queries);
    if (diagnostic_sheet->diagnostic_rejected_property_count == 0) putchar('-');
    for (size_t i = 0;
         i < diagnostic_sheet->diagnostic_rejected_property_count; i++) {
        const StyleDiagnosticProperty *entry =
            &diagnostic_sheet->diagnostic_rejected_properties[i];
        printf("%s%s:%u", i == 0 ? "" : ",", entry->name, entry->count);
    }
    putchar('\n');
    printf("performance-us network=%llu parse=%llu script=%llu style=%llu "
           "resource=%llu layout=%llu relayout=%llu runtime=%llu "
           "fast-relayouts=%zu full-relayouts=%zu raster=%llu frame=%llu "
           "max-raster=%llu max-frame=%llu frame-count=%zu "
           "command-candidates=%llu spatial-bands=%zu "
           "glyphs=%zu/%zu glyph-hits=%zu glyph-misses=%zu "
           "glyph-evictions=%zu scaled-image-hits=%zu builds=%zu "
           "prefetch-rows=%zu prefetch-us=%llu max-prefetch=%llu "
           "overlay-images-prewarmed=%zu overflow-images-prewarmed=%zu\n",
           (unsigned long long) navigation.performance.network_us,
           (unsigned long long) navigation.performance.parse_us,
           (unsigned long long) navigation.performance.script_us,
           (unsigned long long) navigation.performance.style_us,
           (unsigned long long) navigation.performance.resource_us,
           (unsigned long long) navigation.performance.layout_us,
           (unsigned long long) navigation.performance.relayout_us,
           (unsigned long long) navigation.performance.runtime_us,
           navigation.performance.fast_relayouts,
           navigation.performance.full_relayouts,
           (unsigned long long) cache.raster_us,
           (unsigned long long) cache.frame_us,
           (unsigned long long) cache.max_raster_us,
           (unsigned long long) cache.max_frame_us,
           cache.frames_rendered,
           (unsigned long long) cache.command_candidates,
           navigation.page.layout.spatial_band_count,
           cache.glyph_cache_count, cache.glyph_cache_bytes,
           cache.glyph_cache_hits, cache.glyph_cache_misses,
           cache.glyph_cache_evictions, cache.scaled_image_hits,
           cache.scaled_image_builds, cache.prefetch_rows,
           (unsigned long long) cache.prefetch_us,
           (unsigned long long) cache.max_prefetch_us,
           cache.overlay_images_prewarmed,
           cache.overflow_images_prewarmed);
    printf("idle-render-work scheduled=%zu completed=%zu cancelled=%zu "
           "pending=%s slices=%zu units=%zu budget-exhaustions=%zu "
           "overruns=%zu image-admission-skips=%zu glyphs=%zu/%zu "
           "total-us=%llu "
           "max-slice-us=%llu max-unit-us=%llu\n",
           cache.idle_jobs_scheduled, cache.idle_jobs_completed,
           cache.idle_jobs_cancelled,
           tile_cache_idle_work_pending(&cache) ? "yes" : "no",
           cache.idle_slices, cache.idle_units,
           cache.idle_budget_exhaustions, cache.idle_slice_overruns,
           cache.idle_image_admission_skips,
           cache.idle_glyphs_prewarmed,
           cache.idle_glyph_cache_misses,
           (unsigned long long) cache.idle_us,
           (unsigned long long) cache.max_idle_slice_us,
           (unsigned long long) cache.max_idle_unit_us);
    printf("image-work-us decode=%llu/%llu scale=%llu/%llu\n",
           (unsigned long long) cache.decoded_image_us,
           (unsigned long long) cache.max_decoded_image_us,
           (unsigned long long) cache.scaled_image_us,
           (unsigned long long) cache.max_scaled_image_us);
    printf("startup-visual-work slices=%zu total-us=%llu max-slice-us=%llu "
           "max-unit-us=%llu\n",
           cache.startup_visual_slices,
           (unsigned long long) cache.startup_visual_us,
           (unsigned long long) cache.max_startup_visual_slice_us,
           (unsigned long long) cache.max_startup_visual_unit_us);
    printf("load-attribution-us stylesheets=%llu images=%llu fonts=%llu "
           "resource-fingerprint=%llu final-layout=%llu\n",
           (unsigned long long)
             navigation.performance.stylesheet_resource_us,
           (unsigned long long) navigation.performance.image_resource_us,
           (unsigned long long) navigation.performance.font_resource_us,
           (unsigned long long)
             navigation.performance.resource_fingerprint_us,
           (unsigned long long) navigation.performance.final_layout_us);
    printf("blocking-stylesheets builds=%zu reuses=%zu adoptions=%zu "
           "final-reuses=%zu continuations=%zu fallbacks=%zu "
           "continuation-inputs=%zu+%zu continuation-rules=%zu+%zu "
           "continuation-us=%llu compiled-cache=%zu/%zu stores=%zu "
           "evictions=%zu retained=%zu skips=%zu/%zu/%zu/%zu "
           "signatures=%016llx/%016llx "
           "compiled-fingerprints=%016llx/%016llx "
           "fingerprint-us=%llu\n",
           navigation.performance.blocking_stylesheet_builds,
           navigation.performance.blocking_stylesheet_reuses,
           navigation.performance.blocking_stylesheet_adoptions,
           navigation.performance.blocking_stylesheet_final_reuses,
           navigation.performance.blocking_stylesheet_continuations,
           navigation.performance
             .blocking_stylesheet_continuation_fallbacks,
           navigation.performance
             .blocking_stylesheet_continuation_prefix_inputs,
           navigation.performance
             .blocking_stylesheet_continuation_suffix_inputs,
           navigation.performance
             .blocking_stylesheet_continuation_rules_before,
           navigation.performance.blocking_stylesheet_continuation_rules,
           (unsigned long long)
             navigation.performance.blocking_stylesheet_continuation_us,
           navigation.performance.compiled_stylesheet_cache_hits,
           navigation.performance.compiled_stylesheet_cache_misses,
           navigation.performance.compiled_stylesheet_cache_stores,
           navigation.performance.compiled_stylesheet_cache_evictions,
           navigation.performance.compiled_stylesheet_cache_retained_bytes,
           navigation.performance.compiled_stylesheet_cache_unmarked_skips,
           navigation.performance.compiled_stylesheet_cache_generation_skips,
           navigation.performance.compiled_stylesheet_cache_size_skips,
           navigation.performance.compiled_stylesheet_cache_pressure_skips,
           (unsigned long long) navigation.performance
             .compiled_stylesheet_cache_last_stored_signature,
           (unsigned long long) navigation.performance
             .compiled_stylesheet_cache_last_observed_signature,
           (unsigned long long) navigation.performance
             .compiled_stylesheet_cache_last_stored_fingerprint,
           (unsigned long long) navigation.performance
             .compiled_stylesheet_cache_last_observed_fingerprint,
           (unsigned long long)
             navigation.performance.blocking_stylesheet_fingerprint_us);
    uint64_t parser_native_us =
        navigation.performance.parser_feed_us
            >= navigation.performance.parser_callback_us
        ? navigation.performance.parser_feed_us
            - navigation.performance.parser_callback_us
        : 0;
    printf("parser-attribution-us feed=%llu native=%llu callbacks=%llu "
           "metadata=%llu runtime-startup=%llu stylesheets=%llu scripts=%llu "
           "eof-metadata=%llu eof-preloads=%llu finish=%llu\n",
           (unsigned long long) navigation.performance.parser_feed_us,
           (unsigned long long) parser_native_us,
           (unsigned long long) navigation.performance.parser_callback_us,
           (unsigned long long) navigation.performance.parser_metadata_us,
           (unsigned long long)
             navigation.performance.parser_runtime_startup_us,
           (unsigned long long) navigation.performance.parser_stylesheet_us,
           (unsigned long long) navigation.performance.parser_script_us,
           (unsigned long long)
             navigation.performance.parser_eof_metadata_us,
           (unsigned long long)
             navigation.performance.parser_eof_preload_us,
           (unsigned long long) navigation.performance.parser_finish_us);
    printf("parser-blocking-attribution samples=%zu dropped=%zu "
           "compile-us=%llu execute-us=%llu rescan-us=%llu mutations=%zu\n",
           navigation.performance.blocking_script_sample_count,
           navigation.performance.blocking_script_samples_dropped,
           (unsigned long long)
             navigation.performance.parser_script_compile_us,
           (unsigned long long)
             navigation.performance.parser_script_execute_us,
           (unsigned long long)
             navigation.performance.parser_script_rescan_us,
           navigation.performance.parser_script_mutations);
    for (size_t i = 0;
         i < navigation.performance.blocking_script_sample_count; i++) {
        const NavigationBlockingScriptSample *sample =
            &navigation.performance.blocking_script_samples[i];
        printf("parser-blocking-script ordinal=%zu kind=%s "
               "nodes=%zu bytes=%zu total-us=%llu metadata-us=%llu "
               "runtime-startup-us=%llu stylesheet-us=%llu "
               "fingerprint-us=%llu process-us=%llu compile-us=%llu "
               "execute-us=%llu mutations=%zu success=%s\n",
               sample->ordinal, sample->external ? "external" : "inline",
               sample->node_count, sample->source_bytes,
               (unsigned long long) sample->total_us,
               (unsigned long long) sample->metadata_us,
               (unsigned long long) sample->runtime_startup_us,
               (unsigned long long) sample->stylesheet_us,
               (unsigned long long) sample->stylesheet_fingerprint_us,
               (unsigned long long) sample->process_us,
               (unsigned long long) sample->compile_us,
               (unsigned long long) sample->execute_us,
               sample->dom_mutations,
               sample->succeeded ? "yes" : "no");
    }
    printf("streaming-preview checks=%zu attempts=%zu paints=%zu "
           "pre-script=%zu/%zu "
           "visibility=%zu/%zu/%zu "
           "style-mismatches=%zu style-refreshes=%zu/%zu failures=%zu "
           "pressure-skips=%zu visual-readiness-skips=%zu "
           "commit-visual-readiness-skips=%zu "
           "source-bytes=%zu nodes=%zu "
           "layout-us=%llu paint-us=%llu style-refresh-us=%llu\n",
           navigation.performance.streaming_preview_checks,
           navigation.performance.streaming_preview_attempts,
           navigation.performance.streaming_preview_paints,
           navigation.performance.streaming_preview_pre_script_paints,
           navigation.performance.streaming_preview_pre_script_checks,
           navigation.performance.streaming_preview_visibility_skips,
           navigation.performance.streaming_preview_visibility_checks,
           navigation.performance.streaming_preview_visibility_nodes,
           navigation.performance.streaming_preview_style_mismatches,
           navigation.performance.streaming_preview_style_refreshes,
           navigation.performance.streaming_preview_style_refresh_attempts,
           navigation.performance.streaming_preview_style_refresh_failures,
           navigation.performance.streaming_preview_pressure_skips,
           navigation.performance.streaming_preview_visual_readiness_skips,
           navigation.performance.progressive_visual_readiness_skips,
           navigation.performance.streaming_preview_source_bytes,
           navigation.performance.streaming_preview_node_count,
           (unsigned long long)
             navigation.performance.streaming_preview_layout_us,
           (unsigned long long)
             navigation.performance.streaming_preview_paint_us,
           (unsigned long long)
             navigation.performance.streaming_preview_style_refresh_us);
    const LayoutPerformance *layout_performance =
        &navigation.page.layout.performance;
    printf("layout-attribution-us total=%llu root-style=%llu flow=%llu "
           "compact=%llu focus-index=%llu paint-order=%llu spatial-index=%llu "
           "finalize=%llu styles=%llu style-resolve-us=%llu rules=%llu/%llu "
           "vars=%llu/%llu var-cache=%llu/%llu/%llu/%llu/%zu "
           "deferred=%llu/%llu cache=%llu/%llu "
           "intrinsic-visits=%llu/%llu "
           "intrinsic-cache=%llu/%llu paired-text=%llu "
           "margin-visits=%llu cache=%llu/%llu "
           "iterators=%llu/%llu+%llu/%llu "
           "flex-measures=%llu/%llu "
           "float-bands=%llu/%llu style-copy-bytes=%llu\n",
           (unsigned long long) layout_performance->total_us,
           (unsigned long long) layout_performance->root_style_us,
           (unsigned long long) layout_performance->flow_us,
           (unsigned long long) layout_performance->compact_us,
           (unsigned long long) layout_performance->focus_index_us,
           (unsigned long long) layout_performance->paint_order_us,
           (unsigned long long) layout_performance->spatial_index_us,
           (unsigned long long) layout_performance->finalize_us,
           (unsigned long long) layout_performance->style_resolutions,
           (unsigned long long) layout_performance->style_resolve_us,
           (unsigned long long) layout_performance->style_rule_queries,
           (unsigned long long) layout_performance->style_rule_candidates,
           (unsigned long long) layout_performance->style_variable_lookups,
           (unsigned long long)
             layout_performance->style_variable_rule_candidates,
           (unsigned long long)
             layout_performance->style_variable_cache_hits,
           (unsigned long long)
             layout_performance->style_variable_cache_misses,
           (unsigned long long)
             layout_performance->style_variable_cache_negative_hits,
           (unsigned long long)
             layout_performance->style_variable_cache_evictions,
           layout_performance->style_variable_cache_bytes,
           (unsigned long long)
             layout_performance->style_deferred_rule_applications,
           (unsigned long long)
             layout_performance->style_deferred_rule_us,
           (unsigned long long) layout_performance->style_cache_hits,
           (unsigned long long) layout_performance->style_cache_misses,
           (unsigned long long) layout_performance->intrinsic_width_visits,
           (unsigned long long) layout_performance->intrinsic_min_visits,
           (unsigned long long) layout_performance->intrinsic_cache_hits,
           (unsigned long long) layout_performance->intrinsic_cache_misses,
           (unsigned long long)
             layout_performance->intrinsic_paired_text_measurements,
           (unsigned long long)
             layout_performance->margin_collapse_visits,
           (unsigned long long) layout_performance->margin_cache_hits,
           (unsigned long long) layout_performance->margin_cache_misses,
           (unsigned long long) layout_performance->flat_iterator_passes,
           (unsigned long long) layout_performance->flat_iterator_yields,
           (unsigned long long) layout_performance->flex_iterator_passes,
           (unsigned long long) layout_performance->flex_iterator_yields,
           (unsigned long long) layout_performance->flex_basis_requests,
           (unsigned long long) layout_performance->flex_minimum_requests,
           (unsigned long long) layout_performance->float_band_queries,
           (unsigned long long) layout_performance->float_exclusion_probes,
           (unsigned long long) (
             layout_performance->style_resolutions
             * sizeof(ComputedStyle)));
    const Stylesheet *selector_profile = &navigation.page.stylesheet;
    printf("selector-profile calls=%llu matches=%llu characters=%llu "
           "compounds=%llu attributes=%llu pseudos=%llu ancestors=%llu "
           "siblings=%llu descendants=%llu compiled=%zu/%zu/%zu "
           "append-reuse=%llu/%llu "
           "compiled-calls=%llu fallback-calls=%llu compiled-matches=%llu "
           "subject-cache=%llu/%llu tag-id=%llu "
           "compound-rejects=%llu/%llu "
           "relative=%llu/%llu/%llu visits=%llu max-visits=%llu "
           "exhausted=%llu\n",
           (unsigned long long) selector_profile->selector_match_calls,
           (unsigned long long) selector_profile->selector_match_successes,
           (unsigned long long) selector_profile->selector_match_characters,
           (unsigned long long) selector_profile->selector_compound_calls,
           (unsigned long long) selector_profile->selector_attribute_checks,
           (unsigned long long) selector_profile->selector_pseudo_checks,
           (unsigned long long) selector_profile->selector_ancestor_visits,
           (unsigned long long) selector_profile->selector_sibling_visits,
           (unsigned long long) selector_profile->selector_descendant_visits,
           selector_profile->selector_program_rule_count,
           selector_profile->count,
           selector_profile->selector_program_bytes,
           (unsigned long long)
             selector_profile->selector_append_reused_rules,
           (unsigned long long)
             selector_profile->selector_append_compiled_rules,
           (unsigned long long) selector_profile->selector_compiled_rule_calls,
           (unsigned long long) selector_profile->selector_fallback_rule_calls,
           (unsigned long long)
             selector_profile->selector_compiled_rule_matches,
           (unsigned long long)
             selector_profile->selector_subject_cache_hits,
           (unsigned long long)
             selector_profile->selector_subject_cache_misses,
           (unsigned long long) selector_profile->selector_tag_id_checks,
           (unsigned long long)
             selector_profile->rule_compound_filter_rejections,
           (unsigned long long)
             selector_profile->rule_ancestor_filter_rejections,
           (unsigned long long) selector_profile->selector_relative_queries,
           (unsigned long long)
             selector_profile->selector_relative_cache_hits,
           (unsigned long long) selector_profile->selector_relative_walks,
           (unsigned long long) selector_profile->selector_relative_visits,
           (unsigned long long)
             selector_profile->selector_relative_max_visits,
           (unsigned long long)
             selector_profile->selector_relative_exhaustions);
    printf("frame-attribution-us setup=%llu/%llu tiles=%llu/%llu "
           "overflow=%llu/%llu sticky=%llu/%llu fixed=%llu/%llu "
           "indicator=%llu\n",
           (unsigned long long) cache.frame_setup_us,
           (unsigned long long) cache.max_frame_setup_us,
           (unsigned long long) cache.frame_tile_us,
           (unsigned long long) cache.max_frame_tile_us,
           (unsigned long long) cache.frame_overflow_us,
           (unsigned long long) cache.max_frame_overflow_us,
           (unsigned long long) cache.frame_sticky_us,
           (unsigned long long) cache.max_frame_sticky_us,
           (unsigned long long) cache.frame_fixed_us,
           (unsigned long long) cache.max_frame_fixed_us,
           (unsigned long long) cache.frame_indicator_us);
    printf("fixed-cache builds=%zu blits=%zu pixels=%zu bytes=%zu "
           "ready=%s\n",
           cache.fixed_cache_builds, cache.fixed_cache_blits,
           cache.fixed_cache_pixels, cache.fixed_cache_bytes,
           cache.fixed_ready ? "yes" : "no");
    printf("relayout-policy fingerprints=%zu mutation-fast=%zu "
           "mutation-resource=%zu mutation-images=%zu "
           "mutation-conservative=%zu "
           "journal-overflows=%zu semantic-skips=%zu focus-outline-skips=%zu "
           "focus-paint-skips=%zu\n",
           navigation.performance.resource_fingerprint_scans,
           navigation.performance.mutation_fast_relayouts,
           navigation.performance.mutation_resource_rebuilds,
           navigation.performance.mutation_image_resource_scans,
           navigation.performance.mutation_conservative_scans,
           navigation.performance.mutation_journal_overflows,
           navigation.performance.semantic_relayout_skips,
           navigation.performance.focus_outline_relayout_skips,
           navigation.performance.focus_paint_relayout_skips);
    printf("layout-reuse retained=%zu style-hits=%zu style-misses=%zu "
           "intrinsic-hits=%zu intrinsic-misses=%zu "
           "table-row-hits=%zu table-row-misses=%zu scoped-invalidations=%zu "
           "full-resets=%zu pressure-evictions=%zu\n",
           navigation.performance.layout_reuse_retained_bytes,
           navigation.performance.layout_reuse_style_hits,
           navigation.performance.layout_reuse_style_misses,
           navigation.performance.layout_reuse_intrinsic_hits,
           navigation.performance.layout_reuse_intrinsic_misses,
           navigation.performance.layout_reuse_table_row_hits,
           navigation.performance.layout_reuse_table_row_misses,
           navigation.performance.layout_reuse_scoped_invalidations,
           navigation.performance.layout_reuse_full_resets,
           navigation.performance.layout_reuse_pressure_evictions);
    printf("progressive-paint attempts=%zu skips=%zu failures=%zu "
           "adoptions=%zu "
           "layouts=%zu paints=%zu layout-us=%llu paint-us=%llu "
           "limit-y=%d commands=%zu boxes=%zu "
           "priority-nodes=%zu markup-priority=%zu priority-loaded=%zu "
           "image-fallbacks=%zu retained-prefixes=%zu "
           "visible-decodes=%zu visible-scales=%zu "
           "background-images=%zu/%zu/%zu/%zu "
           "background-fonts=%zu/%zu/%zu/%zu pending=%s\n",
           navigation.performance.progressive_layout_attempts,
           navigation.performance.progressive_layout_skips,
           navigation.performance.progressive_layout_failures,
           navigation.performance.progressive_layout_adoptions,
           navigation.performance.partial_layouts,
           navigation.performance.partial_paints,
           (unsigned long long)
             navigation.performance.progressive_layout_us,
           (unsigned long long)
             navigation.performance.progressive_paint_us,
           navigation.performance.progressive_preview_y_limit,
           navigation.performance.progressive_preview_commands,
           navigation.performance.progressive_preview_node_boxes,
           navigation.performance.progressive_image_priority_nodes,
           navigation.performance.markup_image_priority_nodes,
           navigation.performance.progressive_image_priority_loaded,
           navigation.performance.image_pressure_fallbacks,
           navigation.page.images.stats.priority_retained_on_failure,
           navigation.performance.progressive_decoded_image_builds,
           navigation.performance.progressive_scaled_image_builds,
           navigation.performance.background_image_batches,
           navigation.performance.background_images_loaded,
           navigation.performance.background_image_relayouts,
           navigation.performance.background_image_failures,
           navigation.performance.background_font_slices,
           navigation.performance.background_fonts_loaded,
           navigation.performance.background_font_relayouts,
           navigation.performance.background_font_failures,
           navigation_background_resources_pending(&navigation)
               ? "yes" : "no");
    printf("parser-staging slides-avoided=%zu bytes-avoided=%zu "
           "compactions=%zu compacted-bytes=%zu\n",
           navigation.performance.staged_body_slides_avoided,
           navigation.performance.staged_body_slide_bytes_avoided,
           navigation.performance.staged_body_compactions,
           navigation.performance.staged_body_compaction_bytes);
    printf("responsiveness max-slice-us=%llu phase=%s work=%zu\n",
           (unsigned long long) navigation.performance.max_slice_us,
           navigation_slice_phase_name(
               navigation.performance.max_slice_phase),
           navigation.performance.max_slice_work_units);
    for (size_t i = 0; i < NAVIGATION_SLICE_COUNT; i++) {
        const NavigationSliceStats *slice =
            &navigation.performance.slices[i];
        printf("responsiveness-phase name=%s slices=%zu total-us=%llu "
               "max-us=%llu work=%zu max-work=%zu yields=%zu "
               "restarts=%zu discarded=%zu\n",
               navigation_slice_phase_name((NavigationSlicePhase) i),
               slice->slices, (unsigned long long) slice->total_us,
               (unsigned long long) slice->max_us, slice->work_units,
               slice->max_work_units, slice->cooperative_yields,
               slice->restarts, slice->discarded_stale);
    }
    printf("stream bytes=%zu chunks=%zu peak-buffer=%zu parser-pauses=%zu "
           "stalls=%zu headers=%d cancelled=%d truncated=%d "
           "headers-us=%llu first-body-us=%llu first-dom-us=%llu "
           "first-layout-us=%llu first-paint-us=%llu "
           "completion-us=%llu partial-layouts=%zu partial-paints=%zu\n",
           navigation.last_stream.bytes_received,
           navigation.last_stream.chunks_received,
           navigation.last_stream.peak_buffered_bytes,
           navigation.last_stream.parser_pauses,
           navigation.last_stream.replay_stalls,
           navigation.last_stream.headers_delivered ? 1 : 0,
           navigation.last_stream.cancelled ? 1 : 0,
           navigation.last_stream.truncated ? 1 : 0,
           (unsigned long long)
             navigation.performance.response_headers_us,
           (unsigned long long)
             navigation.performance.first_body_byte_us,
           (unsigned long long) navigation.performance.first_dom_us,
           (unsigned long long) navigation.performance.first_layout_us,
           (unsigned long long) navigation.performance.first_paint_us,
           (unsigned long long) navigation.performance.completion_us,
           navigation.performance.partial_layouts,
           navigation.performance.partial_paints);
    FetchSchedulerDomainMetrics fetch_domain_metrics = {0};
    bool fetch_domain_ready = fetch_scheduler_domain_metrics(
        navigation.page.fetch_domain, &fetch_domain_metrics);
    printf("page-fetch-domain present=%d slots=%zu/%zu peak-slots=%zu "
           "rejected=%zu views=%zu peak-views=%zu transient-with-navigation=%zu\n",
           fetch_domain_ready ? 1 : 0,
           fetch_domain_metrics.active_slots,
           fetch_domain_metrics.maximum_active_slots,
           fetch_domain_metrics.peak_active_slots,
           fetch_domain_metrics.rejected_enqueues,
           fetch_domain_metrics.active_views,
           fetch_domain_metrics.peak_active_views,
           fetch_domain_ready
             ? fetch_domain_metrics.maximum_active_slots + 1u : 0u);
    printf("preloads discovered=%zu launched=%zu completed=%zu cache-hits=%zu "
           "failed=%zu deferred=%zu headroom-skips=%zu\n",
           navigation.preloads_discovered, navigation.preloads_launched,
           navigation.preloads_completed, navigation.preloads_cache_hits,
           navigation.preloads_failed, navigation.preloads_deferred,
           navigation.preloads_headroom_skipped);
    printf("javascript summary=\"%s\"\n",
           navigation.page.script_result.summary);
    printf("javascript-external-bytecode hits=%zu misses=%zu stores=%zu "
           "admission-skips=%zu restore-failures=%zu restored-bytes=%zu\n",
           navigation.page.script_result.external_script_bytecode_cache_hits,
           navigation.page.script_result.external_script_bytecode_cache_misses,
           navigation.page.script_result.external_script_bytecode_cache_stores,
           navigation.page.script_result
             .external_script_bytecode_cache_admission_skips,
           navigation.page.script_result
             .external_script_bytecode_cache_restore_failures,
           navigation.page.script_result.external_script_bytecode_cache_bytes);
    printf("javascript-dom-handles live=%zu peak=%zu high-water=%zu "
           "reuses=%zu exhaustions=%zu wrapper-releases=%zu "
           "connected-preserves=%zu stale-releases=%zu capacity=%u\n",
           navigation.page.script_result.dom_handle_slots_live,
           navigation.page.script_result.dom_handle_slots_peak,
           navigation.page.script_result.dom_handle_slots_high_water,
           navigation.page.script_result.dom_handle_slot_reuses,
           navigation.page.script_result.dom_handle_exhaustions,
           navigation.page.script_result.dom_handle_wrapper_releases,
           navigation.page.script_result.dom_handle_connected_preserves,
           navigation.page.script_result.dom_handle_stale_releases,
           SCRIPT_DOM_HANDLE_SLOT_CAPACITY);
    printf("javascript-geometry queries=%zu retained-fast-paths=%zu "
           "ancestor-visits=%zu synchronous-layouts=%zu\n",
           navigation.page.script_result.geometry_queries,
           navigation.page.script_result.geometry_retained_fast_paths,
           navigation.page.script_result.geometry_ancestor_visits,
           navigation.page.script_result.geometry_synchronous_layouts);
    printf("javascript-error=\"%s\" source=\"%s\"\n",
           navigation.page.script_result.error,
           navigation.page.script_result.error_source_context);
    printf("javascript-rejections count=%zu created=%zu handled=%zu "
           "undefined=%zu last=\"%s\"\n",
           navigation.page.script_result.promise_rejections,
           navigation.page.script_result.promise_rejections_created,
           navigation.page.script_result.promise_rejections_handled,
           navigation.page.script_result.promise_rejections_undefined,
           navigation.page.script_result.last_promise_rejection);
    if (trace_page) {
        printf("page-capability-trace=\"%s\"\n",
               navigation.last_page_trace);
        printf("previous-page-capability-trace=\"%s\"\n",
               navigation.previous_page_trace);
    }
    printf("javascript-network requests=%zu failures=%zu status=%ld "
           "last-url=\"%s\" form-submit=%s\n",
           navigation.page.script_result.network_requests,
           navigation.page.script_result.network_failures,
           navigation.page.script_result.last_network_status,
           navigation.page.script_result.last_network_url,
           navigation.page.script_result.form_submission_requested
             ? "yes" : "no");
    printf("javascript-network-async queued=%zu completed=%zu rejected=%zu "
           "cancelled=%zu timed-out=%zu peak-inflight=%zu\n",
           navigation.page.script_result.async_network_queued,
           navigation.page.script_result.async_network_completed,
           navigation.page.script_result.async_network_quota_rejected,
           navigation.page.script_result.async_network_cancelled,
           navigation.page.script_result.async_network_timed_out,
           navigation.page.script_result.async_network_peak_inflight);
    printf("javascript-dynamic-scripts queued=%zu started=%zu completed=%zu "
           "failed=%zu cancelled=%zu cache-hits=%zu quota=%zu "
           "ordered-waits=%zu peak-pending=%zu nomodule=%zu bytes=%zu\n",
           navigation.page.script_result.dynamic_scripts_queued,
           navigation.page.script_result.dynamic_scripts_started,
           navigation.page.script_result.dynamic_scripts_completed,
           navigation.page.script_result.dynamic_scripts_failed,
           navigation.page.script_result.dynamic_scripts_cancelled,
           navigation.page.script_result.dynamic_scripts_cache_hits,
           navigation.page.script_result.dynamic_scripts_quota_rejected,
           navigation.page.script_result.dynamic_scripts_ordered_waits,
           navigation.page.script_result.dynamic_scripts_peak_pending,
           navigation.page.script_result.dynamic_scripts_nomodule_skipped,
           navigation.page.script_result.dynamic_script_bytes);
    printf("javascript-network-logical admitted=%zu completed=%zu "
           "rejected=%zu cancelled=%zu timed-out=%zu peak=%zu "
           "peak-bytes=%zu active-native=%zu pending-logical=%zu\n",
           navigation.page.script_result.async_network_logical_admitted,
           navigation.page.script_result.async_network_logical_completed,
           navigation.page.script_result.async_network_logical_rejected,
           navigation.page.script_result.async_network_logical_cancelled,
           navigation.page.script_result.async_network_logical_timed_out,
           navigation.page.script_result.async_network_logical_peak,
           navigation.page.script_result.async_network_logical_peak_bytes,
           navigation.page.script_result.async_network_active_native,
           navigation.page.script_result.async_network_pending_logical);
    printf("javascript-indexeddb opens=%zu deletes=%zu transactions=%zu "
           "requests=%zu records=%zu bytes=%zu peak-bytes=%zu "
           "quota-errors=%zu\n",
           navigation.page.script_result.indexed_db_opens,
           navigation.page.script_result.indexed_db_deletes,
           navigation.page.script_result.indexed_db_transactions,
           navigation.page.script_result.indexed_db_requests,
           navigation.page.script_result.indexed_db_records,
           navigation.page.script_result.indexed_db_bytes,
           navigation.page.script_result.indexed_db_peak_bytes,
           navigation.page.script_result.indexed_db_quota_errors);
    printf("javascript-clipboard writes=%zu text=\"%s\"\n",
           navigation.page.script_result.clipboard_writes,
           navigation.page.script_result.last_clipboard_text);
    printf("javascript-network-response bytes=%zu hash=%016llx "
           "content-type=\"%s\" content-encoding=\"%s\" server=\"%s\" "
           "cf-ray=\"%s\" cf-mitigated=\"%s\" prefix-hex=\"%s\"\n",
           navigation.page.script_result.last_network_response_bytes,
           navigation.page.script_result.last_network_body_hash,
           navigation.page.script_result.last_network_content_type,
           navigation.page.script_result.last_network_content_encoding,
           navigation.page.script_result.last_network_server,
           navigation.page.script_result.last_network_cf_ray,
           navigation.page.script_result.last_network_cf_mitigated,
           navigation.page.script_result.last_network_body_prefix);
    printf("javascript-xhr sends=%zu last-error=\"%s\"\n",
           navigation.page.script_result.xhr_send_calls,
           navigation.page.script_result.last_xhr_error);
    printf("javascript-xhr-response responses=%zu status=%ld "
           "type=\"%s\" text-units=%zu bytes=%zu states=\"%s\"\n",
           navigation.page.script_result.xhr_response_count,
           navigation.page.script_result.last_xhr_status,
           navigation.page.script_result.last_xhr_response_type,
           navigation.page.script_result.last_xhr_response_text_units,
           navigation.page.script_result.last_xhr_response_bytes,
           navigation.page.script_result.last_xhr_ready_states);
    printf("javascript-promises unhandled=%zu created=%zu handled=%zu "
           "undefined=%zu last=\"%s\"\n",
           navigation.page.script_result.promise_rejections,
           navigation.page.script_result.promise_rejections_created,
           navigation.page.script_result.promise_rejections_handled,
           navigation.page.script_result.promise_rejections_undefined,
           navigation.page.script_result.last_promise_rejection);
    printf("javascript-callback-errors uncaught=%zu last=\"%s\"\n",
           navigation.page.script_result.uncaught_callback_errors,
           navigation.page.script_result.last_uncaught_callback_error);
    printf("javascript-watchdog polls=%zu elapsed-ms=%lu interrupted=%s\n",
           navigation.page.script_result.watchdog_polls,
           navigation.page.script_result.watchdog_elapsed_ms,
           navigation.page.script_result.interrupted ? "yes" : "no");
    printf("javascript-responsiveness work=%zu yields=%zu max-slice-us=%llu "
           "max-slice-work=%zu nonpreemptible-compiles=%zu "
           "max-compile-us=%llu max-compile-bytes=%zu "
           "compile-attempts=%zu compile-source-bytes=%zu "
           "compile-rejections=%zu rejected-bytes=%zu compile-limit=%zu "
           "last-admission=%u last-kind=%u last-bytes=%zu max-kind=%u "
           "nonpreemptible-callbacks=%zu max-callback-us=%llu "
           "host-callbacks=%zu callback-polls=%zu callback-total-us=%llu "
           "max-callback-name=\"%.95s\"\n",
           navigation.page.script_result.script_work_units,
           navigation.page.script_result.script_cooperative_yields,
           (unsigned long long)
             navigation.page.script_result.script_max_slice_us,
           navigation.page.script_result.script_max_slice_work_units,
           navigation.page.script_result.nonpreemptible_compile_count,
           (unsigned long long)
             navigation.page.script_result.max_nonpreemptible_compile_us,
           navigation.page.script_result.max_nonpreemptible_compile_bytes,
           navigation.page.script_result.host_compile_attempts,
           navigation.page.script_result.host_compile_source_bytes,
           navigation.page.script_result.host_compile_rejections,
           navigation.page.script_result.host_compile_rejected_source_bytes,
           navigation.page.script_result.host_compile_source_limit_bytes,
           (unsigned) navigation.page.script_result.last_compile_admission,
           (unsigned) navigation.page.script_result.last_compile_source_kind,
           navigation.page.script_result.last_compile_source_bytes,
           (unsigned) navigation.page.script_result
             .max_nonpreemptible_compile_source_kind,
           navigation.page.script_result.nonpreemptible_callback_count,
           (unsigned long long)
             navigation.page.script_result.max_nonpreemptible_callback_us,
           navigation.page.script_result.host_callback_calls,
           navigation.page.script_result
             .host_callback_calls_with_interrupt_polls,
           (unsigned long long)
             navigation.page.script_result.host_callback_total_us,
           navigation.page.script_result.max_nonpreemptible_callback_name);
    printf("javascript-bootstrap-bytecode restores=%zu failures=%zu "
           "source-preferences=%zu preferred-source-bytes=%zu "
           "stored-bytes=%zu bytecode-bytes=%zu restore-us=%llu\n",
           navigation.page.script_result.bootstrap_bytecode_restores,
           navigation.page.script_result.bootstrap_bytecode_restore_failures,
           navigation.page.script_result
             .bootstrap_bytecode_source_preferences,
           navigation.page.script_result
             .bootstrap_bytecode_preferred_source_bytes,
           navigation.page.script_result.bootstrap_bytecode_stored_bytes,
           navigation.page.script_result.bootstrap_bytecode_bytes,
           (unsigned long long)
             navigation.page.script_result.bootstrap_bytecode_restore_us);
    printf("javascript-lazy-webpack candidates=%zu applied=%zu fallbacks=%zu "
           "factories-deferred=%zu factory-source-bytes=%zu "
           "compressed-source-bytes=%zu bytecode-bytes=%zu "
           "compressed-bytecode-bytes=%zu source-compiles=%zu "
           "bytecode-restores=%zu bytecode-demotions=%zu "
           "bytecode-released=%zu factories-compiled=%zu "
           "compiled-factory-evictions=%zu compile-failures=%zu\n",
           navigation.page.script_result.lazy_webpack_candidates,
           navigation.page.script_result.lazy_webpack_applied,
           navigation.page.script_result.lazy_webpack_fallbacks,
           navigation.page.script_result.lazy_webpack_factories_deferred,
           navigation.page.script_result.lazy_webpack_factory_source_bytes,
           navigation.page.script_result.lazy_webpack_compressed_source_bytes,
           navigation.page.script_result.lazy_webpack_bytecode_bytes,
           navigation.page.script_result
             .lazy_webpack_compressed_bytecode_bytes,
           navigation.page.script_result.lazy_webpack_source_compiles,
           navigation.page.script_result.lazy_webpack_bytecode_restores,
           navigation.page.script_result.lazy_webpack_bytecode_demotions,
           navigation.page.script_result
             .lazy_webpack_bytecode_bytes_released,
           navigation.page.script_result.lazy_webpack_factories_compiled,
           navigation.page.script_result
             .lazy_webpack_compiled_factory_evictions,
           navigation.page.script_result
             .lazy_webpack_factory_compile_failures);
    const char *body_text = document_body_text(&navigation.page.document);
    if (body_text == NULL) body_text = "";
    size_t body_text_length = navigation.page.document.body_text == NULL
                              ? 0
                              : navigation.page.document.body_text_length;
    size_t body_text_shown = body_text_length < 512 ? body_text_length : 512;
    printf("javascript-state mutations=%zu events=%zu body-text-bytes=%zu "
           "body-text=\"%.*s\"\n",
           navigation.page.script_result.dom_mutations,
           navigation.page.script_result.events_dispatched,
           body_text_length, (int) body_text_shown, body_text);
    if (visual_state_marker != NULL) {
        printf("visual-state-marker found=%s marker-bytes=%zu\n",
               strstr(body_text, visual_state_marker) != NULL ? "yes" : "no",
               strlen(visual_state_marker));
    }
    printf("javascript-section-retention wrapper-evictions=%zu "
           "listener-drops=%zu handler-drops=%zu observer-drops=%zu "
           "record-drops=%zu dirty-drops=%zu state-evictions=%zu "
           "control-drops=%zu\n",
           navigation.page.script_result.retention_wrapper_evictions,
           navigation.page.script_result.retention_listener_drops,
           navigation.page.script_result.retention_handler_drops,
           navigation.page.script_result.retention_observer_drops,
           navigation.page.script_result.retention_record_drops,
           navigation.page.script_result.retention_dirty_drops,
           navigation.page.script_result.retention_state_evictions,
           navigation.page.script_result.retention_control_drops);
    const char *challenge_outcome = navigation.last_cf_mitigated[0] == '\0'
        ? "not-applicable"
        : (strstr(body_text, "Browser not supported") != NULL
           ? "browser-unsupported"
           : (strstr(body_text, "Verification successful") != NULL
              ? "verification-waiting" : "challenge-active"));
    bool clearance_found = false;
    for (size_t i = 0; i < BROWSER_COOKIE_ENTRIES; i++) {
        if (session.cookies[i].value != NULL
            && strcmp(session.cookies[i].name, "cf_clearance") == 0) {
            clearance_found = true;
            break;
        }
    }
    printf("challenge outcome=%s clearance=%s\n", challenge_outcome,
           clearance_found ? "present" : "absent");
    printf("navigation loads=%zu destroys=%zu history=%zu pruned=%zu reloads=%zu\n",
           navigation.loads_committed, navigation.page_destroys,
           navigation.history_count, navigation.history_pruned, reloads);
    printf("navigation-replacement mode=%s started=%zu succeeded=%zu "
           "failed=%zu fallbacks=%zu rollback=%s released=%zu peak-released=%zu "
           "frozen-frame=%s overlay=%s message=\"%s\"\n",
           navigation.replacement_mode == NAVIGATION_REPLACEMENT_LOW_MEMORY
             ? "low-memory" : "transactional",
           navigation.performance.low_memory_replacements_started,
           navigation.performance.low_memory_replacements_succeeded,
           navigation.performance.low_memory_replacements_failed,
           navigation.performance.low_memory_transactional_fallbacks,
           navigation.performance.low_memory_last_rollback_available
             ? "available" : "unavailable",
           navigation.performance.low_memory_last_released_bytes,
           navigation.performance.low_memory_peak_released_bytes,
           navigation.performance.low_memory_last_frozen_frame
             ? "yes" : "no",
           navigation.replacement_failure_overlay_active ? "active" : "none",
           navigation.replacement_failure_message);
    printf("network status=%ld server=\"%s\" mitigated=\"%s\" "
           "client-hint-retries=%zu\n",
           navigation.last_http_status, navigation.last_server,
           navigation.last_cf_mitigated, navigation.client_hint_retries);
    {
        /* Handshake attribution, process-wide for this lab run. Fields a
           transport cannot measure print as "absent"; see
           docs/engineering/PSP_TRANSPORT.md. */
        FetchTlsHandshakeCounters handshakes;
        char handshake_counters[128];
        fetch_tls_handshake_counters(&handshakes);
        if (fetch_tls_handshake_counters_format(
                &handshakes, handshake_counters,
                sizeof(handshake_counters))) {
            printf("tls-handshake %s\n", handshake_counters);
        }
    }
    printf("scripts discovered=%zu attempted=%zu loaded=%zu failed=%zu "
           "cross-origin=%zu skipped-modules=%zu skipped-nomodule=%zu "
           "quota=%zu pressure=%zu "
           "pressure-gc=%zu pressure-reclaimed=%zu pressure-caps=%zu "
           "bytes=%zu cache-hits=%zu\n",
           script_metrics.discovered, script_metrics.attempted,
           script_metrics.loaded, script_metrics.failed,
           script_metrics.skipped_cross_origin,
           script_metrics.skipped_module, script_metrics.skipped_nomodule,
           script_metrics.skipped_quota,
           script_metrics.skipped_pressure,
           script_metrics.pressure_collections,
           script_metrics.pressure_reclaimed_bytes,
           script_metrics.pressure_capped_requests,
           script_metrics.bytes, script_metrics.cache_hits);
    printf("script-order blocking=%zu defer=%zu async=%zu modules=%zu "
           "module-map-hits=%zu inline-data=%zu/%zu exemptions=%zu "
           "cost-rejected=%zu watchdog-misses=%zu/%zu loops=%zu "
           "flags=0x%x\n",
           script_metrics.parser_blocking, script_metrics.deferred,
           script_metrics.asynchronous, script_metrics.modules,
           script_metrics.module_map_hits,
           script_metrics.inline_data_fast_paths,
           script_metrics.inline_data_fast_path_bytes,
           script_metrics.inline_data_quota_exemptions,
           script_metrics.cost_class_rejections,
           script_metrics.watchdog_classification_misses,
           script_metrics.watchdog_classification_miss_bytes,
           script_metrics.watchdog_classification_miss_loops,
           script_metrics.watchdog_classification_miss_flags);
    printf("frames discovered=%zu loaded=%zu failed=%zu detached=%zu "
           "messages-posted=%zu messages-delivered=%zu dropped=%zu "
           "to-parent=%zu to-child=%zu last-event=\"%s\"\n",
           navigation.frames_discovered, navigation.frames_loaded,
           navigation.frames_failed, navigation.frames_detached,
           navigation.frame_messages_posted,
           navigation.frame_messages_delivered,
           navigation.frame_messages_dropped,
           navigation.frame_messages_to_parent,
           navigation.frame_messages_to_child,
           navigation.last_frame_message_event);
    printf("frame-message-events=\"%s\"\n",
           navigation.frame_message_event_log);
    printf("frame-message-last sequence=%llu source=%ld target=%ld\n",
           (unsigned long long) navigation.last_frame_message_sequence,
           navigation.last_frame_message_source,
           navigation.last_frame_message_target);
    printf("navigation-degradation optional-work-sheds=%zu "
           "zero-body-retries=%zu frame-message-soft-failures=%zu\n",
           navigation.performance.optional_work_sheds,
           navigation.performance.zero_body_navigation_retries,
           navigation.performance.frame_message_soft_failures);
    printf("frame-last-reject=\"%s\"\n",
           navigation.last_frame_reject_reason);
    if (trace_frames && navigation.last_frame_trace[0] != '\0') {
        printf("last-frame-capability url=\"%s\" trace=\"%s\"\n",
               navigation.last_frame_trace_url,
               navigation.last_frame_trace);
    }
    for (size_t i = 0; i < navigation.page.frame_count; i++) {
        const NavigationFrame *child = &navigation.page.frames[i];
        printf("frame[%zu] status=%s summary=\"%s\" error=\"%s\"\n", i,
               child->retired ? "detached"
                   : (child->loaded ? "loaded" : "failed"),
               child->script_result.summary,
               child->script_result.error);
        printf("frame[%zu] http=%ld server=\"%s\" cf-ray=\"%s\"\n", i,
               child->http_status, child->server, child->cf_ray);
        printf("frame[%zu] callback-errors=%zu last=\"%s\" "
               "watchdog-polls=%zu interrupted=%s\n", i,
               child->script_result.uncaught_callback_errors,
               child->script_result.last_uncaught_callback_error,
               child->script_result.watchdog_polls,
               child->script_result.interrupted ? "yes" : "no");
        if (child->script_result.last_uncaught_callback_task[0] != '\0') {
            printf("frame[%zu] callback-task=\"%s\"\n", i,
                   child->script_result.last_uncaught_callback_task);
        }
        printf("frame[%zu] clocks performance-now=%zu/%.3f "
               "date-now=%zu/%.0f\n", i,
               child->script_result.performance_now_calls,
               child->script_result.performance_now_last_ms,
               child->script_result.date_now_calls,
               child->script_result.date_now_last_ms);
        printf("frame[%zu] network requests=%zu failures=%zu status=%ld "
               "async=%zu/%zu/%zu url=\"%s\"\n", i,
               child->script_result.network_requests,
               child->script_result.network_failures,
               child->script_result.last_network_status,
               child->script_result.async_network_queued,
               child->script_result.async_network_completed,
               child->script_result.async_network_timed_out,
               child->script_result.last_network_url);
        printf("frame[%zu] network-response bytes=%zu hash=%016llx "
               "content-type=\"%s\" server=\"%s\" cf-ray=\"%s\"\n", i,
               child->script_result.last_network_response_bytes,
               child->script_result.last_network_body_hash,
               child->script_result.last_network_content_type,
               child->script_result.last_network_server,
               child->script_result.last_network_cf_ray);
        printf("frame[%zu] xhr sends=%zu responses=%zu status=%ld "
               "type=\"%s\" text-units=%zu bytes=%zu states=\"%s\"\n", i,
               child->script_result.xhr_send_calls,
               child->script_result.xhr_response_count,
               child->script_result.last_xhr_status,
               child->script_result.last_xhr_response_type,
               child->script_result.last_xhr_response_text_units,
               child->script_result.last_xhr_response_bytes,
               child->script_result.last_xhr_ready_states);
        if (trace_frames) {
            printf("frame[%zu] url=\"%s\"\n", i, child->url);
            printf("frame[%zu] capability-trace=\"%s\"\n", i,
                   child->capability_trace);
        }
        if (child->script_result.last_reject_stack[0] != '\0') {
            printf("frame[%zu] reject-stack=\"%s\"\n", i,
                   child->script_result.last_reject_stack);
        }
        if (trace_frames && child->reject_source_context[0] != '\0') {
            printf("frame[%zu] reject-source-context=\"%s\"\n", i,
                   child->reject_source_context);
        }
        if (child->callback_error_source_context[0] != '\0') {
            printf("frame[%zu] callback-source-context=\"%s\"\n", i,
                   child->callback_error_source_context);
        }
    }
    printf("session storage=%zu cookies=%zu cache=%zu cache-hits=%zu "
           "cache-misses=%zu evictions=%zu\n",
           session.storage_bytes, session.cookie_bytes, session.cache_bytes,
           session.cache_hits, session.cache_misses,
           session.cache_evictions);
    printf("session cookie-names=");
    bool printed_cookie = false;
    for (size_t i = 0; i < BROWSER_COOKIE_ENTRIES; i++) {
        if (session.cookies[i].value == NULL) continue;
        printf("%s%s%s", printed_cookie ? "," : "",
               session.cookies[i].name,
               session.cookies[i].http_only ? "(HttpOnly)" : "");
        printed_cookie = true;
    }
    printf("%s\n", printed_cookie ? "" : "none");
    printf("controller moves=%zu activations=%zu edits=%zu action=%s url=\"%s\"\n",
           controller.focus_moves, controller.activations,
           controller.text_edits,
           action.type == CONTROLLER_ACTION_NAVIGATE ? "navigate"
           : (action.type == CONTROLLER_ACTION_CONTROL ? "control"
              : (action.type == CONTROLLER_ACTION_FORM_SUBMIT
                 ? "form" : action.type == CONTROLLER_ACTION_MEDIA
                 ? "media" : "none")),
           action.url);
    printf("structured-audio inspected=%zu/%zu candidates=%zu overflow=%zu "
           "malformed=%zu truncated=%zu matched=%zu ambiguous=%zu\n",
           controller.structured_audio.inspected_bytes,
           controller.structured_audio.inspected_nodes,
           controller.structured_audio.candidate_count,
           controller.structured_audio.candidate_overflow,
           controller.structured_audio.malformed_scripts,
           controller.structured_audio.truncated_scripts,
           controller.structured_audio_matched_activations,
           controller.structured_audio_ambiguous_rejections);
    if (experimental_compressed_sections) {
        printf("experimental-initial-load transfer-us=%llu "
               "first-section-index-us=%llu provisional-commit-us=%llu "
               "first-paint-ready-us=%llu index-complete-us=%llu "
               "final-ready-us=%llu callbacks=%zu provisional=%s "
               "presented=%s\n",
               (unsigned long long) experimental.transfer_complete_us,
               (unsigned long long) section_store.first_section_ready_us,
               (unsigned long long) experimental.provisional_commit_us,
               (unsigned long long) experimental.first_paint_ready_us,
               (unsigned long long) experimental.index_complete_us,
               (unsigned long long) experimental.final_ready_us,
               experimental.index_progress_callbacks,
               experimental.provisional_painted ? "painted" : "skipped",
               experimental.provisional_presented ? "yes" : "no");
        printf("experimental-pager status=%s section=%zu/%zu swaps=%zu "
               "loads=%zu prefetches=%zu hits=%zu evictions=%zu\n",
               experimental.active ? "active" : "left-document",
               experimental.pager.current_section,
               section_store.section_count, experimental.swaps,
               experimental.pager.section_loads,
               experimental.pager.prefetches,
               experimental.pager.prefetch_hits,
               experimental.pager.prefetch_evictions);
        printf("experimental-timing swaps=%zu ready-avg-us=%llu "
               "ready-max-us=%llu first-tile-samples=%zu "
               "first-tile-avg-us=%llu first-tile-max-us=%llu "
               "decode-samples=%zu decode-avg-us=%llu decode-max-us=%llu "
               "prefetch-samples=%zu prefetch-avg-us=%llu "
               "prefetch-max-us=%llu\n",
               experimental.timing_swaps,
               (unsigned long long) (experimental.timing_swaps == 0 ? 0
                   : experimental.ready_total_us
                       / experimental.timing_swaps),
               (unsigned long long) experimental.ready_max_us,
               experimental.first_tile_samples,
               (unsigned long long) (experimental.first_tile_samples == 0
                   ? 0 : experimental.first_tile_total_us
                           / experimental.first_tile_samples),
               (unsigned long long) experimental.first_tile_max_us,
               experimental.pager.decode_samples,
               (unsigned long long) (experimental.pager.decode_samples == 0
                   ? 0 : experimental.pager.decode_total_us
                           / experimental.pager.decode_samples),
               (unsigned long long) experimental.pager.decode_max_us,
               experimental.pager.prefetch_samples,
               (unsigned long long) (experimental.pager.prefetch_samples == 0
                   ? 0 : experimental.pager.prefetch_total_us
                           / experimental.pager.prefetch_samples),
               (unsigned long long) experimental.pager.prefetch_max_us);
        size_t phase_samples = experimental.timing_swaps;
#define EXPERIMENTAL_PHASE_AVG(field)                                    \
    (unsigned long long) (phase_samples == 0 ? 0                         \
        : experimental.field.total_us / phase_samples)
        printf("experimental-phase-timing samples=%zu "
               "state-avg-us=%llu state-max-us=%llu "
               "pager-avg-us=%llu pager-max-us=%llu "
               "commit-avg-us=%llu commit-max-us=%llu "
               "content-avg-us=%llu content-max-us=%llu "
               "restore-avg-us=%llu restore-max-us=%llu "
               "scroll-avg-us=%llu scroll-max-us=%llu gc-runs=%zu "
               "gc-skips=%zu gc-trimmed=%zu\n",
               phase_samples,
               EXPERIMENTAL_PHASE_AVG(state_save_timing),
               (unsigned long long) experimental.state_save_timing.max_us,
               EXPERIMENTAL_PHASE_AVG(pager_timing),
               (unsigned long long) experimental.pager_timing.max_us,
               EXPERIMENTAL_PHASE_AVG(commit_timing),
               (unsigned long long) experimental.commit_timing.max_us,
               EXPERIMENTAL_PHASE_AVG(content_timing),
               (unsigned long long) experimental.content_timing.max_us,
               EXPERIMENTAL_PHASE_AVG(restore_timing),
               (unsigned long long) experimental.restore_timing.max_us,
               EXPERIMENTAL_PHASE_AVG(scroll_timing),
               (unsigned long long) experimental.scroll_timing.max_us,
               navigation.performance.swap_gc_runs,
               navigation.performance.swap_gc_skips,
               navigation.performance.swap_gc_trimmed_bytes);
#undef EXPERIMENTAL_PHASE_AVG
        printf("experimental-semantic-queries queries=%zu sections=%zu "
               "skipped=%zu matches=%zu failures=%zu total-us=%llu "
               "max-us=%llu\n",
               experimental.semantic_queries,
               experimental.semantic_sections,
               experimental.semantic_sections_skipped,
               experimental.semantic_matches,
               experimental.semantic_failures,
               (unsigned long long) experimental.semantic_total_us,
               (unsigned long long) experimental.semantic_max_us);
        printf("experimental-remote-reads reads=%zu bytes=%zu failures=%zu\n",
               experimental.remote_reads,
               experimental.remote_read_bytes,
               experimental.remote_read_failures);
        printf("experimental-remote-geometry reads=%zu layouts=%zu "
               "cache-hits=%zu failures=%zu\n",
               experimental.remote_geometry_reads,
               experimental.remote_geometry_layouts,
               experimental.remote_geometry_cache_hits,
               experimental.remote_geometry_failures);
        printf("experimental-remote-writes writes=%zu bytes=%zu failures=%zu "
               "retained=%zu evictions=%zu\n",
               experimental.remote_writes,
               experimental.remote_write_bytes,
               experimental.remote_write_failures,
               experimental.remote_mutation_count,
               experimental.remote_mutation_evictions);
    }
    if (platform_sim) {
        bool platform_ok = platform_simulator.asset_reads != 0
            && (platform_simulator.input_polls != 0
                || first_present_output != NULL)
            && platform_simulator.frames_presented != 0
            && (first_present_output == NULL
                || platform_simulator.first_frame_written);
        printf("platform-sim assets=%zu input-polls=%zu frames=%zu "
               "last-hash=%016llx status=%s\n",
               platform_simulator.asset_reads,
               platform_simulator.input_polls,
               platform_simulator.frames_presented,
               (unsigned long long) platform_simulator.last_frame_hash,
               platform_ok ? "PASS" : "FAIL");
        if (!platform_ok) goto cleanup;
    }
    if (visual_evidence
        && (!replay_diagnostics_available
            || !print_visual_evidence_record(
                &navigation, visual_state_marker,
                strstr(body_text, visual_state_marker) != NULL,
                body_text, body_text_shown, &script_metrics,
                &replay_diagnostics, &visual_frames))) {
        fprintf(stderr, "visual-evidence-error=record-unavailable\n");
        goto cleanup;
    }
    budget_report_pressure(budget, stdout);
    if (media.player != NULL) interactive_media_print_status(&media);
    budget_report_categories(budget, "interactive-stable", stdout);
    success = true;

cleanup:
    section_route_stream_abort(&experimental_router);
    if (command_stream != NULL && command_stream != stdin) {
        fclose(command_stream);
    }
#ifdef TILEFINCH_HAVE_HOST_MEDIA
    interactive_media_destroy(&media);
#else
    interactive_media_destroy(&media);
#endif
    if (!success && navigation_ready) {
        if (trace_frames) {
            (void) navigation_collect_frame_capability_trace(&navigation);
        }
        const char *engine_failure = engine == NULL
            ? NULL : browser_engine_last_error(engine);
        fprintf(stderr, "interactive failure: %s\n",
                navigation.last_error[0] == '\0'
                  ? (engine_failure == NULL || engine_failure[0] == '\0'
                       ? "operation failed without a navigation diagnostic"
                       : engine_failure)
                  : navigation.last_error);
        if (navigation.page.script_result.error[0] != '\0'
            && strcmp(navigation.page.script_result.error,
                      navigation.last_error) != 0) {
            fprintf(stderr, "page javascript-error: %s\n",
                    navigation.page.script_result.error);
        }
        if (navigation.page.script_result.error_source_context[0] != '\0') {
            fprintf(stderr, "page failure-source=\"%s\"\n",
                    navigation.page.script_result.error_source_context);
        }
        if (navigation.page.script_result.summary[0] != '\0') {
            fprintf(stderr, "page failure-summary=\"%s\"\n",
                    navigation.page.script_result.summary);
        }
        if (navigation.page.script_result.frame_eval_telemetry[0] != '\0') {
            fprintf(stderr, "page frame-eval=\"%s\"\n",
                    navigation.page.script_result.frame_eval_telemetry);
        }
        fprintf(stderr, "page performance-now calls=%zu last-ms=%.0f\n",
                navigation.page.script_result.performance_now_calls,
                navigation.page.script_result.performance_now_last_ms);
        fprintf(stderr,
                "page watchdog polls=%zu elapsed-ms=%lu interrupted=%s\n",
                navigation.page.script_result.watchdog_polls,
                navigation.page.script_result.watchdog_elapsed_ms,
                navigation.page.script_result.interrupted ? "yes" : "no");
        fprintf(stderr, "page date-now calls=%zu last-ms=%.0f\n",
                navigation.page.script_result.date_now_calls,
                navigation.page.script_result.date_now_last_ms);
        fprintf(stderr,
                "page network-response status=%ld bytes=%zu hash=%016llx "
                "content-type=\"%s\" content-encoding=\"%s\" "
                "server=\"%s\" cf-ray=\"%s\" cf-mitigated=\"%s\" "
                "prefix-hex=\"%s\" url=\"%s\"\n",
                navigation.page.script_result.last_network_status,
                navigation.page.script_result.last_network_response_bytes,
                navigation.page.script_result.last_network_body_hash,
                navigation.page.script_result.last_network_content_type,
                navigation.page.script_result.last_network_content_encoding,
                navigation.page.script_result.last_network_server,
                navigation.page.script_result.last_network_cf_ray,
                navigation.page.script_result.last_network_cf_mitigated,
                navigation.page.script_result.last_network_body_prefix,
                navigation.page.script_result.last_network_url);
        fprintf(stderr,
                "page xhr sends=%zu responses=%zu status=%ld type=\"%s\" "
                "text-units=%zu bytes=%zu states=\"%s\"\n",
                navigation.page.script_result.xhr_send_calls,
                navigation.page.script_result.xhr_response_count,
                navigation.page.script_result.last_xhr_status,
                navigation.page.script_result.last_xhr_response_type,
                navigation.page.script_result.last_xhr_response_text_units,
                navigation.page.script_result.last_xhr_response_bytes,
                navigation.page.script_result.last_xhr_ready_states);
        if (navigation.last_page_trace[0] != '\0') {
            fprintf(stderr, "navigation failure-summary=\"%s\"\n",
                    navigation.last_page_trace);
        }
        if (navigation.last_script_error_context[0] != '\0') {
            fprintf(stderr, "navigation failure-source=\"%s\"\n",
                    navigation.last_script_error_context);
        }
        if (trace_frames && navigation.last_frame_trace[0] != '\0') {
            fprintf(stderr,
                    "last-frame-capability url=\"%s\" trace=\"%s\"\n",
                    navigation.last_frame_trace_url,
                    navigation.last_frame_trace);
        }
        for (size_t i = 0; i < navigation.page.frame_count; i++) {
            const NavigationFrame *child = &navigation.page.frames[i];
            fprintf(stderr,
                    "frame[%zu] status=%s http=%ld error=\"%s\" "
                    "network=%zu/%zu/%ld url=\"%s\"\n", i,
                    child->loaded ? "loaded" : "pending-or-failed",
                    child->http_status, child->script_result.error,
                    child->script_result.network_requests,
                    child->script_result.network_failures,
                    child->script_result.last_network_status,
                    child->script_result.last_network_url);
            fprintf(stderr,
                    "frame[%zu] xhr sends=%zu responses=%zu status=%ld "
                    "type=\"%s\" text-units=%zu bytes=%zu states=\"%s\"\n",
                    i, child->script_result.xhr_send_calls,
                    child->script_result.xhr_response_count,
                    child->script_result.last_xhr_status,
                    child->script_result.last_xhr_response_type,
                    child->script_result.last_xhr_response_text_units,
                    child->script_result.last_xhr_response_bytes,
                    child->script_result.last_xhr_ready_states);
            if (trace_frames && child->capability_trace[0] != '\0') {
                fprintf(stderr,
                        "frame[%zu] failure-capability=\"%s\"\n", i,
                        child->capability_trace);
            }
            if (child->reject_source_context[0] != '\0') {
                fprintf(stderr, "frame[%zu] failure-source=\"%s\"\n", i,
                        child->reject_source_context);
            }
            if (child->script_result.summary[0] != '\0') {
                fprintf(stderr, "frame[%zu] failure-summary=\"%s\"\n", i,
                        child->script_result.summary);
            }
            fprintf(stderr, "frame[%zu] url=\"%s\" cf-ray=\"%s\"\n", i,
                    child->url, child->cf_ray);
        }
    }
    print_http_replay_ledger();
    if (navigation_ready && experimental_compressed_sections) {
        /* The transitional section path installs its backing directly on the
           engine-owned navigation session, so detach it before releasing the
           frontend-owned pager/store. */
        document_backing_uninstall(&navigation);
    }
    experimental_script_state_destroy(&experimental);
    experimental_remote_mutations_destroy(&experimental);
    section_pager_destroy(&experimental.pager);
    section_store_stream_abort(&section_stream_builder);
    if (section_fetch.budget != NULL) fetch_result_destroy(&section_fetch);
    section_store_destroy(&section_store);
    budget_free(budget, input);
    budget_free(budget, application);
    bool engine_clean = browser_engine_shutdown(engine);
    /* Scheduler shutdown finalizes or truthfully records cancellation for
       every captured async request. Publish trace authority only afterward,
       when each assigned sequence is guaranteed to have a record pair. */
    fetch_trace_end();
    size_t active_largest = 0;
    size_t active_allocations = budget_active_allocations(
        budget, &active_largest);
    budget_report_categories(budget, "interactive-teardown", stdout);
    if (budget->current != 0) budget_dump_active(budget, stderr, 32);
    bool teardown_pass = success && engine_clean && budget->current == 0;
    printf("interactive teardown=%zu active=%zu largest=%zu peak=%zu "
           "allocations=%zu frees=%zu failures=%zu status=%s\n",
           budget->current, active_allocations, active_largest, budget->peak,
           budget->allocation_count, budget->free_count,
           budget->failure_count, teardown_pass ? "PASS" : "FAIL");
    browser_engine_destroy(engine);
    if (platform_sim) {
        tilefinch_platform_set_services(NULL);
        host_platform_simulator = NULL;
    }
    return teardown_pass ? 0 : 1;
}

#undef session
#undef navigation
#undef cache
#undef controller
#undef fonts
#undef section_store
#undef section_stream_builder
#undef experimental_router
#undef experimental
#undef section_fetch
