#include "tilefinch/psp_ui.h"

/* The decoder picker offers this table and nothing else. */
#include "../src/media_backend_psp_policy.h"

#include <stdio.h>

#define CHECK(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "FAIL %s:%d: %s\n", \
                __FILE__, __LINE__, #condition); \
        return 1; \
    } \
} while (0)

int main(void)
{
    PspUiState ui;
    psp_ui_init(&ui);
    ui.screen = PSP_UI_SCREEN_OPTION_ITEMS;
    ui.options_selection = 28;
    PspUiInput input = {
        .pressed = PSP_UI_BUTTON_CONFIRM,
        .analog_x = 128,
        .analog_y = 128
    };
    PspUiIntent intent = psp_ui_update(&ui, &input);
    CHECK(intent.setting.id == PSP_UI_SETTING_TLS_SESSION_PERSISTENCE
          && intent.setting.value.boolean
          && ui.tls_session_persistence);

    psp_ui_init(&ui);
    ui.screen = PSP_UI_SCREEN_OPTION_ITEMS;
    ui.options_selection = 31;
    intent = psp_ui_update(&ui, &input);
    CHECK(intent.setting.id == PSP_UI_SETTING_NETWORK_PROFILE
          && intent.setting.value.unsigned_value == 2u);

    psp_ui_init(&ui);
    ui.screen = PSP_UI_SCREEN_OPTION_ITEMS;
    ui.options_selection = 32;
    intent = psp_ui_update(&ui, &input);
    CHECK(intent.action == PSP_UI_ACTION_POWER_TEST
          && ui.screen == PSP_UI_SCREEN_PAGE);

    psp_ui_init(&ui);
    ui.screen = PSP_UI_SCREEN_OPTION_ITEMS;
    ui.options_selection = 33;
    intent = psp_ui_update(&ui, &input);
    CHECK(intent.action == PSP_UI_ACTION_MEDIA_TEST
          && ui.screen == PSP_UI_SCREEN_PAGE);

    psp_ui_init(&ui);
    ui.screen = PSP_UI_SCREEN_OPTION_ITEMS;
    ui.options_selection = 34;
    intent = psp_ui_update(&ui, &input);
    CHECK(!ui.update_check_enabled
          && intent.setting.id == PSP_UI_SETTING_UPDATE_CHECK
          && !intent.setting.value.boolean
          && intent.action == PSP_UI_ACTION_NONE);

    psp_ui_init(&ui);
    ui.screen = PSP_UI_SCREEN_OPTION_ITEMS;
    ui.options_selection = 35;
    intent = psp_ui_update(&ui, &input);
    CHECK(ui.screen == PSP_UI_SCREEN_UPDATE
          && intent.action == PSP_UI_ACTION_NONE);

    psp_ui_init(&ui);
    ui.screen = PSP_UI_SCREEN_OPTION_ITEMS;
    ui.options_selection = 36;
    intent = psp_ui_update(&ui, &input);
    CHECK(ui.screen == PSP_UI_SCREEN_DATA_OPTIONS
          && intent.action == PSP_UI_ACTION_NONE);
    /*
     * The decoder-program picker, the seventh Experimental row. Browsing it
     * must not emit anything: each press would otherwise be a Memory Stick
     * write, and walking the six spellings would be six of them.
     */
    psp_ui_init(&ui);
    ui.screen = PSP_UI_SCREEN_EXPERIMENTAL_OPTIONS;
    ui.experimental_options_selection = 6;
    ui.experimental_decoder_choice = 0;
    PspUiInput right = {
        .pressed = PSP_UI_BUTTON_RIGHT, .analog_x = 128, .analog_y = 128
    };
    PspUiInput left = {
        .pressed = PSP_UI_BUTTON_LEFT, .analog_x = 128, .analog_y = 128
    };
    PspUiInput cancel = {
        .pressed = PSP_UI_BUTTON_CANCEL, .analog_x = 128, .analog_y = 128
    };
    intent = psp_ui_update(&ui, &right);
    CHECK(ui.experimental_decoder_choice == 1u
          && intent.action == PSP_UI_ACTION_NONE
          && intent.setting.id == PSP_UI_SETTING_NONE
          && ui.screen == PSP_UI_SCREEN_EXPERIMENTAL_OPTIONS);
    intent = psp_ui_update(&ui, &left);
    CHECK(ui.experimental_decoder_choice == 0u
          && intent.action == PSP_UI_ACTION_NONE);
    /* Both directions wrap inside the table, so the picker can never show a
       spelling the boot config gate would reject. */
    intent = psp_ui_update(&ui, &left);
    CHECK(ui.experimental_decoder_choice
              == PSP_MEDIA_WIDE_PROGRAM_CHOICE_COUNT - 1u);
    intent = psp_ui_update(&ui, &right);
    CHECK(ui.experimental_decoder_choice == 0u);
    for (unsigned step = 0;
         step < PSP_MEDIA_WIDE_PROGRAM_CHOICE_COUNT; step++) {
        CHECK(psp_media_wide_program_name_valid(
            psp_media_wide_program_choice(ui.experimental_decoder_choice)));
        (void) psp_ui_update(&ui, &right);
    }
    CHECK(ui.experimental_decoder_choice == 0u);

    /* Confirm commits the shown spelling, and does not move it. */
    ui.experimental_decoder_choice = 4;
    intent = psp_ui_update(&ui, &input);
    CHECK(intent.action == PSP_UI_ACTION_SET_VIDEO_DECODER
          && ui.experimental_decoder_choice == 4u
          && ui.screen == PSP_UI_SCREEN_EXPERIMENTAL_OPTIONS);

    /*
     * The restart prompt the receiver raises after a successful write. It
     * owns the whole screen: confirm re-emits the same action, which the
     * receiver reads as "restart now" because it set the bit, and cancel
     * declines without leaving the panel or disturbing the saved value.
     */
    ui.experimental_decoder_restart_prompt = 1;
    ui.experimental_options_selection = 0;
    intent = psp_ui_update(&ui, &input);
    CHECK(intent.action == PSP_UI_ACTION_SET_VIDEO_DECODER
          && ui.experimental_voice_input == 0
          && intent.setting.id == PSP_UI_SETTING_NONE);
    intent = psp_ui_update(&ui, &cancel);
    CHECK(!ui.experimental_decoder_restart_prompt
          && ui.screen == PSP_UI_SCREEN_EXPERIMENTAL_OPTIONS
          && intent.action == PSP_UI_ACTION_NONE);
    /* And with the prompt gone the panel behaves normally again. */
    psp_ui_set_voice_component(
        &ui, PSP_UI_VOICE_COMPONENT_READY, -1);
    intent = psp_ui_update(&ui, &input);
    CHECK(ui.experimental_voice_input
          && intent.setting.id == PSP_UI_SETTING_EXPERIMENTAL_VOICE);

    /* The Developer URL row keeps its own action. */
    psp_ui_init(&ui);
    ui.screen = PSP_UI_SCREEN_EXPERIMENTAL_OPTIONS;
    ui.experimental_options_selection = 5;
    intent = psp_ui_update(&ui, &input);
    CHECK(intent.action == PSP_UI_ACTION_EDIT_DEVELOPER_URL);

    CHECK(psp_ui_state_bytes() <= 1024);
    puts("psp-power-menu-tests: ok");
    return 0;
}
