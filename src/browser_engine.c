#include "tilefinch/browser_engine.h"
#include "tilefinch/page_find.h"
#include "tilefinch/platform.h"
#include "tilefinch/site_adapter.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#define KIB (1024u)
#define MIB (1024u * 1024u)
#define BROWSER_PROVISIONAL_FRAME_LIMIT 2u
#define BROWSER_PROVISIONAL_FINAL_RESERVE (1u * MIB)
#define BROWSER_PROVISIONAL_GROW_MAX_ALLOC_US UINT64_C(100000)

typedef struct {
    NavigationLoad *load;
    SiteAdapterLoad *adapter;
    DocumentBacking previous_backing;
    BrowserNavigationJobStatus status;
    TilefinchDiagnosticSubsystem failure_subsystem;
    TilefinchDiagnosticCode failure_code;
    size_t previous_history_index;
    int restore_scroll_y;
    int restore_focus_kind;
    size_t restore_focus_index;
    BrowserNavigationJobMetrics metrics;
    uint64_t started_us;
    uint64_t generation;
    bool previous_backing_valid;
    bool hooks_installed;
    bool history_move;
    bool history_forward;
    bool record_history;
    char url[NAVIGATION_URL_LIMIT];
} BrowserEngineNavigationWork;

struct BrowserEngine {
    BrowserConfig config;
    Budget budget;
    BrowserSession session;
    ContentBlocker *content_blocker;
    NavigationSession navigation;
    BrowserController controller;
    BrowserController candidate_controller;
    FontSet fonts;
    TileCache render;
    TileCache candidate_render;
    uint16_t *provisional_frames;
    size_t provisional_frame_count;
    size_t provisional_current_frame;
    size_t provisional_frame_pixels;
    int provisional_scroll_positions[BROWSER_PROVISIONAL_FRAME_LIMIT];
    bool provisional_ready;
    DocumentBacking backing;
    TilefinchDiagnostics diagnostics;
    uint16_t *framebuffer;
    size_t framebuffer_pixels;
    BrowserEngineState state;
    bool session_ready;
    bool navigation_ready;
    bool fonts_ready;
    bool controller_ready;
    bool render_ready;
    bool candidate_controller_ready;
    bool candidate_render_ready;
    bool candidate_shell_prepared;
    bool replacement_frame_frozen;
    bool forced_dark;
    bool youtube_compact_results;
    size_t render_relayout_generation;
    size_t render_focus_paint_generation;
    size_t unchanged_runtime_frames_suppressed;
    PageFindIndex find;
    uint64_t find_navigation_generation;
    size_t find_layout_generation;
    TilefinchDiagnosticSubsystem last_diagnostic_subsystem;
    TilefinchDiagnosticCode last_diagnostic_code;
    /* Compile-admission rejections already observed on the current page, so
       each rejection produces one warning event rather than one per frame. */
    size_t observed_compile_rejections;
    BrowserEngineNavigationWork navigation_work;
    char last_error[256];
};

/* Lexbor exposes one process-global allocator table rather than a parser-local
   allocator context. Keep the facade singleton explicit so a later engine
   cannot redirect an earlier engine's DOM allocations or leave a dangling
   allocator owner behind when it is destroyed. */
static BrowserEngine *active_engine;

static bool browser_engine_input_ready(const BrowserEngine *engine);
static bool browser_engine_find_refresh(BrowserEngine *engine);

static void copy_error(char *output, size_t capacity, const char *message)
{
    if (output == NULL || capacity == 0) return;
    snprintf(output, capacity, "%s", message == NULL ? "" : message);
}

static bool emit_diagnostic(BrowserEngine *engine,
                            TilefinchDiagnosticSeverity severity,
                            TilefinchDiagnosticSubsystem subsystem,
                            TilefinchDiagnosticCode code,
                            const char *name, const char *detail,
                            uint64_t value, uint64_t auxiliary)
{
    if (engine == NULL) return false;
    engine->last_diagnostic_subsystem = subsystem;
    engine->last_diagnostic_code = code;
    return tilefinch_diagnostics_emit(
        &engine->diagnostics, severity, subsystem, code, name, detail,
        value, auxiliary);
}

static bool set_error_code(BrowserEngine *engine,
                           TilefinchDiagnosticSubsystem subsystem,
                           TilefinchDiagnosticCode code,
                           const char *name, const char *message)
{
    if (engine != NULL) {
        snprintf(engine->last_error, sizeof(engine->last_error), "%s",
                 message == NULL ? "browser engine error" : message);
        (void) emit_diagnostic(
            engine, TILEFINCH_DIAGNOSTIC_ERROR, subsystem, code, name,
            engine->last_error, 0, 0);
    }
    return false;
}

static void clear_error(BrowserEngine *engine)
{
    if (engine == NULL) return;
    engine->last_error[0] = '\0';
}

static bool copy_path(char output[BROWSER_ENGINE_PATH_LIMIT],
                      const char *path)
{
    if (path == NULL) path = "";
    size_t length = strlen(path);
    if (length >= BROWSER_ENGINE_PATH_LIMIT) return false;
    memcpy(output, path, length + 1);
    return true;
}

static bool fixed_string_terminated(const char *value, size_t capacity)
{
    return value != NULL && memchr(value, '\0', capacity) != NULL;
}

void browser_device_profile_psp3000(BrowserDeviceProfile *profile)
{
    if (profile == NULL) return;
    *profile = (BrowserDeviceProfile) {
        .name = "psp-3000-333mhz",
        .target_clock_mhz = 333,
        .framebuffer_width = 480,
        .framebuffer_height = 272,
        .navigation_viewport_width = 480,
        .recommended_memory_limit = BROWSER_PSP_REALISTIC_CONTENT_LIMIT,
        .minimum_non_page_reserve =
            BROWSER_PSP_MINIMUM_NON_PAGE_RESERVE,
        .maximum_tile_capacity = 8
    };
}

void browser_config_init(BrowserConfig *config,
                         const BrowserDeviceProfile *profile)
{
    if (config == NULL) return;
    BrowserDeviceProfile selected;
    if (profile == NULL) browser_device_profile_psp3000(&selected);
    else selected = *profile;
    *config = (BrowserConfig) {
        .device = selected,
        .non_page_memory_reserve = selected.minimum_non_page_reserve,
        .memory_limit = selected.recommended_memory_limit,
        .history_capacity = 16,
        .session_cache_limit = 1u * MIB,
        .maximum_document_bytes = 8u * MIB,
        .navigation_timeout_ms = 20000,
        .navigation_replacement_mode =
            NAVIGATION_REPLACEMENT_TRANSACTIONAL,
        .declared_css_width = 0,
        .declared_css_height = 0,
        .progressive_first_paint = true,
        .tile_capacity = selected.maximum_tile_capacity < 8
                         ? selected.maximum_tile_capacity : 8,
        .idle_work_budget_us = 500,
        .idle_work_maximum_units = 8,
        .javascript = {
            .enabled = false,
            .document_scripts_enabled = false,
            .heap_limit = 4u * MIB,
            .runtime_timeout_ms = 10000,
            .maximum_scripts = 48,
            .maximum_total_bytes = 4u * MIB,
            .maximum_file_bytes = 256u * KIB,
            .network_timeout_ms = 10000
        },
        .resources = {
            .enabled = false,
            .web_fonts_enabled = true,
            .maximum_stylesheets = 16,
            .maximum_stylesheet_bytes = 2u * MIB,
            .maximum_stylesheet_file_bytes = 512u * KIB,
            .maximum_font_attempts = 8,
            .maximum_font_bytes = 256u * KIB,
            .maximum_font_file_bytes = 96u * KIB,
            .maximum_font_face_backend_bytes = 256u * KIB,
            .maximum_images = 24,
            .maximum_image_bytes = 4u * MIB,
            .maximum_image_file_bytes = 1u * MIB,
            .maximum_decoded_image_bytes = 4u * MIB,
            .timeout_ms = 10000
        },
        .fonts = {
            .enabled = false,
            .maximum_total_bytes = 1536u * KIB
        }
    };
    (void) script_execution_policy_for_profile(
        SCRIPT_EXECUTION_PROFILE_PSP_REALISTIC,
        &config->javascript.execution_policy);
}

bool browser_config_apply_psp_memory_profile(
    BrowserConfig *config, BrowserPspMemoryProfile profile)
{
    if (config == NULL
        || (profile != BROWSER_PSP_MEMORY_STRICT
            && profile != BROWSER_PSP_MEMORY_REALISTIC)) return false;
    bool strict = profile == BROWSER_PSP_MEMORY_STRICT;
    config->memory_limit = strict
        ? BROWSER_PSP_STRICT_CONTENT_LIMIT
        : BROWSER_PSP_REALISTIC_CONTENT_LIMIT;
    config->non_page_memory_reserve =
        BROWSER_PSP_MINIMUM_NON_PAGE_RESERVE;
    config->history_capacity = strict ? 8 : 16;
    config->session_cache_limit = strict ? 512u * KIB : 1u * MIB;
    config->tile_capacity = config->device.maximum_tile_capacity < 8
        ? config->device.maximum_tile_capacity : 8;
    config->javascript.heap_limit = strict ? 4u * MIB : 5u * MIB;
    size_t script_source_limit = strict ? 1u * MIB : 2u * MIB;
    if (config->javascript.maximum_total_bytes > script_source_limit)
        config->javascript.maximum_total_bytes = script_source_limit;
    if (config->javascript.maximum_file_bytes > 256u * KIB)
        config->javascript.maximum_file_bytes = 256u * KIB;
    return script_execution_policy_for_profile(
        strict ? SCRIPT_EXECUTION_PROFILE_PSP_STRICT
               : SCRIPT_EXECUTION_PROFILE_PSP_REALISTIC,
        &config->javascript.execution_policy);
}

bool browser_config_set_font_paths(
    BrowserConfig *config, const char *sans_path, const char *serif_path,
    const char *sans_italic_path, const char *sans_bold_path,
    const char *serif_bold_path, const char *metric_sans_path,
    const char *metric_sans_bold_path, size_t maximum_total_bytes)
{
    if (config == NULL || maximum_total_bytes == 0
        || ((sans_path == NULL || sans_path[0] == '\0')
            && (serif_path == NULL || serif_path[0] == '\0')
            && (sans_italic_path == NULL || sans_italic_path[0] == '\0')
            && (sans_bold_path == NULL || sans_bold_path[0] == '\0')
            && (serif_bold_path == NULL || serif_bold_path[0] == '\0')
            && (metric_sans_path == NULL || metric_sans_path[0] == '\0')
            && (metric_sans_bold_path == NULL
                || metric_sans_bold_path[0] == '\0'))) {
        return false;
    }
    BrowserFontConfig candidate = config->fonts;
    if (!copy_path(candidate.sans_path, sans_path)
        || !copy_path(candidate.serif_path, serif_path)
        || !copy_path(candidate.sans_italic_path, sans_italic_path)
        || !copy_path(candidate.sans_bold_path, sans_bold_path)
        || !copy_path(candidate.serif_bold_path, serif_bold_path)
        || !copy_path(candidate.metric_sans_path, metric_sans_path)
        || !copy_path(candidate.metric_sans_bold_path,
                      metric_sans_bold_path)) {
        return false;
    }
    candidate.maximum_total_bytes = maximum_total_bytes;
    candidate.enabled = true;
    config->fonts = candidate;
    return true;
}

bool browser_config_validate(const BrowserConfig *config,
                             char *error, size_t error_capacity)
{
#define CONFIG_REQUIRE(condition, message) do {                              \
    if (!(condition)) { copy_error(error, error_capacity, (message));        \
                       return false; }                                       \
} while (0)
    CONFIG_REQUIRE(config != NULL, "configuration is required");
    CONFIG_REQUIRE(fixed_string_terminated(
                       config->device.name, sizeof(config->device.name))
                       && config->device.name[0] != '\0',
                   "device profile name must be bounded and nonempty");
    CONFIG_REQUIRE(config->device.framebuffer_width > 0
                       && config->device.framebuffer_height > 0
                       && config->device.navigation_viewport_width > 0,
                   "device dimensions must be positive");
    CONFIG_REQUIRE(config->device.framebuffer_width <= 4096
                       && config->device.framebuffer_height <= 4096,
                   "device dimensions exceed the bounded renderer limit");
    size_t width = (size_t) config->device.framebuffer_width;
    size_t height = (size_t) config->device.framebuffer_height;
    CONFIG_REQUIRE(width <= SIZE_MAX / height
                       && width * height <= SIZE_MAX / sizeof(uint16_t),
                   "framebuffer dimensions overflow size_t");
    CONFIG_REQUIRE(config->memory_limit != 0,
                   "content memory limit must be nonzero");
    CONFIG_REQUIRE(config->non_page_memory_reserve
                       <= SIZE_MAX - config->memory_limit,
                   "content limit and non-page reserve overflow size_t");
    CONFIG_REQUIRE(config->non_page_memory_reserve
                       >= config->device.minimum_non_page_reserve,
                   "non-page reserve is below the device profile minimum");
    CONFIG_REQUIRE(config->history_capacity != 0
                       && config->history_capacity <= 64,
                   "history capacity must be between 1 and 64");
    CONFIG_REQUIRE(config->session_cache_limit != 0
                       && config->session_cache_limit < config->memory_limit,
                   "session cache must fit inside the content budget");
    CONFIG_REQUIRE(config->maximum_document_bytes != 0
                       && config->navigation_timeout_ms > 0,
                   "navigation limits must be nonzero");
    CONFIG_REQUIRE(
        config->navigation_replacement_mode
                == NAVIGATION_REPLACEMENT_TRANSACTIONAL
            || config->navigation_replacement_mode
                == NAVIGATION_REPLACEMENT_LOW_MEMORY,
        "navigation replacement mode is invalid");
    CONFIG_REQUIRE(
        (config->declared_css_width == 0
         && config->declared_css_height == 0)
            || (config->declared_css_width > 0
                && config->declared_css_width <= 4096
                && config->declared_css_height > 0
                && config->declared_css_height <= 4096),
        "declared CSS viewport must be zero or a bounded dimension pair");
    CONFIG_REQUIRE(config->device.maximum_tile_capacity == 0
                       || config->tile_capacity
                              <= config->device.maximum_tile_capacity,
                   "tile capacity exceeds the device profile");
    CONFIG_REQUIRE(config->idle_work_budget_us > 0
                       && config->idle_work_budget_us <= 1000000
                       && config->idle_work_maximum_units > 0
                       && config->idle_work_maximum_units <= 64,
                   "idle work limits must be bounded and nonzero");
    CONFIG_REQUIRE(!config->javascript.enabled
                       || (config->javascript.heap_limit != 0
                           && config->javascript.runtime_timeout_ms != 0),
                   "JavaScript heap and runtime limits must be nonzero");
    CONFIG_REQUIRE(!config->javascript.document_scripts_enabled
                       || (config->javascript.enabled
                           && config->javascript.maximum_scripts != 0
                           && config->javascript.maximum_total_bytes != 0
                           && config->javascript.maximum_file_bytes != 0
                           && config->javascript.network_timeout_ms > 0),
                   "document scripts require complete JavaScript limits");
    CONFIG_REQUIRE(
        !config->javascript.document_scripts_enabled
            || config->javascript.execution_policy
                       .maximum_host_compile_source_bytes == 0
            || config->javascript.maximum_file_bytes
                 <= config->javascript.execution_policy
                        .maximum_host_compile_source_bytes,
        "script response limit exceeds the host compile admission limit");
    CONFIG_REQUIRE(
        (config->javascript.execution_policy
                 .maximum_host_compile_projected_us == 0)
            == (config->javascript.execution_policy
                    .modeled_compile_bytes_per_ms == 0),
        "projected compile admission requires both the time budget and the "
        "modeled throughput");
    CONFIG_REQUIRE(!config->resources.enabled
                       || (config->resources.maximum_stylesheets != 0
                           && config->resources.maximum_stylesheet_bytes != 0
                           && config->resources.maximum_stylesheet_file_bytes
                                  != 0
                           && config->resources.maximum_images != 0
                           && config->resources.maximum_image_bytes != 0
                           && config->resources.maximum_image_file_bytes != 0
                           && config->resources.maximum_decoded_image_bytes
                                  != 0
                           && config->resources.timeout_ms > 0),
                   "external resources require complete bounded limits");
    CONFIG_REQUIRE(
        !config->resources.enabled
            || !config->resources.web_fonts_enabled
            || (config->resources.maximum_font_attempts != 0
                && config->resources.maximum_font_attempts
                    <= TILEFINCH_WEB_FONT_FAMILY_LIMIT * 2u
                       * STYLE_WEB_FONT_SOURCE_LIMIT
                && config->resources.maximum_font_bytes != 0
                && config->resources.maximum_font_file_bytes != 0
                && config->resources.maximum_font_file_bytes
                    <= config->resources.maximum_font_bytes
                && config->resources.maximum_font_face_backend_bytes != 0),
        "web fonts require complete bounded limits");
    CONFIG_REQUIRE(!config->fonts.enabled
                       || config->fonts.maximum_total_bytes != 0,
                   "enabled fonts require a nonzero byte limit");
    CONFIG_REQUIRE(
        !config->fonts.enabled
            || (fixed_string_terminated(
                    config->fonts.sans_path,
                    sizeof(config->fonts.sans_path))
                && fixed_string_terminated(
                    config->fonts.serif_path,
                    sizeof(config->fonts.serif_path))
                && fixed_string_terminated(
                    config->fonts.sans_italic_path,
                    sizeof(config->fonts.sans_italic_path))
                && fixed_string_terminated(
                    config->fonts.sans_bold_path,
                    sizeof(config->fonts.sans_bold_path))
                && fixed_string_terminated(
                    config->fonts.serif_bold_path,
                    sizeof(config->fonts.serif_bold_path))
                && fixed_string_terminated(
                    config->fonts.metric_sans_path,
                    sizeof(config->fonts.metric_sans_path))
                && fixed_string_terminated(
                    config->fonts.metric_sans_bold_path,
                    sizeof(config->fonts.metric_sans_bold_path))),
        "font paths must be bounded strings");
    CONFIG_REQUIRE(
        !config->fonts.enabled
            || config->fonts.sans_path[0] != '\0'
            || config->fonts.serif_path[0] != '\0'
            || config->fonts.sans_italic_path[0] != '\0'
            || config->fonts.sans_bold_path[0] != '\0'
            || config->fonts.serif_bold_path[0] != '\0'
            || config->fonts.metric_sans_path[0] != '\0'
            || config->fonts.metric_sans_bold_path[0] != '\0',
        "enabled fonts require at least one font path");
    CONFIG_REQUIRE(config->diagnostics.minimum_severity
                       >= TILEFINCH_DIAGNOSTIC_DEBUG
                       && config->diagnostics.minimum_severity
                              <= TILEFINCH_DIAGNOSTIC_ERROR,
                   "diagnostic severity is invalid");
    copy_error(error, error_capacity, "");
    return true;
#undef CONFIG_REQUIRE
}

static void browser_engine_reset_render_shell(BrowserEngine *engine)
{
    if (engine == NULL) return;
    if (engine->render_ready) tile_cache_destroy(&engine->render);
    else memset(&engine->render, 0, sizeof(engine->render));
    engine->render_ready = false;
    engine->render_relayout_generation = 0;
    engine->render_focus_paint_generation = 0;
}

static void browser_engine_reset_shell(BrowserEngine *engine)
{
    if (engine == NULL) return;
    browser_engine_reset_render_shell(engine);
    memset(&engine->controller, 0, sizeof(engine->controller));
    engine->controller_ready = false;
}

static bool browser_engine_init_tile_cache(
    BrowserEngine *engine, TileCache *cache, const LayoutDocument *layout,
    size_t tile_capacity)
{
    if (engine == NULL || cache == NULL || layout == NULL
        || !tile_cache_init(
               cache, &engine->budget, layout, tile_capacity)) {
        return false;
    }
    tile_cache_set_forced_dark(cache, engine->forced_dark);
    return true;
}

static void browser_engine_reset_provisional_viewport(BrowserEngine *engine)
{
    if (engine == NULL) return;
    budget_free(&engine->budget, engine->provisional_frames);
    engine->provisional_frames = NULL;
    engine->provisional_frame_count = 0;
    engine->provisional_current_frame = 0;
    engine->provisional_frame_pixels = 0;
    memset(engine->provisional_scroll_positions, 0,
           sizeof(engine->provisional_scroll_positions));
    engine->provisional_ready = false;
}

static bool browser_engine_initialize_render_shell(
    BrowserEngine *engine, const char *diagnostic_name,
    const char *failure_message)
{
    if (engine->config.tile_capacity == 0) {
        clear_error(engine);
        return true;
    }
    if (engine->framebuffer == NULL
        || !browser_engine_init_tile_cache(
               engine, &engine->render,
               &engine->navigation.page.layout,
               engine->config.tile_capacity)
        || !tile_cache_set_frame(
               &engine->render, engine->framebuffer,
               engine->framebuffer_pixels)) {
        if (engine->render.budget != NULL)
            tile_cache_destroy(&engine->render);
        return set_error_code(
            engine, TILEFINCH_SUBSYSTEM_RENDER,
            TILEFINCH_DIAGNOSTIC_ALLOCATION_FAILED, diagnostic_name,
            failure_message);
    }
    engine->render_ready = true;
    engine->render_relayout_generation =
        engine->navigation.incremental_relayouts;
    engine->render_focus_paint_generation =
        engine->navigation.focus_paint_generation;
    clear_error(engine);
    return true;
}

static void browser_engine_paint_replacement_failure(BrowserEngine *engine)
{
    if (engine == NULL || engine->framebuffer == NULL
        || engine->framebuffer_pixels == 0) return;
    size_t width = (size_t) engine->config.device.framebuffer_width;
    size_t height = (size_t) engine->config.device.framebuffer_height;
    if (width == 0 || height == 0
        || width > engine->framebuffer_pixels / height) return;
    uint16_t *frame = engine->framebuffer;
    if (!engine->replacement_frame_frozen) {
        for (size_t i = 0; i < width * height; i++) frame[i] = UINT16_C(0xef7d);
    }
    size_t banner_height = height < 40 ? height : 40;
    for (size_t y = 0; y < banner_height; y++) {
        for (size_t x = 0; x < width; x++) {
            frame[y * width + x] = y < 3 ? UINT16_C(0xffff)
                                         : UINT16_C(0xb104);
        }
    }
    /* Allocation-free retry glyph: a white return arrow in the fixed banner.
       The detailed failure text remains in NavigationSession for assistive
       UI and logs rather than requiring a font allocation here. */
    size_t center_x = width < 30 ? width / 2 : width - 22;
    size_t center_y = banner_height / 2;
    for (size_t x = center_x > 8 ? center_x - 8 : 0;
         x < width && x <= center_x + 6; x++) {
        if (center_y < height) frame[center_y * width + x] = UINT16_C(0xffff);
    }
    for (size_t d = 0; d < 7; d++) {
        size_t x = center_x > 8 ? center_x - 8 + d : d;
        if (x >= width) break;
        if (center_y >= d && center_y - d < height)
            frame[(center_y - d) * width + x] = UINT16_C(0xffff);
        if (center_y + d < height)
            frame[(center_y + d) * width + x] = UINT16_C(0xffff);
    }
}

static bool browser_engine_freeze_replacement_frame(
    void *opaque, const NavigationSession *incumbent)
{
    BrowserEngine *engine = opaque;
    if (engine == NULL || incumbent != &engine->navigation) return false;
    bool frozen = engine->framebuffer != NULL && engine->render_ready
        && engine->render.frames_rendered != 0;
    engine->replacement_frame_frozen = frozen;
    browser_engine_reset_shell(engine);
    return frozen;
}

static void browser_engine_replacement_outcome(
    void *opaque, const NavigationSession *session, bool success)
{
    BrowserEngine *engine = opaque;
    if (engine == NULL || session != &engine->navigation) return;
    if (!success) browser_engine_paint_replacement_failure(engine);
    else engine->replacement_frame_frozen = false;
}

static void browser_engine_abort_candidate_shell(void *opaque)
{
    BrowserEngine *engine = opaque;
    if (engine == NULL) return;
    if (engine->candidate_render.budget != NULL)
        tile_cache_destroy(&engine->candidate_render);
    else
        memset(&engine->candidate_render, 0,
               sizeof(engine->candidate_render));
    memset(&engine->candidate_controller, 0,
           sizeof(engine->candidate_controller));
    engine->candidate_controller_ready = false;
    engine->candidate_render_ready = false;
    engine->candidate_shell_prepared = false;
}

static bool browser_engine_prepare_candidate_shell(
    void *opaque, NavigationSession *candidate)
{
    BrowserEngine *engine = opaque;
    if (engine == NULL || engine->state != BROWSER_ENGINE_ACTIVE
        || !engine->navigation_ready
        || candidate == NULL || candidate->budget != &engine->budget
        || !candidate->page.loaded) {
        return false;
    }
    bool keep_progressive_render = engine->candidate_render_ready
        && engine->candidate_render.budget == &engine->budget
        && engine->candidate_render.source_layout
               == &candidate->page.layout;
    if (!keep_progressive_render) {
        browser_engine_abort_candidate_shell(engine);
    } else {
        /* The progressive callback already rasterized this exact final
           layout. Preserve its bounded tile and decoded-image caches; only
           the controller side of the transaction remains to be prepared. */
        memset(&engine->candidate_controller, 0,
               sizeof(engine->candidate_controller));
        engine->candidate_controller_ready = false;
        engine->candidate_shell_prepared = false;
    }
    if (!controller_init(&engine->candidate_controller,
                         candidate)) {
        return set_error_code(
            engine, TILEFINCH_SUBSYSTEM_ENGINE,
            TILEFINCH_DIAGNOSTIC_INTERNAL_FAILED, "candidate-controller",
            "candidate controller initialization failed");
    }
    engine->candidate_controller_ready = true;
    if (engine->config.tile_capacity != 0 && !keep_progressive_render) {
        if (engine->framebuffer == NULL
            || !browser_engine_init_tile_cache(
                   engine, &engine->candidate_render,
                   &candidate->page.layout, engine->config.tile_capacity)
            || !tile_cache_set_frame(
                   &engine->candidate_render, engine->framebuffer,
                   engine->framebuffer_pixels)) {
            return set_error_code(
                engine, TILEFINCH_SUBSYSTEM_RENDER,
                TILEFINCH_DIAGNOSTIC_ALLOCATION_FAILED,
                "candidate-render",
                "candidate render shell exceeded its bounded memory");
        }
        engine->candidate_render_ready = true;
    }
    engine->candidate_shell_prepared = true;
    return true;
}

static void browser_engine_commit_candidate_shell(void *opaque)
{
    BrowserEngine *engine = opaque;
    if (engine == NULL || !engine->candidate_shell_prepared) return;
    browser_engine_reset_shell(engine);
    engine->controller = engine->candidate_controller;
    /* A streamed URL candidate is built in a temporary NavigationSession
       which is memcpy-promoted immediately before this no-fail callback. */
    engine->controller.navigation = &engine->navigation;
    engine->controller_ready = engine->candidate_controller_ready;
    engine->render = engine->candidate_render;
    engine->render.source_layout = &engine->navigation.page.layout;
    if (engine->render.owns_visual_layout) {
        engine->render.layout = &engine->render.visual_layout;
    } else {
        engine->render.layout = engine->render.source_layout;
    }
    engine->render_ready = engine->candidate_render_ready;
    engine->render_relayout_generation =
        engine->navigation.incremental_relayouts;
    engine->render_focus_paint_generation =
        engine->navigation.focus_paint_generation;
    memset(&engine->candidate_controller, 0,
           sizeof(engine->candidate_controller));
    memset(&engine->candidate_render, 0,
           sizeof(engine->candidate_render));
    engine->candidate_controller_ready = false;
    engine->candidate_render_ready = false;
    engine->candidate_shell_prepared = false;
}

static bool browser_engine_capture_provisional_viewport(
    BrowserEngine *engine, NavigationSession *candidate,
    const LayoutDocument *layout, size_t width, size_t height)
{
    if (engine == NULL || candidate == NULL || layout == NULL
        || width == 0 || height == 0
        || engine->config.tile_capacity == 0
        || width > SIZE_MAX / height) return false;
    if (engine->navigation_work.metrics.provisional_capture_started_us == 0) {
        uint64_t now_us =
            tilefinch_platform_monotonic_time_us();
        engine->navigation_work.metrics.provisional_capture_started_us =
            now_us >= engine->navigation_work.started_us
                ? now_us - engine->navigation_work.started_us : 0;
    }
    size_t pixels = width * height;
    if (pixels > SIZE_MAX / sizeof(uint16_t)) return false;
    int viewport_height = candidate->viewport.css_height;
    if (viewport_height <= 0) viewport_height = (int) height;
    int maximum_scroll = layout->height > viewport_height
        ? layout->height - viewport_height : 0;
    /* The retained snapshot window is deliberately one page, not an
       unbounded alternate document surface. The authoritative page owns
       deeper scrolling after commit. */
    if (maximum_scroll > viewport_height) maximum_scroll = viewport_height;
    size_t desired_frame_count = maximum_scroll > 0
        ? BROWSER_PROVISIONAL_FRAME_LIMIT : 1u;
    if (pixels > SIZE_MAX / desired_frame_count
        || pixels * desired_frame_count > SIZE_MAX / sizeof(uint16_t)) {
        return false;
    }
    const size_t frame_bytes = pixels * sizeof(uint16_t);
    if (budget_pressure_required(
            &engine->budget, frame_bytes,
            BROWSER_PROVISIONAL_FINAL_RESERVE)) {
        budget_record_pressure(
            &engine->budget, BUDGET_PRESSURE_SPECULATION, frame_bytes, 0);
        return false;
    }
    browser_engine_reset_provisional_viewport(engine);
    uint64_t alloc_started_us =
        tilefinch_platform_monotonic_time_us();
    engine->provisional_frames = budget_malloc_category(
        &engine->budget, BUDGET_CATEGORY_RENDER, frame_bytes);
    uint64_t alloc_finished_us =
        tilefinch_platform_monotonic_time_us();
    uint64_t first_alloc_us = alloc_finished_us >= alloc_started_us
        ? alloc_finished_us - alloc_started_us : 0;
    candidate->performance.streaming_preview_frame_alloc_us +=
        first_alloc_us;
    if (engine->provisional_frames == NULL) return false;
    engine->provisional_frame_pixels = pixels;
    engine->provisional_scroll_positions[0] = 0;
    size_t frame_count = 1;

    size_t capacity = engine->config.tile_capacity;
    if (capacity > 8) capacity = 8;
    TileCache preview = {0};
    bool first_painted = false;
    uint64_t cache_started_us =
        tilefinch_platform_monotonic_time_us();
    if (capacity != 0
        && browser_engine_init_tile_cache(
               engine, &preview, layout, capacity)) {
        uint64_t cache_ready_us =
            tilefinch_platform_monotonic_time_us();
        if (cache_ready_us >= cache_started_us) {
            candidate->performance.streaming_preview_cache_init_us +=
                cache_ready_us - cache_started_us;
        }
        tile_cache_set_fast_text_raster(&preview, true);
        for (size_t i = 0; i < frame_count; i++) {
            uint16_t *frame = engine->provisional_frames + i * pixels;
            uint64_t raster_started_us =
                tilefinch_platform_monotonic_time_us();
            bool rendered =
                tile_cache_set_frame(&preview, frame, pixels)
                && tile_cache_render_frame(
                    &preview,
                    engine->provisional_scroll_positions[i],
                    (int) width, (int) height, NULL);
            uint64_t raster_finished_us =
                tilefinch_platform_monotonic_time_us();
            if (raster_finished_us >= raster_started_us) {
                candidate->performance.streaming_preview_raster_us +=
                    raster_finished_us - raster_started_us;
            }
            if (!rendered) {
                break;
            }
            if (i == 0) {
                /* A structurally nonempty body can still rasterize as one
                   flat background while its first visible content remains
                   in a later response chunk. Publishing that frame would
                   satisfy a timer without giving the user a page. Require a
                   small, resolution-independent amount of authored visual
                   contrast and let the streaming parser retry at its next
                   bounded checkpoint when it is absent. */
                size_t contrasting = 0;
                uint16_t background = frame[0];
                /* Reject a lone border/placeholder row as well as a fully
                   flat surface. Two scanlines' worth of contrast is still a
                   tiny resolution-relative threshold for real page text. */
                const size_t required =
                    width <= SIZE_MAX / 2u ? width * 2u : 64u;
                for (size_t pixel = 1;
                     pixel < pixels && contrasting < required; pixel++) {
                    if (frame[pixel] != background) contrasting++;
                }
                if (contrasting < required) {
                    candidate->performance
                        .streaming_preview_empty_raster_skips++;
                    break;
                }
            }
            __sync_synchronize();
            engine->provisional_frame_count = i + 1u;
            if (i == 0) {
                first_painted = true;
                engine->provisional_current_frame = 0;
                engine->provisional_ready = true;
                BrowserEngineNavigationWork *work =
                    &engine->navigation_work;
                work->metrics.provisional_paints++;
                work->metrics.provisional_frame_count = 1;
                work->metrics.provisional_bytes =
                    budget_usable_size(engine->provisional_frames);
                uint64_t now_us =
                    tilefinch_platform_monotonic_time_us();
                work->metrics.provisional_first_present_us =
                    now_us >= work->started_us
                        ? now_us - work->started_us : 0;
                uint64_t present_started_us = now_us;
                (void) tilefinch_platform_present_rgb565(
                    frame, width, height, width);
                uint64_t present_finished_us =
                    tilefinch_platform_monotonic_time_us();
                if (present_finished_us >= present_started_us) {
                    candidate->performance.streaming_preview_present_us +=
                        present_finished_us - present_started_us;
                }
            } else {
                engine->navigation_work.metrics.provisional_frame_count =
                    i + 1u;
            }
            if (i == 0 && desired_frame_count > 1u) {
                if (!tilefinch_platform_cooperate(
                        "provisional-raster", i + 1u)) {
                    break;
                }
                /*
                 * Never make the useful first viewport wait for the optional
                 * scroll snapshot. Grow only when the allocator proved cheap
                 * and the second frame still leaves room for authoritative
                 * commit state. Slow admission is a direct signal of PSP
                 * heap pressure/fragmentation and must not create another
                 * unresponsive interval immediately after first paint.
                 */
                size_t grown_bytes = frame_bytes * desired_frame_count;
                if (first_alloc_us <=
                        BROWSER_PROVISIONAL_GROW_MAX_ALLOC_US
                    && !budget_pressure_required(
                        &engine->budget, grown_bytes - frame_bytes,
                        BROWSER_PROVISIONAL_FINAL_RESERVE)) {
                    uint64_t grow_started_us =
                        tilefinch_platform_monotonic_time_ns()
                        / UINT64_C(1000);
                    uint16_t *grown = budget_realloc_category(
                        &engine->budget, BUDGET_CATEGORY_RENDER,
                        engine->provisional_frames, grown_bytes);
                    uint64_t grow_finished_us =
                        tilefinch_platform_monotonic_time_ns()
                        / UINT64_C(1000);
                    if (grow_finished_us >= grow_started_us) {
                        candidate->performance
                            .streaming_preview_frame_alloc_us +=
                            grow_finished_us - grow_started_us;
                    }
                    if (grown != NULL) {
                        engine->provisional_frames = grown;
                        engine->navigation_work.metrics.provisional_bytes =
                            budget_usable_size(
                                engine->provisional_frames);
                        engine->provisional_scroll_positions[1] =
                            maximum_scroll;
                        frame_count = desired_frame_count;
                    }
                }
            } else if (i + 1u < frame_count
                       && !tilefinch_platform_cooperate(
                           "provisional-raster", i + 1u)) {
                break;
            }
        }
        candidate->performance.progressive_decoded_image_builds +=
            preview.decoded_image_builds;
        candidate->performance.progressive_scaled_image_builds +=
            preview.scaled_image_builds;
    }
    if (preview.budget != NULL) tile_cache_destroy(&preview);
    if (!first_painted) browser_engine_reset_provisional_viewport(engine);
    return first_painted;
}

static bool browser_engine_paint_progressive_preview(
    void *opaque, NavigationSession *candidate,
    const LayoutDocument *layout)
{
    BrowserEngine *engine = opaque;
    if (engine == NULL || candidate == NULL
        || candidate->budget != &engine->budget
        || engine->config.device.framebuffer_width <= 0
        || engine->config.device.framebuffer_height <= 0) return false;
    size_t width = (size_t) engine->config.device.framebuffer_width;
    size_t height = (size_t) engine->config.device.framebuffer_height;
    if (width > SIZE_MAX / height
        || width * height > SIZE_MAX / sizeof(uint16_t)) return false;
    size_t pixels = width * height;
    bool asynchronous =
        engine->navigation_work.status == BROWSER_NAVIGATION_JOB_PENDING;
    if (asynchronous) {
        if (layout == NULL) {
            browser_engine_reset_provisional_viewport(engine);
            browser_engine_abort_candidate_shell(engine);
            return true;
        }
        return browser_engine_capture_provisional_viewport(
            engine, candidate, layout, width, height);
    }
    if (layout == NULL) {
        /* A failed cold-start candidate has no incumbent frame to restore.
           Clear its already-presented preview immediately; the frontend can
           then overlay the navigation error without retaining dead content. */
        if (engine->framebuffer != NULL
            && engine->framebuffer_pixels >= pixels) {
            for (size_t i = 0; i < pixels; i++)
                engine->framebuffer[i] = UINT16_C(0xffff);
            (void) tilefinch_platform_present_rgb565(
                engine->framebuffer, width, height, width);
        }
        browser_engine_abort_candidate_shell(engine);
        return true;
    }
    bool transient_preview =
        candidate->performance.partial_layouts != 0;
    if (!transient_preview
        && engine->config.tile_capacity != 0 && engine->framebuffer != NULL
        && engine->framebuffer_pixels >= pixels) {
        browser_engine_abort_candidate_shell(engine);
        bool painted = browser_engine_init_tile_cache(
                engine, &engine->candidate_render, layout,
                engine->config.tile_capacity)
            && tile_cache_set_frame(
                &engine->candidate_render, engine->framebuffer,
                engine->framebuffer_pixels)
            && tile_cache_render_frame(
                &engine->candidate_render, 0, (int) width, (int) height,
                NULL);
        if (!painted) {
            browser_engine_abort_candidate_shell(engine);
            return false;
        }
        engine->candidate_render_ready = true;
        (void) tilefinch_platform_present_rgb565(
            engine->framebuffer, width, height, width);
        /* Presentation is the first-paint boundary. Paint-only preparation
           below must not move this timestamp merely because the browser is
           not interactive yet. */
        if (candidate->performance.first_paint_us == 0
            && candidate->navigation_started_us != 0) {
            uint64_t now_us = tilefinch_platform_monotonic_time_us();
            candidate->performance.first_paint_us =
                now_us >= candidate->navigation_started_us
                ? now_us - candidate->navigation_started_us : 0;
        }
        int prefetch_y = candidate->viewport.css_height
                         + TILEFINCH_TILE_SIZE - 1;
        if (prefetch_y < layout->height) {
            tile_cache_prepare_startup_visuals(
                &engine->candidate_render, prefetch_y, (int) width,
                3000, 4);
        }
        candidate->performance.progressive_decoded_image_builds +=
            engine->candidate_render.decoded_image_builds;
        candidate->performance.progressive_scaled_image_builds +=
            engine->candidate_render.scaled_image_builds;
        return true;
    }
    if (transient_preview && engine->framebuffer != NULL
        && engine->framebuffer_pixels >= pixels) {
        size_t capacity = engine->config.tile_capacity;
        if (capacity == 0) {
            capacity = engine->config.device.maximum_tile_capacity;
        }
        if (capacity > 8) capacity = 8;
        TileCache preview = {0};
        bool painted = capacity != 0
            && browser_engine_init_tile_cache(
                   engine, &preview, layout, capacity)
            && tile_cache_set_frame(
                   &preview, engine->framebuffer,
                   engine->framebuffer_pixels)
            && tile_cache_render_frame(
                   &preview, 0, (int) width, (int) height, NULL);
        if (painted) {
            candidate->performance.progressive_decoded_image_builds +=
                preview.decoded_image_builds;
            candidate->performance.progressive_scaled_image_builds +=
                preview.scaled_image_builds;
            (void) tilefinch_platform_present_rgb565(
                engine->framebuffer, width, height, width);
        }
        if (preview.budget != NULL) tile_cache_destroy(&preview);
        return painted;
    }
    uint16_t *frame = budget_malloc_category(
        &engine->budget, BUDGET_CATEGORY_RENDER,
        pixels * sizeof(*frame));
    if (frame == NULL) return false;
    size_t capacity = engine->config.tile_capacity;
    if (capacity == 0) capacity = engine->config.device.maximum_tile_capacity;
    if (capacity > 8) capacity = 8;
    TileCache preview = {0};
    bool painted = capacity != 0
        && browser_engine_init_tile_cache(
               engine, &preview, layout, capacity)
        && tile_cache_set_frame(&preview, frame, pixels)
        && tile_cache_render_frame(
               &preview, 0, (int) width, (int) height, NULL);
    if (painted) {
        candidate->performance.progressive_decoded_image_builds +=
            preview.decoded_image_builds;
        candidate->performance.progressive_scaled_image_builds +=
            preview.scaled_image_builds;
        /* A host lab without a presentation service still has a genuine
           raster-ready first paint.  PSP and platform-simulator frontends
           install this service and receive the preview immediately. */
        (void) tilefinch_platform_present_rgb565(
            frame, width, height, width);
    }
    if (preview.budget != NULL) tile_cache_destroy(&preview);
    budget_free(&engine->budget, frame);
    return painted;
}

static bool browser_engine_configure_navigation(BrowserEngine *engine)
{
    NavigationSession *navigation = &engine->navigation;
    navigation->declared_css_width = engine->config.declared_css_width;
    navigation->declared_css_height = engine->config.declared_css_height;
    navigation_attach_browser_session(navigation, &engine->session);
    if (!navigation_set_replacement_mode(
            navigation, engine->config.navigation_replacement_mode)
        || !navigation_set_replacement_hooks(
            navigation, browser_engine_freeze_replacement_frame,
            browser_engine_replacement_outcome, engine)) {
        return set_error_code(
            engine, TILEFINCH_SUBSYSTEM_ENGINE,
            TILEFINCH_DIAGNOSTIC_INVALID_INPUT, "replacement-policy",
            "could not apply navigation replacement policy");
    }
    /* Retain half a viewport below the first frame. This is enough to make
       provisional page-down useful while keeping the speculative layout's
       style and intrinsic-size walks local to pixels it can soon present. */
    if (!navigation_set_progressive_preview_lookahead(navigation, 50u)
        || !navigation_set_progressive_paint_hook(
            navigation,
            engine->config.progressive_first_paint
                ? browser_engine_paint_progressive_preview : NULL,
            engine->config.progressive_first_paint ? engine : NULL)) {
        return set_error_code(
            engine, TILEFINCH_SUBSYSTEM_ENGINE,
            TILEFINCH_DIAGNOSTIC_INVALID_INPUT, "progressive-paint-policy",
            "could not apply progressive first-paint policy");
    }
    navigation_set_progressive_paint_preserves_incumbent(
        navigation, engine->config.progressive_first_paint);
    if (!navigation_set_script_execution_policy(
            navigation, &engine->config.javascript.execution_policy)) {
        return set_error_code(
            engine, TILEFINCH_SUBSYSTEM_SCRIPT,
            TILEFINCH_DIAGNOSTIC_INVALID_INPUT, "script-policy",
            "could not apply JavaScript execution policy");
    }
    if (engine->config.javascript.enabled) {
        navigation_enable_scripts(
            navigation, engine->config.javascript.heap_limit,
            engine->config.javascript.runtime_timeout_ms);
    }
    if (engine->config.javascript.document_scripts_enabled) {
        navigation_enable_document_scripts(
            navigation, engine->config.javascript.maximum_scripts,
            engine->config.javascript.maximum_total_bytes,
            engine->config.javascript.maximum_file_bytes,
            engine->config.javascript.network_timeout_ms);
    }
    if (engine->config.resources.enabled) {
        const BrowserResourceConfig *resources = &engine->config.resources;
        navigation_enable_external_resources(
            navigation, resources->maximum_stylesheets,
            resources->maximum_stylesheet_bytes,
            resources->maximum_stylesheet_file_bytes,
            resources->maximum_images, resources->maximum_image_bytes,
            resources->maximum_image_file_bytes,
            resources->maximum_decoded_image_bytes, resources->timeout_ms);
        if (resources->web_fonts_enabled) {
            navigation_enable_web_fonts(
                navigation, resources->maximum_font_attempts,
                resources->maximum_font_bytes,
                resources->maximum_font_file_bytes,
                resources->maximum_font_face_backend_bytes,
                resources->timeout_ms);
        }
    }
    return true;
}

BrowserEngine *browser_engine_create(const BrowserConfig *config,
                                     char *error, size_t error_capacity)
{
    if (!browser_config_validate(config, error, error_capacity)) return NULL;
    if (active_engine != NULL) {
        copy_error(error, error_capacity,
                   "another BrowserEngine is already active");
        return NULL;
    }
    BrowserEngine *engine = calloc(1, sizeof(*engine));
    if (engine == NULL) {
        copy_error(error, error_capacity,
                   "could not allocate the fixed browser control block");
        return NULL;
    }
    engine->config = *config;
    engine->state = BROWSER_ENGINE_ACTIVE;
    tilefinch_diagnostics_init(
        &engine->diagnostics, config->diagnostics.callback,
        config->diagnostics.opaque, config->diagnostics.minimum_severity);
    clear_error(engine);
    budget_init(&engine->budget, config->memory_limit);
    if (!budget_install_lexbor(&engine->budget)) {
        set_error_code(engine, TILEFINCH_SUBSYSTEM_ENGINE,
                       TILEFINCH_DIAGNOSTIC_INTERNAL_FAILED,
                       "lexbor-owner",
                       "another live Lexbor allocation owner is installed");
        goto failed;
    }
    active_engine = engine;

    if (!browser_session_init(&engine->session, &engine->budget,
                              config->session_cache_limit)) {
        set_error_code(engine, TILEFINCH_SUBSYSTEM_STORAGE,
                       TILEFINCH_DIAGNOSTIC_ALLOCATION_FAILED,
                       "session-init",
                       "browser session initialization failed");
        goto failed;
    }
    engine->session_ready = true;
    engine->content_blocker = content_blocker_create(&engine->budget);
    if (engine->content_blocker == NULL) {
        set_error_code(engine, TILEFINCH_SUBSYSTEM_STORAGE,
                       TILEFINCH_DIAGNOSTIC_ALLOCATION_FAILED,
                       "content-blocker-init",
                       "content blocker control allocation failed");
        goto failed;
    }
    engine->session.content_blocker = engine->content_blocker;
    if (!navigation_init(&engine->navigation, &engine->budget,
                         config->history_capacity)) {
        set_error_code(engine, TILEFINCH_SUBSYSTEM_ENGINE,
                       TILEFINCH_DIAGNOSTIC_ALLOCATION_FAILED,
                       "navigation-init", "navigation initialization failed");
        goto failed;
    }
    engine->navigation_ready = true;
    if (!browser_engine_configure_navigation(engine)) goto failed;

    if (config->fonts.enabled) {
        const BrowserFontConfig *fonts = &config->fonts;
        if (!font_set_load(
                &engine->fonts, &engine->budget, fonts->sans_path,
                fonts->serif_path, fonts->sans_italic_path,
                fonts->sans_bold_path, fonts->serif_bold_path,
                fonts->metric_sans_path, fonts->metric_sans_bold_path,
                fonts->maximum_total_bytes)) {
            set_error_code(engine, TILEFINCH_SUBSYSTEM_RENDER,
                           TILEFINCH_DIAGNOSTIC_ALLOCATION_FAILED,
                           "font-load", "bounded font loading failed");
            goto failed;
        }
        engine->fonts_ready = true;
    }

    if (config->tile_capacity != 0) {
        engine->framebuffer_pixels =
            (size_t) config->device.framebuffer_width
            * (size_t) config->device.framebuffer_height;
        engine->framebuffer = budget_calloc_category(
            &engine->budget, BUDGET_CATEGORY_RENDER,
            engine->framebuffer_pixels, sizeof(*engine->framebuffer));
        if (engine->framebuffer == NULL) {
            set_error_code(engine, TILEFINCH_SUBSYSTEM_RENDER,
                           TILEFINCH_DIAGNOSTIC_ALLOCATION_FAILED,
                           "framebuffer-allocation",
                           "bounded framebuffer allocation failed");
            goto failed;
        }
    }
    (void) emit_diagnostic(
        engine, TILEFINCH_DIAGNOSTIC_INFO,
        TILEFINCH_SUBSYSTEM_ENGINE, TILEFINCH_DIAGNOSTIC_LIFECYCLE,
        "engine-created", config->device.name, config->memory_limit,
        engine->framebuffer_pixels * sizeof(*engine->framebuffer));
    copy_error(error, error_capacity, "");
    return engine;

failed: {
        char retained[sizeof(engine->last_error)];
        snprintf(retained, sizeof(retained), "%s", engine->last_error);
        if (browser_engine_shutdown(engine)) free(engine);
        copy_error(error, error_capacity, retained);
        return NULL;
    }
}

bool browser_engine_set_javascript_enabled(
    BrowserEngine *engine, bool enabled)
{
    if (engine == NULL || engine->state != BROWSER_ENGINE_ACTIVE
        || browser_engine_navigation_pending(engine)) return false;
    engine->config.javascript.enabled = enabled;
    engine->config.javascript.document_scripts_enabled = enabled;
    navigation_enable_scripts(
        &engine->navigation,
        enabled ? engine->config.javascript.heap_limit : 0u,
        enabled ? engine->config.javascript.runtime_timeout_ms : 0u);
    navigation_enable_document_scripts(
        &engine->navigation,
        enabled ? engine->config.javascript.maximum_scripts : 0u,
        enabled ? engine->config.javascript.maximum_total_bytes : 0u,
        enabled ? engine->config.javascript.maximum_file_bytes : 0u,
        enabled ? engine->config.javascript.network_timeout_ms : 0);
    return true;
}

bool browser_engine_shutdown(BrowserEngine *engine)
{
    if (engine == NULL) return false;
    if (engine->state == BROWSER_ENGINE_SHUTDOWN) {
        if (active_engine == engine) {
            if (!budget_uninstall_lexbor(&engine->budget)) return false;
            active_engine = NULL;
        }
        return engine->budget.current == 0
            && budget_active_allocations(&engine->budget, NULL) == 0;
    }
    browser_engine_cancel_navigation(engine, "browser engine shutdown");
    browser_engine_find_clear(engine);
    if (engine->navigation_ready)
        document_backing_uninstall(&engine->navigation);
    document_backing_clear(&engine->backing);
    browser_engine_reset_provisional_viewport(engine);
    browser_engine_abort_candidate_shell(engine);
    browser_engine_reset_shell(engine);
    if (engine->navigation_ready) {
        (void) navigation_set_candidate_commit_hooks(
            &engine->navigation, NULL, NULL, NULL, NULL);
        (void) navigation_set_replacement_hooks(
            &engine->navigation, NULL, NULL, NULL);
        navigation_destroy(&engine->navigation);
    }
    engine->navigation_ready = false;
    if (engine->fonts_ready) font_set_destroy(&engine->fonts);
    engine->fonts_ready = false;
    if (engine->session_ready) engine->session.content_blocker = NULL;
    content_blocker_destroy(engine->content_blocker);
    engine->content_blocker = NULL;
    if (engine->session_ready) browser_session_destroy(&engine->session);
    engine->session_ready = false;
    budget_free(&engine->budget, engine->framebuffer);
    engine->framebuffer = NULL;
    engine->framebuffer_pixels = 0;
    engine->state = BROWSER_ENGINE_SHUTDOWN;
    bool clean = engine->budget.current == 0
        && budget_active_allocations(&engine->budget, NULL) == 0
        && budget_categories_reconcile(&engine->budget);
    if (!clean) {
        set_error_code(engine, TILEFINCH_SUBSYSTEM_ENGINE,
                       TILEFINCH_DIAGNOSTIC_INTERNAL_FAILED,
                       "engine-shutdown",
                       "browser engine teardown retained memory");
    } else {
        (void) emit_diagnostic(
            engine, TILEFINCH_DIAGNOSTIC_INFO,
            TILEFINCH_SUBSYSTEM_ENGINE, TILEFINCH_DIAGNOSTIC_LIFECYCLE,
            "engine-shutdown", "clean bounded teardown", 0, 0);
    }
    if (active_engine == engine && clean) {
        clean = budget_uninstall_lexbor(&engine->budget);
        if (clean) active_engine = NULL;
    }
    return clean;
}

void browser_engine_destroy(BrowserEngine *engine)
{
    if (engine == NULL) return;
    if (browser_engine_shutdown(engine)) free(engine);
}

const BrowserConfig *browser_engine_config(const BrowserEngine *engine)
{
    return engine == NULL ? NULL : &engine->config;
}

const char *browser_engine_last_error(const BrowserEngine *engine)
{
    return engine == NULL ? "browser engine is null" : engine->last_error;
}

TilefinchDiagnosticCode browser_engine_last_diagnostic_code(
    const BrowserEngine *engine)
{
    return engine == NULL ? TILEFINCH_DIAGNOSTIC_INVALID_INPUT
                          : engine->last_diagnostic_code;
}

bool browser_engine_metrics(const BrowserEngine *engine,
                            BrowserEngineMetrics *metrics)
{
    if (engine == NULL || metrics == NULL) return false;
    memset(metrics, 0, sizeof(*metrics));
    metrics->state = engine->state;
    metrics->control_bytes = sizeof(*engine);
    metrics->framebuffer_bytes = engine->framebuffer_pixels
                                 * sizeof(*engine->framebuffer);
    metrics->non_page_memory_reserve =
        engine->config.non_page_memory_reserve;
    metrics->budget_limit = engine->budget.limit;
    metrics->budget_current = engine->budget.current;
    metrics->budget_peak = engine->budget.peak;
    metrics->budget_external_reserved = engine->budget.external_reserved;
    metrics->budget_external_reserved_peak =
        engine->budget.external_reserved_peak;
    metrics->session_inline_accounting_bytes =
        engine->session.accounting_bytes;
    metrics->budget_active_allocations = budget_active_allocations(
        &engine->budget, &metrics->budget_largest_allocation);
    metrics->allocation_failures = engine->budget.failure_count;
    metrics->diagnostics_emitted = engine->diagnostics.emitted;
    metrics->diagnostics_filtered = engine->diagnostics.filtered;
    metrics->last_diagnostic_subsystem = engine->last_diagnostic_subsystem;
    metrics->last_diagnostic_code = engine->last_diagnostic_code;
    metrics->fonts_ready = engine->fonts_ready;
    metrics->controller_ready = engine->controller_ready;
    metrics->render_ready = engine->render_ready;
    metrics->backing_kind = document_backing_kind(&engine->backing);
    metrics->section_count = document_backing_section_count(&engine->backing);
    metrics->current_section = document_backing_current_section(
        &engine->backing);
    if (engine->navigation_ready) {
        const NavigationSession *navigation = &engine->navigation;
        metrics->page_loaded = navigation->page.loaded;
        metrics->loads_started = navigation->loads_started;
        metrics->loads_committed = navigation->loads_committed;
        metrics->loads_cancelled = navigation->loads_cancelled;
        metrics->pages_destroyed = navigation->page_destroys;
        metrics->navigation = navigation->performance;
    }
    if (engine->controller_ready) {
        metrics->controller_focus_moves = engine->controller.focus_moves;
        metrics->controller_activations = engine->controller.activations;
        metrics->controller_text_edits = engine->controller.text_edits;
    }
    if (engine->render_ready) {
        metrics->rendered_frames = engine->render.frames_rendered;
        metrics->unchanged_runtime_frames_suppressed =
            engine->unchanged_runtime_frames_suppressed;
        metrics->maximum_render_us = engine->render.max_frame_us;
        metrics->idle_jobs_scheduled = engine->render.idle_jobs_scheduled;
        metrics->idle_jobs_completed = engine->render.idle_jobs_completed;
        metrics->idle_jobs_cancelled = engine->render.idle_jobs_cancelled;
        metrics->idle_slices = engine->render.idle_slices;
        metrics->idle_slice_overruns = engine->render.idle_slice_overruns;
        metrics->idle_image_admission_skips =
            engine->render.idle_image_admission_skips;
        metrics->idle_glyphs_prewarmed =
            engine->render.idle_glyphs_prewarmed;
        metrics->idle_glyph_cache_misses =
            engine->render.idle_glyph_cache_misses;
        metrics->idle_work_us = engine->render.idle_us;
        metrics->maximum_idle_slice_us = engine->render.max_idle_slice_us;
        metrics->maximum_idle_unit_us = engine->render.max_idle_unit_us;
        metrics->startup_visual_slices =
            engine->render.startup_visual_slices;
        metrics->startup_visual_us = engine->render.startup_visual_us;
        metrics->maximum_startup_visual_slice_us =
            engine->render.max_startup_visual_slice_us;
        metrics->maximum_startup_visual_unit_us =
            engine->render.max_startup_visual_unit_us;
    }
    return true;
}

bool browser_engine_content_blocker_configure(
    BrowserEngine *engine, ContentBlockerMode mode,
    const char *custom_path)
{
    return engine != NULL && engine->state == BROWSER_ENGINE_ACTIVE
        && content_blocker_configure(
               engine->content_blocker, mode, custom_path);
}

bool browser_engine_content_blocker_set_allowed_sites(
    BrowserEngine *engine, const char *const *sites, size_t count)
{
    return engine != NULL && engine->state == BROWSER_ENGINE_ACTIVE
        && content_blocker_set_allowed_sites(
               engine->content_blocker, sites, count);
}

bool browser_engine_content_blocker_metrics(
    const BrowserEngine *engine, ContentBlockerMetrics *metrics)
{
    return engine != NULL && engine->state == BROWSER_ENGINE_ACTIVE
        && content_blocker_metrics(engine->content_blocker, metrics);
}

bool browser_engine_refresh_shell(BrowserEngine *engine)
{
    if (engine == NULL || engine->state != BROWSER_ENGINE_ACTIVE
        || !engine->navigation_ready || !engine->navigation.page.loaded) {
        return set_error_code(
            engine, TILEFINCH_SUBSYSTEM_ENGINE,
            TILEFINCH_DIAGNOSTIC_INVALID_INPUT, "shell-refresh",
            "no committed page is available");
    }
    browser_engine_reset_shell(engine);
    if (!controller_init(&engine->controller, &engine->navigation))
        return set_error_code(
            engine, TILEFINCH_SUBSYSTEM_ENGINE,
            TILEFINCH_DIAGNOSTIC_ALLOCATION_FAILED, "controller-init",
            "controller initialization failed");
    engine->controller_ready = true;
    return browser_engine_initialize_render_shell(
        engine, "render-shell-init",
        "render shell initialization failed");
}

bool browser_engine_refresh_render_shell(BrowserEngine *engine)
{
    if (engine == NULL || engine->state != BROWSER_ENGINE_ACTIVE
        || !engine->navigation_ready || !engine->navigation.page.loaded
        || engine->config.tile_capacity == 0) {
        return set_error_code(
            engine, TILEFINCH_SUBSYSTEM_RENDER,
            TILEFINCH_DIAGNOSTIC_INVALID_INPUT, "render-shell-refresh",
            "no engine-owned render shell can be refreshed");
    }
    browser_engine_cancel_idle_work(engine);
    browser_engine_reset_render_shell(engine);
    return browser_engine_initialize_render_shell(
        engine, "render-shell-refresh", "render shell refresh failed");
}

bool browser_engine_apply_layout_damage(BrowserEngine *engine)
{
    if (engine == NULL || engine->state != BROWSER_ENGINE_ACTIVE
        || !engine->navigation_ready || !engine->navigation.page.loaded
        || !engine->render_ready) {
        return set_error_code(
            engine, TILEFINCH_SUBSYSTEM_RENDER,
            TILEFINCH_DIAGNOSTIC_INVALID_INPUT, "render-layout-damage",
            "render shell is not ready for layout damage");
    }
    if (engine->render_relayout_generation
        == engine->navigation.incremental_relayouts) {
        clear_error(engine);
        return true;
    }
    browser_engine_cancel_idle_work(engine);
    if (!tile_cache_replace_layout_damage(
            &engine->render, &engine->navigation.page.layout,
            engine->navigation.relayout_damage_left,
            engine->navigation.relayout_damage_top,
            engine->navigation.relayout_damage_right,
            engine->navigation.relayout_damage_bottom)) {
        return set_error_code(
            engine, TILEFINCH_SUBSYSTEM_RENDER,
            TILEFINCH_DIAGNOSTIC_RENDER_FAILED, "render-layout-damage",
            "render shell could not apply layout damage");
    }
    engine->render_relayout_generation =
        engine->navigation.incremental_relayouts;
    engine->render_focus_paint_generation =
        engine->navigation.focus_paint_generation;
    clear_error(engine);
    return true;
}

bool browser_engine_set_forced_dark(BrowserEngine *engine, bool enabled)
{
    if (engine == NULL || engine->state != BROWSER_ENGINE_ACTIVE) {
        return false;
    }
    if (engine->forced_dark == enabled) return true;
    engine->forced_dark = enabled;
    /*
     * Provisional frames are immutable snapshots. Retaining one across an
     * appearance change would briefly reintroduce the old palette during a
     * pending navigation, so discard it rather than post-processing pixels
     * whose image provenance has already been lost.
     */
    browser_engine_reset_provisional_viewport(engine);
    if (engine->render_ready) {
        tile_cache_set_forced_dark(&engine->render, enabled);
    }
    if (engine->candidate_render_ready) {
        tile_cache_set_forced_dark(&engine->candidate_render, enabled);
    }
    clear_error(engine);
    return true;
}

bool browser_engine_optional_glyphs_updated(BrowserEngine *engine)
{
    if (engine == NULL || engine->state != BROWSER_ENGINE_ACTIVE)
        return false;
    browser_engine_reset_provisional_viewport(engine);
    if (engine->render_ready)
        tile_cache_invalidate_glyphs(&engine->render);
    if (engine->candidate_render_ready)
        tile_cache_invalidate_glyphs(&engine->candidate_render);
    clear_error(engine);
    return true;
}

bool browser_engine_optional_glyph_payloads_ready(BrowserEngine *engine)
{
    if (engine == NULL || engine->state != BROWSER_ENGINE_ACTIVE)
        return false;
    browser_engine_reset_provisional_viewport(engine);
    if (engine->render_ready)
        tile_cache_repaint_glyphs(&engine->render);
    if (engine->candidate_render_ready)
        tile_cache_repaint_glyphs(&engine->candidate_render);
    clear_error(engine);
    return true;
}

bool browser_engine_set_youtube_compact_results(
    BrowserEngine *engine, bool compact)
{
    if (engine == NULL || engine->state != BROWSER_ENGINE_ACTIVE) {
        return false;
    }
    engine->youtube_compact_results = compact;
    clear_error(engine);
    return true;
}

static bool browser_engine_apply_focus_paint(BrowserEngine *engine)
{
    if (engine == NULL || !engine->render_ready
        || engine->render_focus_paint_generation
               == engine->navigation.focus_paint_generation) return true;
    if (!engine->navigation.focus_paint_damage_valid
        || !tile_cache_sync_layout_paint(
            &engine->render,
            engine->navigation.focus_paint_damage_left,
            engine->navigation.focus_paint_damage_top,
            engine->navigation.focus_paint_damage_right,
            engine->navigation.focus_paint_damage_bottom)) {
        return set_error_code(
            engine, TILEFINCH_SUBSYSTEM_RENDER,
            TILEFINCH_DIAGNOSTIC_RENDER_FAILED, "render-focus-paint",
            "render shell could not apply focus paint damage");
    }
    engine->render_focus_paint_generation =
        engine->navigation.focus_paint_generation;
    return true;
}

bool browser_engine_bind_document_backing(BrowserEngine *engine,
                                          const DocumentBacking *backing)
{
    if (engine == NULL || engine->state != BROWSER_ENGINE_ACTIVE
        || !engine->navigation_ready
        || !document_backing_valid(backing)) {
        return set_error_code(
            engine, TILEFINCH_SUBSYSTEM_SECTIONS,
            TILEFINCH_DIAGNOSTIC_INVALID_INPUT, "backing-bind",
            "invalid document backing");
    }
    if (backing->kind == DOCUMENT_BACKING_FULL
        && (!engine->navigation.page.loaded
            || backing->full_document != &engine->navigation.page.document)) {
        return set_error_code(
            engine, TILEFINCH_SUBSYSTEM_SECTIONS,
            TILEFINCH_DIAGNOSTIC_INVALID_INPUT, "backing-bind",
            "full backing must reference the committed document");
    }
    DocumentBacking previous = engine->backing;
    engine->backing = *backing;
    if (!document_backing_install(&engine->backing, &engine->navigation)) {
        engine->backing = previous;
        if (document_backing_valid(&engine->backing))
            (void) document_backing_install(
                &engine->backing, &engine->navigation);
        return set_error_code(
            engine, TILEFINCH_SUBSYSTEM_SECTIONS,
            TILEFINCH_DIAGNOSTIC_INTERNAL_FAILED, "backing-bind",
            "document backing installation failed");
    }
    clear_error(engine);
    return true;
}

static bool browser_engine_bind_current_full_document(BrowserEngine *engine)
{
    DocumentBacking full;
    return document_backing_init_full(
               &full, &engine->navigation.page.document)
        && browser_engine_bind_document_backing(engine, &full);
}

static void browser_engine_navigation_restore_history(
    BrowserEngine *engine, BrowserEngineNavigationWork *work)
{
    if (engine == NULL || work == NULL || !work->history_move
        || engine->navigation.history_count == 0
        || engine->navigation.history_index == work->previous_history_index) {
        return;
    }
    const NavigationEntry *ignored = NULL;
    (void) (work->history_forward
        ? navigation_back(&engine->navigation, &ignored)
        : navigation_forward(&engine->navigation, &ignored));
}

static lxb_dom_node_t *browser_engine_semantic_focus(
    const BrowserEngine *engine, int kind, size_t ordinal)
{
    const LayoutDocument *layout = &engine->navigation.page.layout;
    if (kind == CONTROLLER_FOCUS_CONTROL) {
        return ordinal < layout->control_count
            ? layout->controls[ordinal].node : NULL;
    }
    if (kind != CONTROLLER_FOCUS_LINK || layout->link_count == 0) return NULL;
    size_t i = 0;
    while (ordinal != 0 && ++i < layout->link_count) {
        if (layout->links[i].node != layout->links[i - 1u].node) ordinal--;
    }
    return ordinal == 0 ? layout->links[i].node : NULL;
}

static size_t browser_engine_focus_ordinal(const BrowserEngine *engine)
{
    int kind = engine->controller.focus_kind;
    const LayoutDocument *layout = &engine->navigation.page.layout;
    size_t raw = engine->controller.focus_index;
    if (kind != CONTROLLER_FOCUS_LINK || raw >= layout->link_count) return raw;
    size_t ordinal = 0;
    for (size_t i = 1; i <= raw; i++) {
        if (layout->links[i].node != layout->links[i - 1u].node) ordinal++;
    }
    return ordinal;
}

static void browser_engine_navigation_restore_view(
    BrowserEngine *engine, const BrowserEngineNavigationWork *work)
{
    if (engine == NULL || work == NULL || !work->history_move) return;
    lxb_dom_node_t *focus = browser_engine_semantic_focus(
        engine, work->restore_focus_kind, work->restore_focus_index);
    if (focus != NULL) (void) controller_focus_node(&engine->controller, focus);
    /* Focus notifications may relayout or reveal. History owns the exact
       viewport, so apply its saved position after those side effects. */
    (void) navigation_set_scroll(
        &engine->navigation, work->restore_scroll_y);
}

static bool browser_engine_node_has_autofocus(lxb_dom_node_t *node)
{
    return node != NULL && node->type == LXB_DOM_NODE_TYPE_ELEMENT
        && lxb_dom_element_has_attribute(
            lxb_dom_interface_element(node),
            (const lxb_char_t *) "autofocus", 9);
}

static void browser_engine_apply_autofocus(BrowserEngine *engine)
{
    if (engine == NULL) return;
    const LayoutDocument *layout = &engine->navigation.page.layout;
    for (size_t at = 0; at < layout->link_count; at++) {
        lxb_dom_node_t *node = layout->links[at].node;
        if (node != NULL && browser_engine_node_has_autofocus(node)) {
            (void) controller_focus_node(&engine->controller, node);
            return;
        }
    }
    for (size_t at = 0; at < layout->control_count; at++) {
        lxb_dom_node_t *node = layout->controls[at].node;
        if (node != NULL && browser_engine_node_has_autofocus(node)) {
            (void) controller_focus_node(&engine->controller, node);
            return;
        }
    }
}

static BrowserNavigationJobStatus browser_engine_finish_navigation_work(
    BrowserEngine *engine, NavigationLoadStatus terminal)
{
    BrowserEngineNavigationWork *work = &engine->navigation_work;
    (void) navigation_load_metrics(work->load, &work->metrics.load);
    if (work->adapter != NULL) {
        SiteAdapterLoadMetrics adapter = {0};
        if (site_adapter_load_metrics(work->adapter, &adapter)) {
            work->metrics.completion_per_mille =
                adapter.completion_per_mille;
            work->metrics.load.pump_calls = adapter.network_pumps;
            work->metrics.load.quota_yields = adapter.quota_yields;
            work->metrics.load.body_bytes = adapter.body_bytes;
            work->metrics.load.body_callbacks = adapter.body_callbacks;
            work->metrics.load.peak_buffered_bytes =
                adapter.peak_buffered_bytes;
            work->metrics.load.total_pump_us = adapter.network_us;
            work->metrics.load.maximum_pump_us =
                adapter.maximum_pump_us;
            work->metrics.maximum_transform_slice_us =
                adapter.maximum_transform_slice_us;
            work->metrics.transform_slices =
                adapter.build_slices;
            work->metrics.transform_quota_overruns =
                adapter.transform_quota_overruns;
            if (adapter.maximum_irreducible_unit_us
                    > work->metrics.maximum_irreducible_unit_us) {
                work->metrics.maximum_irreducible_unit_us =
                    adapter.maximum_irreducible_unit_us;
            }
        }
    }
    uint64_t finished_us =
        tilefinch_platform_monotonic_time_us();
    work->metrics.elapsed_us =
        finished_us >= work->started_us
            ? finished_us - work->started_us : 0;
    bool succeeded = terminal == NAVIGATION_LOAD_SUCCEEDED
        && engine->navigation.page.loaded;
    if (work->hooks_installed) {
        (void) navigation_set_candidate_commit_hooks(
            &engine->navigation, NULL, NULL, NULL, NULL);
        work->hooks_installed = false;
    }
    if (engine->candidate_shell_prepared
        || engine->candidate_render.budget != NULL) {
        browser_engine_abort_candidate_shell(engine);
    }
    if (succeeded) {
        succeeded = browser_engine_bind_current_full_document(engine);
    } else if (engine->navigation.page.loaded
               && work->previous_backing_valid) {
        engine->backing = work->previous_backing;
        (void) document_backing_install(
            &engine->backing, &engine->navigation);
        browser_engine_navigation_restore_history(engine, work);
    } else {
        document_backing_clear(&engine->backing);
    }
    bool had_provisional = engine->provisional_ready;
    int provisional_scroll_y = had_provisional
        ? engine->provisional_scroll_positions[
              engine->provisional_current_frame]
        : 0;
    navigation_load_destroy(work->load);
    work->load = NULL;
    site_adapter_load_destroy(work->adapter);
    work->adapter = NULL;
    if (succeeded) {
        browser_engine_navigation_restore_view(engine, work);
        if (!work->history_move) browser_engine_apply_autofocus(engine);
        if (had_provisional && provisional_scroll_y > 0) {
            (void) navigation_set_scroll(
                &engine->navigation, provisional_scroll_y);
            work->metrics.provisional_scroll_y =
                navigation_current(&engine->navigation) == NULL
                    ? 0
                    : navigation_current(
                          &engine->navigation)->scroll_y;
        }
        work->status = BROWSER_NAVIGATION_JOB_SUCCEEDED;
        work->metrics.status = work->status;
        work->metrics.completion_per_mille = 1000;
        clear_error(engine);
        (void) emit_diagnostic(
            engine, TILEFINCH_DIAGNOSTIC_INFO,
            TILEFINCH_SUBSYSTEM_ENGINE, TILEFINCH_DIAGNOSTIC_LIFECYCLE,
            "navigation-committed", "candidate load committed",
            engine->navigation.loads_committed,
            engine->navigation.page.document.node_count);
    } else if (terminal == NAVIGATION_LOAD_CANCELLED) {
        work->status = BROWSER_NAVIGATION_JOB_CANCELLED;
        work->metrics.status = work->status;
        snprintf(engine->last_error, sizeof(engine->last_error), "%.255s",
                 engine->navigation.last_error[0] == '\0'
                   ? "navigation cancelled"
                   : engine->navigation.last_error);
        (void) emit_diagnostic(
            engine, TILEFINCH_DIAGNOSTIC_INFO,
            TILEFINCH_SUBSYSTEM_ENGINE, TILEFINCH_DIAGNOSTIC_CANCELLED,
            "navigation-cancelled", engine->last_error, 0, 0);
    } else {
        const char *message = engine->navigation.last_error[0] != '\0'
            ? engine->navigation.last_error
            : (engine->last_error[0] != '\0'
               ? engine->last_error : "navigation failed");
        work->status = BROWSER_NAVIGATION_JOB_FAILED;
        work->metrics.status = work->status;
        (void) set_error_code(
            engine, work->failure_subsystem, work->failure_code,
            "navigation-failed", message);
    }
    if (!succeeded && had_provisional && engine->framebuffer != NULL) {
        (void) tilefinch_platform_present_rgb565(
            engine->framebuffer,
            (size_t) engine->config.device.framebuffer_width,
            (size_t) engine->config.device.framebuffer_height,
            (size_t) engine->config.device.framebuffer_width);
    }
    browser_engine_reset_provisional_viewport(engine);
    return work->status;
}

static bool browser_engine_begin_navigation_request(
    BrowserEngine *engine, const char *url, const char *method,
    const char *body, size_t body_length, const char *content_type,
    size_t maximum_bytes, long timeout_ms, bool record_history)
{
    if (engine == NULL || engine->state != BROWSER_ENGINE_ACTIVE
        || !engine->navigation_ready || url == NULL || method == NULL
        || url[0] == '\0' || strlen(url) >= NAVIGATION_URL_LIMIT
        || maximum_bytes == 0 || timeout_ms <= 0
        || browser_engine_navigation_pending(engine)) {
        return set_error_code(
            engine, TILEFINCH_SUBSYSTEM_ENGINE,
            TILEFINCH_DIAGNOSTIC_INVALID_INPUT, "navigation-start",
            "browser navigation job cannot be started");
    }
    /* Release incumbent-page transport lanes before the candidate scheduler
       tries to enqueue its authoritative document request. On PSP the six
       shared descriptors can otherwise all belong to thumbnails, fonts or
       script fetches from the page whose link was just activated. */
    (void) navigation_cancel_network_work(
        &engine->navigation, "superseded by a new navigation");
    /* A candidate navigation keeps the incumbent page alive. End any gesture
       against that page before input is suppressed, so a release after a
       cancelled navigation cannot turn into a stale click. */
    if (engine->controller_ready
        && engine->controller.pointer_down_active) {
        size_t relayouts = engine->navigation.incremental_relayouts;
        bool ignored_activation = false;
        (void) controller_pointer_event(
            &engine->controller, CONTROLLER_POINTER_CANCEL, 0, 0,
            &ignored_activation);
        if (engine->render_ready
            && relayouts != engine->navigation.incremental_relayouts) {
            (void) browser_engine_apply_layout_damage(engine);
        }
    }
    controller_pointer_discard_click(&engine->controller);
    BrowserEngineNavigationWork *work = &engine->navigation_work;
    *work = (BrowserEngineNavigationWork) {
        .status = BROWSER_NAVIGATION_JOB_PENDING,
        .failure_subsystem = TILEFINCH_SUBSYSTEM_NETWORK,
        .failure_code = TILEFINCH_DIAGNOSTIC_NETWORK_FAILED,
        .previous_backing = engine->backing,
        .previous_backing_valid =
            document_backing_valid(&engine->backing),
        .started_us =
            tilefinch_platform_monotonic_time_us(),
        .record_history = record_history
    };
    snprintf(work->url, sizeof(work->url), "%s", url);
    work->metrics.status = BROWSER_NAVIGATION_JOB_PENDING;
    if (engine->navigation.page.loaded)
        work->metrics.incumbent_pages_preserved = 1;
    browser_engine_cancel_idle_work(engine);
    browser_engine_cancel_render_job(engine);
    browser_engine_reset_provisional_viewport(engine);
    (void) emit_diagnostic(
        engine, TILEFINCH_DIAGNOSTIC_INFO,
        TILEFINCH_SUBSYSTEM_ENGINE, TILEFINCH_DIAGNOSTIC_LIFECYCLE,
        "navigation-start", "candidate load started",
        engine->navigation.loads_started + 1, 0);
    document_backing_uninstall(&engine->navigation);
    if (!navigation_set_candidate_commit_hooks(
            &engine->navigation, browser_engine_prepare_candidate_shell,
            browser_engine_abort_candidate_shell,
            browser_engine_commit_candidate_shell, engine)) {
        if (work->previous_backing_valid) {
            engine->backing = work->previous_backing;
            (void) document_backing_install(
                &engine->backing, &engine->navigation);
        }
        work->status = BROWSER_NAVIGATION_JOB_FAILED;
        work->metrics.status = work->status;
        return set_error_code(
            engine, TILEFINCH_SUBSYSTEM_ENGINE,
            TILEFINCH_DIAGNOSTIC_INTERNAL_FAILED, "candidate-shell-hook",
            "could not install the candidate render-shell transaction");
    }
    work->hooks_installed = true;
    uint64_t generation = navigation_begin(&engine->navigation);
    work->generation = generation;
    uint64_t navigation_started_us =
        tilefinch_platform_monotonic_time_us();
    work->metrics.navigation_session_started_us =
        navigation_started_us >= work->started_us
            ? navigation_started_us - work->started_us : 0;
    if (site_adapter_handles_navigation(method, url)) {
        char error[256] = {0};
        SiteAdapterPreferences preferences = {
            .youtube_compact_results = engine->youtube_compact_results
        };
        work->adapter = site_adapter_load_begin(
            &engine->budget, &engine->session, method, url, &preferences,
            maximum_bytes, timeout_ms, error, sizeof(error));
        if (work->adapter == NULL) {
            snprintf(engine->navigation.last_error,
                     sizeof(engine->navigation.last_error), "%.255s",
                     error[0] == '\0'
                         ? "site adapter load allocation failed" : error);
            (void) browser_engine_finish_navigation_work(
                engine, NAVIGATION_LOAD_FAILED);
            return false;
        }
        return true;
    }
    work->load = navigation_load_begin_request(
        &engine->navigation, generation, url, method, body, body_length,
        content_type, maximum_bytes, timeout_ms,
        engine->config.device.navigation_viewport_width,
        engine->fonts_ready ? &engine->fonts : NULL, NULL, record_history);
    if (work->load == NULL) {
        if (engine->navigation.last_error[0] == '\0') {
            snprintf(engine->navigation.last_error,
                     sizeof(engine->navigation.last_error), "%s",
                     "navigation load allocation failed");
        }
        (void) browser_engine_finish_navigation_work(
            engine, NAVIGATION_LOAD_FAILED);
        return false;
    }
    return true;
}

bool browser_engine_begin_navigation_url(
    BrowserEngine *engine, const char *url, size_t maximum_bytes,
    long timeout_ms, bool record_history)
{
    return browser_engine_begin_navigation_request(
        engine, url, "GET", NULL, 0, NULL,
        maximum_bytes, timeout_ms, record_history);
}

bool browser_engine_begin_navigation_action(
    BrowserEngine *engine, const ControllerAction *action,
    size_t maximum_bytes, long timeout_ms)
{
    if (engine == NULL || action == NULL
        || (action->type != CONTROLLER_ACTION_NAVIGATE
            && action->type != CONTROLLER_ACTION_FORM_SUBMIT)) {
        return set_error_code(
            engine, TILEFINCH_SUBSYSTEM_ENGINE,
            TILEFINCH_DIAGNOSTIC_INVALID_INPUT, "navigation-action",
            "controller action does not contain a navigation");
    }
    const NavigationEntry *current =
        navigation_current(&engine->navigation);
    if (current != NULL) {
        snprintf(engine->navigation.pending_navigation_referer,
                 sizeof(engine->navigation.pending_navigation_referer),
                 "%s", current->url);
    }
    const char *method = action->type == CONTROLLER_ACTION_FORM_SUBMIT
        ? action->method : "GET";
    return browser_engine_begin_navigation_request(
        engine, action->url, method,
        action->body_length == 0 ? NULL : action->body,
        action->body_length,
        action->content_type[0] == '\0' ? NULL : action->content_type,
        maximum_bytes, timeout_ms, true);
}

bool browser_engine_begin_navigation_history(
    BrowserEngine *engine, bool forward, size_t maximum_bytes,
    long timeout_ms)
{
    if (engine == NULL || engine->state != BROWSER_ENGINE_ACTIVE
        || !engine->navigation_ready
        || browser_engine_navigation_pending(engine)) {
        return set_error_code(
            engine, TILEFINCH_SUBSYSTEM_ENGINE,
            TILEFINCH_DIAGNOSTIC_INVALID_INPUT, "navigation-history",
            "history navigation cannot be started");
    }
    size_t previous_index = engine->navigation.history_index;
    const NavigationEntry *entry = NULL;
    bool moved = forward
        ? navigation_forward(&engine->navigation, &entry)
        : navigation_back(&engine->navigation, &entry);
    if (!moved || entry == NULL) {
        return set_error_code(
            engine, TILEFINCH_SUBSYSTEM_ENGINE,
            TILEFINCH_DIAGNOSTIC_INVALID_INPUT, "navigation-history",
            forward ? "no forward history entry is available"
                    : "no back history entry is available");
    }
    char url[NAVIGATION_URL_LIMIT];
    snprintf(url, sizeof(url), "%s", entry->url);
    int scroll_y = entry->scroll_y;
    int focus_kind = entry->focus_kind;
    size_t focus_index = entry->focus_index;
    if (!browser_engine_begin_navigation_url(
            engine, url, maximum_bytes, timeout_ms, false)) {
        const NavigationEntry *ignored = NULL;
        (void) (forward
            ? navigation_back(&engine->navigation, &ignored)
            : navigation_forward(&engine->navigation, &ignored));
        return false;
    }
    BrowserEngineNavigationWork *work = &engine->navigation_work;
    work->history_move = true;
    work->history_forward = forward;
    work->previous_history_index = previous_index;
    work->restore_scroll_y = scroll_y;
    work->restore_focus_kind = focus_kind;
    work->restore_focus_index = focus_index;
    return true;
}

BrowserNavigationJobStatus browser_engine_pump_navigation(
    BrowserEngine *engine, const BrowserNavigationJobQuota *quota)
{
    if (engine == NULL) return BROWSER_NAVIGATION_JOB_FAILED;
    BrowserEngineNavigationWork *work = &engine->navigation_work;
    if (work->status != BROWSER_NAVIGATION_JOB_PENDING)
        return work->status;
    if (work->adapter != NULL) {
        uint64_t started_us =
            tilefinch_platform_monotonic_time_us();
        work->metrics.pump_calls++;
        const FetchPumpQuota *fetch_quota =
            quota == NULL ? NULL : &quota->load.fetch;
        SiteAdapterLoadStatus adapter_status =
            site_adapter_load_pump(work->adapter, fetch_quota);
        if (adapter_status == SITE_ADAPTER_LOAD_PENDING)
            return BROWSER_NAVIGATION_JOB_PENDING;
        SiteAdapterDocument document = {0};
        bool acquired = adapter_status == SITE_ADAPTER_LOAD_SUCCEEDED
            && site_adapter_load_take_document(work->adapter, &document);
        bool committed = acquired && navigation_commit_static_html(
            &engine->navigation, work->generation, work->url,
            document.html, document.html_length,
            engine->config.device.navigation_viewport_width,
            engine->fonts_ready ? &engine->fonts : NULL, NULL,
            work->record_history);
        if (acquired) {
            engine->navigation.last_http_status = document.status_code;
            snprintf(engine->navigation.last_server,
                     sizeof(engine->navigation.last_server), "%s",
                     document.server);
            snprintf(engine->navigation.last_cf_mitigated,
                     sizeof(engine->navigation.last_cf_mitigated), "%s",
                     document.cf_mitigated);
        }
        site_adapter_document_destroy(&document);
        uint64_t finished_us =
            tilefinch_platform_monotonic_time_us();
        uint64_t unit_us = finished_us >= started_us
            ? finished_us - started_us : 0;
        SiteAdapterLoadMetrics adapter_metrics = {0};
        (void) site_adapter_load_metrics(
            work->adapter, &adapter_metrics);
        work->metrics.maximum_irreducible_unit_us =
            unit_us > adapter_metrics.maximum_irreducible_unit_us
                ? unit_us : adapter_metrics.maximum_irreducible_unit_us;
        work->metrics.maximum_transform_slice_us =
            adapter_metrics.maximum_transform_slice_us;
        work->metrics.transform_slices =
            adapter_metrics.build_slices;
        work->metrics.transform_quota_overruns =
            adapter_metrics.transform_quota_overruns;
        uint64_t advisory_us = quota == NULL
            ? 0 : quota->load.maximum_parser_time_us;
        if (advisory_us != 0
            && work->metrics.maximum_irreducible_unit_us
                   > advisory_us) {
            work->metrics.irreducible_unit_overruns++;
        }
        if (!acquired) {
            const char *adapter_error =
                site_adapter_load_error(work->adapter);
            snprintf(engine->navigation.last_error,
                     sizeof(engine->navigation.last_error), "%.255s",
                     adapter_status == SITE_ADAPTER_LOAD_CANCELLED
                         ? "site adapter navigation cancelled"
                         : (adapter_error[0] == '\0'
                            ? "site adapter produced no document"
                            : adapter_error));
        }
        return browser_engine_finish_navigation_work(
            engine, committed
                ? NAVIGATION_LOAD_SUCCEEDED
                : (adapter_status == SITE_ADAPTER_LOAD_CANCELLED
                    ? NAVIGATION_LOAD_CANCELLED
                    : NAVIGATION_LOAD_FAILED));
    }
    if (work->load == NULL) return BROWSER_NAVIGATION_JOB_FAILED;
    NavigationLoadStatus status = navigation_load_status(work->load);
    const NavigationLoadQuota *load_quota =
        quota == NULL ? NULL : &quota->load;
    if (status == NAVIGATION_LOAD_PENDING) {
        work->metrics.pump_calls++;
        status = navigation_load_pump(work->load, load_quota);
        if (status == NAVIGATION_LOAD_PENDING
            || status == NAVIGATION_LOAD_READY_TO_FINISH) {
            return BROWSER_NAVIGATION_JOB_PENDING;
        }
    } else if (status == NAVIGATION_LOAD_READY_TO_FINISH
               || status == NAVIGATION_LOAD_FINALIZING) {
        work->metrics.pump_calls++;
        (void) navigation_load_finish(work->load, load_quota);
        status = navigation_load_status(work->load);
        if (status == NAVIGATION_LOAD_READY_TO_FINISH
            || status == NAVIGATION_LOAD_FINALIZING) {
            return BROWSER_NAVIGATION_JOB_PENDING;
        }
    }
    return browser_engine_finish_navigation_work(engine, status);
}

void browser_engine_cancel_navigation(BrowserEngine *engine,
                                      const char *reason)
{
    if (engine == NULL
        || engine->navigation_work.status
               != BROWSER_NAVIGATION_JOB_PENDING) return;
    const char *message = reason == NULL
        ? "navigation cancelled by browser UI" : reason;
    if (engine->navigation_work.adapter != NULL) {
        site_adapter_load_cancel(
            engine->navigation_work.adapter, message);
        snprintf(engine->navigation.last_error,
                 sizeof(engine->navigation.last_error), "%s", message);
    } else {
        navigation_load_cancel(engine->navigation_work.load, message);
    }
    (void) browser_engine_finish_navigation_work(
        engine, NAVIGATION_LOAD_CANCELLED);
}

size_t browser_engine_cancel_network_work(
    BrowserEngine *engine, const char *reason)
{
    return engine == NULL ? 0
        : navigation_cancel_network_work(&engine->navigation, reason);
}

BrowserNavigationJobStatus browser_engine_navigation_status(
    const BrowserEngine *engine)
{
    return engine == NULL ? BROWSER_NAVIGATION_JOB_FAILED
                          : engine->navigation_work.status;
}

bool browser_engine_navigation_pending(const BrowserEngine *engine)
{
    return browser_engine_navigation_status(engine)
        == BROWSER_NAVIGATION_JOB_PENDING;
}

const char *browser_engine_pending_navigation_url(
    const BrowserEngine *engine)
{
    return browser_engine_navigation_pending(engine)
        ? engine->navigation_work.url : NULL;
}

bool browser_engine_navigation_job_metrics(
    const BrowserEngine *engine, BrowserNavigationJobMetrics *metrics)
{
    if (engine == NULL || metrics == NULL) return false;
    *metrics = engine->navigation_work.metrics;
    metrics->status = engine->navigation_work.status;
    if (engine->navigation_work.load != NULL) {
        (void) navigation_load_metrics(
            engine->navigation_work.load, &metrics->load);
        metrics->completion_per_mille =
            metrics->load.completion_per_mille;
    } else if (engine->navigation_work.adapter != NULL) {
        SiteAdapterLoadMetrics adapter = {0};
        if (site_adapter_load_metrics(
                engine->navigation_work.adapter, &adapter)) {
            metrics->completion_per_mille =
                adapter.completion_per_mille;
            metrics->load.pump_calls = adapter.network_pumps;
            metrics->load.quota_yields = adapter.quota_yields;
            metrics->load.body_bytes = adapter.body_bytes;
            metrics->load.body_callbacks = adapter.body_callbacks;
            metrics->load.peak_buffered_bytes =
                adapter.peak_buffered_bytes;
            metrics->load.total_pump_us = adapter.network_us;
            metrics->load.maximum_pump_us = adapter.maximum_pump_us;
            metrics->maximum_transform_slice_us =
                adapter.maximum_transform_slice_us;
            metrics->transform_slices = adapter.build_slices;
            metrics->transform_quota_overruns =
                adapter.transform_quota_overruns;
            if (adapter.maximum_irreducible_unit_us
                    > metrics->maximum_irreducible_unit_us) {
                metrics->maximum_irreducible_unit_us =
                    adapter.maximum_irreducible_unit_us;
            }
        }
    }
    if (engine->navigation_work.status
            == BROWSER_NAVIGATION_JOB_SUCCEEDED) {
        metrics->completion_per_mille = 1000;
    } else if (metrics->completion_per_mille > 999u) {
        metrics->completion_per_mille = 999u;
    }
    if (engine->navigation_work.load != NULL
        || engine->navigation_work.adapter != NULL) {
        uint64_t now_us =
            tilefinch_platform_monotonic_time_us();
        metrics->elapsed_us =
            now_us >= engine->navigation_work.started_us
                ? now_us - engine->navigation_work.started_us : 0;
    }
    return true;
}

bool browser_engine_provisional_viewport(
    const BrowserEngine *engine, BrowserProvisionalViewport *viewport)
{
    if (viewport != NULL) memset(viewport, 0, sizeof(*viewport));
    if (engine == NULL || viewport == NULL
        || engine->navigation_work.status
               != BROWSER_NAVIGATION_JOB_PENDING
        || !engine->provisional_ready
        || engine->provisional_frames == NULL
        || engine->provisional_frame_count == 0
        || engine->provisional_current_frame
               >= engine->provisional_frame_count) {
        return false;
    }
    __sync_synchronize();
    size_t current = engine->provisional_current_frame;
    *viewport = (BrowserProvisionalViewport) {
        .pixels = engine->provisional_frames
                  + current * engine->provisional_frame_pixels,
        .pixel_count = engine->provisional_frame_pixels,
        .frame_count = engine->provisional_frame_count,
        .current_frame = current,
        .scroll_y = engine->provisional_scroll_positions[current],
        .maximum_scroll_y =
            engine->provisional_scroll_positions[
                engine->provisional_frame_count - 1u],
        .ready = true
    };
    return true;
}

bool browser_engine_scroll_provisional_page(
    BrowserEngine *engine, int direction)
{
    BrowserProvisionalViewport before;
    if (direction == 0
        || !browser_engine_provisional_viewport(engine, &before)) {
        return false;
    }
    size_t next = before.current_frame;
    if (direction > 0 && next + 1u < before.frame_count) next++;
    else if (direction < 0 && next > 0) next--;
    if (next == before.current_frame) return false;
    engine->provisional_current_frame = next;
    __sync_synchronize();
    engine->navigation_work.metrics.provisional_scrolls++;
    engine->navigation_work.metrics.provisional_scroll_y =
        engine->provisional_scroll_positions[next];
    const uint16_t *frame = engine->provisional_frames
        + next * engine->provisional_frame_pixels;
    (void) tilefinch_platform_present_rgb565(
        frame,
        (size_t) engine->config.device.framebuffer_width,
        (size_t) engine->config.device.framebuffer_height,
        (size_t) engine->config.device.framebuffer_width);
    return true;
}

typedef bool (*BrowserEngineLoadOperation)(BrowserEngine *engine,
                                           uint64_t generation,
                                           void *opaque);

static bool browser_engine_run_load(BrowserEngine *engine,
                                    BrowserEngineLoadOperation operation,
                                    void *opaque,
                                    TilefinchDiagnosticSubsystem subsystem,
                                    TilefinchDiagnosticCode failure_code)
{
    if (engine == NULL || engine->state != BROWSER_ENGINE_ACTIVE
        || !engine->navigation_ready || operation == NULL) {
        return set_error_code(
            engine, TILEFINCH_SUBSYSTEM_ENGINE,
            TILEFINCH_DIAGNOSTIC_INVALID_INPUT, "navigation-start",
            "browser engine is not active");
    }
    if (browser_engine_navigation_pending(engine)) {
        return set_error_code(
            engine, TILEFINCH_SUBSYSTEM_ENGINE,
            TILEFINCH_DIAGNOSTIC_INVALID_INPUT, "navigation-start",
            "a responsive navigation job is already active");
    }
    /* The incumbent remains the visual/DOM rollback candidate, but its
       optional resource and script fetches must not occupy every shared PSP
       transport descriptor ahead of the authoritative replacement. A
       failed candidate can still restore the incumbent pixels and document;
       only unfinished network embellishment is superseded. */
    (void) navigation_cancel_network_work(
        &engine->navigation, "superseded by a new navigation");
    browser_engine_cancel_idle_work(engine);
    (void) emit_diagnostic(
        engine, TILEFINCH_DIAGNOSTIC_INFO,
        TILEFINCH_SUBSYSTEM_ENGINE, TILEFINCH_DIAGNOSTIC_LIFECYCLE,
        "navigation-start", "candidate load started",
        engine->navigation.loads_started + 1, 0);
    DocumentBacking previous = engine->backing;
    bool previous_valid = document_backing_valid(&previous);
    document_backing_uninstall(&engine->navigation);
    if (!navigation_set_candidate_commit_hooks(
            &engine->navigation, browser_engine_prepare_candidate_shell,
            browser_engine_abort_candidate_shell,
            browser_engine_commit_candidate_shell, engine)) {
        if (previous_valid) {
            engine->backing = previous;
            (void) document_backing_install(&engine->backing,
                                            &engine->navigation);
        }
        return set_error_code(
            engine, TILEFINCH_SUBSYSTEM_ENGINE,
            TILEFINCH_DIAGNOSTIC_INTERNAL_FAILED, "candidate-shell-hook",
            "could not install the candidate render-shell transaction");
    }
    uint64_t generation = navigation_begin(&engine->navigation);
    bool loaded = operation(engine, generation, opaque);
    (void) navigation_set_candidate_commit_hooks(
        &engine->navigation, NULL, NULL, NULL, NULL);
    if (engine->candidate_shell_prepared
        || engine->candidate_render.budget != NULL)
        browser_engine_abort_candidate_shell(engine);
    if (loaded && engine->navigation.page.loaded) {
        if (!browser_engine_bind_current_full_document(engine)) loaded = false;
    } else if (engine->navigation.page.loaded && previous_valid) {
        engine->backing = previous;
        (void) document_backing_install(&engine->backing,
                                        &engine->navigation);
    } else {
        document_backing_clear(&engine->backing);
    }
    bool shell_ready = loaded && engine->navigation.page.loaded
        && engine->controller_ready
        && (engine->config.tile_capacity == 0 || engine->render_ready);
    if (!loaded) {
        const char *message = engine->navigation.last_error[0] != '\0'
            ? engine->navigation.last_error
            : (engine->last_error[0] != '\0'
               ? engine->last_error : "navigation failed");
        return set_error_code(engine, subsystem, failure_code,
                              "navigation-failed", message);
    }
    if (!shell_ready) {
        return set_error_code(
            engine, TILEFINCH_SUBSYSTEM_ENGINE,
            TILEFINCH_DIAGNOSTIC_INTERNAL_FAILED, "shell-commit",
            "committed navigation has no promoted render shell");
    }
    (void) emit_diagnostic(
        engine, TILEFINCH_DIAGNOSTIC_INFO,
        TILEFINCH_SUBSYSTEM_ENGINE, TILEFINCH_DIAGNOSTIC_LIFECYCLE,
        "navigation-committed", "candidate load committed",
        engine->navigation.loads_committed,
        engine->navigation.page.document.node_count);
    return true;
}

typedef struct {
    const char *url;
    const char *html;
    size_t html_length;
    bool record_history;
} BrowserEngineHtmlLoad;

static bool browser_engine_do_commit(BrowserEngine *engine,
                                     uint64_t generation, void *opaque)
{
    const BrowserEngineHtmlLoad *load = opaque;
    return navigation_commit_html(
        &engine->navigation, generation, load->url, load->html,
        load->html_length, engine->config.device.navigation_viewport_width,
        engine->fonts_ready ? &engine->fonts : NULL, NULL,
        load->record_history);
}

bool browser_engine_commit_html(BrowserEngine *engine, const char *url,
                                const char *html, size_t html_length,
                                bool record_history)
{
    if (url == NULL || html == NULL) return set_error_code(
        engine, TILEFINCH_SUBSYSTEM_PARSER, TILEFINCH_DIAGNOSTIC_INVALID_INPUT,
        "html-load", "HTML load is invalid");
    BrowserEngineHtmlLoad load = {
        .url = url,
        .html = html,
        .html_length = html_length,
        .record_history = record_history
    };
    return browser_engine_run_load(
        engine, browser_engine_do_commit, &load, TILEFINCH_SUBSYSTEM_PARSER,
        TILEFINCH_DIAGNOSTIC_PARSE_FAILED);
}

typedef struct {
    const char *url;
    bool record_history;
    bool used_site_adapter;
    size_t adapter_source_bytes;
    size_t adapter_result_count;
    char adapter_name[32];
} BrowserEngineUrlLoad;

static bool browser_engine_do_load_url(BrowserEngine *engine,
                                       uint64_t generation, void *opaque)
{
    BrowserEngineUrlLoad *load = opaque;
#ifndef __PSP__
    if (site_adapter_handles_navigation("GET", load->url)) {
        SiteAdapterDocument document = {0};
        char error[256] = {0};
        SiteAdapterPreferences preferences = {
            .youtube_compact_results = engine->youtube_compact_results
        };
        uint64_t started_ns = tilefinch_platform_monotonic_time_ns();
        bool acquired = site_adapter_load_sync(
                &engine->budget, &engine->session, "GET", load->url,
                &preferences,
                engine->config.maximum_document_bytes,
                engine->config.navigation_timeout_ms,
                &document, error, sizeof(error));
        uint64_t finished_ns = tilefinch_platform_monotonic_time_ns();
        if (finished_ns >= started_ns) {
            engine->navigation.performance.network_us +=
                (finished_ns - started_ns) / UINT64_C(1000);
        }
        if (!acquired) {
            snprintf(engine->navigation.last_error,
                     sizeof(engine->navigation.last_error), "%s",
                     error[0] == '\0'
                         ? "site adapter page failed" : error);
            return false;
        }
        engine->navigation.last_http_status = document.status_code;
        snprintf(engine->navigation.last_server,
                 sizeof(engine->navigation.last_server), "%s",
                 document.server);
        snprintf(engine->navigation.last_cf_mitigated,
                 sizeof(engine->navigation.last_cf_mitigated), "%s",
                 document.cf_mitigated);
        load->used_site_adapter = true;
        load->adapter_source_bytes = document.source_bytes;
        load->adapter_result_count = document.result_count;
        snprintf(load->adapter_name, sizeof(load->adapter_name), "%s",
                 document.adapter);
        bool committed = navigation_commit_static_html(
            &engine->navigation, generation, load->url,
            document.html, document.html_length,
            engine->config.device.navigation_viewport_width,
            engine->fonts_ready ? &engine->fonts : NULL, NULL,
            load->record_history);
        site_adapter_document_destroy(&document);
        return committed;
    }
#endif
    return navigation_load_url(
        &engine->navigation, generation, load->url,
        engine->config.maximum_document_bytes,
        engine->config.navigation_timeout_ms,
        engine->config.device.navigation_viewport_width,
        engine->fonts_ready ? &engine->fonts : NULL, NULL,
        load->record_history);
}

bool browser_engine_load_url(BrowserEngine *engine, const char *url,
                             bool record_history)
{
    if (engine == NULL) return false;
    return browser_engine_load_url_with_limits(
        engine, url, engine->config.maximum_document_bytes,
        engine->config.navigation_timeout_ms, record_history);
}

bool browser_engine_load_url_with_limits(
    BrowserEngine *engine, const char *url, size_t maximum_bytes,
    long timeout_ms, bool record_history)
{
    if (url == NULL) return set_error_code(
        engine, TILEFINCH_SUBSYSTEM_NETWORK, TILEFINCH_DIAGNOSTIC_INVALID_INPUT,
        "url-load", "URL load is invalid");
    if (maximum_bytes == 0 || timeout_ms <= 0) return set_error_code(
        engine, TILEFINCH_SUBSYSTEM_NETWORK, TILEFINCH_DIAGNOSTIC_INVALID_INPUT,
        "url-load", "URL load limits are invalid");
    BrowserEngineUrlLoad load = {
        .url = url,
        .record_history = record_history
    };
    size_t saved_maximum = engine->config.maximum_document_bytes;
    long saved_timeout = engine->config.navigation_timeout_ms;
    engine->config.maximum_document_bytes = maximum_bytes;
    engine->config.navigation_timeout_ms = timeout_ms;
    bool loaded = browser_engine_run_load(
        engine, browser_engine_do_load_url, &load, TILEFINCH_SUBSYSTEM_NETWORK,
        TILEFINCH_DIAGNOSTIC_NETWORK_FAILED);
    if (loaded && load.used_site_adapter) {
        (void) emit_diagnostic(
            engine, TILEFINCH_DIAGNOSTIC_INFO,
            TILEFINCH_SUBSYSTEM_ENGINE, TILEFINCH_DIAGNOSTIC_LIFECYCLE,
            load.adapter_name, "bounded site-adapter page committed",
            load.adapter_source_bytes, load.adapter_result_count);
    }
    /* Scripted form default actions (including small edge verification
       forms) are navigations, not telemetry. Follow a bounded chain after
       each committed document while the source DOM is still alive. */
    for (size_t hop = 0; loaded && hop < 4; hop++) {
        lxb_dom_node_t *form = NULL, *submitter = NULL;
        if (engine->navigation.page.runtime == NULL
            || !script_runtime_take_form_submission(
                   engine->navigation.page.runtime, &form, &submitter)) {
            break;
        }
        ControllerAction action;
        if (!controller_build_form_action(
                &engine->controller, form, submitter, false, &action)
            || !browser_engine_execute_action(
                   engine, &action, maximum_bytes, timeout_ms)) {
            loaded = false;
            break;
        }
    }
    /* The cooperative navigation job honors `autofocus` on every fresh
       document. Do the same here so a synchronous load (host lab, scripted
       navigation) lands on the page's declared entry control instead of the
       first focusable node. History restores supply their own focus. */
    if (loaded && record_history) browser_engine_apply_autofocus(engine);
    engine->config.maximum_document_bytes = saved_maximum;
    engine->config.navigation_timeout_ms = saved_timeout;
    return loaded;
}

bool browser_engine_history_move(BrowserEngine *engine, bool forward)
{
    if (engine == NULL || engine->state != BROWSER_ENGINE_ACTIVE
        || !engine->navigation_ready) return set_error_code(
            engine, TILEFINCH_SUBSYSTEM_ENGINE,
            TILEFINCH_DIAGNOSTIC_INVALID_INPUT, "history-move",
            "browser engine is not active");
    browser_engine_cancel_idle_work(engine);
    size_t previous_index = engine->navigation.history_index;
    const NavigationEntry *entry = NULL;
    bool moved = forward
        ? navigation_forward(&engine->navigation, &entry)
        : navigation_back(&engine->navigation, &entry);
    if (!moved || entry == NULL) return set_error_code(
        engine, TILEFINCH_SUBSYSTEM_ENGINE,
        TILEFINCH_DIAGNOSTIC_INVALID_INPUT, "history-move",
        "no history entry is available");
    char url[NAVIGATION_URL_LIMIT];
    snprintf(url, sizeof(url), "%s", entry->url);
    int scroll_y = entry->scroll_y;
    int focus_kind = entry->focus_kind;
    size_t focus_index = entry->focus_index;
    if (!browser_engine_load_url(engine, url, false)) {
        const NavigationEntry *ignored = NULL;
        if (engine->navigation.history_index != previous_index) {
            (void) (forward
                ? navigation_back(&engine->navigation, &ignored)
                : navigation_forward(&engine->navigation, &ignored));
        }
        return false;
    }
    lxb_dom_node_t *focus_node =
        browser_engine_semantic_focus(engine, focus_kind, focus_index);
    if (focus_node != NULL)
        (void) controller_focus_node(&engine->controller, focus_node);
    return navigation_set_scroll(&engine->navigation, scroll_y);
}

bool browser_engine_replace_history(
    BrowserEngine *engine, const NavigationHistoryRecord *records,
    size_t count, size_t current_index)
{
    if (engine == NULL || engine->state != BROWSER_ENGINE_ACTIVE
        || !engine->navigation_ready
        || browser_engine_navigation_pending(engine)) {
        return set_error_code(
            engine, TILEFINCH_SUBSYSTEM_ENGINE,
            TILEFINCH_DIAGNOSTIC_INVALID_INPUT, "history-replace",
            "history cannot be replaced in the current engine state");
    }
    if (!navigation_replace_history(
            &engine->navigation, records, count, current_index)) {
        return set_error_code(
            engine, TILEFINCH_SUBSYSTEM_ENGINE,
            TILEFINCH_DIAGNOSTIC_ALLOCATION_FAILED, "history-replace",
            "bounded tab history could not be installed");
    }
    clear_error(engine);
    return true;
}

bool browser_engine_restore_view(
    BrowserEngine *engine, int scroll_y, int focus_kind,
    size_t focus_index)
{
    if (!browser_engine_input_ready(engine)) return false;
    browser_engine_cancel_idle_work(engine);
    lxb_dom_node_t *focus = browser_engine_semantic_focus(
        engine, focus_kind, focus_index);
    if (focus != NULL)
        (void) controller_focus_node(&engine->controller, focus);
    if (!navigation_set_scroll(&engine->navigation, scroll_y)) return false;
    return browser_engine_apply_focus_paint(engine);
}

static bool browser_engine_input_ready(const BrowserEngine *engine)
{
    return engine != NULL && engine->state == BROWSER_ENGINE_ACTIVE
        && engine->navigation_ready && engine->navigation.page.loaded
        && engine->controller_ready;
}

static bool browser_engine_finish_input(BrowserEngine *engine,
                                        bool succeeded)
{
    if (!succeeded) return false;
    if (engine->render_ready
        && engine->render_relayout_generation
               != engine->navigation.incremental_relayouts
        && !browser_engine_apply_layout_damage(engine)) return false;
    if (!browser_engine_apply_focus_paint(engine)) return false;
    NavigationEntry *entry =
        engine->navigation.history_count != 0
        && engine->navigation.history_index
             < engine->navigation.history_count
        ? &engine->navigation.history[engine->navigation.history_index]
        : NULL;
    if (entry != NULL) {
        entry->focus_kind = (int) engine->controller.focus_kind;
        entry->focus_index = browser_engine_focus_ordinal(engine);
    }
    clear_error(engine);
    return true;
}

bool browser_engine_focus_move(BrowserEngine *engine, bool forward)
{
    if (!browser_engine_input_ready(engine)) return false;
    browser_engine_cancel_idle_work(engine);
    return browser_engine_finish_input(
        engine, forward ? controller_focus_next(&engine->controller)
                        : controller_focus_previous(&engine->controller));
}

bool browser_engine_focus_direction(BrowserEngine *engine,
                                    ControllerFocusDirection direction)
{
    if (!browser_engine_input_ready(engine)) return false;
    browser_engine_cancel_idle_work(engine);
    return browser_engine_finish_input(
        engine, controller_focus_direction(&engine->controller, direction));
}

bool browser_engine_focus_at(BrowserEngine *engine, int x, int y)
{
    if (!browser_engine_input_ready(engine)) return false;
    browser_engine_cancel_idle_work(engine);
    return browser_engine_finish_input(
        engine, controller_focus_at(&engine->controller, x, y));
}

bool browser_engine_pointer_event(BrowserEngine *engine,
                                  ControllerPointerPhase phase,
                                  int x, int y, bool *activate,
                                  bool *page_changed)
{
    if (activate != NULL) *activate = false;
    if (page_changed != NULL) *page_changed = false;
    if (!browser_engine_input_ready(engine)) return false;
    browser_engine_cancel_idle_work(engine);
    size_t relayouts = engine->navigation.incremental_relayouts;
    const NavigationEntry *before_entry =
        navigation_current(&engine->navigation);
    int scroll_y = before_entry == NULL ? 0 : before_entry->scroll_y;
    if (!browser_engine_finish_input(
            engine, controller_pointer_event(
                        &engine->controller, phase, x, y, activate))) {
        return false;
    }
    const NavigationEntry *after_entry =
        navigation_current(&engine->navigation);
    if (page_changed != NULL) {
        *page_changed =
            relayouts != engine->navigation.incremental_relayouts
            || (after_entry != NULL && after_entry->scroll_y != scroll_y);
    }
    return true;
}

void browser_engine_pointer_discard_click(BrowserEngine *engine)
{
    if (engine != NULL)
        controller_pointer_discard_click(&engine->controller);
}

bool browser_engine_pointer_commit_click(BrowserEngine *engine)
{
    if (!browser_engine_input_ready(engine)) return false;
    browser_engine_cancel_idle_work(engine);
    return browser_engine_finish_input(
        engine, controller_commit_pointer_click(&engine->controller));
}

bool browser_engine_focus_node(BrowserEngine *engine, lxb_dom_node_t *node)
{
    if (!browser_engine_input_ready(engine) || node == NULL) return false;
    browser_engine_cancel_idle_work(engine);
    return browser_engine_finish_input(
        engine, controller_focus_node(&engine->controller, node));
}

bool browser_engine_scroll_by(BrowserEngine *engine, int delta_y)
{
    if (!browser_engine_input_ready(engine)) return false;
    browser_engine_cancel_idle_work(engine);
    return browser_engine_finish_input(
        engine, controller_scroll_by(
                    &engine->controller, delta_y,
                    engine->config.device.framebuffer_height));
}

bool browser_engine_scroll_settle(BrowserEngine *engine)
{
    if (!browser_engine_input_ready(engine)) return false;
    browser_engine_cancel_idle_work(engine);
    return browser_engine_finish_input(
        engine, controller_scroll_settle(&engine->controller));
}

LayoutCursor browser_engine_pointer_cursor(const BrowserEngine *engine)
{
    return engine == NULL ? LAYOUT_CURSOR_AUTO
        : controller_pointer_cursor(&engine->controller);
}

LayoutScrollbarWidth browser_engine_root_scrollbar_width(
    const BrowserEngine *engine)
{
    return engine == NULL ? LAYOUT_SCROLLBAR_AUTO
        : controller_root_scrollbar_width(&engine->controller);
}

bool browser_engine_scroll_step(BrowserEngine *engine, int direction,
                                unsigned held_frames)
{
    if (!browser_engine_input_ready(engine)) return false;
    browser_engine_cancel_idle_work(engine);
    return browser_engine_finish_input(
        engine, controller_scroll_step(
                    &engine->controller, direction, held_frames,
                    engine->config.device.framebuffer_height));
}

bool browser_engine_scroll_page(BrowserEngine *engine, int direction)
{
    if (!browser_engine_input_ready(engine)) return false;
    browser_engine_cancel_idle_work(engine);
    return browser_engine_finish_input(
        engine, controller_scroll_page(
                    &engine->controller, direction,
                    engine->config.device.framebuffer_height));
}

bool browser_engine_scroll_to_edge(BrowserEngine *engine, bool bottom)
{
    if (!browser_engine_input_ready(engine)) return false;
    browser_engine_cancel_idle_work(engine);
    return browser_engine_finish_input(
        engine, bottom
            ? controller_scroll_to_bottom(
                  &engine->controller,
                  engine->config.device.framebuffer_height)
            : controller_scroll_to_top(
                  &engine->controller,
                  engine->config.device.framebuffer_height));
}

static void browser_engine_find_copy_snapshot(
    const BrowserEngine *engine, BrowserFindSnapshot *snapshot)
{
    if (snapshot == NULL) return;
    memset(snapshot, 0, sizeof(*snapshot));
    if (engine == NULL || engine->find.query[0] == '\0') return;
    snprintf(snapshot->query, sizeof(snapshot->query), "%s",
             engine->find.query);
    snapshot->match_count = engine->find.match_count;
    snapshot->selected = engine->find.selected;
    snapshot->truncated = engine->find.truncated;
    snapshot->wrapped = engine->find.wrapped;
}

void browser_engine_find_clear(BrowserEngine *engine)
{
    if (engine == NULL) return;
    page_find_clear(&engine->find);
    engine->find_navigation_generation = 0;
    engine->find_layout_generation = 0;
}

static bool browser_engine_find_rebuild(BrowserEngine *engine,
                                        int nearest_y)
{
    char query[PAGE_FIND_QUERY_LIMIT + 1u];
    snprintf(query, sizeof(query), "%s", engine->find.query);
    if (!page_find_build(
            &engine->find, &engine->budget,
            &engine->navigation.page.layout, query)) return false;
    engine->find_navigation_generation = engine->navigation.generation;
    engine->find_layout_generation = engine->navigation.incremental_relayouts;
    if (engine->find.match_count != 0)
        (void) page_find_select_nearest(&engine->find, nearest_y);
    return true;
}

static bool browser_engine_find_refresh(BrowserEngine *engine)
{
    if (engine == NULL || engine->find.query[0] == '\0') return false;
    if (!browser_engine_input_ready(engine)
        || engine->find_navigation_generation
               != engine->navigation.generation) {
        browser_engine_find_clear(engine);
        return false;
    }
    if (engine->find_layout_generation
            == engine->navigation.incremental_relayouts) return true;
    const PageFindMatch *selected = page_find_selected(&engine->find);
    int nearest_y = selected == NULL ? 0 : selected->top;
    if (page_find_refresh_geometry(
            &engine->find, &engine->navigation.page.layout)) {
        engine->find_layout_generation =
            engine->navigation.incremental_relayouts;
        return true;
    }
    if (!browser_engine_find_rebuild(engine, nearest_y)) {
        browser_engine_find_clear(engine);
        return false;
    }
    return true;
}

static bool browser_engine_find_jump(BrowserEngine *engine)
{
    const PageFindMatch *match = page_find_selected(&engine->find);
    if (match == NULL) return false;
    if (match->fixed) return true;
    int context = engine->navigation.viewport.css_height / 3;
    int target = match->top > context ? match->top - context : 0;
    browser_engine_cancel_idle_work(engine);
    return navigation_set_scroll(&engine->navigation, target);
}

bool browser_engine_find_begin(BrowserEngine *engine, const char *query,
                               BrowserFindSnapshot *snapshot)
{
    if (snapshot != NULL) memset(snapshot, 0, sizeof(*snapshot));
    if (!browser_engine_input_ready(engine) || query == NULL) return false;
    browser_engine_find_clear(engine);
    if (!page_find_build(
            &engine->find, &engine->budget,
            &engine->navigation.page.layout, query)) return false;
    engine->find_navigation_generation = engine->navigation.generation;
    engine->find_layout_generation = engine->navigation.incremental_relayouts;
    const NavigationEntry *entry = navigation_current(&engine->navigation);
    int scroll_y = entry == NULL ? 0 : entry->scroll_y;
    if (engine->find.match_count != 0) {
        (void) page_find_select_nearest(&engine->find, scroll_y);
        (void) browser_engine_find_jump(engine);
    }
    browser_engine_find_copy_snapshot(engine, snapshot);
    clear_error(engine);
    return true;
}

bool browser_engine_find_move(BrowserEngine *engine, int direction,
                              BrowserFindSnapshot *snapshot)
{
    if (snapshot != NULL) memset(snapshot, 0, sizeof(*snapshot));
    if (!browser_engine_find_refresh(engine) || direction == 0
        || !page_find_move(&engine->find, direction)) return false;
    (void) browser_engine_find_jump(engine);
    browser_engine_find_copy_snapshot(engine, snapshot);
    clear_error(engine);
    return true;
}

bool browser_engine_find_snapshot(BrowserEngine *engine,
                                  BrowserFindSnapshot *snapshot)
{
    if (snapshot == NULL) return false;
    memset(snapshot, 0, sizeof(*snapshot));
    if (!browser_engine_find_refresh(engine)) return false;
    browser_engine_find_copy_snapshot(engine, snapshot);
    return true;
}

bool browser_engine_capture_text_anchor(
    const BrowserEngine *engine,
    char anchor[BROWSER_TEXT_ANCHOR_LIMIT + 1u], int *document_y)
{
    if (anchor != NULL) anchor[0] = '\0';
    if (!browser_engine_input_ready(engine) || anchor == NULL) return false;
    const NavigationEntry *entry = navigation_current(&engine->navigation);
    int scroll_y = entry == NULL ? 0 : entry->scroll_y;
    const LayoutDocument *layout = &engine->navigation.page.layout;
    for (size_t at = 0; at < layout->count; at++) {
        const DrawCommand *command = &layout->commands[at];
        if (command->type != DRAW_TEXT || command->text == NULL
            || command->text_length < 4u
            || draw_command_is_text_shadow(command)
            || command->y + command->height < scroll_y) continue;
        size_t length = command->text_length;
        if (length > BROWSER_TEXT_ANCHOR_LIMIT)
            length = BROWSER_TEXT_ANCHOR_LIMIT;
        while (length != 0 && length < command->text_length
               && (((unsigned char) command->text[length] & 0xc0u)
                       == 0x80u)) length--;
        memcpy(anchor, command->text, length);
        anchor[length] = '\0';
        if (document_y != NULL) *document_y = command->y;
        return length >= 4u;
    }
    return false;
}

bool browser_engine_restore_text_anchor(
    BrowserEngine *engine, const char *anchor, int nearest_y)
{
    if (!browser_engine_input_ready(engine) || anchor == NULL
        || anchor[0] == '\0') return false;
    PageFindIndex temporary = {0};
    bool restored = page_find_build(
        &temporary, &engine->budget, &engine->navigation.page.layout,
        anchor);
    if (restored && temporary.match_count != 0) {
        (void) page_find_select_nearest(&temporary, nearest_y);
        const PageFindMatch *match = page_find_selected(&temporary);
        restored = match != NULL
            && (match->fixed
                || navigation_set_scroll(&engine->navigation, match->top));
    } else {
        restored = false;
    }
    page_find_clear(&temporary);
    return restored;
}

static void browser_engine_paint_find_highlights(BrowserEngine *engine,
                                                 int scroll_y)
{
    if (!browser_engine_find_refresh(engine)
        || engine->find.match_count == 0) return;
    const LayoutDocument *layout = &engine->navigation.page.layout;
    const ViewportContext *viewport = &engine->navigation.viewport;
    int viewport_width = engine->config.device.framebuffer_width;
    int viewport_height = engine->config.device.framebuffer_height;
    int visible_top = scroll_y;
    int visible_bottom = scroll_y + viewport->css_height;
    for (size_t match_index = 0;
         match_index < engine->find.match_count; match_index++) {
        const PageFindMatch *match = &engine->find.matches[match_index];
        if (!match->fixed
            && (match->bottom < visible_top || match->top > visible_bottom))
            continue;
        PageFindRect rects[PAGE_FIND_RECT_LIMIT];
        size_t count = page_find_match_rects(
            &engine->find, layout, match_index,
            rects, PAGE_FIND_RECT_LIMIT);
        for (size_t i = 0; i < count; i++) {
            int css_y = rects[i].fixed ? rects[i].y
                                      : rects[i].y - scroll_y;
            int left = viewport_css_to_device(viewport, rects[i].x);
            int top = viewport_css_to_device(viewport, css_y);
            int right = viewport_css_to_device(
                viewport, rects[i].x + rects[i].width);
            int bottom = viewport_css_to_device(
                viewport, css_y + rects[i].height);
            render_paint_find_highlight(
                engine->render.frame, engine->render.frame_pixels,
                viewport_width, viewport_height,
                left, top, right - left, bottom - top,
                match_index == engine->find.selected);
        }
    }
}

bool browser_engine_text_value(const BrowserEngine *engine, char *output,
                               size_t capacity, size_t *length)
{
    return browser_engine_input_ready(engine)
        && controller_text_value(
               &engine->controller, output, capacity, length);
}

bool browser_engine_text_input_info(
    const BrowserEngine *engine, ControllerTextInputInfo *info)
{
    return browser_engine_input_ready(engine)
        && controller_text_input_info(&engine->controller, info);
}

bool browser_engine_replace_text(BrowserEngine *engine, const char *utf8,
                                 size_t length)
{
    if (!browser_engine_input_ready(engine) || utf8 == NULL) return false;
    browser_engine_cancel_idle_work(engine);
    return browser_engine_finish_input(
        engine, controller_replace_text(&engine->controller, utf8, length));
}

bool browser_engine_insert_text(BrowserEngine *engine, const char *utf8,
                                size_t length)
{
    if (!browser_engine_input_ready(engine) || utf8 == NULL) return false;
    browser_engine_cancel_idle_work(engine);
    return browser_engine_finish_input(
        engine, controller_insert_text(&engine->controller, utf8, length));
}

bool browser_engine_backspace(BrowserEngine *engine)
{
    if (!browser_engine_input_ready(engine)) return false;
    browser_engine_cancel_idle_work(engine);
    return browser_engine_finish_input(
        engine, controller_backspace(&engine->controller));
}

bool browser_engine_activate(BrowserEngine *engine,
                             ControllerAction *action)
{
    if (!browser_engine_input_ready(engine) || action == NULL) return false;
    browser_engine_cancel_idle_work(engine);
    return browser_engine_finish_input(
        engine, controller_activate(&engine->controller, action));
}

bool browser_engine_advance_runtime(BrowserEngine *engine,
                                    unsigned elapsed_ms,
                                    size_t maximum_callbacks,
                                    bool *visible_layout_changed)
{
    if (visible_layout_changed != NULL) *visible_layout_changed = false;
    if (!browser_engine_input_ready(engine)
        || maximum_callbacks == 0) return false;
    size_t before = engine->navigation.incremental_relayouts;
    bool advanced = navigation_advance_runtime(
        &engine->navigation, elapsed_ms, maximum_callbacks);
    bool changed = engine->navigation.incremental_relayouts != before;
    if (changed && engine->render_ready
        && !browser_engine_apply_layout_damage(engine)) return false;
    bool visible_changed =
        changed && engine->navigation.relayout_damage_valid;
    if (visible_layout_changed != NULL) {
        *visible_layout_changed = visible_changed;
    }
    if (changed && !visible_changed) {
        engine->unchanged_runtime_frames_suppressed++;
    }
    if (advanced) clear_error(engine);
    return advanced;
}

bool browser_engine_execute_action(BrowserEngine *engine,
                                   const ControllerAction *action,
                                   size_t maximum_bytes,
                                   long timeout_ms)
{
    if (engine == NULL || engine->state != BROWSER_ENGINE_ACTIVE
        || !engine->controller_ready || action == NULL
        || maximum_bytes == 0 || timeout_ms <= 0) {
        return set_error_code(
            engine, TILEFINCH_SUBSYSTEM_ENGINE,
            TILEFINCH_DIAGNOSTIC_INVALID_INPUT, "controller-action",
            "controller action is invalid");
    }
    browser_engine_cancel_idle_work(engine);
    /* Activation has already dispatched ordinary control behavior. Only
       navigation and form submission require a second execution phase. */
    if (action->type == CONTROLLER_ACTION_NONE
        || action->type == CONTROLLER_ACTION_CONTROL) {
        clear_error(engine);
        return true;
    }
    const char *method = action->type == CONTROLLER_ACTION_FORM_SUBMIT
        ? action->method : "GET";
    if (strcasecmp(method, "GET") == 0
        && site_adapter_handles_navigation(method, action->url)) {
        const NavigationEntry *current =
            navigation_current(&engine->navigation);
        if (current != NULL) {
            snprintf(engine->navigation.pending_navigation_referer,
                     sizeof(engine->navigation.pending_navigation_referer),
                     "%s", current->url);
        }
        return browser_engine_load_url_with_limits(
            engine, action->url, maximum_bytes, timeout_ms, true);
    }
    size_t loads_before = engine->navigation.loads_committed;
    if (!controller_execute_action(
            &engine->controller, action, maximum_bytes, timeout_ms)) {
        return set_error_code(
            engine, TILEFINCH_SUBSYSTEM_ENGINE,
            TILEFINCH_DIAGNOSTIC_INTERNAL_FAILED, "controller-action",
            engine->navigation.last_error);
    }
    if (engine->navigation.loads_committed != loads_before
        && (!browser_engine_bind_current_full_document(engine)
            || !browser_engine_refresh_shell(engine))) return false;
    clear_error(engine);
    return true;
}

bool browser_engine_commit_section_html(
    BrowserEngine *engine, DocumentBacking *backing,
    DocumentBackingSelection *selection, const char *url,
    const char *html, size_t html_length, bool preserve_runtime,
    bool record_history)
{
    if (engine == NULL || engine->state != BROWSER_ENGINE_ACTIVE
        || !engine->navigation_ready || url == NULL || html == NULL
        || document_backing_kind(backing)
               != DOCUMENT_BACKING_COMPRESSED_SECTIONS
        || (selection != NULL
            && (!selection->active || selection->backing != backing))) {
        return set_error_code(
            engine, TILEFINCH_SUBSYSTEM_SECTIONS,
            TILEFINCH_DIAGNOSTIC_INVALID_INPUT, "section-commit",
            "compressed section commit is invalid");
    }
    browser_engine_cancel_idle_work(engine);
    size_t section = selection == NULL
        ? backing->section_pager->current_section
        : selection->pager_selection.section;
    if (!navigation_set_runtime_section_identity(
            &engine->navigation, section)) {
        return set_error_code(
            engine, TILEFINCH_SUBSYSTEM_SECTIONS,
            TILEFINCH_DIAGNOSTIC_INTERNAL_FAILED, "section-identity",
            "section runtime identity could not be installed");
    }
    uint64_t generation = navigation_begin(&engine->navigation);
    bool committed = preserve_runtime
        ? navigation_commit_html_preserve_runtime(
              &engine->navigation, generation, url, html, html_length,
              engine->config.device.navigation_viewport_width,
              engine->fonts_ready ? &engine->fonts : NULL, NULL,
              record_history)
        : navigation_commit_html(
              &engine->navigation, generation, url, html, html_length,
              engine->config.device.navigation_viewport_width,
              engine->fonts_ready ? &engine->fonts : NULL, NULL,
              record_history);
    if (!committed) return set_error_code(
        engine, TILEFINCH_SUBSYSTEM_SECTIONS,
        TILEFINCH_DIAGNOSTIC_PARSE_FAILED, "section-commit",
        engine->navigation.last_error);
    if (selection != NULL && !document_backing_commit_section(selection)) {
        return set_error_code(
            engine, TILEFINCH_SUBSYSTEM_SECTIONS,
            TILEFINCH_DIAGNOSTIC_INTERNAL_FAILED, "section-selection",
            "prepared section changed before commit");
    }
    if (!browser_engine_bind_document_backing(engine, backing)
        || !browser_engine_refresh_shell(engine)) return false;
    clear_error(engine);
    return true;
}

bool browser_engine_render_frame(BrowserEngine *engine,
                                 const char *optional_ppm_path)
{
    if (engine == NULL || !engine->render_ready
        || !engine->navigation.page.loaded) {
        return set_error_code(
            engine, TILEFINCH_SUBSYSTEM_RENDER,
            TILEFINCH_DIAGNOSTIC_INVALID_INPUT, "frame-render",
            "render shell is not ready");
    }
    if (engine->render_relayout_generation
            != engine->navigation.incremental_relayouts
        && !browser_engine_apply_layout_damage(engine)) return false;
    if (!browser_engine_apply_focus_paint(engine)) return false;
    const NavigationEntry *entry = navigation_current(&engine->navigation);
    int scroll_y = entry == NULL ? 0 : entry->scroll_y;
    bool previous_scroll_valid = engine->render.last_frame_scroll_valid;
    int previous_scroll_y = viewport_device_to_css(
        &engine->navigation.viewport, engine->render.last_frame_scroll_y);
    if (!tile_cache_render_frame(
            &engine->render, scroll_y,
            engine->config.device.framebuffer_width,
            engine->config.device.framebuffer_height, NULL)) {
        return set_error_code(
            engine, TILEFINCH_SUBSYSTEM_RENDER,
            TILEFINCH_DIAGNOSTIC_RENDER_FAILED, "frame-render",
            "frame rendering failed");
    }
    browser_engine_paint_find_highlights(engine, scroll_y);
    int focus_x = 0, focus_y = 0, focus_width = 0, focus_height = 0;
    ControllerFocusOutline focus_outline = {0};
    if (controller_focused_rect(
            &engine->controller, &focus_x, &focus_y,
            &focus_width, &focus_height)
        && controller_focused_outline_style(
            &engine->controller, &focus_outline)) {
        const ViewportContext *viewport = &engine->navigation.viewport;
        int left = viewport_css_to_device(viewport, focus_x);
        int top = viewport_css_to_device(
            viewport, focus_y - scroll_y);
        int right = viewport_css_to_device(
            viewport, focus_x + focus_width);
        int bottom = viewport_css_to_device(
            viewport, focus_y - scroll_y + focus_height);
        int outline_width = viewport_css_to_device(
            viewport, focus_outline.width);
        if (outline_width < 1) outline_width = 1;
        int outline_offset = viewport_css_to_device(
            viewport, focus_outline.offset);
        render_paint_authored_focus_outline(
            engine->render.frame, engine->render.frame_pixels,
            engine->config.device.framebuffer_width,
            engine->config.device.framebuffer_height,
            left, top, right - left, bottom - top,
            outline_width, outline_offset,
            focus_outline.style,
            focus_outline.color, focus_outline.alpha);
    }
    if (optional_ppm_path != NULL
        && !render_write_frame_ppm(
            optional_ppm_path, engine->render.frame,
            engine->config.device.framebuffer_width,
            engine->config.device.framebuffer_height)) {
        return set_error_code(
            engine, TILEFINCH_SUBSYSTEM_RENDER,
            TILEFINCH_DIAGNOSTIC_RENDER_FAILED, "frame-write",
            "rendered frame could not be written");
    }
    if (!previous_scroll_valid || scroll_y != previous_scroll_y) {
        int prefetch_y = previous_scroll_valid && scroll_y < previous_scroll_y
            ? scroll_y - 1
            : scroll_y + engine->navigation.viewport.css_height
                + TILEFINCH_TILE_SIZE - 1;
        if (prefetch_y >= 0
            && prefetch_y < engine->navigation.page.layout.height) {
            tile_cache_schedule_prefetch_row(
                &engine->render, prefetch_y,
                engine->config.device.framebuffer_width);
            (void) tile_cache_run_idle_work(
                &engine->render, engine->config.idle_work_budget_us,
                engine->config.idle_work_maximum_units);
        }
    }
    clear_error(engine);
    size_t compile_rejections =
        engine->navigation.page.script_result.host_compile_rejections;
    if (compile_rejections > engine->observed_compile_rejections) {
        (void) emit_diagnostic(
            engine, TILEFINCH_DIAGNOSTIC_WARNING,
            TILEFINCH_SUBSYSTEM_SCRIPT, TILEFINCH_DIAGNOSTIC_LIMIT_REACHED,
            "compile-admission",
            engine->navigation.page.script_result.error,
            compile_rejections,
            engine->navigation.page.script_result
                .host_compile_projected_rejections);
    }
    engine->observed_compile_rejections = compile_rejections;
    (void) emit_diagnostic(
        engine, TILEFINCH_DIAGNOSTIC_DEBUG,
        TILEFINCH_SUBSYSTEM_RENDER, TILEFINCH_DIAGNOSTIC_OK,
        "frame-rendered", "", engine->render.frames_rendered,
        engine->render.max_frame_us);
    return true;
}

BrowserRenderJobStatus browser_engine_render_frame_bounded(
    BrowserEngine *engine, uint64_t budget_us, size_t maximum_units)
{
    return browser_engine_render_frame_bounded_cancelable(
        engine, budget_us, maximum_units, NULL);
}

BrowserRenderJobStatus browser_engine_render_frame_bounded_cancelable(
    BrowserEngine *engine, uint64_t budget_us, size_t maximum_units,
    const TilefinchCancellation *cancellation)
{
    if (engine == NULL || !engine->render_ready
        || !engine->navigation.page.loaded) {
        (void) set_error_code(
            engine, TILEFINCH_SUBSYSTEM_RENDER,
            TILEFINCH_DIAGNOSTIC_INVALID_INPUT, "frame-job",
            "render shell is not ready");
        return BROWSER_RENDER_JOB_FAILED;
    }
    if (tilefinch_cancellation_requested(cancellation)) {
        browser_engine_cancel_render_job(engine);
        return BROWSER_RENDER_JOB_CANCELLED;
    }
    if (engine->render_relayout_generation
            != engine->navigation.incremental_relayouts
        && !browser_engine_apply_layout_damage(engine)) {
        return BROWSER_RENDER_JOB_FAILED;
    }
    const NavigationEntry *entry = navigation_current(&engine->navigation);
    int scroll_y = entry == NULL ? 0 : entry->scroll_y;
    RenderFrameWorkResult prepared =
        tile_cache_prepare_frame_bounded_cancelable(
        &engine->render, scroll_y,
        engine->config.device.framebuffer_width,
        engine->config.device.framebuffer_height,
        budget_us, maximum_units, cancellation);
    if (prepared == RENDER_FRAME_WORK_CANCELLED) {
        return BROWSER_RENDER_JOB_CANCELLED;
    }
    if (prepared == RENDER_FRAME_WORK_FAILED) {
        (void) set_error_code(
            engine, TILEFINCH_SUBSYSTEM_RENDER,
            TILEFINCH_DIAGNOSTIC_RENDER_FAILED, "frame-job",
            "bounded tile preparation failed");
        return BROWSER_RENDER_JOB_FAILED;
    }
    if (prepared == RENDER_FRAME_WORK_PENDING) {
        return BROWSER_RENDER_JOB_PENDING;
    }
    bool rendered = browser_engine_render_frame(engine, NULL);
    if (tilefinch_cancellation_requested(cancellation)) {
        browser_engine_cancel_render_job(engine);
        return BROWSER_RENDER_JOB_CANCELLED;
    }
    return rendered ? BROWSER_RENDER_JOB_COMPLETE
                    : BROWSER_RENDER_JOB_FAILED;
}

void browser_engine_cancel_render_job(BrowserEngine *engine)
{
    if (engine == NULL || !engine->render_ready) return;
    tile_cache_cancel_frame_work(&engine->render);
}

bool browser_engine_run_idle_work(BrowserEngine *engine)
{
    if (engine == NULL || engine->state != BROWSER_ENGINE_ACTIVE
        || !engine->render_ready) return false;
    size_t relayouts_before =
        engine->navigation.performance.fast_relayouts
        + engine->navigation.performance.full_relayouts;
    if (!navigation_run_background_resources(&engine->navigation)) {
        return set_error_code(
            engine, TILEFINCH_SUBSYSTEM_NETWORK,
            TILEFINCH_DIAGNOSTIC_NETWORK_FAILED, "background-resources",
            "background resource continuation failed");
    }
    size_t relayouts_after =
        engine->navigation.performance.fast_relayouts
        + engine->navigation.performance.full_relayouts;
    if (relayouts_after != relayouts_before
        && !browser_engine_refresh_shell(engine)) return false;
    (void) tile_cache_run_idle_work(
        &engine->render, engine->config.idle_work_budget_us,
        engine->config.idle_work_maximum_units);
    return navigation_background_resources_pending(&engine->navigation)
        || tile_cache_idle_work_pending(&engine->render);
}

void browser_engine_cancel_idle_work(BrowserEngine *engine)
{
    if (engine == NULL || !engine->render_ready) return;
    tile_cache_cancel_idle_work(&engine->render);
}

bool browser_engine_reclaim_optional_memory(
    BrowserEngine *engine, BrowserOptionalMemoryReclaim *reclaim)
{
    if (reclaim != NULL) memset(reclaim, 0, sizeof(*reclaim));
    if (engine == NULL || engine->state != BROWSER_ENGINE_ACTIVE) return false;

    BrowserOptionalMemoryReclaim result = {0};
    if (engine->navigation_ready && engine->navigation.page.runtime != NULL) {
        result.javascript_bytes = script_runtime_collect_and_trim(
            engine->navigation.page.runtime);
    }
    if (engine->session_ready) {
        result.session_cache_bytes = browser_session_cache_reclaim(
            &engine->session, SIZE_MAX);
    }
    if (engine->render_ready) {
        result.render_cache_bytes = tile_cache_reclaim_optional(
            &engine->render);
    }
    result.total_bytes = result.javascript_bytes;
    if (result.total_bytes <= SIZE_MAX - result.session_cache_bytes) {
        result.total_bytes += result.session_cache_bytes;
    } else {
        result.total_bytes = SIZE_MAX;
    }
    if (result.total_bytes <= SIZE_MAX - result.render_cache_bytes) {
        result.total_bytes += result.render_cache_bytes;
    } else {
        result.total_bytes = SIZE_MAX;
    }
    if (reclaim != NULL) *reclaim = result;
    (void) emit_diagnostic(
        engine, TILEFINCH_DIAGNOSTIC_DEBUG,
        TILEFINCH_SUBSYSTEM_ENGINE, TILEFINCH_DIAGNOSTIC_OK,
        "optional-memory-reclaimed", "", result.total_bytes,
        budget_remaining(&engine->budget));
    return true;
}

const DocumentBacking *browser_engine_document_backing(
    const BrowserEngine *engine)
{
    return engine == NULL || engine->state != BROWSER_ENGINE_ACTIVE
        ? NULL : &engine->backing;
}

bool browser_engine_view_snapshot(
    const BrowserEngine *engine, BrowserViewSnapshot *snapshot)
{
    if (snapshot == NULL) return false;
    memset(snapshot, 0, sizeof(*snapshot));
    if (engine == NULL || engine->state != BROWSER_ENGINE_ACTIVE
        || !engine->navigation_ready)
        return false;

    const NavigationSession *navigation = &engine->navigation;
    const NavigationEntry *entry = navigation_current(navigation);
    const char *pending_url = browser_engine_pending_navigation_url(engine);
    const char *url = pending_url != NULL
        ? pending_url
        : (entry == NULL || entry->url == NULL
               ? navigation->page.document_url : entry->url);
    const char *title = pending_url != NULL
        ? "Opening page"
        : (entry == NULL || entry->title == NULL
               ? navigation->page.document.title : entry->title);
    snprintf(snapshot->url, sizeof(snapshot->url), "%s",
             url == NULL ? "" : url);
    snprintf(snapshot->title, sizeof(snapshot->title), "%s",
             title == NULL ? "" : title);
    snapshot->secure = strncmp(snapshot->url, "https://", 8) == 0;
    snapshot->can_go_back =
        navigation->history_count > 0 && navigation->history_index > 0;
    snapshot->can_go_forward =
        navigation->history_count > 0
        && navigation->history_index + 1 < navigation->history_count;
    snapshot->scroll_y = entry == NULL ? 0 : entry->scroll_y;
    snapshot->maximum_scroll_y = viewport_max_scroll_css(
        &navigation->viewport, navigation->page.layout.height);
    snapshot->loading = browser_engine_navigation_pending(engine);
    snapshot->navigation_generation = navigation->generation;

    if (engine->controller_ready) {
        int x = 0, y = 0, width = 0, height = 0;
        if (controller_focused_rect(
                &engine->controller, &x, &y, &width, &height)) {
            snapshot->has_focus = true;
            snapshot->focus_x =
                viewport_css_to_device(&navigation->viewport, x);
            snapshot->focus_y = viewport_css_to_device(
                &navigation->viewport, y - snapshot->scroll_y);
            int right = viewport_css_to_device(
                &navigation->viewport, x + width);
            int bottom = viewport_css_to_device(
                &navigation->viewport,
                y - snapshot->scroll_y + height);
            snapshot->focus_width = right - snapshot->focus_x;
            snapshot->focus_height = bottom - snapshot->focus_y;
        }
    }
    return true;
}

bool browser_engine_frame_view(
    const BrowserEngine *engine, BrowserFrameView *view)
{
    if (view == NULL) return false;
    memset(view, 0, sizeof(*view));
    if (engine == NULL || engine->state != BROWSER_ENGINE_ACTIVE
        || engine->framebuffer == NULL)
        return false;
    view->pixels = engine->framebuffer;
    view->pixel_count = engine->framebuffer_pixels;
    view->width = engine->config.device.framebuffer_width;
    view->height = engine->config.device.framebuffer_height;
    view->stride = engine->config.device.framebuffer_width;
    return true;
}

bool browser_engine_frontend_snapshot(
    const BrowserEngine *engine, BrowserFrontendSnapshot *snapshot)
{
    if (snapshot == NULL) return false;
    memset(snapshot, 0, sizeof(*snapshot));
    if (engine == NULL || engine->state != BROWSER_ENGINE_ACTIVE)
        return false;
    if (engine->framebuffer != NULL) {
        snapshot->frame.pixels = engine->framebuffer;
        snapshot->frame.pixel_count = engine->framebuffer_pixels;
        snapshot->frame.width = engine->config.device.framebuffer_width;
        snapshot->frame.height = engine->config.device.framebuffer_height;
        snapshot->frame.stride = engine->config.device.framebuffer_width;
    }
    if (engine->navigation_ready)
        snapshot->navigation = &engine->navigation;
    if (engine->controller_ready)
        snapshot->controller = &engine->controller;
    return true;
}

bool browser_engine_fill_frame(BrowserEngine *engine, uint16_t color)
{
    if (engine == NULL || engine->state != BROWSER_ENGINE_ACTIVE
        || engine->framebuffer == NULL)
        return false;
    for (size_t index = 0; index < engine->framebuffer_pixels; index++)
        engine->framebuffer[index] = color;
    return true;
}

bool browser_engine_set_user_css(
    BrowserEngine *engine, const char *css, size_t length)
{
    if (engine == NULL || engine->state != BROWSER_ENGINE_ACTIVE
        || !engine->navigation_ready)
        return false;
    return navigation_set_user_css(&engine->navigation, css, length);
}

bool browser_engine_apply_user_css(
    BrowserEngine *engine, const char *css, size_t length)
{
    return engine != NULL && engine->state == BROWSER_ENGINE_ACTIVE
        && engine->navigation_ready
        && navigation_apply_user_css(&engine->navigation, css, length)
        && browser_engine_refresh_shell(engine);
}

const NavigationSession *browser_engine_navigation_view(
    const BrowserEngine *engine)
{
    return engine == NULL || engine->state != BROWSER_ENGINE_ACTIVE
            || !engine->navigation_ready
        ? NULL : &engine->navigation;
}

const BrowserController *browser_engine_controller_view(
    const BrowserEngine *engine)
{
    return engine == NULL || engine->state != BROWSER_ENGINE_ACTIVE
            || !engine->controller_ready
        ? NULL : &engine->controller;
}

const FontFace *browser_engine_font_face(
    const BrowserEngine *engine, FontFamily family)
{
    return engine == NULL || engine->state != BROWSER_ENGINE_ACTIVE
            || !engine->fonts_ready
        ? NULL : font_set_face(&engine->fonts, family);
}

const FontFace *browser_engine_font_face_variant(
    const BrowserEngine *engine, FontFamily family, bool italic, bool bold)
{
    return engine == NULL || engine->state != BROWSER_ENGINE_ACTIVE
            || !engine->fonts_ready
        ? NULL : font_set_face_variant(
            &engine->fonts, family, italic, bold);
}

const TileCache *browser_engine_render_metrics_view(
    const BrowserEngine *engine)
{
    return engine == NULL || engine->state != BROWSER_ENGINE_ACTIVE
            || !engine->render_ready
        ? NULL : &engine->render;
}

Budget *browser_engine_budget(BrowserEngine *engine)
{
    return engine == NULL || engine->state != BROWSER_ENGINE_ACTIVE
        ? NULL : &engine->budget;
}

BrowserSession *browser_engine_session(BrowserEngine *engine)
{
    return engine == NULL || !engine->session_ready ? NULL : &engine->session;
}

const BrowserSession *browser_engine_session_view(const BrowserEngine *engine)
{
    return engine == NULL || !engine->session_ready ? NULL : &engine->session;
}

NavigationSession *browser_engine_navigation(BrowserEngine *engine)
{
    return engine == NULL || !engine->navigation_ready
        ? NULL : &engine->navigation;
}

BrowserController *browser_engine_controller(BrowserEngine *engine)
{
    return engine == NULL || !engine->controller_ready
        ? NULL : &engine->controller;
}

FontSet *browser_engine_fonts(BrowserEngine *engine)
{
    return engine == NULL || !engine->fonts_ready ? NULL : &engine->fonts;
}

TileCache *browser_engine_render_shell(BrowserEngine *engine)
{
    return engine == NULL || !engine->render_ready ? NULL : &engine->render;
}

uint16_t *browser_engine_framebuffer(BrowserEngine *engine,
                                     size_t *pixel_count)
{
    if (pixel_count != NULL)
        *pixel_count = engine == NULL ? 0 : engine->framebuffer_pixels;
    return engine == NULL || engine->state != BROWSER_ENGINE_ACTIVE
        ? NULL : engine->framebuffer;
}
