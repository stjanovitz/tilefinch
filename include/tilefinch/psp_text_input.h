#ifndef TILEFINCH_PSP_TEXT_INPUT_H
#define TILEFINCH_PSP_TEXT_INPUT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "tilefinch/budget.h"
#include "tilefinch/psp_ui.h"
#ifdef TILEFINCH_HAVE_PSP_VOICE
#include "tilefinch/psp_voice_input.h"
#endif

#define PSP_TEXT_INPUT_CAPACITY PSP_UI_TEXT_ENTRY_CAPACITY

typedef void (*PspTextInputPresent)(
    void *user, const uint16_t *frame, const PspUiState *ui);
typedef size_t (*PspTextInputVoicePrepare)(void *user);
typedef bool (*PspTextInputCancelRequested)(void *user);
/* Bracket one text entry, which runs its own pad and present loop: begin
   before the first sample, end after the screen is handed back, with what
   begin returned. The app fences the loading-UI supervisor out between
   them, so the keyboard alone owns the screen and pad. */
typedef bool (*PspTextInputModalBegin)(void *user);
typedef void (*PspTextInputModalEnd)(void *user, bool begun);

typedef struct {
    const char *description;
    const char *initial;
    bool keyboard_url_mode;
    bool suggest_navigation;
    /* A single-line page field may distinguish accepting an edit from
       activating the HTML Enter/default-submit action. Native settings,
       find, and address prompts leave this clear. */
    bool allow_submit;
} PspTextInputRequest;

typedef struct {
    PspTextInputPresent present;
    void *present_user;
    PspTextInputVoicePrepare voice_prepare;
    void *voice_prepare_user;
    PspTextInputCancelRequested cancel_requested;
    void *cancel_user;
    PspTextInputModalBegin modal_begin;
    PspTextInputModalEnd modal_end;
    void *modal_user;
    const BrowserProfile *profile;
    bool danzeff_enabled;
#ifdef TILEFINCH_PSP_VALIDATION_LOG
    /* Replaces samples only during a scripted modal-keyboard qualification. */
    bool (*validation_poll)(PspUiInput *input);
    bool (*validation_finished)(void);
#endif
#ifdef TILEFINCH_HAVE_PSP_VOICE
    PspVoiceInput voice;
#endif
} PspTextInputService;

void psp_text_input_init(
    PspTextInputService *service, Budget *budget,
    const char *voice_model_root,
    PspTextInputPresent present, void *present_user);
void psp_text_input_set_voice_prepare(
    PspTextInputService *service, PspTextInputVoicePrepare prepare,
    void *prepare_user);
bool psp_text_input_set_voice_enabled(
    PspTextInputService *service, bool enabled);
bool psp_text_input_set_voice_model_root(
    PspTextInputService *service, const char *model_root);
void psp_text_input_set_adaptive_voice_memory(
    PspTextInputService *service, bool enabled);
void psp_text_input_set_cancel_requested(
    PspTextInputService *service,
    PspTextInputCancelRequested cancel_requested, void *cancel_user);
void psp_text_input_set_modal(
    PspTextInputService *service, PspTextInputModalBegin begin,
    PspTextInputModalEnd end, void *user);
void psp_text_input_set_profile(
    PspTextInputService *service, const BrowserProfile *profile);
void psp_text_input_set_danzeff_enabled(
    PspTextInputService *service, bool enabled);

bool psp_text_input_request(
    PspTextInputService *service, const uint16_t *frame, PspUiState *ui,
    const PspTextInputRequest *request, char *output, size_t capacity);
bool psp_text_input_request_with_submit(
    PspTextInputService *service, const uint16_t *frame, PspUiState *ui,
    const PspTextInputRequest *request, char *output, size_t capacity,
    bool *submit_requested);
bool psp_text_input_request_voice(
    PspTextInputService *service, const uint16_t *frame, PspUiState *ui,
    const PspTextInputRequest *request, char *output, size_t capacity);

void psp_text_input_before_navigation(PspTextInputService *service);
void psp_text_input_trim(PspTextInputService *service);
void psp_text_input_shutdown(PspTextInputService *service);

#endif
