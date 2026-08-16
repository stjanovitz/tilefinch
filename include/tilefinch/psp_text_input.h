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
    const BrowserProfile *profile;
    bool danzeff_enabled;
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
