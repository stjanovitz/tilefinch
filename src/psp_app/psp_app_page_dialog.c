/* window.alert(), confirm() and prompt() on the PSP: the platform
 * page_dialog service (include/tilefinch/platform.h). The engine has
 * already decided the page may ask (tilefinch/page_dialog.h); this shows
 * the question and returns the user's answer, or false when it cannot be
 * shown now, which the page gets as the default answer.
 *
 * The engine calls this from inside the page's script, on the browser
 * thread, and the script stays suspended until it returns: the HTML pause.
 * That is a nested loop, and it is safe here because:
 *  - only the browser thread runs page script or calls the engine, and it
 *    is in this loop, which never calls the engine: no task, timer, rAF
 *    callback or event of the page can run, and nothing reenters it;
 *  - no GE list is open: WebGL and the compositor start and finish theirs
 *    within one native call;
 *  - the callback-thread supervisor is fenced out for the dialog's life by
 *    its presentation fence, so the dialog alone owns the screen and pad;
 *  - the transport worker, the media and game-audio threads and the HOME
 *    and power callbacks run on their own threads and carry on. Media only
 *    advances while its player is visible, and then the page is hidden and
 *    may not ask.
 * HOME, suspend and lost presentation dismiss the dialog. The time it was
 * open is paused out of the navigation watchdog here and of the script
 * watchdog by the engine.
 */
#include "psp_app_internal.h"

#include <pspctrl.h>
#include <pspdisplay.h>

#include "tilefinch/page_dialog.h"
#include "tilefinch/psp_text_input.h"
#include "tilefinch/work_ledger.h"

static struct {
    PspUiState *ui;
    PspEngineViews *views;
    PspTextInputService *text_input;
    PspInteractiveState *interactive;
    Budget *budget;
    PspApp *app;
    const char *argv0;
    int owner_thread;
    uint64_t open_us;
} psp_page_dialog;

/* What is on screen with the dialog over it. Static: the UI state is about
   1 KiB, and only one dialog is ever open. */
static PspUiState psp_page_dialog_ui;
/* Prepared page text for the open dialog (psp_ui_page_dialog_prepare). */
static char psp_page_dialog_message[PSP_UI_PAGE_DIALOG_SHOWN_MESSAGE_BYTES];
static char psp_page_dialog_value[PSP_UI_PAGE_DIALOG_SHOWN_VALUE_BYTES];

void psp_page_dialog_bind(PspUiState *ui, PspEngineViews *views,
                          PspTextInputService *text_input,
                          PspInteractiveState *interactive,
                          Budget *budget, const char *argv0)
{
    psp_page_dialog.budget = budget;
    psp_page_dialog.ui = ui;
    psp_page_dialog.views = views;
    psp_page_dialog.text_input = text_input;
    psp_page_dialog.interactive = interactive;
    psp_page_dialog.argv0 = argv0;
    psp_page_dialog.owner_thread = sceKernelGetThreadId();
}

void psp_page_dialog_set_app(PspApp *app)
{
    psp_page_dialog.app = app;
}

uint64_t psp_page_dialog_open_us(void)
{
    return psp_page_dialog.open_us;
}

static uint32_t psp_page_dialog_physical(uint32_t buttons)
{
    uint32_t mapped = 0;
    if (buttons & PSP_CTRL_CROSS) mapped |= PAGE_DIALOG_BUTTON_CROSS;
    if (buttons & PSP_CTRL_CIRCLE) mapped |= PAGE_DIALOG_BUTTON_CIRCLE;
    if (buttons & PSP_CTRL_TRIANGLE) mapped |= PAGE_DIALOG_BUTTON_TRIANGLE;
    if (buttons & PSP_CTRL_SQUARE) mapped |= PAGE_DIALOG_BUTTON_SQUARE;
    if (buttons & PSP_CTRL_START) mapped |= PAGE_DIALOG_BUTTON_START;
    return mapped;
}

/* The held dialog buttons this frame; *raw is the pad as sampled. */
static uint32_t psp_page_dialog_sample(uint32_t *raw)
{
    *raw = 0;
#ifdef TILEFINCH_PSP_VALIDATION_LOG
    /* A scripted run drives the dialog as it drives text entry. */
    PspUiInput scripted = { .analog_x = 128, .analog_y = 128 };
    if (psp_input_script_text_frame(&scripted)) {
        uint32_t mapped = 0;
        if (scripted.held & PSP_UI_BUTTON_CONFIRM)
            mapped |= PAGE_DIALOG_BUTTON_CROSS;
        if (scripted.held & PSP_UI_BUTTON_CANCEL)
            mapped |= PAGE_DIALOG_BUTTON_CIRCLE;
        if (scripted.held & PSP_UI_BUTTON_TOOLBAR)
            mapped |= PAGE_DIALOG_BUTTON_TRIANGLE;
        if (scripted.held & PSP_UI_BUTTON_RELOAD)
            mapped |= PAGE_DIALOG_BUTTON_SQUARE;
        if (scripted.held & PSP_UI_BUTTON_ADDRESS)
            mapped |= PAGE_DIALOG_BUTTON_START;
        return mapped;
    }
#endif
    SceCtrlData pad = {0};
    if (sceCtrlPeekBufferPositive(&pad, 1) <= 0) return 0;
    *raw = pad.Buttons;
    return psp_page_dialog_physical(pad.Buttons);
}

static uint64_t psp_page_dialog_now_ms(void)
{
    return (uint64_t) sceKernelGetSystemTimeWide() / 1000u;
}

static bool psp_page_dialog_present(const uint16_t *frame)
{
    return psp_present_internal(frame, &psp_page_dialog_ui, false);
}

/* Pack glyphs for the dialog's own text: a pack its script needs, and the
   glyph data the last draw asked for. Re-prepares and redraws when either
   arrived. At most `units` reads, each one bounded I/O unit. */
static bool psp_page_dialog_glyphs(PspUiPageDialogView *view,
                                   const char *message, const char *value,
                                   const uint16_t *frame, unsigned units)
{
    bool changed = false;
    for (unsigned at = 0; at < units; at++) {
        if (!psp_glyph_component_dialog_glyphs(
                psp_page_dialog.app, message, value)) break;
        changed = true;
    }
    if (!changed) return false;
    view->message = message;
    view->value = value;
    psp_ui_page_dialog_prepare(psp_page_dialog.budget, view,
                               psp_page_dialog_message, psp_page_dialog_value);
    psp_ui_show_page_dialog(&psp_page_dialog_ui, view);
    return psp_page_dialog_present(frame);
}

#ifdef TILEFINCH_PSP_VALIDATION_LOG
/* Validation: the first dialog of each kind a scripted run shows, beside
   the EBOOT as frame-mark-page-dialog-<kind>.ppm (collected by
   scripts/run-ppsspp-input-script.sh). */
static void psp_page_dialog_capture(PageDialogKind kind)
{
    static bool captured[3];
    if (!psp_input_script_running() || (unsigned) kind >= 3u
        || captured[kind]) return;
    static const char *const names[3] = {
        "frame-mark-page-dialog-alert.ppm",
        "frame-mark-page-dialog-confirm.ppm",
        "frame-mark-page-dialog-prompt.ppm"
    };
    const uint16_t *presented = psp_display_front_buffer(&psp_display);
    captured[kind] = presented != NULL
        && psp_dump_frame_strided_named(
               psp_page_dialog.argv0, names[kind], presented,
               PSP_DISPLAY_BUFFER_PIXELS, PSP_DISPLAY_STRIDE);
    printf("tilefinch-page-dialog: capture=%s written=%u\n", names[kind],
           captured[kind] ? 1u : 0u);
}
#endif

bool psp_page_dialog_show(void *context,
                          const TilefinchPageDialogRequest *request,
                          TilefinchPageDialogResponse *response)
{
    (void) context;
    if (request == NULL || response == NULL || response->text == NULL
        || response->text_capacity == 0
        || (unsigned) request->kind > (unsigned) PAGE_DIALOG_PROMPT
        || psp_page_dialog.ui == NULL || psp_page_dialog.views == NULL
        || sceKernelGetThreadId() != psp_page_dialog.owner_thread
        || psp_home_exit_pending()
        || !psp_lifecycle_presentation_allowed(&psp_lifecycle)
        || (psp_active_media != NULL && psp_active_media->ui.visible))
        return false;
    PspNavigationCooperate *cooperate = &psp_navigation_cooperate;
    bool fenced = psp_presentation_fence_acquire();
    /* Under a scope, the supervisor's snapshot is what the user sees. */
    const PspUiState *base = fenced
        ? &cooperate->supervisor_ui : psp_page_dialog.ui;
    const uint16_t *frame = fenced
        ? psp_supervisor_page_frame(cooperate) : psp_page_dialog.views->frame;
    if (frame == NULL || psp_ui_screen_is_native_surface(base->screen)) {
        psp_presentation_fence_release(fenced);
        return false;
    }
    PspLogPhase phase = psp_log_phase();
    psp_page_dialog_ui = *base;
    psp_page_dialog_ui.toast_frames = 0;
    psp_page_dialog_ui.loading = false;
    const char *message = request->message != NULL ? request->message : "";
    const char *glyph_value = request->default_value;
    PspUiPageDialogView view = {
        .kind = request->kind,
        .offer_suppress = request->offer_suppress,
        .host = request->host != NULL ? request->host : "",
        .message = message,
        .value = request->default_value
    };
    /* A script only the dialog uses gets its pack before it is measured. */
    (void) psp_glyph_component_dialog_glyphs(
        psp_page_dialog.app, message, glyph_value);
    psp_ui_page_dialog_prepare(psp_page_dialog.budget, &view,
                               psp_page_dialog_message, psp_page_dialog_value);
    psp_ui_show_page_dialog(&psp_page_dialog_ui, &view);
    uint64_t opened_us = (uint64_t) sceKernelGetSystemTimeWide();
    uint32_t raw = 0;
    PageDialogModal modal;
    page_dialog_modal_open(&modal, request->kind, request->offer_suppress,
                           psp_page_dialog_sample(&raw), opened_us / 1000u);
    bool shown = psp_page_dialog_present(frame);
    /* The first draw asked for the glyph data it lacked; a few reads now
       usually replace its placeholder boxes before anyone sees them. */
    if (shown)
        (void) psp_page_dialog_glyphs(&view, message, glyph_value, frame, 8u);
#ifdef TILEFINCH_PSP_VALIDATION_LOG
    printf("tilefinch-page-dialog: open kind=%u suppress-offer=%u shown=%u\n",
           (unsigned) request->kind, request->offer_suppress ? 1u : 0u,
           shown ? 1u : 0u);
    if (shown) psp_page_dialog_capture(request->kind);
#endif
    PageDialogModalStep step = shown
        ? PAGE_DIALOG_MODAL_WAIT : PAGE_DIALOG_MODAL_DISMISS;
    /* The prompt's shown answer; glyph_value points here after an edit. */
    char shown_value[PAGE_DIALOG_PROMPT_BYTES];
    /* Bounded: page_dialog_modal_step dismisses at the open limit. */
    while (step == PAGE_DIALOG_MODAL_WAIT) {
        if (psp_home_exit_pending()
            || !psp_lifecycle_presentation_allowed(&psp_lifecycle)) {
            step = PAGE_DIALOG_MODAL_DISMISS;
            break;
        }
        psp_log_heartbeat();
        psp_log_set_stage("page-dialog");
        unsigned ledger = work_ledger_enter(WORK_LEDGER_SLEEP);
        sceDisplayWaitVblankStart();
        work_ledger_leave(ledger);
        step = page_dialog_modal_step(
            &modal, psp_page_dialog_sample(&raw), psp_page_dialog_now_ms());
        if (step == PAGE_DIALOG_MODAL_WAIT)
            (void) psp_page_dialog_glyphs(&view, message, glyph_value, frame, 1u);
        if (step != PAGE_DIALOG_MODAL_EDIT) continue;
        /* prompt: the browser's own keyboard edits the answer. */
        char edited[PAGE_DIALOG_PROMPT_BYTES];
        char starting[PAGE_DIALOG_PROMPT_BYTES];
        (void) page_dialog_editable_text(response->text, strlen(response->text),
                                         starting, sizeof(starting));
        PspTextInputRequest text_request = {
            .description = "ENTER TEXT",
            .initial = starting
        };
        if (psp_page_dialog.text_input != NULL
            && psp_text_input_request(
                   psp_page_dialog.text_input, frame, &psp_page_dialog_ui,
                   &text_request, edited, sizeof(edited)))
            snprintf(response->text, response->text_capacity, "%s", edited);
        /* The answer goes to the page as it is, not to the screen. */
        (void) page_dialog_sanitize_text(response->text, strlen(response->text),
                                         true, 1u, shown_value,
                                         sizeof(shown_value), NULL);
        view.message = message;
        view.value = shown_value;
        glyph_value = view.value;
        psp_ui_page_dialog_prepare(psp_page_dialog.budget, &view,
                                   psp_page_dialog_message, psp_page_dialog_value);
        psp_ui_show_page_dialog(&psp_page_dialog_ui, &view);
        (void) psp_page_dialog_present(frame);
        page_dialog_modal_resume(&modal, psp_page_dialog_sample(&raw));
        step = PAGE_DIALOG_MODAL_WAIT;
    }
    response->outcome =
        step == PAGE_DIALOG_MODAL_ACCEPT ? PAGE_DIALOG_OUTCOME_ACCEPTED
        : step == PAGE_DIALOG_MODAL_SUPPRESS ? PAGE_DIALOG_OUTCOME_SUPPRESS
        : step == PAGE_DIALOG_MODAL_LEAVE ? PAGE_DIALOG_OUTCOME_LEAVE
        : PAGE_DIALOG_OUTCOME_DISMISSED;
    psp_ui_clear_page_dialog(&psp_page_dialog_ui);

    /* Hand the screen and pad back. The button that answered is still
       held: every receiver takes it as already seen, so it cannot also act
       on the page (a Circle that dismissed must not go Back too). */
    if (shown) (void) psp_present_internal(frame, base, true);
    SceCtrlData pad = {0};
    uint32_t still = sceCtrlPeekBufferPositive(&pad, 1) > 0
        ? pad.Buttons : raw;
    (void) psp_controller_take_latched_pressed();
    uint64_t now_us = (uint64_t) sceKernelGetSystemTimeWide();
    uint64_t open_us = now_us > opened_us ? now_us - opened_us : 0;
    if (fenced) {
        cooperate->supervisor_previous_buttons = still;
        cooperate->fallback_previous_buttons = still;
        cooperate->last_checkpoint_us = now_us;
    }
    if (psp_page_dialog.interactive != NULL) {
        psp_page_dialog.interactive->previous_buttons = psp_ui_buttons(still);
        psp_navigation_watchdog_pause(
            &psp_page_dialog.interactive->navigation_watchdog, open_us);
    }
    psp_page_dialog.open_us += open_us;
    if (response->outcome == PAGE_DIALOG_OUTCOME_LEAVE) {
        /* Leaving a page that is still loading stops the load, as Circle
           does there; a loaded page goes Back, as Circle does there. */
        if (fenced && !cooperate->owner_thread_only
            && cooperate->engine != NULL) {
            tilefinch_cancellation_request(&cooperate->cancellation);
            cooperate->cancellation_requested_us = now_us;
        } else {
            (void) psp_navigation_cooperate_queue_page_input(
                PSP_UI_BUTTON_CANCEL);
        }
    }
    psp_presentation_fence_release(fenced);
    psp_log_set_phase(phase);
#ifdef TILEFINCH_PSP_VALIDATION_LOG
    printf("tilefinch-page-dialog: close kind=%u outcome=%u open-ms=%llu\n",
           (unsigned) request->kind, (unsigned) response->outcome,
           (unsigned long long) (open_us / 1000u));
#endif
    return shown;
}
