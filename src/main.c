#include <errno.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#include "tilefinch/budget.h"
#include "tilefinch/document.h"
#include "tilefinch/fetch.h"
#include "tilefinch/font.h"
#include "tilefinch/js_runtime.h"
#include "tilefinch/layout.h"
#include "tilefinch/navigation.h"
#include "tilefinch/pixel_math.h"
#include "tilefinch/render.h"
#include "tilefinch/resources.h"
#include "tilefinch/section_store.h"
#include "tilefinch/section_router.h"
#include "tilefinch/style.h"

#define MIB (1024u * 1024u)
#define KIB 1024u
#define VIEWPORT_WIDTH 480
#define VIEWPORT_HEIGHT 272
#define EXPERIMENTAL_SECTION_BLOCK_BYTES (64u * KIB - 1)
#define EXPERIMENTAL_SECTION_MAX_BYTES (512u * KIB)

#ifndef TILEFINCH_PROFILE_DIR
#define TILEFINCH_PROFILE_DIR "profiles"
#endif
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

static void usage(const char *program)
{
    printf("usage: %s [--fixture FILE | --url URL] [--output-dir DIR] "
           "[--limit-mb N] [--js-limit-mb N] [--user-css FILE] "
           "[--reader-profile auto|wikipedia|reddit|chatgpt|nytimes|hacker-news|none] "
           "[--psp-profile strict|realistic] [--tile-count N] "
           "[--navigation-stress N] "
           "[--max-download-kb N] [--fetch-css] [--max-stylesheets N] "
           "[--max-css-kb N] [--max-css-file-kb N] "
           "[--resource-stage-ms N] "
           "[--fetch-images] [--max-images N] [--max-image-kb N] "
           "[--max-image-file-kb N] [--max-decoded-image-kb N] "
           "[--sans-font FILE] [--serif-font FILE] [--sans-italic-font FILE] "
           "[--sans-bold-font FILE] [--serif-bold-font FILE] "
           "[--metric-sans-font FILE] [--metric-sans-bold-font FILE] "
           "[--max-font-kb N] [--no-ttf] "
           "[--viewport-width N] [--viewport-height N] "
           "[--viewport-css-width N] [--viewport-css-height N] "
           "[--dump-links FILE] [--dump-layout FILE] [--scroll-all] [--frame-hashes] [--inline-scripts-only] "
           "[--save-scroll Y] "
           "[--capture-http DIR | --replay-http DIR] "
           "[--deterministic-replay-seed N] "
           "[--adaptive-resources | --no-adaptive-resources] "
           "[--experimental-compressed-sections] [--experimental-section N] "
           "[--skip-js] [--no-render] [--challenge-diagnostic]\n", program);
}

static size_t minimum_size(size_t value, size_t maximum)
{
    return value < maximum ? value : maximum;
}

static bool stream_document_body(void *opaque, const unsigned char *data,
                                 size_t length)
{
    return document_parser_feed(opaque, (const char *) data, length);
}

static bool document_has_external_script(lxb_dom_node_t *node)
{
    for (; node != NULL; node = node->next) {
        size_t name_length = 0, source_length = 0;
        const char *name = document_element_name(node, &name_length);
        if (name != NULL && name_length == 6
            && memcmp(name, "script", 6) == 0
            && document_attribute(node, "src", &source_length) != NULL
            && source_length != 0) return true;
        if (document_has_external_script(node->first_child)) return true;
    }
    return false;
}

static const char *reader_profile_path(const char *selection,
                                       const char *locator, const char *title,
                                       char *path, size_t path_size)
{
    const char *profile = selection;
    if (strcmp(selection, "auto") == 0) {
        if ((locator != NULL && strstr(locator, "wikipedia.org") != NULL)
            || (title != NULL && strstr(title, "Wikipedia") != NULL)) {
            profile = "wikipedia";
        } else if ((locator != NULL && strstr(locator, "reddit") != NULL)
                   || (title != NULL && strstr(title, "reddit") != NULL)) {
            profile = "reddit";
        } else if ((locator != NULL && strstr(locator, "chatgpt.com") != NULL)
                   || (title != NULL && strcmp(title, "ChatGPT") == 0)) {
            profile = "chatgpt";
        } else if ((locator != NULL
                    && (strstr(locator, "nytimes.com") != NULL
                        || strstr(locator, "nytimes-mobile") != NULL))
                   || (title != NULL
                       && strstr(title, "New York Times") != NULL)) {
            profile = "nytimes";
        } else if ((locator != NULL
                    && strstr(locator, "news.ycombinator.com") != NULL)
                   || (title != NULL
                       && strstr(title, "Hacker News") != NULL)) {
            profile = "hacker-news";
        } else {
            return NULL;
        }
    }
    if (strcmp(profile, "none") == 0) return NULL;
    const char *filename = strcmp(profile, "wikipedia") == 0
                           ? "wikipedia.css"
                           : (strcmp(profile, "chatgpt") == 0
                              ? "chatgpt.css"
                              : (strcmp(profile, "nytimes") == 0
                                 ? "nytimes.css"
                                 : (strcmp(profile, "hacker-news") == 0
                                    ? "hacker-news.css"
                                    : "reddit-old.css")));
    int written = snprintf(path, path_size, "%s/%s", TILEFINCH_PROFILE_DIR,
                           filename);
    return written >= 0 && (size_t) written < path_size ? path : NULL;
}

static double now_ms(void)
{
    struct timespec value;
    (void) timespec_get(&value, TIME_UTC);
    return (double) value.tv_sec * 1000.0 + (double) value.tv_nsec / 1000000.0;
}

/* Blank detection only needs "any non-background pixel", so it exits on
   the first one; a typical frame touches a handful of pixels instead of
   the whole buffer. */
static bool frame_is_blank(const uint16_t *frame, size_t pixels,
                           uint16_t background)
{
    for (size_t i = 0; i < pixels; i++) {
        if (frame[i] != background) return false;
    }
    return true;
}

/* Optional per-frame fingerprints (--frame-hashes): byte-order FNV-1a kept
   stable across releases so manifests from different binaries stay
   comparable, with the non-background count folded into the same pass. */
static uint64_t frame_hash_and_count(const uint16_t *frame, size_t pixels,
                                     uint16_t background,
                                     size_t *non_background)
{
    uint64_t hash = UINT64_C(1469598103934665603);
    size_t count = 0;
    for (size_t i = 0; i < pixels; i++) {
        hash ^= frame[i] & 0xffu;
        hash *= UINT64_C(1099511628211);
        hash ^= frame[i] >> 8;
        hash *= UINT64_C(1099511628211);
        if (frame[i] != background) count++;
    }
    *non_background = count;
    return hash;
}

static uint16_t color_rgb565(uint32_t color)
{
    return tilefinch_rgb565_pack_u8(
        (color >> 16) & 0xffu, (color >> 8) & 0xffu, color & 0xffu);
}


static char *read_file(Budget *budget, const char *path, size_t *length)
{
    FILE *file = fopen(path, "rb");
    if (file == NULL) return NULL;
    if (fseek(file, 0, SEEK_END) != 0) { fclose(file); return NULL; }
    long end = ftell(file);
    if (end < 0 || fseek(file, 0, SEEK_SET) != 0) { fclose(file); return NULL; }
    char *data = budget_malloc_category(
        budget, BUDGET_CATEGORY_RESOURCE, (size_t) end + 1);
    if (data == NULL) { fclose(file); return NULL; }
    size_t received = fread(data, 1, (size_t) end, file);
    bool ok = received == (size_t) end && ferror(file) == 0;
    fclose(file);
    if (!ok) { budget_free(budget, data); return NULL; }
    data[received] = '\0';
    *length = received;
    return data;
}

static void report_script_urls(lxb_dom_node_t *node, const char *locator,
                               size_t *count)
{
    for (; node != NULL && *count < 32; node = node->next) {
        size_t name_length = 0;
        const char *name = document_element_name(node, &name_length);
        if (name != NULL && name_length == 6
            && memcmp(name, "script", 6) == 0) {
            size_t source_length = 0;
            const char *source = document_attribute(node, "src",
                                                     &source_length);
            if (source != NULL && source_length != 0
                && source_length < 2048) {
                char reference[2048], resolved[4096];
                memcpy(reference, source, source_length);
                reference[source_length] = '\0';
                const char *reported = reference;
                if (locator != NULL
                    && fetch_resolve_url(locator, reference, resolved,
                                         sizeof(resolved))) {
                    reported = resolved;
                }
                printf("challenge-script[%zu]=\"%s\"\n", (*count)++,
                       reported);
            }
        }
        report_script_urls(node->first_child, locator, count);
    }
}

/* The static lab keeps all of these objects alive for the complete load.  They
   are intentionally budget-owned instead of consuming the small PSP thread
   stack, and their fixed cost therefore participates in envelope reporting. */
typedef struct {
    PocDocument document;
    DocumentParser network_parser;
    Stylesheet stylesheet;
    LayoutDocument layout;
    TileCache cache;
    ScriptResult scripts;
    FetchResult fetch;
    FetchStreamMetrics network_stream_metrics;
    ExternalStylesheetStats external_css;
    ImageResources images;
    FontSet fonts;
    CompressedSectionStore section_store;
    SectionStoreStreamBuilder section_stream_builder;
    SectionRouteStream section_router;
    NavigationSession stress_navigation;
} LabApplicationContext;

int main(int argc, char **argv)
{
    const char *fixture = "fixtures/demo.html";
    const char *url = NULL;
    const char *output_dir = "frames";
    const char *user_css_path = NULL;
    const char *reader_profile = "none";
    const char *links_path = NULL;
    const char *layout_path = NULL;
    const char *capture_http = NULL;
    const char *replay_http = NULL;
    const char *psp_profile = NULL;
    const char *sans_font_path = TILEFINCH_SANS_FONT;
    const char *serif_font_path = TILEFINCH_SERIF_FONT;
    const char *sans_italic_font_path = TILEFINCH_SANS_ITALIC_FONT;
    const char *sans_bold_font_path = TILEFINCH_SANS_BOLD_FONT;
    const char *serif_bold_font_path = TILEFINCH_SERIF_BOLD_FONT;
    const char *metric_sans_font_path = TILEFINCH_METRIC_SANS_FONT;
    const char *metric_sans_bold_font_path = TILEFINCH_METRIC_SANS_BOLD_FONT;
    size_t limit_mb = 48;
    size_t js_limit_mb = 8;
    size_t max_download_kb = 4096;
    size_t max_stylesheets = 6;
    size_t max_css_kb = 1024;
    size_t max_css_file_kb = 384;
    size_t max_font_kb = 1536;
    size_t max_images = 64;
    size_t max_image_kb = 512;
    size_t max_image_file_kb = 256;
    size_t max_decoded_image_kb = 512;
    size_t viewport_width = VIEWPORT_WIDTH;
    size_t viewport_height = VIEWPORT_HEIGHT;
    size_t viewport_css_width = 0;
    size_t viewport_css_height = 0;
    size_t tile_capacity = 8;
    size_t navigation_stress = 0;
    long resource_stage_ms = 15000;
    int save_scrolls[16];
    size_t save_scroll_count = 0;
    bool run_javascript = true;
    bool fetch_external_css = false;
    bool fetch_external_images = false;
    bool render_frames = true;
    bool load_truetype = true;
    bool scroll_all = false;
    bool frame_hashes = false;
    bool inline_scripts_only = false;
    bool limit_explicit = false;
    bool js_limit_explicit = false;
    bool tile_capacity_explicit = false;
    bool challenge_diagnostic = false;
    bool adaptive_resources = false;
    bool adaptive_resources_explicit = false;
    bool deterministic_replay_requested = false;
    uint64_t deterministic_replay_seed = 0;
    bool experimental_compressed_sections = false;
    bool experimental_section_explicit = false;
    size_t experimental_section = 0;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--help") == 0) { usage(argv[0]); return 0; }
        if (strcmp(argv[i], "--skip-js") == 0) {
            run_javascript = false;
            continue;
        }
        if (strcmp(argv[i], "--fetch-css") == 0) {
            fetch_external_css = true;
            continue;
        }
        if (strcmp(argv[i], "--fetch-images") == 0) {
            fetch_external_images = true;
            continue;
        }
        if (strcmp(argv[i], "--no-render") == 0) {
            render_frames = false;
            continue;
        }
        if (strcmp(argv[i], "--no-ttf") == 0) {
            load_truetype = false;
            continue;
        }
        if (strcmp(argv[i], "--scroll-all") == 0) {
            scroll_all = true;
            continue;
        }
        if (strcmp(argv[i], "--frame-hashes") == 0) {
            frame_hashes = true;
            continue;
        }
        if (strcmp(argv[i], "--inline-scripts-only") == 0) {
            inline_scripts_only = true;
            continue;
        }
        if (strcmp(argv[i], "--challenge-diagnostic") == 0) {
            challenge_diagnostic = true;
            continue;
        }
        if (strcmp(argv[i], "--experimental-compressed-sections") == 0) {
            experimental_compressed_sections = true;
            continue;
        }
        if (strcmp(argv[i], "--adaptive-resources") == 0) {
            adaptive_resources = true;
            adaptive_resources_explicit = true;
            continue;
        }
        if (strcmp(argv[i], "--no-adaptive-resources") == 0) {
            adaptive_resources = false;
            adaptive_resources_explicit = true;
            continue;
        }
        if (i + 1 >= argc) { usage(argv[0]); return 2; }
        const char *value = argv[++i];
        if (strcmp(argv[i - 1], "--fixture") == 0) fixture = value;
        else if (strcmp(argv[i - 1], "--url") == 0) url = value;
        else if (strcmp(argv[i - 1], "--output-dir") == 0) output_dir = value;
        else if (strcmp(argv[i - 1], "--limit-mb") == 0) {
            limit_mb = (size_t) strtoul(value, NULL, 10);
            limit_explicit = true;
        }
        else if (strcmp(argv[i - 1], "--js-limit-mb") == 0) {
            js_limit_mb = (size_t) strtoul(value, NULL, 10);
            js_limit_explicit = true;
        }
        else if (strcmp(argv[i - 1], "--psp-profile") == 0) psp_profile = value;
        else if (strcmp(argv[i - 1], "--tile-count") == 0) {
            tile_capacity = (size_t) strtoul(value, NULL, 10);
            tile_capacity_explicit = true;
        }
        else if (strcmp(argv[i - 1], "--navigation-stress") == 0) {
            navigation_stress = (size_t) strtoul(value, NULL, 10);
        }
        else if (strcmp(argv[i - 1], "--user-css") == 0) user_css_path = value;
        else if (strcmp(argv[i - 1], "--reader-profile") == 0) reader_profile = value;
        else if (strcmp(argv[i - 1], "--max-download-kb") == 0) max_download_kb = (size_t) strtoul(value, NULL, 10);
        else if (strcmp(argv[i - 1], "--max-stylesheets") == 0) max_stylesheets = (size_t) strtoul(value, NULL, 10);
        else if (strcmp(argv[i - 1], "--max-css-kb") == 0) max_css_kb = (size_t) strtoul(value, NULL, 10);
        else if (strcmp(argv[i - 1], "--max-css-file-kb") == 0) max_css_file_kb = (size_t) strtoul(value, NULL, 10);
        else if (strcmp(argv[i - 1], "--resource-stage-ms") == 0) resource_stage_ms = strtol(value, NULL, 10);
        else if (strcmp(argv[i - 1], "--sans-font") == 0) sans_font_path = value;
        else if (strcmp(argv[i - 1], "--serif-font") == 0) serif_font_path = value;
        else if (strcmp(argv[i - 1], "--sans-italic-font") == 0) sans_italic_font_path = value;
        else if (strcmp(argv[i - 1], "--sans-bold-font") == 0) sans_bold_font_path = value;
        else if (strcmp(argv[i - 1], "--serif-bold-font") == 0) serif_bold_font_path = value;
        else if (strcmp(argv[i - 1], "--metric-sans-font") == 0) metric_sans_font_path = value;
        else if (strcmp(argv[i - 1], "--metric-sans-bold-font") == 0) metric_sans_bold_font_path = value;
        else if (strcmp(argv[i - 1], "--max-font-kb") == 0) max_font_kb = (size_t) strtoul(value, NULL, 10);
        else if (strcmp(argv[i - 1], "--max-images") == 0) max_images = (size_t) strtoul(value, NULL, 10);
        else if (strcmp(argv[i - 1], "--max-image-kb") == 0) max_image_kb = (size_t) strtoul(value, NULL, 10);
        else if (strcmp(argv[i - 1], "--max-image-file-kb") == 0) max_image_file_kb = (size_t) strtoul(value, NULL, 10);
        else if (strcmp(argv[i - 1], "--max-decoded-image-kb") == 0) max_decoded_image_kb = (size_t) strtoul(value, NULL, 10);
        else if (strcmp(argv[i - 1], "--viewport-width") == 0) viewport_width = (size_t) strtoul(value, NULL, 10);
        else if (strcmp(argv[i - 1], "--viewport-height") == 0) viewport_height = (size_t) strtoul(value, NULL, 10);
        else if (strcmp(argv[i - 1], "--viewport-css-width") == 0) viewport_css_width = (size_t) strtoul(value, NULL, 10);
        else if (strcmp(argv[i - 1], "--viewport-css-height") == 0) viewport_css_height = (size_t) strtoul(value, NULL, 10);
        else if (strcmp(argv[i - 1], "--dump-links") == 0) links_path = value;
        else if (strcmp(argv[i - 1], "--dump-layout") == 0) layout_path = value;
        else if (strcmp(argv[i - 1], "--capture-http") == 0) capture_http = value;
        else if (strcmp(argv[i - 1], "--replay-http") == 0) replay_http = value;
        else if (strcmp(argv[i - 1], "--deterministic-replay-seed") == 0) {
            deterministic_replay_seed = strtoull(value, NULL, 0);
            deterministic_replay_requested = true;
        }
        else if (strcmp(argv[i - 1], "--experimental-section") == 0) {
            char *end = NULL;
            errno = 0;
            unsigned long long parsed = strtoull(value, &end, 10);
            if (value[0] == '-' || end == value || *end != '\0'
                || errno == ERANGE || parsed > SIZE_MAX) {
                usage(argv[0]); return 2;
            }
            experimental_section = (size_t) parsed;
            experimental_compressed_sections = true;
            experimental_section_explicit = true;
        }
        else if (strcmp(argv[i - 1], "--save-scroll") == 0) {
            unsigned long parsed = strtoul(value, NULL, 10);
            if (save_scroll_count >= sizeof(save_scrolls) / sizeof(save_scrolls[0])
                || parsed > INT_MAX) {
                usage(argv[0]); return 2;
            }
            save_scrolls[save_scroll_count++] = (int) parsed;
        }
        else { usage(argv[0]); return 2; }
    }
    if (psp_profile != NULL) {
        bool strict = strcmp(psp_profile, "strict") == 0;
        bool realistic = strcmp(psp_profile, "realistic") == 0;
        if (!strict && !realistic) {
            fprintf(stderr, "invalid PSP profile\n");
            return 2;
        }
        if (!limit_explicit) limit_mb = strict ? 16 : 24;
        if (!js_limit_explicit) js_limit_mb = 4;
        if (!tile_capacity_explicit) tile_capacity = 8;
        if (!adaptive_resources_explicit) adaptive_resources = true;
    }
    if (limit_mb == 0 || js_limit_mb == 0 || tile_capacity == 0
        || tile_capacity > 32 || navigation_stress > 1000
        || max_download_kb == 0
        || resource_stage_ms < 100 || resource_stage_ms > 60000
        || max_stylesheets == 0 || max_stylesheets > 32
        || max_css_kb == 0 || max_css_file_kb == 0
        || max_font_kb == 0 || max_font_kb > 4096
        || max_images == 0 || max_images > 64
        || max_image_kb == 0 || max_image_file_kb == 0
        || max_decoded_image_kb == 0
        || viewport_width < 240 || viewport_width > 1024
        || viewport_height < 136 || viewport_height > 768
        || limit_mb > SIZE_MAX / MIB || js_limit_mb > SIZE_MAX / MIB
        || max_download_kb > SIZE_MAX / KIB || max_css_kb > SIZE_MAX / KIB
        || max_css_file_kb > SIZE_MAX / KIB
        || max_font_kb > SIZE_MAX / KIB
        || max_image_kb > SIZE_MAX / KIB
        || max_image_file_kb > SIZE_MAX / KIB
        || max_decoded_image_kb > SIZE_MAX / KIB
        || (capture_http != NULL && replay_http != NULL)
        || (deterministic_replay_requested
            && capture_http == NULL && replay_http == NULL)
        || (experimental_compressed_sections && navigation_stress != 0)
        || (strcmp(reader_profile, "auto") != 0
            && strcmp(reader_profile, "wikipedia") != 0
            && strcmp(reader_profile, "reddit") != 0
            && strcmp(reader_profile, "chatgpt") != 0
            && strcmp(reader_profile, "nytimes") != 0
            && strcmp(reader_profile, "hacker-news") != 0
            && strcmp(reader_profile, "none") != 0)) {
        fprintf(stderr, "invalid memory limit\n");
        return 2;
    }

    Budget budget;
    budget_init(&budget, limit_mb * MIB);
    LabApplicationContext *application = budget_calloc_category(
        &budget, BUDGET_CATEGORY_NAVIGATION, 1, sizeof(*application));
    if (application == NULL) {
        fprintf(stderr, "lab application context reservation failed\n");
        return 1;
    }
#define document (application->document)
#define network_parser (application->network_parser)
#define stylesheet (application->stylesheet)
#define layout (application->layout)
#define cache (application->cache)
#define scripts (application->scripts)
#define fetch (application->fetch)
#define network_stream_metrics (application->network_stream_metrics)
#define external_css (application->external_css)
#define images (application->images)
#define fonts (application->fonts)
#define section_store (application->section_store)
#define section_stream_builder (application->section_stream_builder)
#define section_router (application->section_router)
    char trace_error[256] = {0};
    script_runtime_configure_deterministic_replay(
        deterministic_replay_requested, deterministic_replay_seed);
    if ((capture_http != NULL
         && !fetch_trace_capture_begin(capture_http, trace_error,
                                       sizeof(trace_error)))
        || (replay_http != NULL
            && !fetch_trace_replay_begin(replay_http, trace_error,
                                         sizeof(trace_error)))) {
        fprintf(stderr, "HTTP trace setup failed: %s\n", trace_error);
        budget_free(&budget, application);
        return 1;
    }
    if (!budget_install_lexbor(&budget)) {
        fprintf(stderr, "Lexbor allocator owner is still live\n");
        fetch_trace_end();
        budget_free(&budget, application);
        return 1;
    }
    char *html = NULL;
    char *user_css = NULL;
    char *profile_css = NULL;
    uint16_t *frame = NULL;
    uint16_t *scroll_reference = NULL;
    bool network_input = url != NULL;
    int status = 1;
    double started_ms = now_ms();
    double input_ms = 0.0;
    double parse_ms = 0.0;
    double css_ms = 0.0;
    double layout_ms = 0.0;
    double javascript_ms = 0.0;
    double render_ms = 0.0;
    size_t scroll_frames = 0;
    size_t blank_frames = 0;
    bool scroll_revisit_match = true;
    bool adaptive_javascript_skipped = false;
    bool adaptive_css_reduced = false;
    bool adaptive_images_reduced = false;
    bool adaptive_tiles_reduced = false;
    bool document_already_parsed = false;
    bool external_script_boundary_skipped = false;
    bool html_is_extracted_fragment = false;
    bool experimental_sections_active = false;
    size_t source_html_length = 0;

    printf("runtime-profile=%s limit_mb=%zu js_limit_mb=%zu tiles=%zu\n",
           psp_profile == NULL ? "custom" : psp_profile,
           limit_mb, js_limit_mb, tile_capacity);

    frame = budget_malloc_category(
        &budget, BUDGET_CATEGORY_RENDER,
        viewport_width * viewport_height * sizeof(*frame));
    if (frame == NULL) {
        fprintf(stderr, "framebuffer reservation failed\n");
        goto cleanup;
    }
    budget_report(&budget, "framebuffer", stdout);
    if (load_truetype) {
        if (font_set_load(&fonts, &budget, sans_font_path, serif_font_path,
                          sans_italic_font_path, sans_bold_font_path,
                          serif_bold_font_path, metric_sans_font_path,
                          metric_sans_bold_font_path,
                          max_font_kb * KIB)) {
            printf("fonts sans=%s serif=%s sans-italic=%s sans-bold=%s "
                   "serif-bold=%s metric-sans=%s metric-sans-bold=%s "
                   "bytes=%zu limit=%zu\n",
                   fonts.sans.loaded ? "loaded" : "fallback",
                   fonts.serif.loaded ? "loaded" : "fallback",
                   fonts.sans_italic.loaded ? "loaded" : "synthetic",
                   fonts.sans_bold.loaded ? "loaded" : "synthetic",
                   fonts.serif_bold.loaded ? "loaded" : "synthetic",
                   fonts.metric_sans.loaded ? "loaded" : "fallback",
                   fonts.metric_sans_bold.loaded ? "loaded" : "synthetic",
                   fonts.sans.data_length + fonts.serif.data_length
                       + fonts.sans_italic.data_length
                       + fonts.sans_bold.data_length
                       + fonts.serif_bold.data_length
                       + fonts.metric_sans.data_length
                       + fonts.metric_sans_bold.data_length,
                   max_font_kb * KIB);
            budget_report(&budget, "fonts", stdout);
        } else {
            printf("fonts unavailable; using 5x7 fallback\n");
        }
    } else {
        printf("fonts disabled; using 5x7 fallback\n");
    }

    size_t html_length = 0;
    double phase_ms = now_ms();
    if (network_input) {
        bool fetched = false;
        if (experimental_compressed_sections) {
            if (!section_route_stream_begin(
                    &section_router, &section_stream_builder, &section_store,
                    &budget, EXPERIMENTAL_SECTION_BLOCK_BYTES,
                    EXPERIMENTAL_SECTION_MAX_BYTES,
                    experimental_section_explicit)) {
                fprintf(stderr, "experimental compressed stream startup failed\n");
                goto cleanup;
            }
            FetchStreamOptions stream = {
                .on_body = section_route_stream_body,
                .opaque = &section_router,
                .chunk_bytes = 16384
            };
            fetched = fetch_request_stream_cancelable(
                &budget, url, NULL, max_download_kb * KIB, 30000,
                NULL, NULL, &stream, &network_stream_metrics, &fetch);
            if (fetched && !section_route_stream_finish(&section_router)) {
                snprintf(fetch.error, sizeof(fetch.error), "%s",
                         "compressed section stream finalization failed");
                fetched = false;
            }
        } else if (navigation_stress != 0) {
            fetched = fetch_url(&budget, url, max_download_kb * KIB,
                                30000, &fetch);
        } else if (!document_parser_begin(&network_parser, &budget)) {
            fprintf(stderr, "streaming HTML parser startup failed\n");
            goto cleanup;
        } else if (challenge_diagnostic) {
            FetchRequest request = {
                .method = "GET", .allow_http_errors = true
            };
            FetchStreamOptions stream = {
                .on_body = stream_document_body,
                .opaque = &network_parser,
                .chunk_bytes = 16384
            };
            fetched = fetch_request_stream_cancelable(
                &budget, url, &request, max_download_kb * KIB, 30000,
                NULL, NULL, &stream, &network_stream_metrics, &fetch);
        } else {
            FetchStreamOptions stream = {
                .on_body = stream_document_body,
                .opaque = &network_parser,
                .chunk_bytes = 16384
            };
            fetched = fetch_request_stream_cancelable(
                &budget, url, NULL, max_download_kb * KIB, 30000,
                NULL, NULL, &stream, &network_stream_metrics, &fetch);
        }
        if (!fetched) {
            input_ms = now_ms() - phase_ms;
            html_length = network_stream_metrics.bytes_received;
            printf("stream bytes=%zu chunks=%zu peak-buffer=%zu "
                   "headers=%s cancelled=%s truncated=%s\n",
                   network_stream_metrics.bytes_received,
                   network_stream_metrics.chunks_received,
                   network_stream_metrics.peak_buffered_bytes,
                   network_stream_metrics.headers_delivered ? "yes" : "no",
                   network_stream_metrics.cancelled ? "yes" : "no",
                   network_stream_metrics.truncated ? "yes" : "no");
            fprintf(stderr, "could not fetch %s: %s\n", url, fetch.error);
            goto cleanup;
        }
        if (experimental_compressed_sections && !section_router.sectioned) {
            html = (char *) section_router.buffer;
            html_length = section_router.length;
            section_router.buffer = NULL;
            fetch.data = html;
            fetch.length = html_length;
            fetch.capacity = section_router.capacity;
            printf("experimental-adaptive mode=full-document source=%zu "
                   "threshold=%zu elements=%zu attributes=%zu depth=%zu\n",
                   html_length, section_router.byte_threshold,
                   section_router.element_count, section_router.attribute_count,
                   section_router.maximum_depth);
        } else {
            html = fetch.data;
            html_length = fetch.length;
        }
        experimental_sections_active = experimental_compressed_sections
            && section_router.sectioned;
        if (experimental_compressed_sections) {
            printf("experimental-input bytes=%zu chunks=%zu peak-buffer=%zu "
                   "mode=%s\n",
                   html_length, network_stream_metrics.chunks_received,
                   network_stream_metrics.peak_buffered_bytes,
                   experimental_sections_active
                     ? "direct-compressed-stream" : "adaptive-full-document");
        } else {
            printf("stream bytes=%zu chunks=%zu peak-buffer=%zu headers=%s\n",
                   network_stream_metrics.bytes_received,
                   network_stream_metrics.chunks_received,
                   network_stream_metrics.peak_buffered_bytes,
                   network_stream_metrics.headers_delivered ? "yes" : "no");
        }
        if (navigation_stress == 0 && !experimental_compressed_sections) {
            phase_ms = now_ms();
            if (!document_parser_finish(&network_parser, &document)) {
                fprintf(stderr, "streaming HTML parser finalization failed\n");
                goto cleanup;
            }
            parse_ms = now_ms() - phase_ms;
            document_already_parsed = true;
        }
        printf("network status=%ld content-type=\"%s\" effective=\"%s\"\n",
               fetch.status_code, fetch.content_type, fetch.effective_url);
        if (challenge_diagnostic) {
            printf("challenge-response mitigated=\"%s\" server=\"%s\" "
                   "accept-ch=\"%s\" critical-ch=\"%s\"\n",
                   fetch.cf_mitigated, fetch.server, fetch.accept_ch,
                   fetch.critical_ch);
        }
    } else {
        html = read_file(&budget, fixture, &html_length);
        if (html != NULL && experimental_compressed_sections) {
            SectionRouteStream classifier = {
                .budget = &budget,
                .byte_threshold = section_route_stream_threshold(&budget)
            };
            section_route_stream_scan(
                &classifier, (const unsigned char *) html, html_length);
            experimental_sections_active = experimental_section_explicit
                || section_route_stream_prefers_sections(
                       &classifier, html_length);
            printf("experimental-adaptive mode=%s source=%zu threshold=%zu "
                   "elements=%zu attributes=%zu depth=%zu fixture=yes\n",
                   experimental_sections_active ? "sections" : "full-document",
                   html_length, classifier.byte_threshold,
                   classifier.element_count, classifier.attribute_count,
                   classifier.maximum_depth);
        }
    }
    input_ms = now_ms() - phase_ms;
    if (html == NULL && !document_already_parsed
        && !(experimental_sections_active && network_input
             && section_store.source_length != 0)) {
        fprintf(stderr, "could not read %s\n", fixture);
        goto cleanup;
    }
    const char *locator = network_input && fetch.effective_url[0] != '\0'
                          ? fetch.effective_url
                          : (network_input ? url : fixture);
    source_html_length = html_length;
    if (experimental_sections_active) {
        if (fetch_external_css || fetch_external_images || run_javascript
            || strcmp(reader_profile, "none") != 0) {
            printf("experimental-policy javascript=disabled external-css=disabled "
                   "images=disabled reader-profile=none\n");
        }
        run_javascript = false;
        fetch_external_css = false;
        fetch_external_images = false;
        reader_profile = "none";
        if (!network_input
            && !section_store_build(&section_store, &budget, html, html_length,
                                    EXPERIMENTAL_SECTION_BLOCK_BYTES,
                                    EXPERIMENTAL_SECTION_MAX_BYTES)) {
            fprintf(stderr, "experimental compressed section indexing failed\n");
            goto cleanup;
        }
        if (experimental_section >= section_store.section_count) {
            fprintf(stderr, "experimental section %zu is out of range (count=%zu)\n",
                    experimental_section, section_store.section_count);
            goto cleanup;
        }
        char *fragment = NULL;
        size_t fragment_length = 0;
        if (!section_store_extract_html(&section_store, experimental_section,
                                        &fragment, &fragment_length)) {
            fprintf(stderr, "experimental section extraction failed\n");
            goto cleanup;
        }
        if (network_input) {
            budget_free(&budget, fetch.data);
            fetch.data = NULL;
            fetch.length = 0;
            fetch.capacity = 0;
        } else {
            budget_free(&budget, html);
        }
        html = fragment;
        html_length = fragment_length;
        html_is_extracted_fragment = true;
        const SectionStoreEntry *selected =
            &section_store.sections[experimental_section];
        printf("experimental-section-store source=%zu stored=%zu ratio=%.3f "
               "blocks=%zu compressed=%zu raw=%zu sections=%zu anchors=%zu "
               "block-bytes=%zu section-cap=%zu\n",
               section_store.source_length, section_store.stored_length,
               section_store.source_length == 0 ? 0.0
                 : (double) section_store.stored_length
                   / (double) section_store.source_length,
               section_store.block_count, section_store.compressed_blocks,
               section_store.raw_blocks, section_store.section_count,
               section_store.anchor_count,
               section_store.block_bytes,
               section_store.maximum_section_bytes);
        printf("experimental-section selected=%zu source-offset=%u "
               "source-bytes=%u fragment-bytes=%zu heading-level=%u "
               "continuation=%s\n",
               experimental_section, selected->source_offset,
               selected->source_length, html_length,
               (unsigned) selected->heading_level,
               selected->continuation ? "yes" : "no");
    }
    budget_report(&budget, "input", stdout);
    if (navigation_stress != 0) {
        size_t before_navigation = budget.current;
        NavigationSession *stress_navigation = &application->stress_navigation;
        if (!navigation_init(stress_navigation, &budget, 16)) {
            fprintf(stderr, "navigation stress initialization failed\n");
            goto cleanup;
        }
        bool navigation_ok = true;
        for (size_t i = 0; i < navigation_stress; i++) {
            uint64_t generation = navigation_begin(stress_navigation);
            const char *stress_url = (i & 1u) == 0
                                     ? "https://stress.test/a"
                                     : "https://stress.test/b";
            if (!navigation_commit_html(stress_navigation, generation, stress_url,
                                        html, html_length,
                                        (int) viewport_width,
                                        (fonts.sans.loaded || fonts.serif.loaded)
                                            ? &fonts : NULL,
                                        NULL, true)) {
                navigation_ok = false;
                break;
            }
        }
        printf("navigation-stress requested=%zu committed=%zu destroys=%zu "
               "history=%zu pruned=%zu status=%s\n",
               navigation_stress, stress_navigation->loads_committed,
               stress_navigation->page_destroys,
               stress_navigation->history_count,
               stress_navigation->history_pruned,
               navigation_ok ? "PASS" : "FAIL");
        navigation_destroy(stress_navigation);
        if (!navigation_ok || budget.current != before_navigation) {
            fprintf(stderr, "navigation stress lifecycle failed\n");
            goto cleanup;
        }
    }
    if (!document_already_parsed) {
        phase_ms = now_ms();
        if (!document_parse(&document, &budget, html, html_length, 512)) {
            parse_ms = now_ms() - phase_ms;
            fprintf(stderr, "HTML parsing failed within the memory budget\n"); goto cleanup;
        }
        parse_ms = now_ms() - phase_ms;
    }
    char resource_base_url[4096] = {0};
    char document_policy[FETCH_REFERRER_POLICY_LIMIT] = {0};
    if (network_input) {
        bool referrer_header_present = false;
        if (!fetch_response_referrer_policy(
                &fetch, &referrer_header_present,
                document_policy)
            || !document_referrer_policy(
                   &document, document_policy, document_policy,
                   sizeof(document_policy))
            || !document_base_url(
                   &document, locator, resource_base_url,
                   sizeof(resource_base_url))) {
            fprintf(stderr, "document resource metadata failed\n");
            goto cleanup;
        }
        (void) referrer_header_present;
    } else {
        snprintf(resource_base_url, sizeof(resource_base_url), "%s",
                 locator);
    }
    ViewportContext viewport;
    if (!viewport_context_resolve(&viewport, &document,
                                  (int) viewport_width,
                                  (int) viewport_height, 980)) {
        fprintf(stderr, "mobile viewport resolution failed\n");
        goto cleanup;
    }
    if (viewport_css_width > 0 && viewport_css_height > 0) {
        /* A declared layout viewport (the fidelity oracle's emulated CSS
           viewport) overrides the document meta resolution; the device
           frame keeps its own size and the content is scaled to fit. */
        if (!viewport_context_init(&viewport, (int) viewport_css_width,
                                   (int) viewport_css_height,
                                   (int) viewport_width,
                                   (int) viewport_height)) {
            fprintf(stderr, "declared viewport resolution failed\n");
            goto cleanup;
        }
    }
    budget_report(&budget, "DOM", stdout);
    if (run_javascript
        && document_has_external_script(
            lxb_dom_interface_node(document.html))) {
        if (inline_scripts_only) {
            /* Fidelity replays compare against a reference browser whose
               external scripts also never completed (the trace aborts
               unmatched requests), so executing only the document's inline
               scripts mirrors the oracle's actual state. */
            printf("javascript boundary=external-script-loader-unavailable "
                   "action=inline-scripts-only\n");
        } else {
            /* The one-shot analysis binary has no external-script loader.
               Running only later inline scripts creates a state no live
               browser exposes and can hide server fallbacks.
               NavigationSession owns the complete bounded external-script
               pipeline used by the interactive browser. */
            run_javascript = false;
            external_script_boundary_skipped = true;
            printf("javascript boundary=external-script-loader-unavailable "
                   "action=skip-document-scripts\n");
        }
    }
    if (adaptive_resources && run_javascript
        && budget_pressure_required(&budget, js_limit_mb * MIB, 2 * MIB)) {
        run_javascript = false;
        adaptive_javascript_skipped = true;
        budget_record_pressure(&budget, BUDGET_PRESSURE_JAVASCRIPT,
                               js_limit_mb * MIB, 0);
        printf("adaptive-degradation stage=javascript action=skip "
               "reason=working-set-reserve remaining=%zu "
               "required-reserve=%zu avoided-bytes=%zu\n",
               budget_remaining(&budget), js_limit_mb * MIB + 2 * MIB,
               js_limit_mb * MIB);
    }
    if (run_javascript) {
        ScriptExecutionPolicy execution_policy;
        const ScriptExecutionPolicy *selected_execution_policy = NULL;
        if (psp_profile != NULL) {
            ScriptExecutionProfile execution_profile =
                strcmp(psp_profile, "strict") == 0
                    ? SCRIPT_EXECUTION_PROFILE_PSP_STRICT
                    : SCRIPT_EXECUTION_PROFILE_PSP_REALISTIC;
            if (!script_execution_policy_for_profile(
                    execution_profile, &execution_policy)) {
                fprintf(stderr, "PSP script execution policy setup failed\n");
                goto cleanup;
            }
            selected_execution_policy = &execution_policy;
        }
        /* Parser interrupt polls make a blown watchdog budget observable
           during compiles, so the lab budget must genuinely cover engine
           bootstrap.  Sanitizer builds run several times slower; widen the
           budget the same way the runtime widens its JS stack guard. */
        unsigned script_timeout_ms = 100;
#if defined(__has_feature)
#if __has_feature(address_sanitizer)
        script_timeout_ms = 1000;
#endif
#elif defined(__SANITIZE_ADDRESS__)
        script_timeout_ms = 1000;
#endif
        phase_ms = now_ms();
        if (!scripts_run_document_at_context_with_policy(
                &document, &budget, js_limit_mb * MIB, script_timeout_ms,
                locator, &viewport, selected_execution_policy, &scripts)) {
            javascript_ms = now_ms() - phase_ms;
            fprintf(stderr, "script failed%s: %s\n",
                    scripts.interrupted ? " (watchdog)" : "", scripts.error);
            goto cleanup;
        }
        javascript_ms = now_ms() - phase_ms;
        budget_report(&budget, "JavaScript", stdout);
        if (challenge_diagnostic) {
            size_t challenge_script_count = 0;
            report_script_urls(lxb_dom_interface_node(document.html),
                               locator, &challenge_script_count);
            printf("challenge-diagnostic scripts=%zu status=%s\n",
                   challenge_script_count,
                   strcmp(fetch.cf_mitigated, "challenge") == 0
                     ? "managed-challenge" : "ordinary-response");
        }
    } else {
        printf("javascript skipped by %s policy\n",
               adaptive_javascript_skipped ? "adaptive memory"
                 : external_script_boundary_skipped
                   ? "external loader boundary" : "benchmark");
    }
    phase_ms = now_ms();
    bool defer_document_styles =
        fetch_external_css && network_input;
    bool stylesheet_built = defer_document_styles
        ? stylesheet_build_context_deferred(
              &stylesheet, &budget, &document, &viewport)
        : stylesheet_build_context(
              &stylesheet, &budget, &document, &viewport);
    if (!stylesheet_built) {
        css_ms = now_ms() - phase_ms;
        fprintf(stderr, "stylesheet construction failed\n"); goto cleanup;
    }
    char profile_path[1024];
    if (adaptive_resources && fetch_external_css
        && budget_pressure_required(&budget, 0, 6 * MIB)) {
        size_t old_stylesheets = max_stylesheets;
        size_t old_css_kb = max_css_kb;
        size_t old_css_file_kb = max_css_file_kb;
        max_stylesheets = minimum_size(max_stylesheets, 2);
        max_css_kb = minimum_size(max_css_kb, 256);
        max_css_file_kb = minimum_size(max_css_file_kb, 192);
        adaptive_css_reduced = old_stylesheets != max_stylesheets
                               || old_css_kb != max_css_kb
                               || old_css_file_kb != max_css_file_kb;
        if (adaptive_css_reduced) {
            size_t avoided = (old_css_kb - max_css_kb) * KIB;
            budget_record_pressure(&budget, BUDGET_PRESSURE_STYLESHEET,
                                   avoided, 0);
            printf("adaptive-degradation stage=stylesheets action=cap "
                   "reason=layout-reserve remaining=%zu count=%zu "
                   "total-kb=%zu file-kb=%zu avoided-bytes=%zu\n",
                   budget_remaining(&budget), max_stylesheets, max_css_kb,
                   max_css_file_kb, avoided);
        }
    }
    if (fetch_external_css) {
        if (!network_input) {
            printf("external stylesheets skipped: fixture has no network base URL\n");
        } else if (!stylesheets_load_external_with_context(
                       &document, &stylesheet, &budget, resource_base_url,
                       locator, document_policy,
                       max_stylesheets, max_css_kb * KIB,
                       max_css_file_kb * KIB, resource_stage_ms, NULL, NULL,
                       &external_css)) {
            css_ms = now_ms() - phase_ms;
            fprintf(stderr, "external stylesheet parsing exceeded the budget\n");
            goto cleanup;
        }
        printf("external-css discovered=%zu attempted=%zu loaded=%zu failed=%zu "
               "skipped-limit=%zu skipped-media=%zu duplicate=%zu cache-hits=%zu "
               "imports=%zu/%zu import-condition-skips=%zu import-depth-skips=%zu bytes=%zu "
               "rules=%zu variables=%zu batches=%zu first-batch=%zu "
               "deadline-cancelled=%zu deadline-exceeded=%s elapsed-ms=%llu\n",
               external_css.discovered, external_css.attempted,
               external_css.loaded, external_css.failed,
               external_css.skipped_limit, external_css.skipped_media,
               external_css.duplicate, external_css.cache_hits,
               external_css.imports_loaded,
               external_css.imports_discovered,
               external_css.imports_skipped_conditions,
               external_css.imports_skipped_depth,
               external_css.bytes,
               external_css.rules_added, external_css.variables_added,
               external_css.batches, external_css.first_batch_loaded,
               external_css.deadline_cancelled,
               external_css.deadline_exceeded ? "yes" : "no",
               (unsigned long long) external_css.elapsed_ms);
        printf("external-css-responsiveness work=%zu yields=%zu "
               "max-slice-us=%llu max-slice-work=%zu\n",
               external_css.work_units, external_css.cooperative_yields,
               (unsigned long long) external_css.max_slice_us,
               external_css.max_slice_work_units);
    }
    const char *selected_profile = reader_profile_path(reader_profile, locator,
                                                        document.title,
                                                        profile_path,
                                                        sizeof(profile_path));
    if (selected_profile != NULL) {
        size_t profile_css_length = 0;
        profile_css = read_file(&budget, selected_profile, &profile_css_length);
        if (profile_css == NULL
            || !stylesheet_add_user_css(&stylesheet, profile_css,
                                        profile_css_length)) {
            css_ms = now_ms() - phase_ms;
            fprintf(stderr, "reader profile failed: %s\n", selected_profile);
            goto cleanup;
        }
        const char *profile_name = strstr(selected_profile, "wikipedia") != NULL
                                   ? "wikipedia"
                                   : (strstr(selected_profile, "chatgpt") != NULL
                                      ? "chatgpt"
                                      : (strstr(selected_profile, "nytimes") != NULL
                                         ? "nytimes"
                                         : (strstr(selected_profile,
                                                   "hacker-news") != NULL
                                            ? "hacker-news" : "reddit")));
        printf("reader profile=%s stylesheet=\"%s\"\n",
               profile_name, selected_profile);
    } else {
        printf("reader profile=none\n");
    }
    if (user_css_path != NULL) {
        size_t user_css_length = 0;
        user_css = read_file(&budget, user_css_path, &user_css_length);
        if (user_css == NULL
            || !stylesheet_add_user_css(&stylesheet, user_css,
                                        user_css_length)) {
            css_ms = now_ms() - phase_ms;
            fprintf(stderr, "user stylesheet failed: %s\n", user_css_path);
            goto cleanup;
        }
    }
    if (adaptive_resources && fetch_external_images
        && budget_pressure_required(&budget, 0, 4 * MIB)) {
        size_t old_images = max_images;
        size_t old_image_kb = max_image_kb;
        size_t old_image_file_kb = max_image_file_kb;
        size_t old_decoded_image_kb = max_decoded_image_kb;
        max_images = minimum_size(max_images, 8);
        max_image_kb = minimum_size(max_image_kb, 128);
        max_image_file_kb = minimum_size(max_image_file_kb, 64);
        max_decoded_image_kb = minimum_size(max_decoded_image_kb, 128);
        adaptive_images_reduced = old_images != max_images
                                  || old_image_kb != max_image_kb
                                  || old_image_file_kb != max_image_file_kb
                                  || old_decoded_image_kb
                                     != max_decoded_image_kb;
        if (adaptive_images_reduced) {
            size_t avoided = (old_image_kb - max_image_kb) * KIB;
            size_t decoded_difference = old_decoded_image_kb
                                        - max_decoded_image_kb;
            if (decoded_difference <= (SIZE_MAX - avoided) / KIB) {
                avoided += decoded_difference * KIB;
            } else {
                avoided = SIZE_MAX;
            }
            budget_record_pressure(&budget, BUDGET_PRESSURE_IMAGE,
                                   avoided, 0);
            printf("adaptive-degradation stage=images action=cap "
                   "reason=layout-render-reserve remaining=%zu count=%zu "
                   "encoded-kb=%zu file-kb=%zu decoded-kb=%zu "
                   "avoided-bytes=%zu\n", budget_remaining(&budget),
                   max_images, max_image_kb, max_image_file_kb,
                   max_decoded_image_kb, avoided);
        }
    }
    if (fetch_external_images) {
        if (!network_input) {
            printf("external images skipped: fixture has no network base URL\n");
        } else if (!images_load_external(
                       &document, &stylesheet, &images, &budget,
                       resource_base_url, locator,
                       document_policy,
                       max_images,
                       max_image_kb * KIB, max_image_file_kb * KIB,
                       max_decoded_image_kb * KIB, resource_stage_ms, NULL,
                       NULL)) {
            css_ms = now_ms() - phase_ms;
            fprintf(stderr, "external image loading exceeded the budget\n");
            goto cleanup;
        }
        printf("external-images discovered=%zu attempted=%zu loaded=%zu "
               "failed=%zu unsupported=%zu skipped-limit=%zu duplicate=%zu "
               "cache-hits=%zu "
               "encoded=%zu decoded=%zu downsampled=%zu source-peak=%zu "
               "target-peak=%zu masks=%zu backgrounds=%zu "
               "deadline-cancelled=%zu deadline-exceeded=%s elapsed-ms=%llu\n",
               images.stats.discovered, images.stats.attempted,
               images.stats.loaded, images.stats.failed,
               images.stats.unsupported, images.stats.skipped_limit,
               images.stats.duplicate, images.stats.cache_hits,
               images.stats.encoded_bytes,
               images.stats.decoded_bytes, images.stats.downsampled,
               images.stats.largest_source_decode_bytes,
               images.stats.largest_target_decode_bytes,
               images.stats.masks_loaded, images.stats.backgrounds_loaded,
               images.stats.deadline_cancelled,
               images.stats.deadline_exceeded ? "yes" : "no",
               (unsigned long long) images.stats.elapsed_ms);
        printf("image-network fetch-failures=%zu/%zu/%zu/%zu/%zu/%zu "
               "progress=%zu/%zu/%zu stalled-polls=%zu "
               "no-progress-cancelled=%zu origin-cooldown=%zu/%zu "
               "max-no-progress-ms=%llu max-request-ms=%llu\n",
               images.stats.fetch_failures_http_4xx,
               images.stats.fetch_failures_http_5xx,
               images.stats.fetch_failures_timeout,
               images.stats.fetch_failures_cancelled,
               images.stats.fetch_failures_quota,
               images.stats.fetch_failures_transport,
               images.stats.progress_samples,
               images.stats.progress_events,
               images.stats.progress_bytes,
               images.stats.stalled_polls,
               images.stats.no_progress_cancelled,
               images.stats.no_progress_origin_cooldowns,
               images.stats.no_progress_origin_skipped,
               (unsigned long long)
                 images.stats.maximum_no_progress_ms,
               (unsigned long long) images.stats.maximum_request_ms);
        printf("external-image-responsiveness work=%zu yields=%zu "
               "max-slice-us=%llu max-slice-work=%zu\n",
               images.stats.work_units,
               images.stats.cooperative_yields,
               (unsigned long long) images.stats.max_slice_us,
               images.stats.max_slice_work_units);
    }
    css_ms = now_ms() - phase_ms;
    budget_report(&budget, "CSS", stdout);
    phase_ms = now_ms();
    if (!layout_build_context(
            &layout, &budget, &document, &stylesheet,
            (fonts.sans.loaded || fonts.serif.loaded) ? &fonts : NULL,
            fetch_external_images ? &images : NULL,
            &viewport)) {
        layout_ms = now_ms() - phase_ms;
        fprintf(stderr, "layout failed\n"); goto cleanup;
    }
    layout_ms = now_ms() - phase_ms;
    budget_report(&budget, "layout", stdout);
    if (layout_path != NULL) {
        FILE *layout_file = fopen(layout_path, "w");
        if (layout_file == NULL) {
            fprintf(stderr, "could not write layout dump: %s\n", layout_path);
            goto cleanup;
        }
        fprintf(layout_file, "type\tx\ty\twidth\theight\tcolor\tfont_size\tfamily\tbold\ttext\n");
        for (size_t i = 0; i < layout.count; i++) {
            const DrawCommand *command = &layout.commands[i];
            const char *type = command->type == DRAW_TEXT ? "text"
                               : (command->type == DRAW_IMAGE ? "image"
                                  : (command->type == DRAW_STROKE_RECT
                                     ? "stroke"
                                     : (command->type == DRAW_SHADOW_RECT
                                        ? "shadow" : "fill")));
            fprintf(layout_file, "%s\t%d\t%d\t%d\t%d\t%06x\t%d\t%d\t%d\t",
                    type,
                    command->x, command->y, command->width, command->height,
                    command->color, command->font_size,
                    (int) draw_command_font_family(command),
                    draw_command_font_weight_code(command) >= 60 ? 1 : 0);
            if (command->text != NULL) {
                fprintf(layout_file, "%.*s", (int) command->text_length,
                        command->text);
            }
            fputc('\n', layout_file);
        }
        fclose(layout_file);
    }
    if (links_path != NULL) {
        FILE *links_file = fopen(links_path, "w");
        if (links_file == NULL) {
            fprintf(stderr, "could not write link map: %s\n", links_path);
            goto cleanup;
        }
        fprintf(links_file, "x\ty\twidth\theight\turl\n");
        for (size_t i = 0; i < layout.link_count; i++) {
            const LinkRegion *link = &layout.links[i];
            char reference[2048];
            char resolved[4096];
            const char *output_url = NULL;
            if (link->url_length < sizeof(reference)) {
                memcpy(reference, link->url, link->url_length);
                reference[link->url_length] = '\0';
                output_url = reference;
                if (network_input
                    && fetch_resolve_url(locator, reference, resolved,
                                         sizeof(resolved))) {
                    output_url = resolved;
                }
            }
            fprintf(links_file, "%d\t%d\t%d\t%d\t",
                    link->x, link->y, link->width, link->height);
            if (output_url != NULL) fputs(output_url, links_file);
            else fprintf(links_file, "%.*s", (int) link->url_length, link->url);
            fputc('\n', links_file);
        }
        fclose(links_file);
    }
    if (render_frames) {
        if (mkdir(output_dir, 0755) != 0 && errno != EEXIST) {
            fprintf(stderr, "could not create %s\n", output_dir); goto cleanup;
        }
        if (adaptive_resources && tile_capacity > 4
            && budget_pressure_required(&budget, 0, 3 * MIB)) {
            size_t old_tile_capacity = tile_capacity;
            tile_capacity = 4;
            adaptive_tiles_reduced = true;
            size_t avoided = (old_tile_capacity - tile_capacity)
                             * sizeof(RenderTile);
            budget_record_pressure(&budget, BUDGET_PRESSURE_TILE,
                                   avoided, 0);
            printf("adaptive-degradation stage=tiles action=cap "
                   "reason=frame-reserve remaining=%zu count=%zu "
                   "avoided-bytes=%zu\n", budget_remaining(&budget),
                   tile_capacity, avoided);
        }
        bool cache_ready = tile_cache_init(
            &cache, &budget, &layout, tile_capacity);
        while (!cache_ready && adaptive_resources && tile_capacity > 1) {
            size_t old_tile_capacity = tile_capacity;
            tile_capacity /= 2;
            size_t avoided = (old_tile_capacity - tile_capacity)
                             * sizeof(RenderTile);
            adaptive_tiles_reduced = true;
            budget_record_pressure(&budget, BUDGET_PRESSURE_TILE,
                                   avoided, 0);
            printf("adaptive-degradation stage=tiles action=cap "
                   "reason=allocation-retry remaining=%zu count=%zu "
                   "avoided-bytes=%zu\n", budget_remaining(&budget),
                   tile_capacity, avoided);
            cache_ready = tile_cache_init(
                &cache, &budget, &layout, tile_capacity);
        }
        if (!cache_ready
            || !tile_cache_set_frame(&cache, frame,
                                     viewport_width * viewport_height)) {
            fprintf(stderr, "tile cache allocation failed\n"); goto cleanup;
        }
        budget_report(&budget, "tile cache", stdout);

        phase_ms = now_ms();
        int max_scroll = viewport_max_scroll_css(&layout.viewport,
                                                 layout.height);
        size_t frame_pixels = viewport_width * viewport_height;
        uint16_t background = color_rgb565(layout.page_background);
        if (scroll_all) {
            char manifest_path[1024];
            int manifest_written = snprintf(manifest_path,
                                            sizeof(manifest_path),
                                            "%s/scroll-manifest.tsv",
                                            output_dir);
            FILE *manifest = manifest_written > 0
                             && (size_t) manifest_written < sizeof(manifest_path)
                             ? fopen(manifest_path, "w") : NULL;
            if (manifest == NULL) {
                fprintf(stderr, "could not create scroll manifest\n");
                goto cleanup;
            }
            fprintf(manifest, "frame\tscroll_y\thash_fnv1a64\t"
                    "non_background\tsaved\n");
            /* Harness-only revisit reference; plain malloc keeps it out of
               the engine's budget accounting. */
            scroll_reference = malloc(frame_pixels * sizeof(*frame));
            if (scroll_reference == NULL) {
                fprintf(stderr, "could not allocate revisit reference\n");
                fclose(manifest); goto cleanup;
            }
            int step = layout.viewport.css_height / 2;
            if (step < 1) step = 1;
            bool middle_saved = false;
            for (size_t i = 0;; i++) {
                int64_t candidate = (int64_t) i * step;
                int scroll = candidate < max_scroll
                             ? (int) candidate : max_scroll;
                bool save = i == 0 || scroll == max_scroll
                            || (!middle_saved && scroll >= max_scroll / 2);
                char path[1024];
                const char *output_path = NULL;
                if (save) {
                    int written = snprintf(path, sizeof(path),
                                           "%s/frame_%05zu_y%05d.ppm",
                                           output_dir, i, scroll);
                    if (written < 0 || (size_t) written >= sizeof(path)) {
                        fclose(manifest); goto cleanup;
                    }
                    output_path = path;
                    if (i != 0 && scroll != max_scroll) middle_saved = true;
                }
                if (!tile_cache_render_frame(&cache, scroll,
                                             (int) viewport_width,
                                             (int) viewport_height,
                                             output_path)) {
                    fprintf(stderr, "full-scroll frame %zu failed\n", i);
                    fclose(manifest); goto cleanup;
                }
                if (i == 0) {
                    memcpy(scroll_reference, frame,
                           frame_pixels * sizeof(*frame));
                }
                if (frame_hashes) {
                    size_t non_background = 0;
                    uint64_t hash = frame_hash_and_count(
                        frame, frame_pixels, background, &non_background);
                    if (non_background == 0) blank_frames++;
                    fprintf(manifest, "%zu\t%d\t%016llx\t%zu\t%s\n", i,
                            scroll, (unsigned long long) hash,
                            non_background, save ? "yes" : "no");
                } else {
                    if (frame_is_blank(frame, frame_pixels, background)) {
                        blank_frames++;
                    }
                    fprintf(manifest, "%zu\t%d\t-\t-\t%s\n", i, scroll,
                            save ? "yes" : "no");
                }
                scroll_frames++;
                int prefetch_y = scroll + layout.viewport.css_height;
                if (prefetch_y < layout.height) {
                    tile_cache_prefetch_row(&cache, prefetch_y,
                                            (int) viewport_width);
                }
                if (scroll == max_scroll) break;
            }
            for (size_t i = 0; i < save_scroll_count; i++) {
                int scroll = save_scrolls[i] < max_scroll
                             ? save_scrolls[i] : max_scroll;
                char path[1024];
                int written = snprintf(path, sizeof(path),
                                       "%s/sample_%02zu_y%05d.ppm",
                                       output_dir, i, scroll);
                if (written < 0 || (size_t) written >= sizeof(path)
                    || !tile_cache_render_frame(&cache, scroll,
                                                (int) viewport_width,
                                                (int) viewport_height,
                                                path)) {
                    fprintf(stderr, "saved scroll frame %zu failed\n", i);
                    fclose(manifest); goto cleanup;
                }
                if (frame_hashes) {
                    size_t non_background = 0;
                    uint64_t hash = frame_hash_and_count(
                        frame, frame_pixels, background, &non_background);
                    if (non_background == 0) blank_frames++;
                    fprintf(manifest,
                            "sample%zu\t%d\t%016llx\t%zu\tyes\n",
                            i, scroll, (unsigned long long) hash,
                            non_background);
                } else {
                    if (frame_is_blank(frame, frame_pixels, background)) {
                        blank_frames++;
                    }
                    fprintf(manifest, "sample%zu\t%d\t-\t-\tyes\n",
                            i, scroll);
                }
                scroll_frames++;
            }
            if (!tile_cache_render_frame(&cache, 0, (int) viewport_width,
                                         (int) viewport_height, NULL)) {
                fprintf(stderr, "full-scroll revisit failed\n");
                fclose(manifest); goto cleanup;
            }
            /* Full-buffer comparison is both cheaper and stronger than the
               former hash equality (no collision window). */
            scroll_revisit_match = memcmp(scroll_reference, frame,
                                          frame_pixels
                                          * sizeof(*frame)) == 0;
            if (frame_hashes) {
                size_t non_background = 0;
                uint64_t revisit_hash = frame_hash_and_count(
                    frame, frame_pixels, background, &non_background);
                fprintf(manifest, "revisit\t0\t%016llx\t%zu\tno\n",
                        (unsigned long long) revisit_hash, non_background);
            } else {
                fprintf(manifest, "revisit\t0\t-\t-\tno\n");
            }
            scroll_frames++;
            if (fclose(manifest) != 0 || !scroll_revisit_match
                || blank_frames != 0) {
                fprintf(stderr, "full-scroll validation failed: blanks=%zu "
                        "revisit=%s\n", blank_frames,
                        scroll_revisit_match ? "match" : "mismatch");
                goto cleanup;
            }
        } else {
            int requested_visual[] = {0, 136, 272, 408, 544, 272, 0};
            for (size_t i = 0;
                 i < sizeof(requested_visual) / sizeof(requested_visual[0]);
                 i++) {
                int requested = viewport_device_to_css(
                    &layout.viewport, requested_visual[i]);
                int scroll = requested < max_scroll ? requested : max_scroll;
                char path[1024];
                int written = snprintf(path, sizeof(path),
                                       "%s/frame_%02zu_y%04d.ppm",
                                       output_dir, i, scroll);
                if (written < 0 || (size_t) written >= sizeof(path)
                    || !tile_cache_render_frame(&cache, scroll,
                                                (int) viewport_width,
                                                (int) viewport_height, path)) {
                    fprintf(stderr, "frame %zu failed\n", i); goto cleanup;
                }
                scroll_frames++;
                int prefetch_y = scroll + layout.viewport.css_height;
                if (prefetch_y < layout.height) {
                    tile_cache_prefetch_row(&cache, prefetch_y,
                                            (int) viewport_width);
                }
            }
            for (size_t i = 0; i < save_scroll_count; i++) {
                int scroll = save_scrolls[i] < max_scroll
                             ? save_scrolls[i] : max_scroll;
                char path[1024];
                int written = snprintf(path, sizeof(path),
                                       "%s/sample_%02zu_y%05d.ppm",
                                       output_dir, i, scroll);
                if (written < 0 || (size_t) written >= sizeof(path)
                    || !tile_cache_render_frame(&cache, scroll,
                                                (int) viewport_width,
                                                (int) viewport_height,
                                                path)) {
                    fprintf(stderr, "saved scroll frame %zu failed\n", i);
                    goto cleanup;
                }
                if (frame_is_blank(frame, frame_pixels, background)) {
                    blank_frames++;
                }
                scroll_frames++;
            }
        }
        render_ms = now_ms() - phase_ms;
        budget_report(&budget, "frames", stdout);
    } else {
        printf("rendering skipped by analysis policy\n");
    }
    budget_report_categories(&budget, "stable-page", stdout);
    printf("document title=\"%s\" nodes=%zu elements=%zu text-nodes=%zu "
           "attributes=%zu attribute-bytes=%zu body-text=%zu bytes\n",
           document.title, document.node_count, document.element_count,
           document.text_node_count, document.attribute_count,
           document.attribute_value_bytes, document.text_bytes);
    printf("dom-struct-size node=%zu element=%zu attr=%zu draw=%zu "
           "node-box=%zu style-rule=%zu computed-style=%zu\n",
           sizeof(lxb_dom_node_t), sizeof(lxb_dom_element_t),
           sizeof(lxb_dom_attr_t), sizeof(DrawCommand),
           sizeof(LayoutNodeBox), sizeof(StyleRule), sizeof(ComputedStyle));
    printf("mobile-viewport declared=%s device-width=%s layout=%dx%d "
           "device=%zux%zu scale=%d/%d\n",
           viewport.declared ? "yes" : "no",
           viewport.device_width_declared ? "yes" : "no",
           viewport.css_width, viewport.css_height,
           viewport_width, viewport_height,
           viewport.scale_numerator,
           viewport.scale_denominator);
    printf("layout width=%d scroll-width=%d height=%d commands=%zu links=%zu sticky=%zu fixed=%zu\n",
           layout.width, layout.scroll_width, layout.height, layout.count, layout.link_count,
           layout.sticky_count, layout.fixed_count);
    size_t layout_retained_bytes =
        layout.capacity * sizeof(*layout.commands)
        + layout.link_capacity * sizeof(*layout.links)
        + layout.control_capacity * sizeof(*layout.controls)
        + layout.sticky_capacity * sizeof(*layout.sticky_ranges)
        + layout.fixed_capacity * sizeof(*layout.fixed_ranges)
        + layout.node_box_capacity * sizeof(*layout.node_boxes)
        + layout.paint_order_count * sizeof(*layout.paint_order)
        + layout.count * sizeof(*layout.command_flags)
        + (layout.spatial_band_count + 1) * sizeof(*layout.spatial_band_offsets)
        + layout.spatial_band_order_count * sizeof(*layout.spatial_band_orders)
        + layout.spatial_global_count * sizeof(*layout.spatial_global_orders)
        + layout.overflow_order_count * sizeof(*layout.overflow_orders);
    printf("layout-retained bytes=%zu command-size=%zu command-capacity=%zu "
           "node-box-size=%zu node-boxes=%zu/%zu links=%zu/%zu "
           "controls=%zu/%zu paint-order=%zu spatial-orders=%zu/%zu/%zu\n",
           layout_retained_bytes, sizeof(*layout.commands), layout.capacity,
           sizeof(*layout.node_boxes), layout.node_box_count,
           layout.node_box_capacity, layout.link_count, layout.link_capacity,
           layout.control_count, layout.control_capacity,
           layout.paint_order_count, layout.spatial_band_order_count,
           layout.spatial_global_count, layout.overflow_order_count);
    printf("layout-responsiveness work=%zu yields=%zu max-slice-us=%llu "
           "max-slice-work=%zu\n",
           layout.layout_work_units, layout.cooperative_yields,
           (unsigned long long) layout.max_work_slice_us,
           layout.max_work_slice_units);
    for (size_t i = 0; i < layout.fixed_count; i++) {
        const FixedRange *range = &layout.fixed_ranges[i];
        printf("layout fixed[%zu] commands=%zu..%zu origin_y=%d height=%d %s=%d\n",
               i, range->command_start, range->command_end, range->origin_y,
               range->height, range->from_bottom ? "bottom" : "top",
               range->inset);
    }
    printf("stylesheet rules=%zu important-rules=%zu layers=%zu variables=%zu scoped-variables=%zu "
           "generated-text=%zu/%zu deferred-bytes=%zu external-loaded=%zu external-imports=%zu/%zu "
           "external-bytes=%zu\n",
           stylesheet.count, stylesheet.important_rule_count,
           stylesheet.layer_count,
           stylesheet.variable_count,
           stylesheet.custom_rule_count, stylesheet.generated_text_count,
           stylesheet.generated_text_bytes, stylesheet.deferred_bytes,
           external_css.loaded, external_css.imports_loaded,
           external_css.imports_discovered, external_css.bytes);
    printf("deferred-program instructions=%zu/%zu bytes=%zu "
           "executions=%llu fallbacks=%llu executed-instructions=%llu\n",
           stylesheet.deferred_instruction_count,
           stylesheet.deferred_instruction_capacity,
           stylesheet.deferred_program_bytes,
           (unsigned long long) stylesheet.deferred_program_executions,
           (unsigned long long) stylesheet.deferred_program_fallbacks,
           (unsigned long long) stylesheet.deferred_program_instructions);
    printf("stylesheet-diagnostics declarations=%llu supported=%llu "
           "rejected=%llu deferred=%llu custom-drops=%llu "
           "selector-drops=%llu unknown-media=%llu supports-false=%llu "
           "rejected-properties=",
           (unsigned long long) stylesheet.diagnostic_declarations,
           (unsigned long long)
             stylesheet.diagnostic_supported_declarations,
           (unsigned long long) stylesheet.diagnostic_rejected_declarations,
           (unsigned long long) stylesheet.diagnostic_deferred_declarations,
           (unsigned long long) stylesheet.diagnostic_custom_property_drops,
           (unsigned long long) stylesheet.diagnostic_selector_drops,
           (unsigned long long)
             stylesheet.diagnostic_unknown_media_features,
           (unsigned long long)
             stylesheet.diagnostic_supports_false_queries);
    if (stylesheet.diagnostic_rejected_property_count == 0) putchar('-');
    for (size_t i = 0;
         i < stylesheet.diagnostic_rejected_property_count; i++) {
        const StyleDiagnosticProperty *entry =
            &stylesheet.diagnostic_rejected_properties[i];
        printf("%s%s:%u", i == 0 ? "" : ",", entry->name, entry->count);
    }
    putchar('\n');
    printf("style-retained bytes=%zu rule-size=%zu rules=%zu/%zu "
           "declarations=%zu/%zu declaration-size=%zu "
           "selector-bytes=%zu/%zu "
           "variables=%zu/%zu custom=%zu/%zu generated-capacity=%zu "
           "image-urls=%zu/%zu image-url-bytes=%zu index-bytes=%zu "
           "index-queries=%llu index-candidates=%llu index-fallbacks=%llu\n",
           stylesheet_retained_bytes(&stylesheet),
           sizeof(*stylesheet.rules), stylesheet.count, stylesheet.capacity,
           stylesheet.declaration_count, stylesheet.declaration_capacity,
           sizeof(*stylesheet.declarations), stylesheet.selector_bytes,
           stylesheet.selector_storage_bytes,
           stylesheet.variable_count, stylesheet.variable_capacity,
           stylesheet.custom_rule_count, stylesheet.custom_rule_capacity,
           stylesheet.generated_text_capacity,
           stylesheet.image_url_count, stylesheet.image_url_capacity,
           stylesheet.image_url_bytes, stylesheet.rule_index_bytes,
           (unsigned long long) stylesheet.rule_index_queries,
           (unsigned long long) stylesheet.rule_index_candidates,
           (unsigned long long) stylesheet.rule_index_fallbacks);
    printf("javascript scripts=%zu failed=%zu external-skipped=%zu external-loaded=%zu "
           "external-failed=%zu external-bytes=%zu dom-ready=%s "
           "form-submit=%s mutations=%zu events=%zu batched-relayout=%s "
           "ticks=%zu callbacks=%zu pending=%zu summary=\"%s\"\n",
           scripts.scripts_evaluated, scripts.scripts_failed,
           scripts.external_scripts_skipped,
           scripts.external_scripts_loaded, scripts.external_scripts_failed,
           scripts.external_script_bytes,
           scripts.dom_content_loaded_dispatched ? "yes" : "no",
           scripts.form_submission_requested ? "yes" : "no",
           scripts.dom_mutations, scripts.events_dispatched,
           scripts.relayout_required ? "yes" : "no",
           scripts.runtime_ticks, scripts.timer_callbacks_run,
           scripts.pending_tasks, scripts.summary);
    printf("javascript-external-bytecode hits=%zu misses=%zu stores=%zu "
           "admission-skips=%zu restore-failures=%zu restored-bytes=%zu\n",
           scripts.external_script_bytecode_cache_hits,
           scripts.external_script_bytecode_cache_misses,
           scripts.external_script_bytecode_cache_stores,
           scripts.external_script_bytecode_cache_admission_skips,
           scripts.external_script_bytecode_cache_restore_failures,
           scripts.external_script_bytecode_cache_bytes);
    printf("javascript-dom-handles live=%zu peak=%zu high-water=%zu "
           "reuses=%zu exhaustions=%zu wrapper-releases=%zu "
           "connected-preserves=%zu stale-releases=%zu capacity=%u\n",
           scripts.dom_handle_slots_live, scripts.dom_handle_slots_peak,
           scripts.dom_handle_slots_high_water,
           scripts.dom_handle_slot_reuses, scripts.dom_handle_exhaustions,
           scripts.dom_handle_wrapper_releases,
           scripts.dom_handle_connected_preserves,
           scripts.dom_handle_stale_releases,
           SCRIPT_DOM_HANDLE_SLOT_CAPACITY);
    printf("javascript-geometry queries=%zu retained-fast-paths=%zu "
           "ancestor-visits=%zu synchronous-layouts=%zu\n",
           scripts.geometry_queries, scripts.geometry_retained_fast_paths,
           scripts.geometry_ancestor_visits,
           scripts.geometry_synchronous_layouts);
    printf("javascript-responsiveness work=%zu yields=%zu max-slice-us=%llu "
           "max-slice-work=%zu nonpreemptible-compiles=%zu "
           "max-compile-us=%llu max-compile-bytes=%zu "
           "compile-attempts=%zu compile-source-bytes=%zu "
           "compile-rejections=%zu rejected-bytes=%zu compile-limit=%zu "
           "last-admission=%u last-kind=%u last-bytes=%zu max-kind=%u "
           "nonpreemptible-callbacks=%zu max-callback-us=%llu "
           "host-callbacks=%zu callback-polls=%zu callback-total-us=%llu "
           "max-callback-name=\"%.95s\"\n",
           scripts.script_work_units, scripts.script_cooperative_yields,
           (unsigned long long) scripts.script_max_slice_us,
           scripts.script_max_slice_work_units,
           scripts.nonpreemptible_compile_count,
           (unsigned long long) scripts.max_nonpreemptible_compile_us,
           scripts.max_nonpreemptible_compile_bytes,
           scripts.host_compile_attempts,
           scripts.host_compile_source_bytes,
           scripts.host_compile_rejections,
           scripts.host_compile_rejected_source_bytes,
           scripts.host_compile_source_limit_bytes,
           (unsigned) scripts.last_compile_admission,
           (unsigned) scripts.last_compile_source_kind,
           scripts.last_compile_source_bytes,
           (unsigned) scripts.max_nonpreemptible_compile_source_kind,
           scripts.nonpreemptible_callback_count,
           (unsigned long long) scripts.max_nonpreemptible_callback_us,
           scripts.host_callback_calls,
           scripts.host_callback_calls_with_interrupt_polls,
           (unsigned long long) scripts.host_callback_total_us,
           scripts.max_nonpreemptible_callback_name);
    printf("tiles capacity=%zu bytes=%zu hits=%zu misses=%zu evictions=%zu "
           "rasterized=%zu raster-us=%llu max-raster-us=%llu "
           "frame-us=%llu max-frame-us=%llu frame-io-us=%llu "
           "candidates=%llu spatial-bands=%zu "
           "glyphs=%zu/%zu glyph-hits=%zu glyph-misses=%zu "
           "glyph-evictions=%zu scaled-image-hits=%zu builds=%zu\n",
           cache.tile_capacity, cache.tile_capacity * sizeof(RenderTile),
           cache.hits, cache.misses, cache.evictions, cache.rasterized,
           (unsigned long long) cache.raster_us,
           (unsigned long long) cache.max_raster_us,
           (unsigned long long) cache.frame_us,
           (unsigned long long) cache.max_frame_us,
           (unsigned long long) cache.frame_io_us,
           (unsigned long long) cache.command_candidates,
           layout.spatial_band_count, cache.glyph_cache_count,
           cache.glyph_cache_bytes, cache.glyph_cache_hits,
           cache.glyph_cache_misses, cache.glyph_cache_evictions,
           cache.scaled_image_hits, cache.scaled_image_builds);
    printf("fixed-cache builds=%zu blits=%zu pixels=%zu bytes=%zu "
           "bounds=%dx%d invalidations=%zu\n",
           cache.fixed_cache_builds, cache.fixed_cache_blits,
           cache.fixed_cache_pixels, cache.fixed_cache_bytes,
           cache.fixed_width, cache.fixed_height, cache.invalidations);
    printf("scroll mode=%s frames=%zu blank=%zu revisit=%s\n",
           scroll_all ? "full" : "sample", scroll_frames, blank_frames,
           scroll_revisit_match ? "match" : "mismatch");
    printf("adaptive-degradation enabled=%s active=%s javascript=%s "
           "stylesheets=%s images=%s tiles=%s\n",
           adaptive_resources ? "yes" : "no",
           adaptive_javascript_skipped || adaptive_css_reduced
             || adaptive_images_reduced || adaptive_tiles_reduced
             ? "yes" : "no",
           adaptive_javascript_skipped ? "skipped" : "unchanged",
           adaptive_css_reduced ? "reduced" : "unchanged",
           adaptive_images_reduced ? "reduced" : "unchanged",
           adaptive_tiles_reduced ? "reduced" : "unchanged");
    budget_report_pressure(&budget, stdout);
    status = 0;

cleanup:;
    document_parser_abort(&network_parser);
    section_route_stream_abort(&section_router);
    section_store_stream_abort(&section_stream_builder);
    size_t measured_nodes = document.node_count;
    size_t measured_elements = document.element_count;
    int measured_height = layout.height;
    size_t measured_commands = layout.count;
    size_t measured_hits = cache.hits;
    size_t measured_misses = cache.misses;
    size_t measured_evictions = cache.evictions;
    uint64_t measured_candidates = cache.command_candidates;
    size_t measured_spatial_bands = layout.spatial_band_count;
    size_t measured_glyph_hits = cache.glyph_cache_hits;
    size_t measured_glyph_misses = cache.glyph_cache_misses;
    size_t measured_scaled_hits = cache.scaled_image_hits;
    size_t measured_scaled_builds = cache.scaled_image_builds;
    size_t measured_font_bytes = fonts.sans.data_length
                                 + fonts.serif.data_length
                                 + fonts.sans_italic.data_length
                                 + fonts.sans_bold.data_length
                                 + fonts.serif_bold.data_length;
    ExternalImageStats measured_images = images.stats;
    size_t measured_css_loaded = external_css.loaded;
    size_t measured_css_bytes = external_css.bytes;
    tile_cache_destroy(&cache);
    layout_destroy(&layout);
    images_destroy(&images);
    font_set_destroy(&fonts);
    stylesheet_destroy(&stylesheet);
    document_destroy(&document);
    budget_free(&budget, user_css);
    budget_free(&budget, profile_css);
    if (html_is_extracted_fragment) budget_free(&budget, html);
    else if (!network_input) budget_free(&budget, html);
    if (network_input) fetch_result_destroy(&fetch);
    section_store_destroy(&section_store);
    free(scroll_reference);
    budget_free(&budget, frame);
    budget_free(&budget, application);
    fetch_trace_end();
    budget_report(&budget, "teardown", stdout);
    budget_report_categories(&budget, "teardown", stdout);
    if (budget.current != 0) {
        fprintf(stderr, "owned-memory leak: %zu bytes remain\n", budget.current);
        budget_dump_active(&budget, stderr, 32);
        status = 1;
    }
    if (budget.current == 0 && !budget_uninstall_lexbor(&budget)) {
        fprintf(stderr, "Lexbor allocator owner did not release cleanly\n");
        status = 1;
    }
    double total_ms = now_ms() - started_ms;
    printf("timing input_ms=%.3f parse_ms=%.3f css_ms=%.3f layout_ms=%.3f "
           "javascript_ms=%.3f render_ms=%.3f total_ms=%.3f\n",
           input_ms, parse_ms, css_ms, layout_ms, javascript_ms, render_ms,
           total_ms);
    printf("benchmark status=%s input_bytes=%zu peak_bytes=%zu nodes=%zu "
           "elements=%zu page_height=%d commands=%zu tile_hits=%zu "
           "tile_misses=%zu tile_evictions=%zu command_candidates=%llu "
           "spatial_bands=%zu glyph_hits=%zu glyph_misses=%zu "
           "scaled_image_hits=%zu scaled_image_builds=%zu javascript=%s "
           "viewport=%zux%zu css_loaded=%zu css_bytes=%zu rendered=%s "
           "scroll_mode=%s scroll_frames=%zu blank_frames=%zu revisit=%s\n",
           status == 0 ? "ok" : "failed", source_html_length, budget.peak,
           measured_nodes, measured_elements, measured_height,
           measured_commands, measured_hits, measured_misses,
           measured_evictions, (unsigned long long) measured_candidates,
           measured_spatial_bands, measured_glyph_hits,
           measured_glyph_misses, measured_scaled_hits,
           measured_scaled_builds,
           run_javascript ? "strict"
             : (adaptive_javascript_skipped ? "adaptive-skipped"
                : external_script_boundary_skipped
                  ? "external-boundary-skipped" : "skipped"),
           viewport_width, viewport_height, measured_css_loaded,
           measured_css_bytes, render_frames ? "yes" : "no",
           scroll_all ? "full" : "sample", scroll_frames, blank_frames,
           scroll_revisit_match ? "match" : "mismatch");
    printf("font-benchmark loaded=%s bytes=%zu limit=%zu\n",
           measured_font_bytes != 0 ? "yes" : "no", measured_font_bytes,
           max_font_kb * KIB);
    printf("image-benchmark enabled=%s loaded=%zu masks=%zu backgrounds=%zu encoded=%zu "
           "decoded=%zu downsampled=%zu source-peak=%zu target-peak=%zu "
           "limit=%zu\n", fetch_external_images ? "yes" : "no",
           measured_images.loaded, measured_images.masks_loaded,
           measured_images.backgrounds_loaded,
           measured_images.encoded_bytes, measured_images.decoded_bytes,
           measured_images.downsampled,
           measured_images.largest_source_decode_bytes,
           measured_images.largest_target_decode_bytes,
           max_decoded_image_kb * KIB);
    return status;
}

#undef document
#undef network_parser
#undef stylesheet
#undef layout
#undef cache
#undef scripts
#undef fetch
#undef network_stream_metrics
#undef external_css
#undef images
#undef fonts
#undef section_store
#undef section_stream_builder
#undef section_router
