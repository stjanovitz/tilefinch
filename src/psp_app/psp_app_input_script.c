/* Device glue for the scripted-input harness.
 *
 * The parser and stepper live in src/psp_input_script.c, which knows nothing
 * about the PSP. This TU owns the one script the EBOOT can run, resolves its
 * path the way every other boot asset is resolved, and turns each frame into
 * the `tilefinch-input-script:` lines the golden is made of.
 *
 * Compiled into psp-browser-script only when TILEFINCH_PSP_VALIDATION_LOG is
 * on (see cmake/TilefinchTargets.cmake). Shipping EBOOTs omit this TU.
 */
#include "psp_app_internal.h"

#include "tilefinch/pixel_math.h"
#include "tilefinch/psp_input_script.h"

/*
 * One interactive loop means one script. File-static storage keeps
 * validation-only state out of the shipping owner records.
 */
static PspInputScript psp_input_script;
static bool psp_input_script_ever_ready;
static bool psp_input_script_saw_exit;
static uint32_t psp_input_script_previous_buttons;
static uint16_t psp_input_script_last_press_step;

/* End-to-end Page-controls evidence. Script step counts prove only that the
   validation source emitted input; these counters sit at the Gamepad API
   publication boundary and prove that a connected page received it. */
static uint32_t psp_input_gamepad_connected_frames;
static uint32_t psp_input_gamepad_button_frames;
static uint32_t psp_input_gamepad_analog_frames;
static uint32_t psp_input_gamepad_publications;
static uint32_t psp_input_gamepad_connections;
static uint32_t psp_input_gamepad_buttons_seen;
static int16_t psp_input_gamepad_axis_x_min;
static int16_t psp_input_gamepad_axis_x_max;
static int16_t psp_input_gamepad_axis_y_min;
static int16_t psp_input_gamepad_axis_y_max;
static bool psp_input_gamepad_was_connected;

/* Coverage tally. Indexed by enum value; both spaces are small and closed,
   and the summary prints only the entries a run actually reached. */
#define PSP_INPUT_SCRIPT_ACTION_SLOTS ((unsigned) PSP_UI_ACTION_EXIT + 1u)
#define PSP_INPUT_SCRIPT_SETTING_SLOTS \
    ((unsigned) PSP_UI_SETTING_GAMEPAD_FACE_MAPPING + 1u)
static uint16_t psp_input_script_action_hits[PSP_INPUT_SCRIPT_ACTION_SLOTS];
static uint16_t psp_input_script_setting_hits[PSP_INPUT_SCRIPT_SETTING_SLOTS];

/* Temporal snapshots are copied to a small RAM ring at marked frames and
   written only after the scenario. Memory Stick I/O therefore cannot alter
   the input-to-feedback latency being measured. */
#define PSP_INPUT_SCRIPT_CAPTURE_LIMIT 3u
#define PSP_INPUT_SCRIPT_CAPTURE_WIDTH 240u
#define PSP_INPUT_SCRIPT_CAPTURE_HEIGHT 136u
typedef struct {
    char mark[PSP_INPUT_SCRIPT_MARK_CAPACITY];
    uint8_t pixels[
        PSP_INPUT_SCRIPT_CAPTURE_WIDTH * PSP_INPUT_SCRIPT_CAPTURE_HEIGHT];
} PspInputScriptCapture;
static PspInputScriptCapture
    psp_input_script_captures[PSP_INPUT_SCRIPT_CAPTURE_LIMIT];
static size_t psp_input_script_capture_count;
/* A live mark lasts for one scripted advance, but cooperative work can run a
   second advance before the browser thread reaches its end-of-frame capture.
   Retain that checkpoint until one presentation owner consumes it. */
static char psp_input_script_pending_capture_mark[
    PSP_INPUT_SCRIPT_MARK_CAPACITY];
static bool psp_input_script_pending_capture_valid;
static TilefinchInstallPaths psp_input_script_install_paths;
static const char *psp_input_script_argv0;

/*
 * A cue transition can expose one bad scanout for only 16--33 ms. PSPLink's
 * scrshot command proves what the panel currently scans, but writing a full
 * bitmap over USB is much too slow to catch alternating buffers. Retain one
 * preceding publish and nineteen following publishes from the exact caption
 * footprint at native PSP pixel resolution, packed as RGB332. Trigger on any
 * caption-text transition: a direct cue-to-cue handoff is just as capable of
 * exposing a one-frame ground error as a cue-to-empty handoff. This is about
 * 444 KiB of validation-only BSS, including the preceding-frame and scratch
 * copies, and zero Memory Stick traffic while playback is live.
 */
#define PSP_SUBTITLE_BURST_LIMIT 20u
#define PSP_SUBTITLE_BURST_LEFT 38u
#define PSP_SUBTITLE_BURST_WIDTH 404u
#define PSP_SUBTITLE_BURST_HEIGHT 50u
#define PSP_SUBTITLE_BURST_CONTROLS_TOP 136u
#define PSP_SUBTITLE_BURST_PLAIN_TOP 204u

typedef struct {
    uint8_t pixels[PSP_SUBTITLE_BURST_WIDTH * PSP_SUBTITLE_BURST_HEIGHT];
    uint64_t captured_us;
    uint64_t frame_identity;
    uint64_t frame_epoch;
    uint32_t frame_generation;
    uint32_t subtitle_hash;
    uint32_t pixel_hash;
    uint64_t record_identity[PSP_DISPLAY_PAGE_BUFFER_COUNT];
    uint8_t record_valid_mask;
    uint8_t record_buffer[PSP_DISPLAY_PAGE_BUFFER_COUNT];
    uint8_t buffer_index;
    uint8_t ui_flags;
    uint8_t capture_top;
    bool video_surface;
    bool skipped;
    bool supervisor;
    char subtitle[64];
} PspSubtitleBurstFrame;

static PspSubtitleBurstFrame psp_subtitle_burst[PSP_SUBTITLE_BURST_LIMIT];
static PspSubtitleBurstFrame psp_subtitle_previous;
static PspSubtitleBurstFrame psp_subtitle_current_scratch;
static size_t psp_subtitle_burst_count;
static unsigned psp_subtitle_burst_remaining;
static bool psp_subtitle_previous_valid;
static bool psp_subtitle_seen_nonempty;

static void psp_input_script_warning(
    void *context, const char *path, size_t line_number, const char *reason)
{
    (void) context;
    printf("tilefinch-input-script: rejected path=\"%s\" line=%zu "
           "reason=%s\n",
           path == NULL ? "(null)" : path, line_number,
           reason == NULL ? "unknown" : reason);
}

bool psp_input_script_begin(
    const TilefinchInstallPaths *install_paths, const char *argv0,
    const char *name)
{
    psp_input_script_reset(&psp_input_script);
    psp_input_script_ever_ready = false;
    psp_input_script_saw_exit = false;
    psp_input_script_previous_buttons = 0;
    psp_input_script_last_press_step = 0;
    psp_input_gamepad_connected_frames = 0;
    psp_input_gamepad_button_frames = 0;
    psp_input_gamepad_analog_frames = 0;
    psp_input_gamepad_publications = 0;
    psp_input_gamepad_connections = 0;
    psp_input_gamepad_buttons_seen = 0;
    psp_input_gamepad_axis_x_min = 0;
    psp_input_gamepad_axis_x_max = 0;
    psp_input_gamepad_axis_y_min = 0;
    psp_input_gamepad_axis_y_max = 0;
    psp_input_gamepad_was_connected = false;
    psp_input_script_capture_count = 0;
    psp_input_script_pending_capture_mark[0] = '\0';
    psp_input_script_pending_capture_valid = false;
    psp_subtitle_burst_count = 0;
    psp_subtitle_burst_remaining = 0;
    psp_subtitle_previous_valid = false;
    psp_subtitle_seen_nonempty = false;
    psp_input_script_argv0 = argv0;
    if (install_paths != NULL)
        psp_input_script_install_paths = *install_paths;
    else
        memset(&psp_input_script_install_paths, 0,
               sizeof(psp_input_script_install_paths));
    memset(psp_input_script_action_hits, 0,
           sizeof(psp_input_script_action_hits));
    memset(psp_input_script_setting_hits, 0,
           sizeof(psp_input_script_setting_hits));
    if (name == NULL || name[0] == '\0') return false;
    char path[TILEFINCH_INSTALL_PATH_LIMIT];
    /* argv[0] belongs to the PSP loader's startup frame and is not a durable
       path source once the interactive loop begins. install_paths is the
       owned copy derived during boot and works for slot, Memory Stick, and
       host0: runs alike. */
    if (install_paths != NULL) {
        if (!tilefinch_install_program_path(
                install_paths, name, path, sizeof(path))) {
            printf("tilefinch-input-script: rejected path=\"%s\" line=0 "
                   "reason=unresolvable\n", name);
            return false;
        }
    } else {
        psp_sibling_path(path, sizeof(path), argv0, name);
    }
    if (!psp_input_script_load(
            &psp_input_script, path, psp_input_script_warning, NULL))
        return false;
    printf("tilefinch-input-script: armed script=%s steps=%u "
           "stall-limit=%u\n",
           name, (unsigned) psp_input_script.step_count,
           (unsigned) psp_input_script.stall_limit);
    return true;
}

static void psp_input_script_latch_live_capture_mark(void)
{
    const char *mark = psp_input_script_mark(&psp_input_script);
    if (mark == NULL || psp_input_script_pending_capture_valid
        || psp_input_script.step >= psp_input_script.step_count
        || (!psp_input_script.steps[psp_input_script.step].advance_while_busy
            && !psp_input_script.steps[psp_input_script.step].advance_when_painted))
        return;
    /* Measurement delimiters and the Page-controls state transition are
       control records, not visual checkpoints. */
    if (strcmp(mark, "webgl-measure-start") == 0
        || strcmp(mark, "webgl-measure-end") == 0
        || strcmp(mark, "auto-controls") == 0
        || strcmp(mark, "controls-exited") == 0) return;
    snprintf(psp_input_script_pending_capture_mark,
             sizeof(psp_input_script_pending_capture_mark), "%s", mark);
    psp_input_script_pending_capture_valid = true;
}

bool psp_input_script_running(void)
{
    return psp_input_script_armed(&psp_input_script)
        && !psp_input_script.finished;
}

bool psp_input_script_report_pending(void)
{
    return psp_input_script_exit_pending(&psp_input_script);
}

void psp_input_script_interrupt_by_user(void)
{
    if (!psp_input_script_running()) return;
    psp_input_script_interrupt(&psp_input_script);
    psp_input_script_previous_buttons = 0;
    printf("tilefinch-input-script: event=physical-input-handoff\n");
}

bool psp_input_script_frame(
    PspUiInput *input, bool ready, bool page_ready)
{
    if (ready) psp_input_script_ever_ready = true;
    uint16_t frame_step = psp_input_script.step;
    bool driving = psp_input_script_advance_with_page(
        &psp_input_script, input, psp_input_script_previous_buttons,
        ready, page_ready);
    if (input != NULL) {
        psp_input_script_previous_buttons = input->held;
        if (input->pressed != 0) {
            psp_input_script_last_press_step = frame_step;
            printf("tilefinch-input-script-edge: step=%u buttons=0x%04x "
                   "receiver=main ready=%d at-us=%llu\n",
                   (unsigned) frame_step, (unsigned) input->pressed,
                   ready ? 1 : 0,
                   (unsigned long long) sceKernelGetSystemTimeWide());
        }
    }
    psp_webgl_measurement_mark(
        psp_input_script_mark(&psp_input_script));
    psp_input_script_latch_live_capture_mark();
    return driving;
}

bool psp_input_script_busy_frame(PspUiInput *input)
{
    /* The callback-thread supervisor may advance explicit live commands while
       the main thread is inside the receiver for its original press. Preserve
       that main-thread step attribution until the receiver returns. */
    uint16_t frame_step = psp_input_script.step;
    bool driving = psp_input_script_advance(
        &psp_input_script, input, psp_input_script_previous_buttons, false);
    if (input != NULL) {
        psp_input_script_previous_buttons = input->held;
        if (input->pressed != 0) {
            psp_input_script_last_press_step = frame_step;
            printf("tilefinch-input-script-edge: step=%u buttons=0x%04x "
                   "receiver=supervisor ready=0 at-us=%llu\n",
                   (unsigned) frame_step, (unsigned) input->pressed,
                   (unsigned long long) sceKernelGetSystemTimeWide());
        }
    }
    psp_webgl_measurement_mark(
        psp_input_script_mark(&psp_input_script));
    psp_input_script_latch_live_capture_mark();
    return driving;
}

bool psp_input_script_text_frame(PspUiInput *input)
{
    if (!psp_input_script_running()) return false;
    SceCtrlData pad = {0};
    if (sceCtrlPeekBufferPositive(&pad, 1) > 0 && pad.Buttons != 0) {
        psp_input_script_interrupt_by_user();
        return false;
    }
    if (psp_input_script_frame(input, true, true)) return true;
    if (!psp_input_script_cancel_exhausted_modal(&psp_input_script, input))
        return false;
    printf("tilefinch-input-script: event=keyboard-input-exhausted step=%u\n",
           (unsigned) psp_input_script.step);
    return true;
}

/*
 * The coverage record. One line per frame that produced something, which is
 * what makes the golden diffable: idle frames say nothing, and every action
 * or setting that reached a receiver says so exactly once.
 */
void psp_input_script_observe(const PspUiIntent *intent, const PspUiState *ui)
{
    if (!psp_input_script_armed(&psp_input_script)) return;
    const char *screen = ui == NULL
        ? "unknown" : psp_input_script_screen_name(ui->screen);
    const char *mark = psp_input_script_mark(&psp_input_script);
    if (mark != NULL) {
        printf("tilefinch-input-script: mark=%s step=%u screen=%s\n",
               mark, (unsigned) psp_input_script.step, screen);
        if (ui != NULL)
            printf("tilefinch-input-chrome: mark=%s visible=%u loading=%u "
                   "reader=%u basic=%u status=\"%s\"\n",
                   mark, ui->chrome_visible ? 1u : 0u,
                   ui->loading ? 1u : 0u, ui->reader_mode ? 1u : 0u,
                   ui->basic_mode ? 1u : 0u, ui->status);
        printf("tilefinch-focus-probe: mark=%s visible=%d rect=%d,%d,%d,%d at-us=%llu\n",
               mark, ui != NULL && ui->has_focus ? 1 : 0,
               ui == NULL ? 0 : ui->focus_x,
               ui == NULL ? 0 : ui->focus_y,
               ui == NULL ? 0 : ui->focus_width,
               ui == NULL ? 0 : ui->focus_height,
               (unsigned long long) sceKernelGetSystemTimeWide());
    }
    if (intent == NULL) return;
    PspUiAction action = intent->action;
    PspUiSettingId setting = intent->setting.id;
    if (action == PSP_UI_ACTION_NONE && setting == PSP_UI_SETTING_NONE)
        return;
    if (action == PSP_UI_ACTION_EXIT) psp_input_script_saw_exit = true;
    if ((unsigned) action < PSP_INPUT_SCRIPT_ACTION_SLOTS
        && psp_input_script_action_hits[(unsigned) action] < 0xFFFFu)
        psp_input_script_action_hits[(unsigned) action]++;
    if ((unsigned) setting < PSP_INPUT_SCRIPT_SETTING_SLOTS
        && psp_input_script_setting_hits[(unsigned) setting] < 0xFFFFu)
        psp_input_script_setting_hits[(unsigned) setting]++;
    printf("tilefinch-input-script: step=%u action=%s setting=%s "
           "screen=%s list=%u tab=%u\n",
           (unsigned) psp_input_script_last_press_step,
           psp_input_script_action_name(action),
           psp_input_script_setting_name(setting),
           screen, (unsigned) intent->list_index,
           (unsigned) intent->tab_index);
}

void psp_input_script_observe_page(
    const PspEngineViews *views)
{
    const NavigationSession *navigation = views == NULL ? NULL : views->navigation;
    const char *mark = psp_input_script_mark(&psp_input_script);
    if (mark == NULL || navigation == NULL) return;
    const NavigationPage *page = &navigation->page;
    const BrowserController *controller = views->controller;
    lxb_dom_node_t *focus = NULL;
    if (controller != NULL && controller->focus_kind == CONTROLLER_FOCUS_LINK
        && controller->focus_index < page->layout.link_count)
        focus = page->layout.links[controller->focus_index].node;
    else if (controller != NULL && controller->focus_kind == CONTROLLER_FOCUS_CONTROL
             && controller->focus_index < page->layout.control_count)
        focus = page->layout.controls[controller->focus_index].node;
    else if (controller != NULL && controller->focus_kind == CONTROLLER_FOCUS_POINTER)
        focus = controller->pointer_node;
    if (focus != NULL) {
        int focus_width = 0, focus_height = 0;
        bool has_area = controller_focused_rect(
            controller, NULL, NULL, &focus_width, &focus_height)
            && focus_width > 0 && focus_height > 0;
        size_t id_length = 0, class_length = 0;
        const char *id = document_attribute(focus, "id", &id_length);
        const char *class_name = document_attribute(focus, "class", &class_length);
        printf("tilefinch-input-focus-target: mark=%s kind=%d index=%zu indicator=%s id=%.*s class=%.*s url=%.256s\n",
               mark, (int) controller->focus_kind, controller->focus_index,
               !has_area ? "none" : controller->has_authored_focus_outline
                    ? "authored" : "browser",
               (int) (id_length < 96 ? id_length : 96), id == NULL ? "" : id,
               (int) (class_length < 128 ? class_length : 128),
               class_name == NULL ? "" : class_name, controller->focus_link_url);
    }
    const ExternalImageStats *images = &page->images.stats;
    const char *page_url = navigation_active_document_url(navigation);
    printf("tilefinch-input-page: mark=%s generation=%llu loaded=%u "
           "resources-pending=%u url=%.512s\n", mark,
           (unsigned long long) navigation->generation, page->loaded ? 1u : 0u,
           navigation_background_resources_pending(navigation) ? 1u : 0u,
           page_url == NULL ? "" : page_url);
    printf("tilefinch-input-script-js: mark=%s discovered=%zu attempted=%zu "
           "loaded=%zu failed=%zu bytecode=%zu/%zu/%zu "
           "dynamic=%zu/%zu/%zu/%zu/%zu/%zu/%zu pending=%zu summary=\"%.96s\" "
           "error=\"%.160s\"\n",
           mark, navigation->script_discovered,
           navigation->script_attempted, navigation->script_loaded,
           navigation->script_failed,
           page->script_result.external_script_bytecode_cache_hits,
           page->script_result.external_script_bytecode_cache_misses,
           page->script_result.external_script_bytecode_cache_restore_failures,
           page->script_result.dynamic_scripts_queued,
           page->script_result.dynamic_scripts_started,
           page->script_result.dynamic_scripts_completed,
           page->script_result.dynamic_scripts_failed,
           page->script_result.dynamic_scripts_cache_hits,
           page->script_result.dynamic_scripts_quota_rejected,
           page->script_result.dynamic_script_bytes,
           page->script_result.pending_tasks,
           page->script_result.summary, page->script_result.error);
    printf("tilefinch-input-script-images: mark=%s cursor=%zu/%zu "
           "job=%d batch=%u attempts=%zu loaded=%zu failed=%zu "
           "bytes=%zu/%zu progress=%zu/%zu/%zu "
           "fetch-failures=%zu/%zu/%zu/%zu/%zu/%zu "
           "idle=%zu/%zu/%zu/%zu\n",
           mark, page->deferred_image_cursor, page->deferred_image_count,
           page->deferred_image_job != NULL ? 1 : 0,
           (unsigned) page->deferred_image_batch_count,
           images->attempted, images->loaded, images->failed,
           images->encoded_bytes, images->decoded_bytes,
           images->progress_samples, images->progress_events,
           images->progress_bytes,
           images->fetch_failures_http_4xx,
           images->fetch_failures_http_5xx,
           images->fetch_failures_timeout,
           images->fetch_failures_cancelled,
           images->fetch_failures_quota,
           images->fetch_failures_transport,
           navigation->performance.background_image_batches,
           navigation->performance.background_images_loaded,
           navigation->performance.background_image_relayouts,
           navigation->performance.background_image_failures);
}

/*
 * Media controls bypass PspUiIntent and are dispatched through their own
 * receiver.  Record that seam explicitly: a controller trace which only says
 * "Right was pressed" cannot prove autoplay was active, that the highlighted
 * seek target advanced while a preview job was in flight, or that the final
 * seek returned to playback.  Marks and actions are deliberately sparse, so
 * this adds no per-frame log traffic to a device run.
 */
void psp_input_script_observe_media(
    const PspUiMediaIntent *intent, const PspUiMediaState *media)
{
    if (!psp_input_script_armed(&psp_input_script) || media == NULL) return;
    const char *mark = psp_input_script_mark(&psp_input_script);
    if (mark != NULL) {
        printf("tilefinch-input-script-media: mark=%s step=%u "
               "visible=%d playing=%d resolving=%d failed=%d buffering=%d "
               "preview=%d current=%lluus target=%lluus duration=%lluus\n",
               mark, (unsigned) psp_input_script.step,
               media->visible ? 1 : 0, media->playing ? 1 : 0,
               media->resolving ? 1 : 0, media->failed ? 1 : 0,
               media->buffering ? 1 : 0,
               media->seek_preview_active ? 1 : 0,
               (unsigned long long) media->current_time_us,
               (unsigned long long) media->seek_preview_time_us,
               (unsigned long long) media->duration_us);
    }
    if (intent == NULL || intent->action == PSP_UI_MEDIA_ACTION_NONE) return;
    printf("tilefinch-input-script-media: step=%u action=%s "
           "requested=%lluus visible=%d playing=%d resolving=%d preview=%d "
           "current=%lluus target=%lluus\n",
           (unsigned) psp_input_script_last_press_step,
           psp_media_action_name(intent->action),
           (unsigned long long) intent->seek_time_us,
           media->visible ? 1 : 0, media->playing ? 1 : 0,
           media->resolving ? 1 : 0,
           media->seek_preview_active ? 1 : 0,
           (unsigned long long) media->current_time_us,
           (unsigned long long) media->seek_preview_time_us);
}

void psp_input_script_observe_gamepad(
    bool connected, uint32_t buttons, int16_t axis_x, int16_t axis_y,
    bool published)
{
    if (!psp_input_script_armed(&psp_input_script)) return;
    if (connected && !psp_input_gamepad_was_connected)
        psp_input_gamepad_connections++;
    psp_input_gamepad_was_connected = connected;
    if (!connected) return;
    psp_input_gamepad_connected_frames++;
    if (published) psp_input_gamepad_publications++;
    if (buttons != 0u) {
        psp_input_gamepad_button_frames++;
        psp_input_gamepad_buttons_seen |= buttons;
    }
    if (axis_x != 0 || axis_y != 0) psp_input_gamepad_analog_frames++;
    if (axis_x < psp_input_gamepad_axis_x_min)
        psp_input_gamepad_axis_x_min = axis_x;
    if (axis_x > psp_input_gamepad_axis_x_max)
        psp_input_gamepad_axis_x_max = axis_x;
    if (axis_y < psp_input_gamepad_axis_y_min)
        psp_input_gamepad_axis_y_min = axis_y;
    if (axis_y > psp_input_gamepad_axis_y_max)
        psp_input_gamepad_axis_y_max = axis_y;
}

uint16_t psp_input_script_diagnostic_step(void)
{
    return psp_input_script.step;
}

uint32_t psp_input_script_diagnostic_buttons(void)
{
    return psp_input_script_previous_buttons;
}

void psp_input_script_capture_named(
    const char *mark, const uint16_t *frame, size_t pixels,
    size_t stride_pixels)
{
    if (mark == NULL || frame == NULL
        || psp_input_script_capture_count >= PSP_INPUT_SCRIPT_CAPTURE_LIMIT
        || stride_pixels < PSP_SCREEN_WIDTH
        || pixels < stride_pixels * (size_t) PSP_SCREEN_HEIGHT) return;
    /* The callback supervisor can observe one live MARK for several 16 ms
       ticks while the browser thread is busy. A mark names a checkpoint, not
       a frame sequence: retain its first image once so repeated observations
       cannot consume all three slots before the named cancel acknowledgement
       is presented. */
    for (size_t at = 0; at < psp_input_script_capture_count; at++) {
        if (strcmp(psp_input_script_captures[at].mark, mark) == 0) return;
    }
    PspInputScriptCapture *capture =
        &psp_input_script_captures[psp_input_script_capture_count++];
    snprintf(capture->mark, sizeof(capture->mark), "%s", mark);
    for (size_t y = 0; y < PSP_INPUT_SCRIPT_CAPTURE_HEIGHT; y++) {
        const uint16_t *source = frame + (y * 2u) * stride_pixels;
        uint8_t *destination = capture->pixels
            + y * PSP_INPUT_SCRIPT_CAPTURE_WIDTH;
        for (size_t x = 0; x < PSP_INPUT_SCRIPT_CAPTURE_WIDTH; x++) {
            uint16_t pixel = source[x * 2u];
            destination[x] = (uint8_t) (
                (tilefinch_rgb565_red_code(pixel) >> 2u) << 5u
                | (tilefinch_rgb565_green_code(pixel) >> 3u) << 2u
                | (tilefinch_rgb565_blue_code(pixel) >> 3u));
        }
    }
}

void psp_input_script_capture_live_mark(
    const uint16_t *frame, size_t pixels, size_t stride_pixels)
{
    if (!psp_input_script_pending_capture_valid) return;
    psp_input_script_capture_named(
        psp_input_script_pending_capture_mark, frame, pixels, stride_pixels);
    psp_input_script_pending_capture_mark[0] = '\0';
    psp_input_script_pending_capture_valid = false;
}

static uint32_t psp_subtitle_burst_hash_string(const char *text)
{
    uint32_t hash = UINT32_C(2166136261);
    if (text == NULL) return hash;
    for (size_t at = 0; at < PSP_UI_MEDIA_SUBTITLE_TEXT_CAPACITY
         && text[at] != '\0'; at++) {
        hash = (hash ^ (unsigned char) text[at]) * UINT32_C(16777619);
    }
    return hash;
}

static void psp_subtitle_burst_sample(
    PspSubtitleBurstFrame *capture, const PspUiMediaState *media,
    const uint16_t *pixels_565, const uint32_t *pixels_8888,
    size_t stride_pixels, bool video_surface, unsigned buffer_index,
    bool skipped, bool supervisor, const MediaVideoFrame *frame,
    const PspMediaPresentRecord *records, size_t record_count)
{
    memset(capture, 0, sizeof(*capture));
    capture->captured_us = sceKernelGetSystemTimeWide();
    capture->buffer_index = (uint8_t) buffer_index;
    capture->video_surface = video_surface;
    capture->skipped = skipped;
    capture->supervisor = supervisor;
    if (frame != NULL) {
        capture->frame_identity = frame->identity;
        capture->frame_epoch = frame->epoch;
        capture->frame_generation = frame->generation;
    }
    const char *subtitle = media != NULL && media->presentation != NULL
        ? media->presentation->subtitle_text : "";
    capture->subtitle_hash = psp_subtitle_burst_hash_string(subtitle);
    snprintf(capture->subtitle, sizeof(capture->subtitle), "%.*s",
             (int) sizeof(capture->subtitle) - 1, subtitle);
    for (size_t at = 0; capture->subtitle[at] != '\0'; at++) {
        unsigned char ch = (unsigned char) capture->subtitle[at];
        if (ch < 0x20u || ch == 0x7fu) capture->subtitle[at] = ' ';
    }
    if (media != NULL) {
        if (media->controls_visible) capture->ui_flags |= 1u << 0;
        if (media->buffering) capture->ui_flags |= 1u << 1;
        if (media->playing) capture->ui_flags |= 1u << 2;
        if (media->resolving) capture->ui_flags |= 1u << 3;
        if (media->seek_in_progress) capture->ui_flags |= 1u << 4;
    }
    if (records != NULL) {
        if (record_count > PSP_DISPLAY_PAGE_BUFFER_COUNT)
            record_count = PSP_DISPLAY_PAGE_BUFFER_COUNT;
        for (size_t at = 0; at < record_count; at++) {
            capture->record_identity[at] = records[at].identity;
            capture->record_buffer[at] = records[at].buffer_index;
            if (records[at].valid)
                capture->record_valid_mask |= (uint8_t) (1u << at);
        }
    }

    capture->capture_top = (uint8_t) (
        media != NULL && media->controls_visible
            ? PSP_SUBTITLE_BURST_CONTROLS_TOP
            : PSP_SUBTITLE_BURST_PLAIN_TOP);
    uint32_t pixel_hash = UINT32_C(2166136261);
    for (size_t y = 0; y < PSP_SUBTITLE_BURST_HEIGHT; y++) {
        size_t source_y = capture->capture_top + y;
        uint8_t *destination = capture->pixels
            + y * PSP_SUBTITLE_BURST_WIDTH;
        for (size_t x = 0; x < PSP_SUBTITLE_BURST_WIDTH; x++) {
            size_t source_x = PSP_SUBTITLE_BURST_LEFT + x;
            uint8_t packed;
            if (video_surface) {
                const unsigned char *rgba = (const unsigned char *)
                    &pixels_8888[source_y * stride_pixels + source_x];
                packed = (uint8_t) ((rgba[0] & 0xe0u)
                    | ((rgba[1] >> 3u) & 0x1cu)
                    | (rgba[2] >> 6u));
            } else {
                uint16_t pixel =
                    pixels_565[source_y * stride_pixels + source_x];
                packed = (uint8_t) (
                    (tilefinch_rgb565_red_code(pixel) >> 2u) << 5u
                    | (tilefinch_rgb565_green_code(pixel) >> 3u) << 2u
                    | (tilefinch_rgb565_blue_code(pixel) >> 3u));
            }
            destination[x] = packed;
            pixel_hash = (pixel_hash ^ packed) * UINT32_C(16777619);
        }
    }
    capture->pixel_hash = pixel_hash;
}

void psp_input_script_capture_media_present(
    const PspUiMediaState *media, const uint16_t *pixels_565,
    const uint32_t *pixels_8888, size_t stride_pixels,
    bool video_surface, unsigned buffer_index, bool skipped,
    bool supervisor, const MediaVideoFrame *frame,
    const PspMediaPresentRecord *records, size_t record_count)
{
    if (!psp_input_script_armed(&psp_input_script)
        || media == NULL || stride_pixels < PSP_SCREEN_WIDTH
        || (video_surface ? pixels_8888 == NULL : pixels_565 == NULL)
        || psp_subtitle_burst_count == PSP_SUBTITLE_BURST_LIMIT)
        return;

    PspSubtitleBurstFrame *current = &psp_subtitle_current_scratch;
    psp_subtitle_burst_sample(
        current, media, pixels_565, pixels_8888, stride_pixels,
        video_surface, buffer_index, skipped, supervisor, frame,
        records, record_count);
    bool nonempty = current->subtitle[0] != '\0';
    if (!psp_subtitle_seen_nonempty) {
        if (nonempty) psp_subtitle_seen_nonempty = true;
    } else if (psp_subtitle_burst_remaining == 0
               && psp_subtitle_previous_valid
               && psp_subtitle_previous.subtitle[0] != '\0'
               && current->subtitle_hash
                      != psp_subtitle_previous.subtitle_hash) {
        psp_subtitle_burst[0] = psp_subtitle_previous;
        psp_subtitle_burst[1] = *current;
        psp_subtitle_burst_count = 2u;
        psp_subtitle_burst_remaining = PSP_SUBTITLE_BURST_LIMIT - 2u;
    } else if (psp_subtitle_burst_remaining != 0) {
        psp_subtitle_burst[psp_subtitle_burst_count++] = *current;
        psp_subtitle_burst_remaining--;
    }
    if (psp_subtitle_burst_remaining == 0
        && psp_subtitle_burst_count != 0) return;
    psp_subtitle_previous = *current;
    psp_subtitle_previous_valid = true;
}

static bool psp_subtitle_burst_path(
    const char *name, char *path, size_t path_size)
{
    if (name == NULL || path == NULL || path_size == 0) return false;
    if (psp_input_script_install_paths.slotted) {
        return tilefinch_install_data_path(
            &psp_input_script_install_paths, name, path, path_size);
    }
    psp_sibling_path(path, path_size, psp_input_script_argv0, name);
    return path[0] != '\0';
}

static bool psp_subtitle_burst_write_frame(
    const PspSubtitleBurstFrame *capture, size_t index)
{
    char name[64];
    snprintf(name, sizeof(name), "frame-subtitle-burst-%02u.ppm",
             (unsigned) index);
    char path[TILEFINCH_INSTALL_PATH_LIMIT];
    if (!psp_subtitle_burst_path(name, path, sizeof(path))) return false;
    FILE *file = fopen(path, "wb");
    if (file == NULL) return false;
    bool okay = fprintf(
        file, "P6\n%u %u\n255\n",
        PSP_SUBTITLE_BURST_WIDTH, PSP_SUBTITLE_BURST_HEIGHT) > 0;
    uint8_t row[PSP_SUBTITLE_BURST_WIDTH * 3u];
    for (size_t y = 0; okay && y < PSP_SUBTITLE_BURST_HEIGHT; y++) {
        const uint8_t *source = capture->pixels
            + y * PSP_SUBTITLE_BURST_WIDTH;
        for (size_t x = 0; x < PSP_SUBTITLE_BURST_WIDTH; x++) {
            uint8_t pixel = source[x];
            row[x * 3u] = (uint8_t) (((pixel >> 5) & 7u) * 255u / 7u);
            row[x * 3u + 1u] =
                (uint8_t) (((pixel >> 2) & 7u) * 255u / 7u);
            row[x * 3u + 2u] = (uint8_t) ((pixel & 3u) * 255u / 3u);
        }
        okay = fwrite(row, 1u, sizeof(row), file) == sizeof(row);
    }
    return fclose(file) == 0 && okay;
}

static bool psp_subtitle_burst_write_metadata(void)
{
    char path[TILEFINCH_INSTALL_PATH_LIMIT];
    if (!psp_subtitle_burst_path(
            "frame-subtitle-burst.txt", path, sizeof(path))) return false;
    FILE *file = fopen(path, "wb");
    if (file == NULL) return false;
    bool okay = fprintf(
        file, "tilefinch-subtitle-burst-v2\n"
              "frames=%u left=%u width=%u height=%u step=1\n",
        (unsigned) psp_subtitle_burst_count,
        (unsigned) PSP_SUBTITLE_BURST_LEFT,
        (unsigned) PSP_SUBTITLE_BURST_WIDTH,
        (unsigned) PSP_SUBTITLE_BURST_HEIGHT) > 0;
    for (size_t at = 0; okay && at < psp_subtitle_burst_count; at++) {
        const PspSubtitleBurstFrame *capture = &psp_subtitle_burst[at];
        okay = fprintf(
            file,
            "frame=%u us=%llu top=%u source=%s surface=%s buffer=%u "
            "skipped=%u "
            "ui=0x%02x frame-id=%llu epoch=%llu generation=%lu "
            "subtitle-hash=0x%08lx pixel-hash=0x%08lx records=0x%02x "
            "record-buffers=%u/%u/%u record-identities=%llu/%llu/%llu "
            "subtitle=%s\n",
            (unsigned) at, (unsigned long long) capture->captured_us,
            (unsigned) capture->capture_top,
            capture->supervisor ? "supervisor" : "ordinary",
            capture->video_surface ? "8888" : "565",
            (unsigned) capture->buffer_index,
            capture->skipped ? 1u : 0u,
            (unsigned) capture->ui_flags,
            (unsigned long long) capture->frame_identity,
            (unsigned long long) capture->frame_epoch,
            (unsigned long) capture->frame_generation,
            (unsigned long) capture->subtitle_hash,
            (unsigned long) capture->pixel_hash,
            (unsigned) capture->record_valid_mask,
            (unsigned) capture->record_buffer[0],
            (unsigned) capture->record_buffer[1],
            (unsigned) capture->record_buffer[2],
            (unsigned long long) capture->record_identity[0],
            (unsigned long long) capture->record_identity[1],
            (unsigned long long) capture->record_identity[2],
            capture->subtitle) > 0;
    }
    return fclose(file) == 0 && okay;
}

static bool psp_input_script_write_capture(
    const PspInputScriptCapture *capture)
{
    char safe_mark[PSP_INPUT_SCRIPT_MARK_CAPACITY];
    size_t length = strlen(capture->mark);
    if (length == 0 || length >= sizeof(safe_mark)) return false;
    for (size_t at = 0; at < length; at++) {
        char ch = capture->mark[at];
        safe_mark[at] = (ch >= 'a' && ch <= 'z')
                || (ch >= 'A' && ch <= 'Z')
                || (ch >= '0' && ch <= '9') || ch == '-' || ch == '_'
            ? ch : '_';
    }
    safe_mark[length] = '\0';
    char name[64];
    snprintf(name, sizeof(name), "frame-mark-%s.ppm", safe_mark);
    char path[TILEFINCH_INSTALL_PATH_LIMIT];
    bool resolved;
    if (psp_input_script_install_paths.slotted) {
        resolved = tilefinch_install_data_path(
            &psp_input_script_install_paths, name, path, sizeof(path));
    } else {
        psp_sibling_path(
            path, sizeof(path), psp_input_script_argv0, name);
        resolved = path[0] != '\0';
    }
    if (!resolved) return false;
    FILE *file = fopen(path, "wb");
    if (file == NULL) return false;
    bool okay = fprintf(
        file, "P6\n%u %u\n255\n",
        PSP_INPUT_SCRIPT_CAPTURE_WIDTH,
        PSP_INPUT_SCRIPT_CAPTURE_HEIGHT) > 0;
    uint8_t row[PSP_INPUT_SCRIPT_CAPTURE_WIDTH * 3u];
    for (size_t y = 0; okay && y < PSP_INPUT_SCRIPT_CAPTURE_HEIGHT; y++) {
        const uint8_t *source = capture->pixels
            + y * PSP_INPUT_SCRIPT_CAPTURE_WIDTH;
        for (size_t x = 0; x < PSP_INPUT_SCRIPT_CAPTURE_WIDTH; x++) {
            uint8_t pixel = source[x];
            row[x * 3u] = (uint8_t) (((pixel >> 5) & 7u) * 255u / 7u);
            row[x * 3u + 1u] =
                (uint8_t) (((pixel >> 2) & 7u) * 255u / 7u);
            row[x * 3u + 2u] = (uint8_t) ((pixel & 3u) * 255u / 3u);
        }
        okay = fwrite(row, 1u, sizeof(row), file) == sizeof(row);
    }
    return fclose(file) == 0 && okay;
}

void psp_input_script_summary(void)
{
    if (!psp_input_script_armed(&psp_input_script)) return;
    unsigned actions = 0;
    unsigned settings = 0;
    for (unsigned at = 1; at < PSP_INPUT_SCRIPT_ACTION_SLOTS; at++) {
        if (psp_input_script_action_hits[at] == 0) continue;
        actions++;
        printf("tilefinch-input-script: covered action=%s count=%u\n",
               psp_input_script_action_name((PspUiAction) at),
               (unsigned) psp_input_script_action_hits[at]);
    }
    for (unsigned at = 1; at < PSP_INPUT_SCRIPT_SETTING_SLOTS; at++) {
        if (psp_input_script_setting_hits[at] == 0) continue;
        settings++;
        printf("tilefinch-input-script: covered setting=%s count=%u\n",
               psp_input_script_setting_name((PspUiSettingId) at),
               (unsigned) psp_input_script_setting_hits[at]);
    }
    /*
     * `outcome` is the only claim this harness makes about itself, so it
     * reports what the stepper observed rather than that the steps were
     * attempted: a script that ran out of readiness says `stalled`, and a
     * script whose steps never began says `idle`. A script that drove the
     * menu's own EXIT row says so, because the app leaving first is the
     * intended ending there rather than a truncated run.
     */
    const char *outcome = psp_input_script.stalled
        ? "stalled"
        : (psp_input_script_saw_exit
               ? "exit-action"
               : (psp_input_script.reached_end
                      ? "complete"
                      : (psp_input_script_ever_ready
                             ? "interrupted" : "idle")));
    printf("tilefinch-input-script: outcome=%s steps=%u/%u frames=%u "
           "held-frames=%u actions=%u settings=%u\n",
           outcome, (unsigned) psp_input_script.step,
           (unsigned) psp_input_script.step_count,
           (unsigned) psp_input_script.ticks,
           (unsigned) psp_input_script.held_ticks,
           actions, settings);
    /*
     * The busy counters are telemetry about the machine, not about the
     * script: they count frames the browser was not ready to take a press
     * on, which is a function of how fast the host executed the work. The
     * same script crosses a different number of them on 333 MHz Allegrex
     * silicon than under PPSSPP, so a golden blessed on one would fail on
     * the other purely on timing. They keep their own prefix, which the
     * runner's trace extraction does not match, so they stay visible in the
     * validation log -- and the live-scenario gates still read them there --
     * without entering the compared trace.
     */
    printf("tilefinch-input-telemetry: busy-frames=%u busy-presses=%u\n",
           (unsigned) psp_input_script.busy_ticks,
           (unsigned) psp_input_script.busy_press_edges);
    printf("tilefinch-input-gamepad: connected-frames=%u button-frames=%u "
           "analog-frames=%u publications=%u connections=%u "
           "buttons=0x%08X axis-x=%d/%d axis-y=%d/%d\n",
           (unsigned) psp_input_gamepad_connected_frames,
           (unsigned) psp_input_gamepad_button_frames,
           (unsigned) psp_input_gamepad_analog_frames,
           (unsigned) psp_input_gamepad_publications,
           (unsigned) psp_input_gamepad_connections,
           (unsigned) psp_input_gamepad_buttons_seen,
           (int) psp_input_gamepad_axis_x_min,
           (int) psp_input_gamepad_axis_x_max,
           (int) psp_input_gamepad_axis_y_min,
           (int) psp_input_gamepad_axis_y_max);
    for (size_t at = 0; at < psp_input_script_capture_count; at++) {
        bool written = psp_input_script_write_capture(
            &psp_input_script_captures[at]);
        printf(
            "tilefinch-input-script: capture=%s written=%d size=%ux%u\n",
            psp_input_script_captures[at].mark, written ? 1 : 0,
            PSP_INPUT_SCRIPT_CAPTURE_WIDTH,
            PSP_INPUT_SCRIPT_CAPTURE_HEIGHT);
    }
    if (psp_subtitle_burst_count != 0) {
        bool metadata_written = psp_subtitle_burst_write_metadata();
        size_t frames_written = 0;
        for (size_t at = 0; at < psp_subtitle_burst_count; at++) {
            if (psp_subtitle_burst_write_frame(
                    &psp_subtitle_burst[at], at)) frames_written++;
        }
        printf("tilefinch-subtitle-burst: frames=%u/%u metadata=%d\n",
               (unsigned) frames_written,
               (unsigned) psp_subtitle_burst_count,
               metadata_written ? 1 : 0);
    }
}
