#include "tilefinch/psp_text_input.h"

#include <pspctrl.h>
#include <pspdisplay.h>
#include <pspgu.h>
#include <psputility.h>

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "tilefinch/danzeff_input.h"
#include "tilefinch/psp_log.h"

#define PSP_SCREEN_WIDTH 480
#define PSP_SCREEN_HEIGHT 272
#define PSP_VRAM_STRIDE 512
#define PSP_GU_LIST_WORDS 4096

typedef enum {
    TRANSCRIPT_CANCEL = 0,
    TRANSCRIPT_USE,
    TRANSCRIPT_RETRY,
    TRANSCRIPT_EDIT
} TranscriptAction;

static unsigned int psp_text_input_gu_list[PSP_GU_LIST_WORDS]
    __attribute__((aligned(16)));

static bool text_input_cancel_requested(
    const PspTextInputService *service);

static void present(
    PspTextInputService *service, const uint16_t *frame,
    const PspUiState *ui)
{
    if (service != NULL && service->present != NULL)
        service->present(service->present_user, frame, ui);
}

static void show_status(
    PspTextInputService *service, const uint16_t *frame,
    PspUiState *ui, const char *status)
{
    if (ui == NULL) return;
    psp_ui_show_status(ui, status, 30);
    present(service, frame, ui);
}

static size_t utf8_to_ucs2(
    const char *input, unsigned short *output, size_t capacity)
{
    if (output == NULL || capacity == 0) return 0;
    size_t in = 0;
    size_t out = 0;
    while (input != NULL && input[in] != '\0' && out + 1 < capacity) {
        unsigned char first = (unsigned char) input[in++];
        unsigned codepoint = first;
        if ((first & 0xe0u) == 0xc0u && input[in] != '\0') {
            codepoint = (first & 0x1fu) << 6;
            codepoint |= (unsigned char) input[in++] & 0x3fu;
        } else if ((first & 0xf0u) == 0xe0u && input[in] != '\0'
                   && input[in + 1] != '\0') {
            codepoint = (first & 0x0fu) << 12;
            codepoint |= ((unsigned char) input[in++] & 0x3fu) << 6;
            codepoint |= (unsigned char) input[in++] & 0x3fu;
        } else if (first >= 0x80u) {
            codepoint = '?';
            while (((unsigned char) input[in] & 0xc0u) == 0x80u) in++;
        }
        output[out++] = (unsigned short)
            (codepoint <= 0xffffu ? codepoint : '?');
    }
    output[out] = 0;
    return out;
}

static size_t ucs2_to_utf8(
    const unsigned short *input, char *output, size_t capacity)
{
    if (output == NULL || capacity == 0) return 0;
    size_t in = 0;
    size_t out = 0;
    while (input != NULL && input[in] != 0) {
        unsigned value = input[in++];
        if (value < 0x80u) {
            if (out + 1 >= capacity) break;
            output[out++] = (char) value;
        } else if (value < 0x800u) {
            if (out + 2 >= capacity) break;
            output[out++] = (char) (0xc0u | (value >> 6));
            output[out++] = (char) (0x80u | (value & 0x3fu));
        } else {
            if (out + 3 >= capacity) break;
            output[out++] = (char) (0xe0u | (value >> 12));
            output[out++] = (char) (0x80u | ((value >> 6) & 0x3fu));
            output[out++] = (char) (0x80u | (value & 0x3fu));
        }
    }
    output[out] = '\0';
    return out;
}

static bool open_keyboard(
    PspTextInputService *service,
    const char *description, const char *initial, bool url_mode,
    char *output, size_t capacity)
{
    psp_log_set_phase(PSP_LOG_PHASE_INPUT);
    psp_log_heartbeat();
    unsigned short description_ucs2[48] = {0};
    unsigned short input_ucs2[PSP_TEXT_INPUT_CAPACITY + 1] = {0};
    unsigned short output_ucs2[PSP_TEXT_INPUT_CAPACITY + 1] = {0};
    utf8_to_ucs2(description, description_ucs2,
                sizeof(description_ucs2) / sizeof(description_ucs2[0]));
    utf8_to_ucs2(initial, input_ucs2,
                sizeof(input_ucs2) / sizeof(input_ucs2[0]));

    SceUtilityOskData data;
    memset(&data, 0, sizeof(data));
    data.language = PSP_UTILITY_OSK_LANGUAGE_DEFAULT;
    data.lines = 1;
    data.unk_24 = 1;
    data.inputtype = url_mode ? PSP_UTILITY_OSK_INPUTTYPE_URL
                              : PSP_UTILITY_OSK_INPUTTYPE_ALL;
    data.desc = description_ucs2;
    data.intext = input_ucs2;
    data.outtextlength = PSP_TEXT_INPUT_CAPACITY + 1;
    data.outtextlimit = PSP_TEXT_INPUT_CAPACITY;
    data.outtext = output_ucs2;

    SceUtilityOskParams params;
    memset(&params, 0, sizeof(params));
    params.base.size = sizeof(params);
    sceUtilityGetSystemParamInt(
        PSP_SYSTEMPARAM_ID_INT_LANGUAGE, &params.base.language);
    /* PSPSDK exposes firmware setting 9 under its historical UNKNOWN name;
       its own OSK sample identifies it as the X/O accept-button setting. */
    sceUtilityGetSystemParamInt(
        PSP_SYSTEMPARAM_ID_INT_UNKNOWN, &params.base.buttonSwap);
    params.base.graphicsThread = 17;
    params.base.accessThread = 19;
    params.base.fontThread = 18;
    params.base.soundThread = 16;
    params.datacount = 1;
    params.data = &data;

    const int buffer_bytes =
        PSP_VRAM_STRIDE * PSP_SCREEN_HEIGHT * (int) sizeof(uint16_t);
    sceGuInit();
    sceGuStart(GU_DIRECT, psp_text_input_gu_list);
    sceGuDrawBuffer(
        GU_PSM_5650, (void *) (uintptr_t) buffer_bytes, PSP_VRAM_STRIDE);
    sceGuDispBuffer(
        PSP_SCREEN_WIDTH, PSP_SCREEN_HEIGHT, (void *) 0, PSP_VRAM_STRIDE);
    sceGuOffset(
        2048 - PSP_SCREEN_WIDTH / 2, 2048 - PSP_SCREEN_HEIGHT / 2);
    sceGuViewport(2048, 2048, PSP_SCREEN_WIDTH, PSP_SCREEN_HEIGHT);
    sceGuScissor(0, 0, PSP_SCREEN_WIDTH, PSP_SCREEN_HEIGHT);
    sceGuEnable(GU_SCISSOR_TEST);
    sceGuFinish();
    sceGuSync(GU_SYNC_FINISH, GU_SYNC_WHAT_DONE);
    sceDisplayWaitVblankStart();
    sceGuDisplay(GU_TRUE);

    bool initialized = sceUtilityOskInitStart(&params) >= 0;
    bool done = !initialized;
    bool cancelled = false;
    bool shutdown_requested = false;
    while (!done) {
        psp_log_heartbeat();
        sceGuStart(GU_DIRECT, psp_text_input_gu_list);
        sceGuClearColor(0x001c1207u);
        sceGuClear(GU_COLOR_BUFFER_BIT);
        sceGuFinish();
        sceGuSync(GU_SYNC_FINISH, GU_SYNC_WHAT_DONE);
        int status = sceUtilityOskGetStatus();
        if (!shutdown_requested && text_input_cancel_requested(service)) {
            cancelled = true;
            if (status == PSP_UTILITY_DIALOG_VISIBLE) {
                sceUtilityOskShutdownStart();
                shutdown_requested = true;
            }
        }
        switch (status) {
            case PSP_UTILITY_DIALOG_VISIBLE:
                if (!shutdown_requested) sceUtilityOskUpdate(1);
                break;
            case PSP_UTILITY_DIALOG_QUIT:
                if (!shutdown_requested) {
                    sceUtilityOskShutdownStart();
                    shutdown_requested = true;
                }
                break;
            case PSP_UTILITY_DIALOG_NONE:
                done = true;
                break;
            default:
                break;
        }
        sceDisplayWaitVblankStart();
        sceGuSwapBuffers();
    }
    sceGuDisplay(GU_FALSE);
    sceGuTerm();

    if (!initialized || cancelled
        || data.result == PSP_UTILITY_OSK_RESULT_CANCELLED)
        return false;
    if (data.result == PSP_UTILITY_OSK_RESULT_UNCHANGED) {
        snprintf(output, capacity, "%s", initial == NULL ? "" : initial);
    } else {
        ucs2_to_utf8(output_ucs2, output, capacity);
    }
    return true;
}

static size_t refresh_danzeff_view(
    PspTextInputService *service, PspUiState *ui,
    const PspTextInputRequest *request, const char *text, size_t cursor,
    bool replace_all, unsigned cell, DanzeffInputMode mode, bool shifted,
    BrowserProfileSuggestion suggestions[BROWSER_PROFILE_SUGGESTION_LIMIT],
    size_t existing_count, bool recompute, int suggestion_selection,
    size_t output_capacity,
    PspUiTextEntryView *view)
{
    size_t count = existing_count;
    if (recompute) {
        count = 0;
        if (request->suggest_navigation && !replace_all
            && service->profile != NULL && text[0] != '\0') {
            count = browser_profile_suggest(
                service->profile, text, suggestions,
                BROWSER_PROFILE_SUGGESTION_LIMIT);
            size_t kept = 0;
            for (size_t at = 0; at < count; at++) {
                if (suggestions[at].url == NULL
                    || strlen(suggestions[at].url) >= output_capacity)
                    continue;
                suggestions[kept++] = suggestions[at];
            }
            count = kept;
        } else {
            memset(suggestions, 0,
                   BROWSER_PROFILE_SUGGESTION_LIMIT
                       * sizeof(*suggestions));
        }
    }
    if (suggestion_selection < 0
        || (size_t) suggestion_selection >= count)
        suggestion_selection = -1;
    *view = (PspUiTextEntryView) {
        .description = request->description,
        .text = text,
        .suggestions = suggestions,
        .cursor = cursor,
        .suggestion_count = count,
        .suggestion_selection = suggestion_selection,
        .cell = cell,
        .shifted = shifted,
        .numbers = mode == DANZEFF_INPUT_NUMBERS,
        .replace_all = replace_all,
        .allow_submit = request->allow_submit,
        .navigation = request->suggest_navigation
    };
    psp_ui_set_text_entry(ui, view);
    return count;
}

static bool open_danzeff_keyboard(
    PspTextInputService *service, const uint16_t *frame, PspUiState *ui,
    const PspTextInputRequest *request, bool select_initial,
    char *output, size_t capacity, bool *submit_requested)
{
    if (service == NULL || ui == NULL || request == NULL
        || output == NULL || capacity == 0) return false;
    if (submit_requested != NULL) *submit_requested = false;
    snprintf(output, capacity, "%s",
             request->initial == NULL ? "" : request->initial);
    size_t cursor = strlen(output);
    bool replace_all = select_initial && cursor != 0;
    unsigned cell = 4;
    DanzeffInputMode mode = DANZEFF_INPUT_LETTERS;
    bool shifted = false;
    int suggestion_selection = -1;
    BrowserProfileSuggestion suggestions[BROWSER_PROFILE_SUGGESTION_LIMIT]
        = {{0}};
    PspUiTextEntryView view = {0};
    PspUiScreen previous_screen = ui->screen;
    bool previous_chrome = ui->chrome_visible;
    size_t suggestion_count = refresh_danzeff_view(
        service, ui, request, output, cursor, replace_all,
        cell, mode, shifted, suggestions, 0, true,
        suggestion_selection, capacity, &view);
    present(service, frame, ui);

    const unsigned face_buttons = PSP_CTRL_TRIANGLE | PSP_CTRL_SQUARE
        | PSP_CTRL_CROSS | PSP_CTRL_CIRCLE;
    const unsigned input_buttons = face_buttons | PSP_CTRL_LTRIGGER
        | PSP_CTRL_RTRIGGER | PSP_CTRL_UP | PSP_CTRL_DOWN
        | PSP_CTRL_LEFT | PSP_CTRL_RIGHT | PSP_CTRL_START
        | PSP_CTRL_SELECT;
    SceCtrlData pad = {0};
    do {
        if (text_input_cancel_requested(service)) goto cancelled;
        psp_log_heartbeat();
        sceDisplayWaitVblankStart();
        if (sceCtrlPeekBufferPositive(&pad, 1) <= 0)
            memset(&pad, 0, sizeof(pad));
    } while ((pad.Buttons & input_buttons) != 0);

    unsigned previous_buttons = 0;
    unsigned backspace_hold_frames = 0;
    for (;;) {
        if (text_input_cancel_requested(service)) goto cancelled;
        psp_log_heartbeat();
        sceDisplayWaitVblankStart();
        if (sceCtrlPeekBufferPositive(&pad, 1) <= 0) continue;
        unsigned pressed = pad.Buttons & ~previous_buttons;
        previous_buttons = pad.Buttons;
        unsigned next_cell = danzeff_input_cell(pad.Lx, pad.Ly);
        bool next_shifted = (pad.Buttons & PSP_CTRL_RTRIGGER) != 0;
        bool changed = next_cell != cell || next_shifted != shifted;
        bool suggestion_query_changed = false;
        cell = next_cell;
        shifted = next_shifted;

        /* Cross accepts the visibly selected suggestion. START remains the
           explicit submit key when no suggestion is selected. */
        if ((pressed & PSP_CTRL_CROSS) != 0
            && suggestion_selection >= 0
            && (size_t) suggestion_selection < suggestion_count
            && suggestions[suggestion_selection].url != NULL) {
            snprintf(output, capacity, "%s",
                     suggestions[suggestion_selection].url);
            ui->screen = previous_screen;
            ui->chrome_visible = previous_chrome;
            psp_ui_clear_text_entry(ui);
            return true;
        }

        DanzeffInputFinish finish = danzeff_input_finish(
            (pressed & PSP_CTRL_START) != 0,
            (pressed & PSP_CTRL_SELECT) != 0,
            request->allow_submit,
            (pad.Buttons & PSP_CTRL_RTRIGGER) != 0);
        if (finish == DANZEFF_INPUT_CANCEL) goto cancelled;
        if (finish == DANZEFF_INPUT_DONE
            || finish == DANZEFF_INPUT_SUBMIT) {
            if (suggestion_selection >= 0
                && (size_t) suggestion_selection < suggestion_count
                && suggestions[suggestion_selection].url != NULL) {
                snprintf(
                    output, capacity, "%s",
                    suggestions[suggestion_selection].url);
            }
            /* R+START is deliberately distinct from START: accepting text
               must not accidentally submit a form, while the chord gives
               the PSP a real Enter key without consuming any Danzeff face
               button. The request gate keeps settings and multiline-like
               prompts from advertising a semantic they cannot honor. */
            if (submit_requested != NULL)
                *submit_requested = finish == DANZEFF_INPUT_SUBMIT;
            ui->screen = previous_screen;
            ui->chrome_visible = previous_chrome;
            psp_ui_clear_text_entry(ui);
            return true;
        }
        if (pressed & PSP_CTRL_LTRIGGER) {
            mode = mode == DANZEFF_INPUT_LETTERS
                ? DANZEFF_INPUT_NUMBERS : DANZEFF_INPUT_LETTERS;
            changed = true;
        }
        if (pressed & PSP_CTRL_UP) {
            if (suggestion_count != 0) {
                suggestion_selection--;
                if (suggestion_selection < -1)
                    suggestion_selection = (int) suggestion_count - 1;
                changed = true;
            }
        } else if (pressed & PSP_CTRL_DOWN) {
            if (suggestion_count != 0) {
                suggestion_selection++;
                if ((size_t) suggestion_selection >= suggestion_count)
                    suggestion_selection = -1;
                changed = true;
            }
        } else if (pressed & PSP_CTRL_LEFT) {
            bool moved = danzeff_input_move_cursor(
                output, &cursor, -1, &replace_all);
            changed = moved || changed;
            if (moved) suggestion_selection = -1;
        } else if (pressed & PSP_CTRL_RIGHT) {
            bool moved = danzeff_input_move_cursor(
                output, &cursor, 1, &replace_all);
            changed = moved || changed;
            if (moved) suggestion_selection = -1;
        }

        DanzeffInputFace face = DANZEFF_INPUT_TRIANGLE;
        bool chose_character = (pressed & face_buttons) != 0;
        if (pressed & PSP_CTRL_TRIANGLE)
            face = DANZEFF_INPUT_TRIANGLE;
        else if (pressed & PSP_CTRL_SQUARE)
            face = DANZEFF_INPUT_SQUARE;
        else if (pressed & PSP_CTRL_CROSS)
            face = DANZEFF_INPUT_CROSS;
        else if (pressed & PSP_CTRL_CIRCLE)
            face = DANZEFF_INPUT_CIRCLE;
        unsigned held_face = pad.Buttons & face_buttons;
        if (held_face != 0 && (held_face & (held_face - 1u)) == 0) {
            DanzeffInputFace repeat_face = held_face == PSP_CTRL_TRIANGLE
                ? DANZEFF_INPUT_TRIANGLE
                : (held_face == PSP_CTRL_SQUARE ? DANZEFF_INPUT_SQUARE
                    : (held_face == PSP_CTRL_CROSS ? DANZEFF_INPUT_CROSS
                                                   : DANZEFF_INPUT_CIRCLE));
            if (danzeff_input_character(
                    mode, shifted, cell, repeat_face) == '\b') {
                backspace_hold_frames++;
                if (backspace_hold_frames >= 24u
                    && (backspace_hold_frames - 24u) % 4u == 0) {
                    pressed |= held_face;
                    face = repeat_face;
                    chose_character = true;
                }
            } else {
                backspace_hold_frames = 0;
            }
        } else {
            backspace_hold_frames = 0;
        }
        if (chose_character) {
            char character = danzeff_input_character(
                mode, shifted, cell, face);
            bool edited = character == '\b'
                ? danzeff_input_backspace(
                      output, &cursor, &replace_all)
                : danzeff_input_insert(
                      output, capacity, &cursor, character, &replace_all);
            if (edited) {
                suggestion_selection = -1;
                changed = true;
                suggestion_query_changed = true;
            }
        }
        if (!changed) continue;
        suggestion_count = refresh_danzeff_view(
            service, ui, request, output, cursor, replace_all,
            cell, mode, shifted, suggestions, suggestion_count,
            suggestion_query_changed, suggestion_selection, capacity, &view);
        if (suggestion_selection >= 0
            && (size_t) suggestion_selection >= suggestion_count)
            suggestion_selection = -1;
        present(service, frame, ui);
    }

cancelled:
    ui->screen = previous_screen;
    ui->chrome_visible = previous_chrome;
    psp_ui_clear_text_entry(ui);
    output[0] = '\0';
    return false;
}

static bool text_input_cancel_requested(
    const PspTextInputService *service)
{
    return service != NULL && service->cancel_requested != NULL
        && service->cancel_requested(service->cancel_user);
}

static unsigned wait_for_choice(
    const PspTextInputService *service, unsigned accepted)
{
    SceCtrlData pad = {0};
    do {
        if (text_input_cancel_requested(service)) return 0;
        psp_log_heartbeat();
        sceDisplayWaitVblankStart();
        (void) sceCtrlPeekBufferPositive(&pad, 1);
    } while (pad.Buttons & accepted);

    unsigned previous = 0;
    for (;;) {
        if (text_input_cancel_requested(service)) return 0;
        psp_log_heartbeat();
        sceDisplayWaitVblankStart();
        if (sceCtrlPeekBufferPositive(&pad, 1) <= 0) continue;
        unsigned pressed = pad.Buttons & ~previous;
        previous = pad.Buttons;
        if (pressed & accepted) return pressed & accepted;
    }
}

#ifdef TILEFINCH_HAVE_PSP_VOICE
typedef struct {
    PspTextInputService *service;
    const uint16_t *frame;
    PspUiState *ui;
} VoicePresentation;

static void voice_progress(void *user, const char *status)
{
    VoicePresentation *presentation = user;
    if (presentation == NULL) return;
    show_status(
        presentation->service, presentation->frame,
        presentation->ui, status);
}

static TranscriptAction confirm_transcript(
    PspTextInputService *service, const uint16_t *frame,
    PspUiState *ui, const char *transcript)
{
    char status[128];
    snprintf(
        status, sizeof(status), "HEARD %.48s | X USE  [] RETRY  TRI EDIT  O CANCEL",
        transcript == NULL ? "" : transcript);
    show_status(service, frame, ui, status);
    const unsigned accepted = PSP_CTRL_CROSS | PSP_CTRL_SQUARE
        | PSP_CTRL_TRIANGLE | PSP_CTRL_CIRCLE;
    unsigned choice = wait_for_choice(service, accepted);
    if (choice & PSP_CTRL_CROSS) return TRANSCRIPT_USE;
    if (choice & PSP_CTRL_SQUARE) return TRANSCRIPT_RETRY;
    if (choice & PSP_CTRL_TRIANGLE) return TRANSCRIPT_EDIT;
    return TRANSCRIPT_CANCEL;
}
#endif

void psp_text_input_init(
    PspTextInputService *service, Budget *budget,
    const char *voice_model_root,
    PspTextInputPresent present_callback, void *present_user)
{
    if (service == NULL) return;
    memset(service, 0, sizeof(*service));
    service->present = present_callback;
    service->present_user = present_user;
#ifdef TILEFINCH_HAVE_PSP_VOICE
    psp_voice_input_init(&service->voice, budget, voice_model_root);
#else
    (void) budget;
    (void) voice_model_root;
#endif
}

void psp_text_input_set_voice_prepare(
    PspTextInputService *service, PspTextInputVoicePrepare prepare,
    void *prepare_user)
{
    if (service == NULL) return;
    service->voice_prepare = prepare;
    service->voice_prepare_user = prepare_user;
}

bool psp_text_input_set_voice_enabled(
    PspTextInputService *service, bool enabled)
{
#ifdef TILEFINCH_HAVE_PSP_VOICE
    return service != NULL
        && psp_voice_input_set_enabled(&service->voice, enabled);
#else
    (void) service;
    return !enabled;
#endif
}

bool psp_text_input_set_voice_model_root(
    PspTextInputService *service, const char *model_root)
{
#ifdef TILEFINCH_HAVE_PSP_VOICE
    if (service == NULL || model_root == NULL || model_root[0] == '\0'
        || strlen(model_root) >= sizeof(service->voice.model_root))
        return false;
    (void) psp_voice_input_set_enabled(&service->voice, false);
    snprintf(service->voice.model_root,
             sizeof(service->voice.model_root), "%s", model_root);
    return true;
#else
    (void) service;
    (void) model_root;
    return false;
#endif
}

void psp_text_input_set_adaptive_voice_memory(
    PspTextInputService *service, bool enabled)
{
#ifdef TILEFINCH_HAVE_PSP_VOICE
    if (service != NULL)
        psp_voice_input_set_adaptive_memory(&service->voice, enabled);
#else
    (void) service;
    (void) enabled;
#endif
}

void psp_text_input_set_cancel_requested(
    PspTextInputService *service,
    PspTextInputCancelRequested cancel_requested, void *cancel_user)
{
    if (service == NULL) return;
    service->cancel_requested = cancel_requested;
    service->cancel_user = cancel_user;
#ifdef TILEFINCH_HAVE_PSP_VOICE
    psp_voice_input_set_cancel_requested(
        &service->voice, cancel_requested, cancel_user);
#endif
}

void psp_text_input_set_profile(
    PspTextInputService *service, const BrowserProfile *profile)
{
    if (service != NULL) service->profile = profile;
}

void psp_text_input_set_danzeff_enabled(
    PspTextInputService *service, bool enabled)
{
    if (service != NULL) service->danzeff_enabled = enabled;
}

static bool open_selected_keyboard(
    PspTextInputService *service, const uint16_t *frame, PspUiState *ui,
    const PspTextInputRequest *request, bool select_initial,
    char *output, size_t capacity, bool *submit_requested)
{
    if (submit_requested != NULL) *submit_requested = false;
    if (service->danzeff_enabled) {
        return open_danzeff_keyboard(
            service, frame, ui, request, select_initial,
            output, capacity, submit_requested);
    }
    return open_keyboard(
        service, request->description, request->initial,
        request->keyboard_url_mode, output, capacity);
}

static bool text_input_collect(
    PspTextInputService *service, const uint16_t *frame, PspUiState *ui,
    const PspTextInputRequest *request, char *output, size_t capacity,
    bool *submit_requested)
{
    /* Ordinary activation always enters the platform OSK immediately. Voice
       has a separate, explicitly requested experimental entry point below. */
    (void) service;
    (void) frame;
    (void) ui;
    return open_selected_keyboard(
        service, frame, ui, request, request->suggest_navigation,
        output, capacity, submit_requested);
}

static bool text_input_collect_voice(
    PspTextInputService *service, const uint16_t *frame, PspUiState *ui,
    const PspTextInputRequest *request, char *output, size_t capacity)
{
#ifdef TILEFINCH_HAVE_PSP_VOICE
    if (service->voice_prepare != NULL)
        (void) service->voice_prepare(service->voice_prepare_user);
    for (;;) {
        char transcript[PSP_TEXT_INPUT_CAPACITY + 1] = {0};
        VoicePresentation presentation = {
            .service = service,
            .frame = frame,
            .ui = ui
        };
        if (!psp_voice_input_transcribe(
                &service->voice, voice_progress, &presentation,
                transcript, sizeof(transcript))) return false;
        TranscriptAction action = confirm_transcript(
            service, frame, ui, transcript);
        if (action == TRANSCRIPT_CANCEL) return false;
        if (action == TRANSCRIPT_RETRY) continue;
        if (action == TRANSCRIPT_EDIT) {
            PspTextInputRequest edited = *request;
            edited.initial = transcript;
            return open_selected_keyboard(
                service, frame, ui, &edited, false,
                output, capacity, NULL);
        }
        snprintf(output, capacity, "%s", transcript);
        return true;
    }
#else
    (void) service;
    (void) frame;
    (void) ui;
    (void) request;
    (void) output;
    (void) capacity;
    return false;
#endif
}

bool psp_text_input_request(
    PspTextInputService *service, const uint16_t *frame, PspUiState *ui,
    const PspTextInputRequest *request, char *output, size_t capacity)
{
    return psp_text_input_request_with_submit(
        service, frame, ui, request, output, capacity, NULL);
}

bool psp_text_input_request_with_submit(
    PspTextInputService *service, const uint16_t *frame, PspUiState *ui,
    const PspTextInputRequest *request, char *output, size_t capacity,
    bool *submit_requested)
{
    if (service == NULL || request == NULL
        || output == NULL || capacity == 0) return false;
    psp_log_set_phase(PSP_LOG_PHASE_INPUT);
    psp_log_heartbeat();
    output[0] = '\0';
    if (submit_requested != NULL) *submit_requested = false;

    bool accepted = text_input_collect(
        service, frame, ui, request, output, capacity,
        submit_requested);

    /*
     * Hand the screen back before returning.
     *
     * The system keyboard takes over the GE for as long as it runs: it draws
     * through both of the browser's scanout buffers and, on the way out,
     * sceGuDisplay(GU_FALSE) leaves the display switched off. The browser
     * repaints only when something marks the page dirty, and cancelling the
     * keyboard marks nothing — so without this the user is left looking at a
     * black screen until they happen to press something that forces a
     * repaint. Restore on every path, accepted or cancelled.
     */
    present(service, frame, ui);
    return accepted;
}

bool psp_text_input_request_voice(
    PspTextInputService *service, const uint16_t *frame, PspUiState *ui,
    const PspTextInputRequest *request, char *output, size_t capacity)
{
    if (service == NULL || request == NULL
        || output == NULL || capacity == 0) return false;
    psp_log_set_phase(PSP_LOG_PHASE_INPUT);
    psp_log_heartbeat();
    output[0] = '\0';
    bool accepted = text_input_collect_voice(
        service, frame, ui, request, output, capacity);
    present(service, frame, ui);
    return accepted;
}

void psp_text_input_before_navigation(PspTextInputService *service)
{
#ifdef TILEFINCH_HAVE_PSP_VOICE
    if (service != NULL) psp_voice_input_evict(&service->voice);
#else
    (void) service;
#endif
}

void psp_text_input_trim(PspTextInputService *service)
{
#ifdef TILEFINCH_HAVE_PSP_VOICE
    if (service != NULL) psp_voice_input_trim(&service->voice);
#else
    (void) service;
#endif
}

void psp_text_input_shutdown(PspTextInputService *service)
{
#ifdef TILEFINCH_HAVE_PSP_VOICE
    if (service != NULL) {
        psp_voice_input_report(&service->voice);
        psp_voice_input_shutdown(&service->voice);
    }
#endif
}
