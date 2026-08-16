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

/* Coverage tally. Indexed by enum value; both spaces are small and closed,
   and the summary prints only the entries a run actually reached. */
#define PSP_INPUT_SCRIPT_ACTION_SLOTS 64u
#define PSP_INPUT_SCRIPT_SETTING_SLOTS 32u
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
static TilefinchInstallPaths psp_input_script_install_paths;
static const char *psp_input_script_argv0;

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
    psp_input_script_capture_count = 0;
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
    if (install_paths != NULL && install_paths->slotted) {
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

bool psp_input_script_running(void)
{
    return psp_input_script_armed(&psp_input_script)
        && !psp_input_script.finished;
}

void psp_input_script_interrupt_by_user(void)
{
    if (!psp_input_script_running()) return;
    psp_input_script_interrupt(&psp_input_script);
    psp_input_script_previous_buttons = 0;
    printf("tilefinch-input-script: event=physical-input-handoff\n");
}

bool psp_input_script_frame(
    PspUiInput *input, bool ready)
{
    if (ready) psp_input_script_ever_ready = true;
    uint16_t frame_step = psp_input_script.step;
    bool driving = psp_input_script_advance(
        &psp_input_script, input, psp_input_script_previous_buttons, ready);
    if (input != NULL) {
        psp_input_script_previous_buttons = input->held;
        if (input->pressed != 0) {
            psp_input_script_last_press_step = frame_step;
            printf("tilefinch-input-script-edge: step=%u buttons=0x%04x "
                   "receiver=main ready=%d\n",
                   (unsigned) frame_step, (unsigned) input->pressed,
                   ready ? 1 : 0);
        }
    }
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
                   "receiver=supervisor ready=0\n",
                   (unsigned) frame_step, (unsigned) input->pressed);
        }
    }
    return driving;
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
    if (mark != NULL)
        printf("tilefinch-input-script: mark=%s step=%u screen=%s\n",
               mark, (unsigned) psp_input_script.step, screen);
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
    const char *mark = psp_input_script_mark(&psp_input_script);
    if (mark == NULL || psp_input_script.step >= psp_input_script.step_count
        || !psp_input_script.steps[psp_input_script.step].advance_while_busy)
        return;
    psp_input_script_capture_named(mark, frame, pixels, stride_pixels);
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
    for (size_t at = 0; at < psp_input_script_capture_count; at++) {
        bool written = psp_input_script_write_capture(
            &psp_input_script_captures[at]);
        printf(
            "tilefinch-input-script: capture=%s written=%d size=%ux%u\n",
            psp_input_script_captures[at].mark, written ? 1 : 0,
            PSP_INPUT_SCRIPT_CAPTURE_WIDTH,
            PSP_INPUT_SCRIPT_CAPTURE_HEIGHT);
    }
}
