#include "tilefinch/psp_ui.h"
#include "tilefinch/psp_power_policy.h"
#include "../src/psp_ui_theme.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define CHECK(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition); \
        return false; \
    } \
} while (0)

static bool test_input_mapping_and_menu(void)
{
    PspUiState ui;
    psp_ui_init(&ui);
    ui.javascript_enabled = true;
    ui.site_javascript_enabled = true;
    ui.site_data_allowed = true;
    CHECK(!ui.experimental_voice_input
          && ui.screen == PSP_UI_SCREEN_PAGE
          && ui.content_blocker_mode == CONTENT_BLOCKER_BASIC
          && ui.content_blocker_cosmetic_hiding
          && ui.cookie_banner_hidden);
    PspUiInput input = {
        .pressed = PSP_UI_BUTTON_DOWN,
        .analog_x = 128,
        .analog_y = 128
    };
    PspUiIntent intent = psp_ui_update(&ui, &input);
    CHECK(intent.action == PSP_UI_ACTION_FOCUS_DOWN);
    CHECK(!psp_ui_intent_has_predispatch_visual(&intent));
    input.pressed = PSP_UI_BUTTON_UP;
    intent = psp_ui_update(&ui, &input);
    CHECK(intent.action == PSP_UI_ACTION_FOCUS_UP);
    input.pressed = PSP_UI_BUTTON_LEFT;
    intent = psp_ui_update(&ui, &input);
    CHECK(intent.action == PSP_UI_ACTION_FOCUS_LEFT);
    input.pressed = PSP_UI_BUTTON_RIGHT;
    intent = psp_ui_update(&ui, &input);
    CHECK(intent.action == PSP_UI_ACTION_FOCUS_RIGHT);

    input.pressed = PSP_UI_BUTTON_MENU;
    intent = psp_ui_update(&ui, &input);
    CHECK(ui.screen == PSP_UI_SCREEN_MENU && ui.chrome_visible);
    CHECK(psp_ui_intent_has_predispatch_visual(&intent));
    input.pressed = PSP_UI_BUTTON_DOWN;
    intent = psp_ui_update(&ui, &input);
    CHECK(intent.action == PSP_UI_ACTION_NONE && ui.menu_selection == 1);
    CHECK(psp_ui_intent_predispatch_is_complete(&intent));
    input.pressed = PSP_UI_BUTTON_CONFIRM;
    intent = psp_ui_update(&ui, &input);
    CHECK(intent.action == PSP_UI_ACTION_HOME
          && ui.screen == PSP_UI_SCREEN_PAGE);

    PspUiTabsView tabs = {
        .count = 2,
        .active_index = 0,
        .can_create = true
    };
    snprintf(tabs.titles[0], sizeof(tabs.titles[0]), "Wikipedia");
    snprintf(tabs.titles[1], sizeof(tabs.titles[1]), "YouTube");
    psp_ui_set_tabs(&ui, &tabs);
    input.pressed = PSP_UI_BUTTON_MENU;
    intent = psp_ui_update(&ui, &input);
    ui.menu_selection = 6;
    input.pressed = PSP_UI_BUTTON_CONFIRM;
    intent = psp_ui_update(&ui, &input);
    CHECK(intent.action == PSP_UI_ACTION_TOGGLE_READER
          && ui.screen == PSP_UI_SCREEN_PAGE);

    input.pressed = PSP_UI_BUTTON_MENU;
    (void) psp_ui_update(&ui, &input);
    ui.menu_selection = 6;
    input.pressed = PSP_UI_BUTTON_RELOAD;
    intent = psp_ui_update(&ui, &input);
    CHECK(intent.action == PSP_UI_ACTION_TOGGLE_READER_SITE
          && ui.screen == PSP_UI_SCREEN_PAGE);

    input.pressed = PSP_UI_BUTTON_MENU;
    (void) psp_ui_update(&ui, &input);
    ui.menu_selection = 2;
    input.pressed = PSP_UI_BUTTON_CONFIRM;
    intent = psp_ui_update(&ui, &input);
    CHECK(ui.screen == PSP_UI_SCREEN_TABS
          && ui.tab_selection == 0);
    input.pressed = PSP_UI_BUTTON_DOWN;
    intent = psp_ui_update(&ui, &input);
    CHECK(ui.tab_selection == 1
          && psp_ui_intent_predispatch_is_complete(&intent));
    input.pressed = PSP_UI_BUTTON_CONFIRM;
    intent = psp_ui_update(&ui, &input);
    CHECK(intent.action == PSP_UI_ACTION_SWITCH_TAB
          && intent.tab_index == 1
          && ui.screen == PSP_UI_SCREEN_PAGE);
    input.pressed = PSP_UI_BUTTON_MENU;
    (void) psp_ui_update(&ui, &input);
    ui.menu_selection = 2;
    input.pressed = PSP_UI_BUTTON_CONFIRM;
    (void) psp_ui_update(&ui, &input);
    ui.tab_selection = 2;
    input.pressed = PSP_UI_BUTTON_CONFIRM;
    intent = psp_ui_update(&ui, &input);
    CHECK(intent.action == PSP_UI_ACTION_NEW_TAB);
    input.pressed = PSP_UI_BUTTON_MENU;
    (void) psp_ui_update(&ui, &input);
    ui.menu_selection = 2;
    input.pressed = PSP_UI_BUTTON_CONFIRM;
    (void) psp_ui_update(&ui, &input);
    ui.tab_selection = 1;
    input.pressed = PSP_UI_BUTTON_RELOAD;
    intent = psp_ui_update(&ui, &input);
    CHECK(intent.action == PSP_UI_ACTION_CLOSE_TAB
          && intent.tab_index == 1);

    /* The library verb menu is retired: its three list entries are top-level
       menu rows that open the one native surface on their section. */
    input.pressed = PSP_UI_BUTTON_MENU;
    intent = psp_ui_update(&ui, &input);
    ui.menu_selection = 3;
    input.pressed = PSP_UI_BUTTON_CONFIRM;
    intent = psp_ui_update(&ui, &input);
    CHECK(intent.action == PSP_UI_ACTION_SHOW_OFFLINE
          && ui.screen == PSP_UI_SCREEN_PAGE);

    input.pressed = PSP_UI_BUTTON_MENU;
    (void) psp_ui_update(&ui, &input);
    ui.menu_selection = 4;
    input.pressed = PSP_UI_BUTTON_CONFIRM;
    intent = psp_ui_update(&ui, &input);
    CHECK(intent.action == PSP_UI_ACTION_SHOW_BOOKMARKS);

    input.pressed = PSP_UI_BUTTON_MENU;
    (void) psp_ui_update(&ui, &input);
    ui.menu_selection = 5;
    input.pressed = PSP_UI_BUTTON_CONFIRM;
    intent = psp_ui_update(&ui, &input);
    CHECK(intent.action == PSP_UI_ACTION_SHOW_HISTORY);

    input.pressed = PSP_UI_BUTTON_MENU;
    (void) psp_ui_update(&ui, &input);
    ui.menu_selection = 8;
    input.pressed = PSP_UI_BUTTON_CONFIRM;
    intent = psp_ui_update(&ui, &input);
    CHECK(intent.action == PSP_UI_ACTION_SAVE_FOR_LATER);

    input.pressed = PSP_UI_BUTTON_MENU;
    (void) psp_ui_update(&ui, &input);
    ui.menu_selection = 9;
    input.pressed = PSP_UI_BUTTON_CONFIRM;
    intent = psp_ui_update(&ui, &input);
    CHECK(intent.action == PSP_UI_ACTION_TOGGLE_BOOKMARK);

    input.pressed = PSP_UI_BUTTON_MENU;
    (void) psp_ui_update(&ui, &input);
    ui.menu_selection = 10;
    input.pressed = PSP_UI_BUTTON_CONFIRM;
    intent = psp_ui_update(&ui, &input);
    CHECK(intent.action == PSP_UI_ACTION_SHOW_SCREENSHOTS);

    input.pressed = PSP_UI_BUTTON_MENU;
    (void) psp_ui_update(&ui, &input);
    ui.menu_selection = 7;
    input.pressed = PSP_UI_BUTTON_CONFIRM;
    intent = psp_ui_update(&ui, &input);
    CHECK(intent.action == PSP_UI_ACTION_OPEN_FIND
          && ui.screen == PSP_UI_SCREEN_PAGE);
    PspUiFindView find_view = {
        .query = "portable",
        .match_count = 12,
        .selected = 3,
        .truncated = false
    };
    psp_ui_set_find(&ui, &find_view);
    CHECK(ui.screen == PSP_UI_SCREEN_FIND && ui.find_view == &find_view);
    input.pressed = PSP_UI_BUTTON_DOWN;
    input.held = PSP_UI_BUTTON_DOWN;
    intent = psp_ui_update(&ui, &input);
    CHECK(intent.action == PSP_UI_ACTION_FIND_NEXT
          && !psp_ui_intent_has_predispatch_visual(&intent));
    input.pressed = 0;
    input.elapsed_ms = 64;
    for (size_t repeat_frame = 0; repeat_frame < 5; repeat_frame++) {
        intent = psp_ui_update(&ui, &input);
        CHECK(intent.action == PSP_UI_ACTION_NONE);
    }
    input.elapsed_ms = 40;
    intent = psp_ui_update(&ui, &input);
    CHECK(intent.action == PSP_UI_ACTION_FIND_NEXT);
    input.held = 0;
    input.elapsed_ms = 16;
    input.pressed = PSP_UI_BUTTON_UP;
    intent = psp_ui_update(&ui, &input);
    CHECK(intent.action == PSP_UI_ACTION_FIND_PREVIOUS);
    input.pressed = PSP_UI_BUTTON_CONFIRM;
    intent = psp_ui_update(&ui, &input);
    CHECK(intent.action == PSP_UI_ACTION_FIND_EDIT
          && ui.screen == PSP_UI_SCREEN_PAGE && ui.find_view == NULL);
    psp_ui_set_find(&ui, &find_view);
    input.pressed = PSP_UI_BUTTON_CANCEL;
    intent = psp_ui_update(&ui, &input);
    CHECK(intent.action == PSP_UI_ACTION_FIND_CLOSE
          && ui.screen == PSP_UI_SCREEN_PAGE && ui.find_view == NULL
          && !psp_ui_intent_has_predispatch_visual(&intent));

    input.pressed = PSP_UI_BUTTON_MENU;
    (void) psp_ui_update(&ui, &input);
    ui.menu_selection = 12;
    input.pressed = PSP_UI_BUTTON_CONFIRM;
    intent = psp_ui_update(&ui, &input);
    CHECK(intent.action == PSP_UI_ACTION_SCREENSHOT
          && ui.screen == PSP_UI_SCREEN_PAGE
          && psp_ui_intent_has_predispatch_visual(&intent));

    input.pressed = PSP_UI_BUTTON_MENU;
    (void) psp_ui_update(&ui, &input);
    ui.menu_selection = 11;
    input.pressed = PSP_UI_BUTTON_CONFIRM;
    intent = psp_ui_update(&ui, &input);
    CHECK(ui.screen == PSP_UI_SCREEN_OPTIONS);
    input.pressed = PSP_UI_BUTTON_CONFIRM;
    intent = psp_ui_update(&ui, &input);
    CHECK(ui.screen == PSP_UI_SCREEN_OPTION_ITEMS
          && ui.options_selection == 0);
    input.pressed = PSP_UI_BUTTON_RIGHT;
    intent = psp_ui_update(&ui, &input);
    CHECK(ui.browser_ui_scale == 2 && intent.visual_changed
          && intent.setting.id == PSP_UI_SETTING_BROWSER_UI_SCALE
          && intent.setting.value.unsigned_value == 2);
    CHECK(!psp_ui_intent_predispatch_is_complete(&intent));
    input.pressed = PSP_UI_BUTTON_DOWN;
    intent = psp_ui_update(&ui, &input);
    CHECK(ui.options_selection == 1);
    input.pressed = PSP_UI_BUTTON_RIGHT;
    intent = psp_ui_update(&ui, &input);
    CHECK(intent.setting.id == PSP_UI_SETTING_PAGE_FONT_PERCENT
          && intent.setting.value.unsigned_value == 125
          && ui.page_font_percent == 125);
    input.pressed = PSP_UI_BUTTON_DOWN;
    intent = psp_ui_update(&ui, &input);
    CHECK(ui.options_selection == 2);
    input.pressed = PSP_UI_BUTTON_RIGHT;
    intent = psp_ui_update(&ui, &input);
    CHECK(ui.reader_font_serif
          && intent.setting.id == PSP_UI_SETTING_READER_FONT
          && intent.setting.value.reader_font == BROWSER_READER_FONT_SERIF);
    input.pressed = PSP_UI_BUTTON_DOWN;
    intent = psp_ui_update(&ui, &input);
    CHECK(ui.options_selection == 3);
    input.pressed = PSP_UI_BUTTON_RIGHT;
    intent = psp_ui_update(&ui, &input);
    CHECK(ui.remember_reader_site_scale
          && intent.setting.id
                 == PSP_UI_SETTING_REMEMBER_READER_SITE_SCALE
          && intent.setting.value.boolean);
    input.pressed = PSP_UI_BUTTON_CANCEL;
    (void) psp_ui_update(&ui, &input);
    CHECK(ui.screen == PSP_UI_SCREEN_OPTIONS
          && ui.options_group_selection == 0);
    input.pressed = PSP_UI_BUTTON_DOWN;
    (void) psp_ui_update(&ui, &input);
    input.pressed = PSP_UI_BUTTON_CONFIRM;
    intent = psp_ui_update(&ui, &input);
    CHECK(ui.screen == PSP_UI_SCREEN_OPTION_ITEMS
          && ui.options_selection == 9);
    input.pressed = PSP_UI_BUTTON_RIGHT;
    intent = psp_ui_update(&ui, &input);
    CHECK(ui.danzeff_text_input
          && intent.setting.id == PSP_UI_SETTING_TEXT_ENTRY_MODE
          && intent.setting.value.text_entry_mode
                 == BROWSER_TEXT_ENTRY_DANZEFF);
    input.pressed = PSP_UI_BUTTON_RIGHT;
    intent = psp_ui_update(&ui, &input);
    CHECK(!ui.danzeff_text_input
          && intent.setting.id == PSP_UI_SETTING_TEXT_ENTRY_MODE
          && intent.setting.value.text_entry_mode
                 == BROWSER_TEXT_ENTRY_OSK);
    ui.options_selection = 4;
    input.pressed = PSP_UI_BUTTON_RIGHT;
    intent = psp_ui_update(&ui, &input);
    CHECK(ui.custom_homepage_enabled
          && intent.setting.id == PSP_UI_SETTING_CUSTOM_HOMEPAGE
          && intent.setting.value.boolean);
    input.pressed = PSP_UI_BUTTON_DOWN;
    intent = psp_ui_update(&ui, &input);
    CHECK(ui.options_selection == 5);
    input.pressed = PSP_UI_BUTTON_RIGHT;
    intent = psp_ui_update(&ui, &input);
    CHECK(ui.history_enabled
          && intent.setting.id == PSP_UI_SETTING_HISTORY
          && intent.setting.value.boolean);
    input.pressed = PSP_UI_BUTTON_DOWN;
    intent = psp_ui_update(&ui, &input);
    CHECK(ui.options_selection == 6);
    input.pressed = PSP_UI_BUTTON_RIGHT;
    intent = psp_ui_update(&ui, &input);
    CHECK(ui.restore_last_page
          && intent.setting.id == PSP_UI_SETTING_RESTORE_LAST_PAGE
          && intent.setting.value.boolean);
    input.pressed = PSP_UI_BUTTON_DOWN;
    intent = psp_ui_update(&ui, &input);
    CHECK(ui.options_selection == 7);
    input.pressed = PSP_UI_BUTTON_CONFIRM;
    intent = psp_ui_update(&ui, &input);
    CHECK(ui.tab_hibernation_enabled
          && intent.setting.id == PSP_UI_SETTING_TAB_HIBERNATION
          && intent.setting.value.boolean);
    input.pressed = PSP_UI_BUTTON_CANCEL;
    intent = psp_ui_update(&ui, &input);
    CHECK(ui.screen == PSP_UI_SCREEN_OPTIONS
          && ui.options_group_selection == 1);
    input.pressed = PSP_UI_BUTTON_DOWN;
    (void) psp_ui_update(&ui, &input);
    input.pressed = PSP_UI_BUTTON_DOWN;
    (void) psp_ui_update(&ui, &input);
    input.pressed = PSP_UI_BUTTON_DOWN;
    (void) psp_ui_update(&ui, &input);
    CHECK(ui.options_group_selection == 4);
    input.pressed = PSP_UI_BUTTON_CONFIRM;
    intent = psp_ui_update(&ui, &input);
    CHECK(ui.screen == PSP_UI_SCREEN_EXPERIMENTAL_OPTIONS
          && ui.experimental_options_selection == 0
          && ui.overlay_motion == 1u
          && intent.voice_component_probe_requested);
    psp_ui_set_voice_component(
        &ui, PSP_UI_VOICE_COMPONENT_NOT_INSTALLED, -1);
    input.pressed = PSP_UI_BUTTON_CONFIRM;
    intent = psp_ui_update(&ui, &input);
    CHECK(!intent.voice_component_primary_requested
          && !ui.experimental_voice_input
          && ui.experimental_options_selection == 1);
    input.pressed = PSP_UI_BUTTON_CONFIRM;
    intent = psp_ui_update(&ui, &input);
    CHECK(intent.voice_component_primary_requested);
    ui.experimental_options_selection = 0;
    psp_ui_set_voice_component(
        &ui, PSP_UI_VOICE_COMPONENT_READY, -1);
    input.pressed = PSP_UI_BUTTON_CONFIRM;
    intent = psp_ui_update(&ui, &input);
    CHECK(ui.experimental_voice_input
          && intent.setting.id == PSP_UI_SETTING_EXPERIMENTAL_VOICE
          && intent.setting.value.boolean);
    input.pressed = PSP_UI_BUTTON_DOWN;
    intent = psp_ui_update(&ui, &input);
    CHECK(ui.experimental_options_selection == 1);
    psp_ui_set_voice_component(
        &ui, PSP_UI_VOICE_COMPONENT_DOWNLOADING, 500);
    input.pressed = PSP_UI_BUTTON_CONFIRM;
    intent = psp_ui_update(&ui, &input);
    CHECK(intent.voice_component_cancel_requested);
    psp_ui_set_voice_component(
        &ui, PSP_UI_VOICE_COMPONENT_READY, -1);
    input.pressed = PSP_UI_BUTTON_CONFIRM;
    intent = psp_ui_update(&ui, &input);
    CHECK(ui.voice_component_remove_confirmation
          && !intent.voice_component_remove_requested);
    input.pressed = PSP_UI_BUTTON_CONFIRM;
    intent = psp_ui_update(&ui, &input);
    CHECK(!ui.voice_component_remove_confirmation
          && intent.voice_component_remove_requested);
    input.pressed = PSP_UI_BUTTON_DOWN;
    intent = psp_ui_update(&ui, &input);
    CHECK(ui.experimental_options_selection == 2);
    input.pressed = PSP_UI_BUTTON_DOWN;
    intent = psp_ui_update(&ui, &input);
    CHECK(ui.experimental_options_selection == 3);
    input.pressed = PSP_UI_BUTTON_RIGHT;
    intent = psp_ui_update(&ui, &input);
    CHECK(ui.adaptive_voice_memory
          && intent.setting.id == PSP_UI_SETTING_ADAPTIVE_VOICE_MEMORY
          && intent.setting.value.boolean);
    input.pressed = PSP_UI_BUTTON_DOWN;
    intent = psp_ui_update(&ui, &input);
    CHECK(ui.experimental_options_selection == 4);
    input.pressed = PSP_UI_BUTTON_RIGHT;
    intent = psp_ui_update(&ui, &input);
    CHECK(ui.update_channel == BROWSER_UPDATE_CHANNEL_BETA
          && intent.setting.id == PSP_UI_SETTING_UPDATE_CHANNEL
          && intent.setting.value.update_channel
                 == BROWSER_UPDATE_CHANNEL_BETA);
    ui.developer_update_available = true;
    input.pressed = PSP_UI_BUTTON_RIGHT;
    intent = psp_ui_update(&ui, &input);
    CHECK(ui.update_channel == BROWSER_UPDATE_CHANNEL_DEVELOPER
          && intent.setting.value.update_channel
                 == BROWSER_UPDATE_CHANNEL_DEVELOPER);
    input.pressed = PSP_UI_BUTTON_DOWN;
    intent = psp_ui_update(&ui, &input);
    CHECK(ui.experimental_options_selection == 5);
    input.pressed = PSP_UI_BUTTON_CONFIRM;
    intent = psp_ui_update(&ui, &input);
    CHECK(intent.action == PSP_UI_ACTION_EDIT_DEVELOPER_URL
          && ui.screen == PSP_UI_SCREEN_EXPERIMENTAL_OPTIONS);
    input.pressed = PSP_UI_BUTTON_CANCEL;
    intent = psp_ui_update(&ui, &input);
    CHECK(ui.screen == PSP_UI_SCREEN_OPTIONS
          && ui.options_group_selection == 4
          && ui.overlay_motion == 2u);
    input.pressed = PSP_UI_BUTTON_UP;
    (void) psp_ui_update(&ui, &input);
    input.pressed = PSP_UI_BUTTON_UP;
    (void) psp_ui_update(&ui, &input);
    input.pressed = PSP_UI_BUTTON_UP;
    (void) psp_ui_update(&ui, &input);
    CHECK(ui.options_group_selection == 1);
    input.pressed = PSP_UI_BUTTON_CONFIRM;
    intent = psp_ui_update(&ui, &input);
    ui.options_selection = 9;
    CHECK(ui.options_selection == 9);
    input.pressed = PSP_UI_BUTTON_LEFT;
    intent = psp_ui_update(&ui, &input);
    CHECK(ui.danzeff_text_input
          && intent.setting.id == PSP_UI_SETTING_TEXT_ENTRY_MODE
          && intent.setting.value.text_entry_mode
                 == BROWSER_TEXT_ENTRY_DANZEFF);
    input.pressed = PSP_UI_BUTTON_DOWN;
    intent = psp_ui_update(&ui, &input);
    CHECK(ui.options_selection == 10);
    input.pressed = PSP_UI_BUTTON_LEFT;
    intent = psp_ui_update(&ui, &input);
    CHECK(!ui.analog_cursor_enabled
          && intent.setting.id == PSP_UI_SETTING_ANALOG_CURSOR
          && !intent.setting.value.boolean);
    input.pressed = PSP_UI_BUTTON_DOWN;
    intent = psp_ui_update(&ui, &input);
    CHECK(ui.options_selection == 11);
    input.pressed = PSP_UI_BUTTON_LEFT;
    intent = psp_ui_update(&ui, &input);
    CHECK(!ui.javascript_enabled
          && intent.setting.id == PSP_UI_SETTING_JAVASCRIPT
          && !intent.setting.value.boolean);
    input.pressed = PSP_UI_BUTTON_DOWN;
    intent = psp_ui_update(&ui, &input);
    CHECK(ui.options_selection == 12);
    input.pressed = PSP_UI_BUTTON_LEFT;
    intent = psp_ui_update(&ui, &input);
    CHECK(!ui.site_javascript_enabled
          && intent.setting.id == PSP_UI_SETTING_SITE_JAVASCRIPT
          && !intent.setting.value.boolean);
    input.pressed = PSP_UI_BUTTON_DOWN;
    intent = psp_ui_update(&ui, &input);
    CHECK(ui.options_selection == 13);
    input.pressed = PSP_UI_BUTTON_LEFT;
    intent = psp_ui_update(&ui, &input);
    CHECK(ui.search_engine == BROWSER_SEARCH_DUCKDUCKGO
          && intent.setting.id == PSP_UI_SETTING_SEARCH_ENGINE
          && intent.setting.value.search_engine
                 == BROWSER_SEARCH_DUCKDUCKGO);
    input.pressed = PSP_UI_BUTTON_CANCEL;
    (void) psp_ui_update(&ui, &input);
    input.pressed = PSP_UI_BUTTON_UP;
    (void) psp_ui_update(&ui, &input);
    CHECK(ui.screen == PSP_UI_SCREEN_OPTIONS
          && ui.options_group_selection == 0);
    input.pressed = PSP_UI_BUTTON_CONFIRM;
    intent = psp_ui_update(&ui, &input);
    ui.options_selection = 14;
    CHECK(ui.options_selection == 14);
    input.pressed = PSP_UI_BUTTON_RIGHT;
    intent = psp_ui_update(&ui, &input);
    CHECK(ui.color_mode == BROWSER_COLOR_MODE_LIGHT
          && intent.setting.id == PSP_UI_SETTING_COLOR_MODE
          && intent.setting.value.color_mode
                 == BROWSER_COLOR_MODE_LIGHT);
    input.pressed = PSP_UI_BUTTON_DOWN;
    intent = psp_ui_update(&ui, &input);
    CHECK(ui.options_selection == 15);
    input.pressed = PSP_UI_BUTTON_RIGHT;
    intent = psp_ui_update(&ui, &input);
    CHECK(ui.chrome_theme == BROWSER_CHROME_THEME_OCEAN
          && intent.setting.id == PSP_UI_SETTING_CHROME_THEME
          && intent.setting.value.chrome_theme
                 == BROWSER_CHROME_THEME_OCEAN);
    /* Three accents cycle; the legacy fourth value steps into the cycle
       rather than presenting the default accent twice. */
    input.pressed = PSP_UI_BUTTON_RIGHT;
    intent = psp_ui_update(&ui, &input);
    CHECK(ui.chrome_theme == BROWSER_CHROME_THEME_PLUM);
    input.pressed = PSP_UI_BUTTON_RIGHT;
    intent = psp_ui_update(&ui, &input);
    CHECK(ui.chrome_theme == BROWSER_CHROME_THEME_FINCH);
    ui.chrome_theme = BROWSER_CHROME_THEME_EMBER;
    input.pressed = PSP_UI_BUTTON_RIGHT;
    intent = psp_ui_update(&ui, &input);
    CHECK(ui.chrome_theme == BROWSER_CHROME_THEME_OCEAN);
    input.pressed = PSP_UI_BUTTON_LEFT;
    intent = psp_ui_update(&ui, &input);
    CHECK(ui.chrome_theme == BROWSER_CHROME_THEME_FINCH);
    /* CPU ambient motion is retired and no longer occupies an Options row. */
    CHECK(!ui.wave_enabled && ui.options_selection == 15);
    ui.options_selection = 16;
    input.pressed = PSP_UI_BUTTON_CONFIRM;
    intent = psp_ui_update(&ui, &input);
    CHECK(ui.screen == PSP_UI_SCREEN_GLYPH_OPTIONS
          && intent.glyph_component_probe_requested);
    input.pressed = PSP_UI_BUTTON_RIGHT;
    intent = psp_ui_update(&ui, &input);
    CHECK(ui.glyph_language == BROWSER_GLYPH_LANGUAGE_JAPANESE
          && intent.setting.id == PSP_UI_SETTING_GLYPH_LANGUAGE
          && intent.setting.value.glyph_language
                 == BROWSER_GLYPH_LANGUAGE_JAPANESE);
    input.pressed = PSP_UI_BUTTON_DOWN;
    (void) psp_ui_update(&ui, &input);
    input.pressed = PSP_UI_BUTTON_CONFIRM;
    intent = psp_ui_update(&ui, &input);
    CHECK(ui.color_emoji
          && intent.setting.id == PSP_UI_SETTING_COLOR_EMOJI
          && intent.setting.value.boolean);
    input.pressed = PSP_UI_BUTTON_CANCEL;
    (void) psp_ui_update(&ui, &input);
    CHECK(ui.screen == PSP_UI_SCREEN_OPTION_ITEMS
          && ui.options_selection == 16);
    input.pressed = PSP_UI_BUTTON_CANCEL;
    (void) psp_ui_update(&ui, &input);
    input.pressed = PSP_UI_BUTTON_DOWN;
    (void) psp_ui_update(&ui, &input);
    CHECK(ui.screen == PSP_UI_SCREEN_OPTIONS
          && ui.options_group_selection == 1);
    input.pressed = PSP_UI_BUTTON_CONFIRM;
    intent = psp_ui_update(&ui, &input);
    ui.options_selection = 18;
    CHECK(ui.options_selection == 18);
    input.pressed = PSP_UI_BUTTON_CONFIRM;
    intent = psp_ui_update(&ui, &input);
    CHECK(ui.youtube_240p
          && intent.setting.id == PSP_UI_SETTING_YOUTUBE_QUALITY
          && intent.setting.value.youtube_quality
                 == BROWSER_YOUTUBE_QUALITY_240P);
    input.pressed = PSP_UI_BUTTON_DOWN;
    intent = psp_ui_update(&ui, &input);
    CHECK(ui.options_selection == 19);
    input.pressed = PSP_UI_BUTTON_CONFIRM;
    intent = psp_ui_update(&ui, &input);
    CHECK(ui.youtube_compact_results
          && intent.setting.id
                 == PSP_UI_SETTING_YOUTUBE_COMPACT_RESULTS
          && intent.setting.value.boolean);
    input.pressed = PSP_UI_BUTTON_DOWN;
    intent = psp_ui_update(&ui, &input);
    CHECK(ui.options_selection == 20);
    input.pressed = PSP_UI_BUTTON_CONFIRM;
    intent = psp_ui_update(&ui, &input);
    CHECK(ui.video_startup_buffering
          && intent.setting.id
                 == PSP_UI_SETTING_VIDEO_STARTUP_BUFFERING
          && intent.setting.value.boolean);
    input.pressed = PSP_UI_BUTTON_DOWN;
    intent = psp_ui_update(&ui, &input);
    CHECK(ui.options_selection == 21);
    input.pressed = PSP_UI_BUTTON_CONFIRM;
    intent = psp_ui_update(&ui, &input);
    CHECK(ui.resume_offline_downloads
          && intent.setting.id
                 == PSP_UI_SETTING_RESUME_OFFLINE_DOWNLOADS
          && intent.setting.value.boolean);
    /*
     * Video scaling is the last Appearance row. It defaults to Smooth -- the
     * graphics chip's bilinear -- and a press asks for Sharp, which is the
     * software scaler's nearest neighbour rather than the same filter with a
     * flag flipped.
     */
    ui.options_selection = 17;
    input.pressed = PSP_UI_BUTTON_CONFIRM;
    intent = psp_ui_update(&ui, &input);
    CHECK(ui.video_scaling_sharp
          && intent.setting.id == PSP_UI_SETTING_VIDEO_SCALING
          && intent.setting.value.video_scaling
                 == BROWSER_VIDEO_SCALING_SHARP);
    input.pressed = PSP_UI_BUTTON_CONFIRM;
    intent = psp_ui_update(&ui, &input);
    CHECK(!ui.video_scaling_sharp
          && intent.setting.id == PSP_UI_SETTING_VIDEO_SCALING
          && intent.setting.value.video_scaling
                 == BROWSER_VIDEO_SCALING_SMOOTH);
    ui.options_selection = 21;
    input.pressed = PSP_UI_BUTTON_CANCEL;
    (void) psp_ui_update(&ui, &input);
    CHECK(ui.screen == PSP_UI_SCREEN_OPTIONS
          && ui.options_group_selection == 1);
    input.pressed = PSP_UI_BUTTON_DOWN;
    (void) psp_ui_update(&ui, &input);
    CHECK(ui.options_group_selection == 2);
    input.pressed = PSP_UI_BUTTON_CONFIRM;
    intent = psp_ui_update(&ui, &input);
    CHECK(ui.screen == PSP_UI_SCREEN_OPTION_ITEMS
          && ui.options_selection == 22);
    input.pressed = PSP_UI_BUTTON_CONFIRM;
    intent = psp_ui_update(&ui, &input);
    CHECK(ui.content_blocker_mode == CONTENT_BLOCKER_CUSTOM
          && intent.setting.id == PSP_UI_SETTING_CONTENT_BLOCKER_MODE
          && intent.setting.value.content_blocker_mode
                 == CONTENT_BLOCKER_CUSTOM);
    psp_ui_set_page(&ui, "TEST", "https://publisher.example/", true);
    ui.content_blocker_cosmetic_hiding = true;
    input.pressed = PSP_UI_BUTTON_DOWN;
    intent = psp_ui_update(&ui, &input);
    CHECK(ui.options_selection == 23);
    input.pressed = PSP_UI_BUTTON_CONFIRM;
    intent = psp_ui_update(&ui, &input);
    CHECK(!ui.content_blocker_cosmetic_hiding
          && intent.setting.id
                 == PSP_UI_SETTING_CONTENT_BLOCKER_COSMETIC_HIDING
          && !intent.setting.value.boolean);
    input.pressed = PSP_UI_BUTTON_DOWN;
    intent = psp_ui_update(&ui, &input);
    CHECK(ui.options_selection == 24);
    input.pressed = PSP_UI_BUTTON_CONFIRM;
    intent = psp_ui_update(&ui, &input);
    CHECK(!ui.cookie_banner_hidden
          && intent.setting.id == PSP_UI_SETTING_COOKIE_BANNER_HIDDEN
          && !intent.setting.value.boolean);
    input.pressed = PSP_UI_BUTTON_DOWN;
    intent = psp_ui_update(&ui, &input);
    CHECK(ui.options_selection == 25);
    input.pressed = PSP_UI_BUTTON_CONFIRM;
    intent = psp_ui_update(&ui, &input);
    CHECK(ui.content_blocker_site_allowed
          && intent.setting.id
                 == PSP_UI_SETTING_CONTENT_BLOCKER_SITE_ALLOWED
          && intent.setting.value.boolean);
    input.pressed = PSP_UI_BUTTON_DOWN;
    intent = psp_ui_update(&ui, &input);
    CHECK(ui.options_selection == 26);
    input.pressed = PSP_UI_BUTTON_CONFIRM;
    intent = psp_ui_update(&ui, &input);
    CHECK(intent.load_content_blocker_allowlist_requested);
    input.pressed = PSP_UI_BUTTON_DOWN;
    intent = psp_ui_update(&ui, &input);
    CHECK(ui.options_selection == 27);
    input.pressed = PSP_UI_BUTTON_CONFIRM;
    intent = psp_ui_update(&ui, &input);
    CHECK(!ui.site_data_allowed
          && intent.setting.id == PSP_UI_SETTING_SITE_DATA_ALLOWED
          && !intent.setting.value.boolean);
    ui.tls_session_persistence = true;
    input.pressed = PSP_UI_BUTTON_DOWN;
    intent = psp_ui_update(&ui, &input);
    CHECK(ui.options_selection == 28);
    input.pressed = PSP_UI_BUTTON_CONFIRM;
    intent = psp_ui_update(&ui, &input);
    CHECK(!ui.tls_session_persistence
          && intent.setting.id
                 == PSP_UI_SETTING_TLS_SESSION_PERSISTENCE
          && !intent.setting.value.boolean);
    input.pressed = PSP_UI_BUTTON_DOWN;
    intent = psp_ui_update(&ui, &input);
    CHECK(ui.options_selection == 29);
    input.pressed = PSP_UI_BUTTON_CONFIRM;
    intent = psp_ui_update(&ui, &input);
    CHECK(ui.mixed_content_site_allowed
          && intent.setting.id == PSP_UI_SETTING_MIXED_CONTENT_SITE
          && intent.setting.value.boolean);
    input.pressed = PSP_UI_BUTTON_DOWN;
    intent = psp_ui_update(&ui, &input);
    CHECK(ui.options_selection == 30);
    input.pressed = PSP_UI_BUTTON_CONFIRM;
    intent = psp_ui_update(&ui, &input);
    CHECK(ui.third_party_cookie_site_allowed
          && intent.setting.id
                 == PSP_UI_SETTING_THIRD_PARTY_COOKIES_SITE
          && intent.setting.value.boolean);
    input.pressed = PSP_UI_BUTTON_CANCEL;
    (void) psp_ui_update(&ui, &input);
    CHECK(ui.screen == PSP_UI_SCREEN_OPTIONS
          && ui.options_group_selection == 2);
    input.pressed = PSP_UI_BUTTON_DOWN;
    (void) psp_ui_update(&ui, &input);
    input.pressed = PSP_UI_BUTTON_CONFIRM;
    intent = psp_ui_update(&ui, &input);
    psp_ui_set_network_profile(&ui, 2u, "Cafe Wi-Fi");
    CHECK(ui.network_profile == 2u
          && ui.network_profile_label_valid
          && strcmp(ui.network_profile_label, "Cafe Wi-Fi (P2)") == 0);
    ui.options_selection = 31;
    CHECK(ui.options_selection == 31);
    input.pressed = PSP_UI_BUTTON_RIGHT;
    intent = psp_ui_update(&ui, &input);
    CHECK(intent.setting.id == PSP_UI_SETTING_NETWORK_PROFILE
          && intent.setting.value.unsigned_value == 1u
          && ui.network_profile == 2u);
    input.pressed = PSP_UI_BUTTON_LEFT;
    intent = psp_ui_update(&ui, &input);
    CHECK(intent.setting.id == PSP_UI_SETTING_NETWORK_PROFILE
          && intent.setting.value.unsigned_value == 0u);
    input.pressed = PSP_UI_BUTTON_CONFIRM;
    intent = psp_ui_update(&ui, &input);
    CHECK(intent.setting.id == PSP_UI_SETTING_NETWORK_PROFILE
          && intent.setting.value.unsigned_value == 2u);
    input.pressed = PSP_UI_BUTTON_DOWN;
    intent = psp_ui_update(&ui, &input);
    CHECK(ui.options_selection == 32);
    input.pressed = PSP_UI_BUTTON_CONFIRM;
    intent = psp_ui_update(&ui, &input);
    CHECK(!ui.update_check_enabled
          && intent.setting.id == PSP_UI_SETTING_UPDATE_CHECK
          && !intent.setting.value.boolean);
    input.pressed = PSP_UI_BUTTON_RIGHT;
    intent = psp_ui_update(&ui, &input);
    CHECK(ui.update_check_enabled
          && intent.setting.id == PSP_UI_SETTING_UPDATE_CHECK
          && intent.setting.value.boolean);
    input.pressed = PSP_UI_BUTTON_DOWN;
    intent = psp_ui_update(&ui, &input);
    CHECK(ui.options_selection == 33);
    input.pressed = PSP_UI_BUTTON_CONFIRM;
    intent = psp_ui_update(&ui, &input);
    CHECK(ui.screen == PSP_UI_SCREEN_UPDATE);
    psp_ui_set_update(
        &ui, "0.1.0", "UPDATE AVAILABLE", "Safer update.", -1,
        "DOWNLOAD", true, false);
    CHECK(!ui.network_profile_label_valid
          && strcmp(ui.update_notes, "Safer update.") == 0);
    input.pressed = PSP_UI_BUTTON_CONFIRM;
    intent = psp_ui_update(&ui, &input);
    CHECK(intent.update_primary_requested);
    input.pressed = PSP_UI_BUTTON_CANCEL;
    intent = psp_ui_update(&ui, &input);
    CHECK(ui.screen == PSP_UI_SCREEN_OPTION_ITEMS);
    input.pressed = PSP_UI_BUTTON_DOWN;
    intent = psp_ui_update(&ui, &input);
    CHECK(ui.options_selection == 34);
    input.pressed = PSP_UI_BUTTON_CONFIRM;
    intent = psp_ui_update(&ui, &input);
    CHECK(ui.screen == PSP_UI_SCREEN_DATA_OPTIONS
          && ui.data_options_selection == 0);
    input.pressed = PSP_UI_BUTTON_RIGHT;
    intent = psp_ui_update(&ui, &input);
    CHECK(ui.live_cache_kib == 1024
          && intent.setting.id == PSP_UI_SETTING_LIVE_CACHE_KIB
          && intent.setting.value.unsigned_value == 1024);
    input.pressed = PSP_UI_BUTTON_DOWN;
    intent = psp_ui_update(&ui, &input);
    input.pressed = PSP_UI_BUTTON_RIGHT;
    intent = psp_ui_update(&ui, &input);
    CHECK(ui.persistent_cache_mb == 1
          && intent.setting.id == PSP_UI_SETTING_PERSISTENT_CACHE_MB
          && intent.setting.value.unsigned_value == 1);
    input.pressed = PSP_UI_BUTTON_DOWN;
    intent = psp_ui_update(&ui, &input);
    input.pressed = PSP_UI_BUTTON_CONFIRM;
    intent = psp_ui_update(&ui, &input);
    CHECK(ui.persist_local_storage
          && intent.setting.id == PSP_UI_SETTING_PERSIST_LOCAL_STORAGE
          && intent.setting.value.boolean);
    input.pressed = PSP_UI_BUTTON_DOWN;
    (void) psp_ui_update(&ui, &input);
    input.pressed = PSP_UI_BUTTON_CONFIRM;
    intent = psp_ui_update(&ui, &input);
    CHECK(!intent.clear_cache_requested
          && ui.data_clear_confirmation == 4);
    intent = psp_ui_update(&ui, &input);
    CHECK(intent.clear_cache_requested);
    input.pressed = PSP_UI_BUTTON_DOWN;
    (void) psp_ui_update(&ui, &input);
    input.pressed = PSP_UI_BUTTON_CONFIRM;
    intent = psp_ui_update(&ui, &input);
    CHECK(!intent.clear_cookies_requested
          && ui.data_clear_confirmation == 5);
    intent = psp_ui_update(&ui, &input);
    CHECK(intent.clear_cookies_requested);
    input.pressed = PSP_UI_BUTTON_DOWN;
    (void) psp_ui_update(&ui, &input);
    input.pressed = PSP_UI_BUTTON_CONFIRM;
    intent = psp_ui_update(&ui, &input);
    CHECK(!intent.clear_local_storage_requested
          && ui.data_clear_confirmation == 6);
    intent = psp_ui_update(&ui, &input);
    CHECK(intent.clear_local_storage_requested);
    input.pressed = PSP_UI_BUTTON_DOWN;
    (void) psp_ui_update(&ui, &input);
    input.pressed = PSP_UI_BUTTON_CONFIRM;
    intent = psp_ui_update(&ui, &input);
    CHECK(!intent.clear_session_storage_requested
          && ui.data_clear_confirmation == 7);
    intent = psp_ui_update(&ui, &input);
    CHECK(intent.clear_session_storage_requested);
    input.pressed = PSP_UI_BUTTON_CANCEL;
    intent = psp_ui_update(&ui, &input);
    CHECK(ui.screen == PSP_UI_SCREEN_OPTION_ITEMS);
    input.pressed = PSP_UI_BUTTON_CANCEL;
    intent = psp_ui_update(&ui, &input);
    CHECK(ui.screen == PSP_UI_SCREEN_OPTIONS);
    input.pressed = PSP_UI_BUTTON_CANCEL;
    intent = psp_ui_update(&ui, &input);
    CHECK(ui.screen == PSP_UI_SCREEN_PAGE);

    BrowserProfileSuggestion text_suggestion = {
        .url = "https://en.wikipedia.org/",
        .title = "Wikipedia",
        .bookmark = true
    };
    PspUiTextEntryView text_view = {
        .description = "SEARCH OR ADDRESS",
        .text = "wiki",
        .suggestions = &text_suggestion,
        .cursor = 4,
        .suggestion_count = 1,
        .suggestion_selection = 0,
        .cell = 0
    };
    psp_ui_set_text_entry(&ui, &text_view);
    CHECK(ui.screen == PSP_UI_SCREEN_TEXT_ENTRY
          && ui.text_entry == &text_view
          && strcmp(ui.text_entry->text, "wiki") == 0
          && ui.text_entry->cursor == 4
          && ui.text_entry->suggestion_count == 1
          && ui.text_entry->suggestion_selection == 0);
    psp_ui_clear_text_entry(&ui);
    CHECK(ui.text_entry == NULL);
    ui.screen = PSP_UI_SCREEN_PAGE;

    /* START remains the omnibox shortcut ordinarily, but is the missing
       HTML Enter key while a single-line page field owns focus. Opening the
       menu must keep its existing address shortcut rather than inheriting
       that page-only context. */
    ui.focus_editable = true;
    input = (PspUiInput) {.pressed = PSP_UI_BUTTON_ADDRESS};
    intent = psp_ui_update(&ui, &input);
    CHECK(intent.action == PSP_UI_ACTION_SUBMIT_FOCUSED_TEXT);
    ui.screen = PSP_UI_SCREEN_MENU;
    input.pressed = PSP_UI_BUTTON_ADDRESS;
    intent = psp_ui_update(&ui, &input);
    CHECK(intent.action == PSP_UI_ACTION_OPEN_ADDRESS
          && ui.screen == PSP_UI_SCREEN_PAGE);
    ui.focus_editable = false;

    PspUiState voice_ui;
    psp_ui_init(&voice_ui);
    voice_ui.screen = PSP_UI_SCREEN_EXPERIMENTAL_OPTIONS;
    voice_ui.experimental_voice_input = true;
    voice_ui.voice_component_phase = PSP_UI_VOICE_COMPONENT_READY;
    voice_ui.experimental_options_selection = 2;
    input.pressed = PSP_UI_BUTTON_CONFIRM;
    intent = psp_ui_update(&voice_ui, &input);
    CHECK(intent.action == PSP_UI_ACTION_OPEN_VOICE_ADDRESS
          && voice_ui.screen == PSP_UI_SCREEN_PAGE
          && psp_ui_intent_has_predispatch_visual(&intent));

    input.pressed = 0;
    input.analog_y = 255;
    intent = psp_ui_update(&ui, &input);
    CHECK(intent.scroll_delta > 0);
    input.analog_y = 0;
    intent = psp_ui_update(&ui, &input);
    CHECK(intent.scroll_delta < 0);
    return true;
}

static int accumulated_analog_scroll(unsigned step_ms, unsigned total_ms,
                                     uint8_t analog_y)
{
    PspUiState ui;
    psp_ui_init(&ui);
    ui.analog_cursor_enabled = false;
    PspUiInput input = {
        .analog_x = 128,
        .analog_y = analog_y,
        .elapsed_ms = step_ms
    };
    int total = 0;
    for (unsigned elapsed = 0; elapsed < total_ms; elapsed += step_ms) {
        total += psp_ui_update(&ui, &input).scroll_delta;
    }
    return total;
}

static bool test_time_based_analog_scroll(void)
{
    int ten_ms = accumulated_analog_scroll(10, 1000, 255);
    int twenty_ms = accumulated_analog_scroll(20, 1000, 255);
    int gentle = accumulated_analog_scroll(20, 1000, 170);
    int reverse = accumulated_analog_scroll(20, 1000, 0);
    int cadence_difference = ten_ms - twenty_ms;
    if (cadence_difference < 0) cadence_difference = -cadence_difference;
    CHECK(ten_ms > 1000 && twenty_ms > 1000
          && cadence_difference <= 20
          && gentle > 0 && gentle < twenty_ms / 4
          && reverse < 0);

    PspUiState stalled_ui;
    psp_ui_init(&stalled_ui);
    stalled_ui.analog_cursor_enabled = false;
    PspUiInput stalled = {
        .analog_x = 128, .analog_y = 255, .elapsed_ms = 1000
    };
    PspUiIntent stalled_intent = psp_ui_update(&stalled_ui, &stalled);
    CHECK(stalled_intent.scroll_delta > 0
          && stalled_intent.scroll_delta < 150);
    stalled.analog_y = 128;
    stalled_intent = psp_ui_update(&stalled_ui, &stalled);
    CHECK(stalled_ui.analog_hold_ms == 0
          && stalled_ui.analog_scroll_remainder == 0
          && stalled_ui.analog_scroll_direction == 0
          && stalled_intent.scroll_settle);
    psp_ui_set_page_interaction(
        &stalled_ui, PSP_UI_CURSOR_TEXT, 2);
    CHECK(stalled_ui.cursor_shape == PSP_UI_CURSOR_TEXT
          && stalled_ui.page_scrollbar_width == 2);
    return true;
}

static bool test_focus_hold_repeat(void)
{
    PspUiState ui;
    psp_ui_init(&ui);
    PspUiInput input = {
        .held = PSP_UI_BUTTON_DOWN,
        .pressed = PSP_UI_BUTTON_DOWN,
        .analog_x = 128,
        .analog_y = 128,
        .elapsed_ms = 20
    };
    CHECK(psp_ui_update(&ui, &input).action
          == PSP_UI_ACTION_FOCUS_DOWN);
    input.pressed = 0;
    for (unsigned elapsed = 20; elapsed < 360; elapsed += 20)
        CHECK(psp_ui_update(&ui, &input).action == PSP_UI_ACTION_NONE);
    CHECK(psp_ui_update(&ui, &input).action
          == PSP_UI_ACTION_FOCUS_DOWN);
    for (unsigned elapsed = 20; elapsed < 120; elapsed += 20)
        CHECK(psp_ui_update(&ui, &input).action == PSP_UI_ACTION_NONE);
    CHECK(psp_ui_update(&ui, &input).action
          == PSP_UI_ACTION_FOCUS_DOWN);

    input.held = PSP_UI_BUTTON_LEFT;
    CHECK(psp_ui_update(&ui, &input).action == PSP_UI_ACTION_NONE);
    input.pressed = PSP_UI_BUTTON_LEFT;
    CHECK(psp_ui_update(&ui, &input).action
          == PSP_UI_ACTION_FOCUS_LEFT);
    psp_ui_suspend_page_input(&ui);
    CHECK(ui.focus_repeat_direction == 0
          && ui.focus_repeat_elapsed_ms == 0);

    ui.screen = PSP_UI_SCREEN_MENU;
    ui.menu_selection = 0;
    input.pressed = 0;
    input.held = PSP_UI_BUTTON_DOWN;
    for (unsigned elapsed = 0; elapsed < 600; elapsed += 20)
        (void) psp_ui_update(&ui, &input);
    CHECK(ui.menu_selection == 0);
    return true;
}

static bool test_page_dark_transform(void)
{
    CHECK(!psp_ui_color_mode_is_dark(BROWSER_COLOR_MODE_AUTO, 12));
    CHECK(psp_ui_color_mode_is_dark(BROWSER_COLOR_MODE_AUTO, 19));
    CHECK(psp_ui_color_mode_is_dark(BROWSER_COLOR_MODE_AUTO, 6));
    CHECK(!psp_ui_color_mode_is_dark(BROWSER_COLOR_MODE_LIGHT, 23));
    CHECK(psp_ui_color_mode_is_dark(BROWSER_COLOR_MODE_DARK, 12));

    uint16_t pixels[] = {
        UINT16_C(0xffff), UINT16_C(0x0000), UINT16_C(0xf800),
        UINT16_C(0x07e0), UINT16_C(0x001f), UINT16_C(0x55aa)
    };
    uint16_t guard = pixels[5];
    psp_ui_apply_page_dark_rgb565(pixels, 5, 1, 6);
    CHECK(pixels[0] != UINT16_C(0xffff)
          && pixels[1] != UINT16_C(0x0000)
          && pixels[2] != pixels[3]
          && pixels[5] == guard);
    unsigned white_luma =
        (((pixels[0] >> 11) & 31u) << 1)
        + 2u * ((pixels[0] >> 5) & 63u)
        + ((pixels[0] & 31u) << 1);
    unsigned black_luma =
        (((pixels[1] >> 11) & 31u) << 1)
        + 2u * ((pixels[1] >> 5) & 63u)
        + ((pixels[1] & 31u) << 1);
    CHECK(white_luma < black_luma);
    return true;
}

typedef struct {
    unsigned calls;
    unsigned cpu_mhz;
    unsigned bus_mhz;
    uint64_t elapsed_us;
    bool fail_next;
} FakePowerClock;

static bool fake_power_set_clock(
    void *context, unsigned cpu_mhz, unsigned bus_mhz,
    uint64_t *transition_us)
{
    FakePowerClock *clock = context;
    clock->calls++;
    if (clock->fail_next) {
        clock->fail_next = false;
        return false;
    }
    clock->cpu_mhz = cpu_mhz;
    clock->bus_mhz = bus_mhz;
    *transition_us = clock->elapsed_us;
    return true;
}

static bool test_battery_clock_policy(void)
{
    FakePowerClock clock = {.elapsed_us = 37};
    PspPowerPolicy policy;
    psp_power_policy_init(
        &policy,
        (PspPowerPolicyBackend) {
            .set_clock = fake_power_set_clock,
            .context = &clock
        });
    CHECK(!psp_power_policy_update(
              &policy, PSP_POWER_POLICY_IDLE_DELAY_MS - 1u,
              false, false)
          && clock.calls == 0 && !policy.idle_clock);
    CHECK(psp_power_policy_update(&policy, 1, false, false)
          && clock.calls == 1 && policy.idle_clock
          && clock.cpu_mhz == PSP_POWER_POLICY_IDLE_CPU_MHZ
          && clock.bus_mhz == PSP_POWER_POLICY_IDLE_BUS_MHZ);
    CHECK(!psp_power_policy_update(&policy, 500, false, false)
          && clock.calls == 1);
    CHECK(psp_power_policy_update(&policy, 16, true, false)
          && !policy.idle_clock
          && clock.cpu_mhz == PSP_POWER_POLICY_HIGH_CPU_MHZ
          && clock.bus_mhz == PSP_POWER_POLICY_HIGH_BUS_MHZ
          && policy.maximum_transition_us == 37);

    clock.fail_next = true;
    CHECK(!psp_power_policy_update(
              &policy, PSP_POWER_POLICY_IDLE_DELAY_MS, false, false)
          && !policy.idle_clock && policy.failures == 1);
    CHECK(psp_power_policy_update(&policy, 1, false, false)
          && policy.idle_clock && clock.calls == 4);
    CHECK(psp_power_policy_update(&policy, 16, false, true)
          && !policy.idle_clock);
    return true;
}

/*
 * The status line is static UI-side state, so it is asserted through the
 * pixels it produces: a HOME composite is the only observer there is.
 */
static bool test_device_status_clamp_and_clock_format(void)
{
    enum { WIDTH = 480, HEIGHT = 272 };
    static uint16_t clamped[WIDTH * HEIGHT];
    static uint16_t full[WIDTH * HEIGHT];
    static uint16_t twelve[WIDTH * HEIGHT];
    static uint16_t blanked[WIDTH * HEIGHT];
    PspUiState ui;
    psp_ui_init(&ui);
    PspUiHomeView home = {
        .tile_count = 1, .continue_count = 0, .engine_ready = true,
        .tiles = {{"WIKIPEDIA", "Articles and search"}}
    };
    psp_ui_show_home(&ui);
    psp_ui_set_home(&ui, &home);

    /* A pack reporting over 100% is a reading to clamp; blanking the clock
       and the battery with it would be a worse answer than 100%. */
    psp_ui_set_device_status(13, 42, 100, false, false, -1);
    psp_ui_composite(&ui, full, WIDTH, HEIGHT, WIDTH);
    psp_ui_set_device_status(13, 42, 137, false, false, -1);
    psp_ui_composite(&ui, clamped, WIDTH, HEIGHT, WIDTH);
    CHECK(memcmp(clamped, full, sizeof(full)) == 0);

    /* An hour that cannot exist is still a bad reading, not a clampable
       one, and still invalidates the line. */
    psp_ui_set_device_status(24, 42, 62, false, false, -1);
    psp_ui_composite(&ui, blanked, WIDTH, HEIGHT, WIDTH);
    CHECK(memcmp(blanked, full, sizeof(full)) != 0);
    psp_ui_set_device_status(13, 42, -1, false, false, -1);
    psp_ui_composite(&ui, clamped, WIDTH, HEIGHT, WIDTH);
    CHECK(memcmp(clamped, blanked, sizeof(blanked)) == 0);

    /* The 12-hour preference reaches the pixels: "1:42 PM" is not the same
       string, nor the same width, as "13:42". */
    psp_ui_set_device_status(13, 42, 100, false, true, -1);
    psp_ui_composite(&ui, twelve, WIDTH, HEIGHT, WIDTH);
    CHECK(memcmp(twelve, full, sizeof(full)) != 0);
    CHECK(memcmp(twelve, blanked, sizeof(blanked)) != 0);

    /* Midnight and noon are the two hours the 12-hour mapping gets wrong
       if it forgets that 0 means 12; they must not render alike. */
    psp_ui_set_device_status(0, 42, 100, false, true, -1);
    psp_ui_composite(&ui, clamped, WIDTH, HEIGHT, WIDTH);
    psp_ui_set_device_status(12, 42, 100, false, true, -1);
    psp_ui_composite(&ui, full, WIDTH, HEIGHT, WIDTH);
    CHECK(memcmp(clamped, full, sizeof(full)) != 0);

    psp_ui_set_device_status(14, 8, 62, false, false, -1);
    return true;
}

/*
 * Wifi bars: a negative count is absent (no cluster drawn), a non-negative
 * count draws the cluster with that many bars filled, and counts above the
 * maximum clamp rather than overflow. Each distinction has to reach the
 * pixels, and "absent" must not render like "zero".
 */
static bool test_wifi_status_bars(void)
{
    enum { WIDTH = 480, HEIGHT = 272 };
    static uint16_t absent[WIDTH * HEIGHT];
    static uint16_t zero[WIDTH * HEIGHT];
    static uint16_t some[WIDTH * HEIGHT];
    static uint16_t full[WIDTH * HEIGHT];
    static uint16_t over[WIDTH * HEIGHT];
    PspUiState ui;
    psp_ui_init(&ui);
    PspUiHomeView home = {
        .tile_count = 1, .continue_count = 0, .engine_ready = true,
        .tiles = {{"WIKIPEDIA", "Articles and search"}}
    };
    psp_ui_show_home(&ui);
    psp_ui_set_home(&ui, &home);

    psp_ui_set_device_status(13, 42, 62, false, false, -1);
    psp_ui_composite(&ui, absent, WIDTH, HEIGHT, WIDTH);
    /* Zero bars is still a cluster (empty outlines), so it is not absent. */
    psp_ui_set_device_status(13, 42, 62, false, false, 0);
    psp_ui_composite(&ui, zero, WIDTH, HEIGHT, WIDTH);
    CHECK(memcmp(zero, absent, sizeof(absent)) != 0);
    /* Filling bars changes the pixels, and a stronger reading differs from
       a weaker one. */
    psp_ui_set_device_status(13, 42, 62, false, false, 2);
    psp_ui_composite(&ui, some, WIDTH, HEIGHT, WIDTH);
    CHECK(memcmp(some, zero, sizeof(zero)) != 0);
    psp_ui_set_device_status(13, 42, 62, false, false, 4);
    psp_ui_composite(&ui, full, WIDTH, HEIGHT, WIDTH);
    CHECK(memcmp(full, some, sizeof(some)) != 0);
    /* Above the maximum clamps to the maximum rather than overflowing. */
    psp_ui_set_device_status(13, 42, 62, false, false, 9);
    psp_ui_composite(&ui, over, WIDTH, HEIGHT, WIDTH);
    CHECK(memcmp(over, full, sizeof(full)) == 0);

    psp_ui_set_device_status(14, 8, 62, false, false, -1);
    return true;
}

/*
 * Only the three exact legacy collection URLs map onto a COLLECTIONS
 * section; a trailing slash is tolerated, but a deeper /offline item route
 * or a look-alike host is not a collection. The dispatch that routes these
 * to the native surface is PSP-only, so the classifier it leans on is
 * exercised here directly.
 */
static bool test_legacy_collection_url_classification(void)
{
    PspUiCollectionSection section = PSP_UI_COLLECTION_OFFLINE;

    section = PSP_UI_COLLECTION_OFFLINE;
    CHECK(psp_ui_legacy_collection_url(
              "https://tilefinch.local/bookmarks", &section));
    CHECK(section == PSP_UI_COLLECTION_BOOKMARKS);
    CHECK(psp_ui_legacy_collection_url(
              "https://tilefinch.local/bookmarks/", &section));
    CHECK(section == PSP_UI_COLLECTION_BOOKMARKS);

    section = PSP_UI_COLLECTION_OFFLINE;
    CHECK(psp_ui_legacy_collection_url(
              "https://tilefinch.local/history", &section));
    CHECK(section == PSP_UI_COLLECTION_HISTORY);

    section = PSP_UI_COLLECTION_BOOKMARKS;
    CHECK(psp_ui_legacy_collection_url(
              "https://tilefinch.local/offline", &section));
    CHECK(section == PSP_UI_COLLECTION_OFFLINE);
    section = PSP_UI_COLLECTION_BOOKMARKS;
    CHECK(psp_ui_legacy_collection_url(
              "https://tilefinch.local/offline/", &section));
    CHECK(section == PSP_UI_COLLECTION_OFFLINE);

    /* Deeper offline routes stay with the ordinary navigation arms. */
    CHECK(!psp_ui_legacy_collection_url(
              "https://tilefinch.local/offline/video?id=3", NULL));
    CHECK(!psp_ui_legacy_collection_url(
              "https://tilefinch.local/offline/youtube?id=abc", NULL));
    /* No boundary, wrong host, and NULL are all non-matches. */
    CHECK(!psp_ui_legacy_collection_url(
              "https://tilefinch.local/bookmarksss", NULL));
    CHECK(!psp_ui_legacy_collection_url(
              "https://example.com/bookmarks", NULL));
    CHECK(!psp_ui_legacy_collection_url(NULL, NULL));

    /* A non-match must not disturb the caller's section. */
    section = PSP_UI_COLLECTION_HISTORY;
    CHECK(!psp_ui_legacy_collection_url(
              "https://tilefinch.local/settings", &section));
    CHECK(section == PSP_UI_COLLECTION_HISTORY);
    return true;
}

/*
 * The built-in start page is native chrome; the engine generator that once
 * served https://tilefinch.local/home is gone and no adapter claims the
 * host. Seven navigation sites can hand that URL to a page load -- a
 * submitted link, history back/forward, reload, a tab destination, a typed
 * address, a bookmark or history row, and the network gate -- and all of
 * them ask this one classifier first, so what it accepts is what decides
 * whether the user reaches HOME or an error page. That makes the accepted
 * set worth pinning here rather than only on the device.
 */
static bool test_native_home_url_classification(void)
{
    CHECK(psp_ui_native_home_url("https://tilefinch.local/home"));
    /* A trailing slash is the same address, and a restored tab or a
       bookmark may well have stored it that way. */
    CHECK(psp_ui_native_home_url("https://tilefinch.local/home/"));

    /* Anything under /home is a different document and must keep going to
       the ordinary navigation arms rather than silently becoming HOME. */
    CHECK(!psp_ui_native_home_url("https://tilefinch.local/home/x"));
    CHECK(!psp_ui_native_home_url("https://tilefinch.local/home?q=1"));
    /* No boundary: a longer path that merely starts with the prefix. */
    CHECK(!psp_ui_native_home_url("https://tilefinch.local/homepage"));
    /* The bare host is not the start page; the five sites that already
       routed never accepted it, so neither does the shared classifier. */
    CHECK(!psp_ui_native_home_url("https://tilefinch.local/"));
    /* Other internal surfaces and other hosts are not HOME. */
    CHECK(!psp_ui_native_home_url("https://tilefinch.local/bookmarks"));
    CHECK(!psp_ui_native_home_url("https://example.com/home"));
    CHECK(!psp_ui_native_home_url("http://tilefinch.local/home"));
    CHECK(!psp_ui_native_home_url(""));
    CHECK(!psp_ui_native_home_url(NULL));

    /* The origin boundary is broader than HOME because it is also the
       network-fallthrough guard for generated pages.  Hostname prefixes and
       insecure lookalikes must not be mistaken for the reserved origin. */
    CHECK(psp_ui_internal_url("https://tilefinch.local"));
    CHECK(psp_ui_internal_url("https://tilefinch.local/"));
    CHECK(psp_ui_internal_url("https://tilefinch.local?view=compact"));
    CHECK(psp_ui_internal_url("https://tilefinch.local:443/screenshots"));
    CHECK(psp_ui_internal_url("https://tilefinch.local/screenshots"));
    CHECK(psp_ui_internal_url("https://tilefinch.local/unknown/path"));
    CHECK(!psp_ui_internal_url("https://tilefinch.local.evil/home"));
    CHECK(!psp_ui_internal_url("https://tilefinch.local:444/home"));
    CHECK(!psp_ui_internal_url("http://tilefinch.local/home"));
    CHECK(!psp_ui_internal_url(""));
    CHECK(!psp_ui_internal_url(NULL));
    return true;
}

/*
 * The tab thumbnail placeholder is one letter. It used to be produced by
 * character-capping the whole title, which tripped the shared text
 * helper's ellipsis path with no right bound to stop it, so three dots
 * marched out of the 60px box and onto the domain line. Rendering the
 * same first letter as a whole title and as a one-character title must
 * therefore produce the same pixels.
 */
static bool test_tab_placeholder_draws_one_letter(void)
{
    enum { WIDTH = 480, HEIGHT = 272 };
    static uint16_t from_long[WIDTH * HEIGHT];
    static uint16_t from_short[WIDTH * HEIGHT];
    PspUiState ui;
    psp_ui_init(&ui);
    PspUiTabsView tabs = {.count = 1, .active_index = 0};
    snprintf(tabs.titles[0], sizeof(tabs.titles[0]), "PlayStation Portable");
    snprintf(tabs.domains[0], sizeof(tabs.domains[0]), "en.wikipedia.org");
    psp_ui_set_tabs(&ui, &tabs);
    ui.screen = PSP_UI_SCREEN_TABS;
    ui.tab_selection = 0;
    ui.toast_frames = 0;
    psp_ui_composite(&ui, from_long, WIDTH, HEIGHT, WIDTH);

    snprintf(tabs.titles[0], sizeof(tabs.titles[0]), "P");
    psp_ui_set_tabs(&ui, &tabs);
    psp_ui_composite(&ui, from_short, WIDTH, HEIGHT, WIDTH);

    /*
     * Compare everything left of the title column. draw_tabs puts the
     * panel at x=52, the 60px thumbnail box at +13 (so it ends at 125),
     * and the title at +82 (134) -- where the two fixtures are supposed
     * to differ. The dots used to land in the gap between the two.
     */
    for (int y = 0; y < HEIGHT; y++) {
        for (int x = 0; x < 134; x++) {
            size_t at = (size_t) y * WIDTH + (size_t) x;
            CHECK(from_long[at] == from_short[at]);
        }
    }
    return true;
}

static bool test_analog_cursor_and_drag(void)
{
    PspUiState threshold_ui;
    psp_ui_init(&threshold_ui);
    PspUiInput threshold = {
        .analog_x = 153, .analog_y = 128, .elapsed_ms = 16
    };
    PspUiIntent threshold_intent = psp_ui_update(&threshold_ui, &threshold);
    CHECK(threshold_intent.pointer_phase == PSP_UI_POINTER_MOVE
          && threshold_intent.pointer_x == 241
          && threshold_ui.cursor_fade == PSP_THEME_MOTION_FOCUS_FRAMES);

    PspUiState ui;
    psp_ui_init(&ui);
    CHECK(ui.analog_cursor_enabled && !ui.cursor_visible);
    PspUiInput input = {
        .analog_x = 255, .analog_y = 128, .elapsed_ms = 20
    };
    PspUiIntent intent = psp_ui_update(&ui, &input);
    CHECK(ui.cursor_visible && intent.pointer_phase == PSP_UI_POINTER_MOVE
          && intent.pointer_x > 240 && intent.pointer_y == 136
          && intent.scroll_delta == 0);
    int retained_x = intent.pointer_x;

    input.analog_x = 128;
    input.pressed = PSP_UI_BUTTON_CONFIRM;
    input.held = PSP_UI_BUTTON_CONFIRM;
    intent = psp_ui_update(&ui, &input);
    CHECK(intent.pointer_phase == PSP_UI_POINTER_DOWN
          && ui.cursor_pointer_down);
    input.pressed = 0;
    input.analog_x = 255;
    input.analog_y = 255;
    intent = psp_ui_update(&ui, &input);
    CHECK(intent.pointer_phase == PSP_UI_POINTER_MOVE
          && ui.cursor_pointer_down
          && intent.pointer_x > retained_x && intent.pointer_y > 136);
    input.held = 0;
    input.analog_x = 128;
    input.analog_y = 128;
    intent = psp_ui_update(&ui, &input);
    CHECK(intent.pointer_phase == PSP_UI_POINTER_UP
          && !ui.cursor_pointer_down);

    int before_dpad_x = ui.cursor_x_milli;
    input.pressed = PSP_UI_BUTTON_LEFT;
    intent = psp_ui_update(&ui, &input);
    /* The first direction after pointing hands the cursor position to focus
       instead of moving focus from wherever it happened to be. */
    CHECK(!ui.cursor_visible
          && intent.action == PSP_UI_ACTION_FOCUS_AT
          && ui.cursor_x_milli == before_dpad_x);
    intent = psp_ui_update(&ui, &input);
    CHECK(intent.action == PSP_UI_ACTION_FOCUS_LEFT
          && ui.cursor_x_milli == before_dpad_x);
    input.pressed = 0;
    input.analog_x = 255;
    intent = psp_ui_update(&ui, &input);
    CHECK(ui.cursor_visible && ui.cursor_x_milli > before_dpad_x);

    input.analog_x = 128;
    for (unsigned elapsed = 0; elapsed < 2100; elapsed += 20)
        intent = psp_ui_update(&ui, &input);
    CHECK(!ui.cursor_visible);

    ui.cursor_y_milli = 271000;
    input.analog_y = 255;
    intent = psp_ui_update(&ui, &input);
    CHECK(ui.cursor_visible && intent.scroll_delta > 0);

    input.pressed = PSP_UI_BUTTON_CONFIRM;
    input.held = PSP_UI_BUTTON_CONFIRM;
    input.analog_y = 128;
    intent = psp_ui_update(&ui, &input);
    CHECK(intent.pointer_phase == PSP_UI_POINTER_DOWN);
    input.pressed = PSP_UI_BUTTON_LEFT;
    intent = psp_ui_update(&ui, &input);
    CHECK(intent.pointer_phase == PSP_UI_POINTER_CANCEL
          && intent.action == PSP_UI_ACTION_FOCUS_AT
          && !ui.cursor_pointer_down && !ui.cursor_visible);
    input.pressed = 0;
    input.held = 0;
    input.analog_x = 255;
    intent = psp_ui_update(&ui, &input);
    CHECK(ui.cursor_visible);
    input.pressed = PSP_UI_BUTTON_CONFIRM;
    input.held = PSP_UI_BUTTON_CONFIRM;
    input.analog_x = 128;
    intent = psp_ui_update(&ui, &input);
    CHECK(intent.pointer_phase == PSP_UI_POINTER_DOWN);
    input.pressed = PSP_UI_BUTTON_MENU;
    intent = psp_ui_update(&ui, &input);
    CHECK(intent.pointer_phase == PSP_UI_POINTER_CANCEL
          && !ui.cursor_pointer_down
          && ui.screen == PSP_UI_SCREEN_MENU);

    ui.screen = PSP_UI_SCREEN_PAGE;
    input.pressed = 0;
    input.held = 0;
    input.analog_x = 255;
    intent = psp_ui_update(&ui, &input);
    CHECK(ui.cursor_visible);
    input.pressed = PSP_UI_BUTTON_CONFIRM;
    input.held = PSP_UI_BUTTON_CONFIRM;
    input.analog_x = 128;
    intent = psp_ui_update(&ui, &input);
    CHECK(intent.pointer_phase == PSP_UI_POINTER_DOWN
          && ui.cursor_pointer_down);
    psp_ui_suspend_page_input(&ui);
    CHECK(!ui.cursor_pointer_down);
    input.pressed = 0;
    input.held = 0;
    intent = psp_ui_update(&ui, &input);
    CHECK(intent.pointer_phase == PSP_UI_POINTER_NONE);
    return true;
}

static bool test_autohide_and_loading(void)
{
    PspUiState ui;
    psp_ui_init(&ui);
    PspUiInput idle = { .analog_x = 128, .analog_y = 128 };
    for (unsigned at = 0; at < 241; at++) (void) psp_ui_update(&ui, &idle);
    CHECK(!ui.chrome_visible);
    psp_ui_set_loading(&ui, true, -1);
    CHECK(ui.chrome_visible);
    psp_ui_set_loading(&ui, true, 420);
    psp_ui_set_loading(&ui, true, 310);
    CHECK(ui.progress_per_mille == 420);
    psp_ui_set_loading(&ui, true, 735);
    CHECK(ui.progress_per_mille == 735);
    for (unsigned at = 0; at < 300; at++) (void) psp_ui_update(&ui, &idle);
    CHECK(ui.chrome_visible);
    psp_ui_set_loading(&ui, false, 1000);
    return true;
}

static bool test_navigation_target_replaces_incumbent_address(void)
{
    PspUiState ui;
    psp_ui_init(&ui);
    psp_ui_set_page(
        &ui, "YouTube", "https://www.youtube.com/watch?v=old", true);
    psp_ui_set_navigation_target(
        &ui, "https://en.wikipedia.org/wiki/PlayStation_Portable");
    CHECK(strcmp(
              ui.url,
              "https://en.wikipedia.org/wiki/PlayStation_Portable") == 0
          && strcmp(ui.title, "Opening page") == 0
          && ui.secure);
    psp_ui_set_navigation_target(&ui, "http://example.test/plain");
    CHECK(strcmp(ui.url, "http://example.test/plain") == 0
          && !ui.secure);
    return true;
}

static bool test_clamping_and_bounded_state(void)
{
    PspUiState ui;
    psp_ui_init(&ui);
    char long_text[1024];
    memset(long_text, 'x', sizeof(long_text));
    long_text[sizeof(long_text) - 1] = '\0';
    psp_ui_set_page(&ui, long_text, long_text, true);
    CHECK(ui.title[sizeof(ui.title) - 1] == '\0');
    CHECK(ui.url[sizeof(ui.url) - 1] == '\0');
    psp_ui_set_scroll(&ui, 900, 100);
    CHECK(ui.scroll_y == 100 && ui.maximum_scroll_y == 100);
    psp_ui_set_focus(&ui, true, 120, 80, 0, 0);
    CHECK(ui.has_focus && ui.focus_width == 0 && ui.focus_height == 0);
    psp_ui_set_focus(&ui, true, 120, 80, -1, 0);
    CHECK(!ui.has_focus);
    CHECK(psp_ui_state_bytes() <= 1024);
    return true;
}

/*
 * Every overlay panel draws the same warm token ground, whatever accent the
 * theme selects. This is the regression guard for the chrome re-theme: a
 * panel that reintroduces its own colour (the pre-Ember cold navy, or a
 * per-theme tint) fails here rather than in a render nobody looked at.
 */
static bool test_panels_draw_the_token_ground(void)
{
    enum { WIDTH = 480, HEIGHT = 272 };
    static uint16_t frame[WIDTH * HEIGHT];
    static const PspUiScreen panels[] = {
        PSP_UI_SCREEN_MENU, PSP_UI_SCREEN_OPTIONS,
        PSP_UI_SCREEN_OPTION_ITEMS, PSP_UI_SCREEN_DATA_OPTIONS,
        PSP_UI_SCREEN_EXPERIMENTAL_OPTIONS, PSP_UI_SCREEN_GLYPH_OPTIONS,
        PSP_UI_SCREEN_TABS,
        PSP_UI_SCREEN_UPDATE
    };
    static const unsigned themes[] = {
        BROWSER_CHROME_THEME_FINCH, BROWSER_CHROME_THEME_OCEAN,
        BROWSER_CHROME_THEME_PLUM, BROWSER_CHROME_THEME_EMBER
    };
    for (size_t theme = 0; theme < sizeof(themes) / sizeof(themes[0]);
         theme++) {
        for (size_t at = 0; at < sizeof(panels) / sizeof(panels[0]); at++) {
            PspUiState ui;
            psp_ui_init(&ui);
            psp_ui_set_page(
                &ui, "Wikipedia", "https://en.wikipedia.org/", true);
            ui.chrome_theme = (uint8_t) themes[theme];
            ui.screen = panels[at];
            ui.toast_frames = 0;
            ui.overlay_animation_frames = 0;
            for (size_t pixel = 0; pixel < WIDTH * HEIGHT; pixel++)
                frame[pixel] = 0xffffu;
            psp_ui_composite(&ui, frame, WIDTH, HEIGHT, WIDTH);
            /*
             * Counted, not sampled: the panels do not share a footer y or a
             * clear interior pixel, only the tokens they are made of. A
             * panel that brings back its own ground (the cold navy, or a
             * per-theme tint) collapses these counts.
             */
            size_t ground = 0, hint = 0;
            for (size_t pixel = 0; pixel < WIDTH * HEIGHT; pixel++) {
                if (frame[pixel] == PSP_THEME_PANEL) ground++;
                else if (frame[pixel] == PSP_THEME_HINT_BAR) hint++;
            }
            CHECK(ground > 10000u);
            CHECK(hint > 1000u);
        }
    }
    return true;
}

/*
 * The page-view chrome over a light page. This is the pairing the user
 * actually stares at, and the one the cold-era bar was tuned for, so the
 * grounds and the bar's own edge are pinned here rather than left to a
 * render nobody re-runs.
 */
static bool test_page_chrome_holds_its_edge_over_a_light_page(void)
{
    enum { WIDTH = 480, HEIGHT = 272 };
    static uint16_t frame[WIDTH * HEIGHT];
    for (size_t pixel = 0; pixel < WIDTH * HEIGHT; pixel++)
        frame[pixel] = 0xffffu;

    PspUiState ui;
    psp_ui_init(&ui);
    psp_ui_set_page(
        &ui, "PlayStation Portable",
        "https://en.wikipedia.org/wiki/PlayStation_Portable", true);
    psp_ui_set_scroll(&ui, 200, 2000);
    ui.toast_frames = 0;
    psp_ui_composite(&ui, frame, WIDTH, HEIGHT, WIDTH);

    /* Top bar ground, and the hairline that keeps it off the page. */
    CHECK(frame[(size_t) 2 * WIDTH + 400u] == PSP_THEME_CHROME_BAR);
    CHECK(frame[(size_t) 38 * WIDTH + 400u] == PSP_THEME_LINE);
    CHECK(frame[(size_t) 39 * WIDTH + 400u] == 0xffffu);
    /* The URL pill is a warm raised surface, not the old light field. */
    CHECK(frame[(size_t) 18 * WIDTH + 300u] == PSP_THEME_SURFACE);
    /* Bottom legend bar, and its own content edge. */
    CHECK(frame[(size_t) 255 * WIDTH + 400u] == PSP_THEME_HINT_BAR);
    CHECK(frame[(size_t) 251 * WIDTH + 400u] == PSP_THEME_LINE);
    CHECK(frame[(size_t) 250 * WIDTH + 400u] == 0xffffu);

    /* Loading paints the progress band under the bar's edge; the band is a
       ramp, so it is not one flat colour across its run. */
    psp_ui_set_loading(&ui, true, 900);
    ui.loading_phase = 46u;
    psp_ui_composite(&ui, frame, WIDTH, HEIGHT, WIDTH);
    bool varied = false;
    uint16_t first = frame[(size_t) 41 * WIDTH + 20u];
    for (int x = 20; x < 400; x++) {
        if (frame[(size_t) 41 * WIDTH + (size_t) x] != first) varied = true;
    }
    CHECK(varied);
    return true;
}

/*
 * The boot entrance. What matters here is that it is a pure function of the
 * frame index (so the frontend can present whatever it reached), that its
 * last frame IS the settled HOME rather than something that resembles it,
 * and that the slow branch is exactly one held mark and one line.
 */
static bool test_boot_entrance_becomes_home(void)
{
    enum { WIDTH = 480, HEIGHT = 272, GUARD = 32 };
    static uint16_t guarded[GUARD + WIDTH * HEIGHT + GUARD];
    static uint16_t settled[WIDTH * HEIGHT];
    static uint16_t entrance[WIDTH * HEIGHT];
    static uint16_t held[WIDTH * HEIGHT];
    for (size_t at = 0; at < sizeof(guarded) / sizeof(guarded[0]); at++)
        guarded[at] = 0x5aa5u;
    uint16_t *frame = guarded + GUARD;

    PspUiHomeView home_view = {
        .tile_count = 2,
        .continue_count = 1,
        .engine_ready = true,
        .tiles = {
            {"WIKIPEDIA", "Articles and search"},
            {"YOUTUBE", "Browse and play video"}
        },
        .continues = {{"PlayStation Portable", "en.wikipedia.org"}}
    };
    PspUiState ui;
    psp_ui_init(&ui);
    psp_ui_set_home(&ui, &home_view);
    psp_ui_show_home(&ui);
    psp_ui_set_device_status(14, 8, 62, false, false, -1);

    /* The last entrance frame and the ordinary HOME composite are the same
       pixels: the entrance hands over rather than dissolving into. */
    psp_ui_composite(&ui, settled, WIDTH, HEIGHT, WIDTH);
    PspUiBootEntranceView view = {
        .frame = PSP_UI_BOOT_ENTRANCE_FRAMES,
        .wave = true,
        .branch_status = NULL
    };
    psp_ui_boot_entrance_composite(
        &view, &ui, entrance, WIDTH, HEIGHT, WIDTH);
    CHECK(memcmp(settled, entrance, sizeof(settled)) == 0);

    /* Frame one is the mark and the safe-start line on the ground, and
       nothing else: no tile has arrived and no bar has been drawn. */
    view.frame = 0u;
    psp_ui_boot_entrance_composite(
        &view, &ui, frame, WIDTH, HEIGHT, WIDTH);
    for (size_t at = 0; at < GUARD; at++) {
        CHECK(guarded[at] == 0x5aa5u);
        CHECK(guarded[GUARD + WIDTH * HEIGHT + at] == 0x5aa5u);
    }
    /* The status line's own row is bare ground, not a bar. */
    CHECK(frame[(size_t) 8 * WIDTH + 240u] == PSP_THEME_GROUND);
    CHECK(frame[(size_t) 60 * WIDTH + 240u] == PSP_THEME_GROUND);
    /* The mark is there, in the accent. */
    CHECK(frame[(size_t) 124 * WIDTH + 240u] != PSP_THEME_GROUND);
    memcpy(entrance, frame, sizeof(entrance));

    /* Every frame is a pure function of its index: re-presenting frame one
       reproduces frame one exactly, whatever came in between. */
    view.frame = 14u;
    psp_ui_boot_entrance_composite(
        &view, &ui, frame, WIDTH, HEIGHT, WIDTH);
    CHECK(memcmp(entrance, frame, sizeof(entrance)) != 0);
    view.frame = 0u;
    psp_ui_boot_entrance_composite(
        &view, &ui, frame, WIDTH, HEIGHT, WIDTH);
    CHECK(memcmp(entrance, frame, sizeof(entrance)) == 0);

    /* The legacy wave preference is deliberately inert. Boot output is
       identical for both values until a GPU-owned implementation exists. */
    view.frame = 8u;
    view.wave = true;
    psp_ui_boot_entrance_composite(
        &view, &ui, frame, WIDTH, HEIGHT, WIDTH);
    view.wave = false;
    psp_ui_boot_entrance_composite(
        &view, &ui, held, WIDTH, HEIGHT, WIDTH);
    CHECK(memcmp(frame, held, sizeof(held)) == 0);
    /* At this early frame the static entrance is still the mark alone. */
    CHECK(memcmp(entrance, held, sizeof(held)) == 0);

    /* The slow branch holds the mark wherever the choreography had got to,
       and adds one line. Nothing else moves. */
    view.frame = 16u;
    view.wave = true;
    view.branch_status = "READING SETTINGS";
    psp_ui_boot_entrance_composite(
        &view, &ui, held, WIDTH, HEIGHT, WIDTH);
    view.frame = 3u;
    psp_ui_boot_entrance_composite(
        &view, &ui, frame, WIDTH, HEIGHT, WIDTH);
    CHECK(memcmp(frame, held, sizeof(held)) == 0);
    /* Held is frame one plus exactly the status line's rows. */
    size_t differing_rows = 0;
    for (int y = 0; y < HEIGHT; y++) {
        if (memcmp(&entrance[(size_t) y * WIDTH],
                   &held[(size_t) y * WIDTH],
                   WIDTH * sizeof(uint16_t)) != 0) differing_rows++;
    }
    CHECK(differing_rows > 0 && differing_rows <= 10);
    return true;
}

/*
 * Cursor movement must be fully visible in the sampled frame. Only departure
 * uses the focus-settle fade; fading the first movement made device input
 * feel one frame behind even though the event was already acknowledged.
 */
static bool test_cursor_fades_in_and_out(void)
{
    PspUiState ui;
    psp_ui_init(&ui);
    ui.analog_cursor_enabled = true;
    CHECK(ui.cursor_fade == 0u && !ui.cursor_visible);

    PspUiInput input = {.analog_x = 255, .analog_y = 128, .elapsed_ms = 16};
    (void) psp_ui_update(&ui, &input);
    CHECK(ui.cursor_visible
          && ui.cursor_fade == PSP_THEME_MOTION_FOCUS_FRAMES);

    /* The d-pad takes the cursor away at once as far as input is concerned;
       only the pixels are allowed to take their time. */
    PspUiInput dpad = {
        .analog_x = 128, .analog_y = 128, .elapsed_ms = 16,
        .pressed = PSP_UI_BUTTON_DOWN
    };
    (void) psp_ui_update(&ui, &dpad);
    CHECK(!ui.cursor_visible);
    CHECK(ui.cursor_fade != 0u);
    PspUiInput idle = {.analog_x = 128, .analog_y = 128, .elapsed_ms = 16};
    for (int at = 0; at < PSP_THEME_MOTION_FOCUS_FRAMES; at++)
        (void) psp_ui_update(&ui, &idle);
    CHECK(ui.cursor_fade == 0u);

    /* And the fade is visible: a half-faded cursor is not the same pixels
       as a settled one. */
    enum { WIDTH = 480, HEIGHT = 272 };
    static uint16_t faint[WIDTH * HEIGHT];
    static uint16_t full[WIDTH * HEIGHT];
    psp_ui_set_page(&ui, "Wikipedia", "https://en.wikipedia.org/", true);
    ui.toast_frames = 0;
    ui.cursor_visible = true;
    ui.cursor_fade = 1u;
    psp_ui_composite(&ui, faint, WIDTH, HEIGHT, WIDTH);
    ui.cursor_fade = PSP_THEME_MOTION_FOCUS_FRAMES;
    psp_ui_composite(&ui, full, WIDTH, HEIGHT, WIDTH);
    CHECK(memcmp(faint, full, sizeof(full)) != 0);
    /* The default cursor is a true cross: both perpendicular centre arms
       are accent pixels in one freshly composed frame. */
    int cursor_x = ui.cursor_x_milli / 1000;
    int cursor_y = ui.cursor_y_milli / 1000;
    uint16_t vertical_arm = full[
        (size_t) (cursor_y - 5) * WIDTH + (size_t) cursor_x];
    uint16_t horizontal_arm = full[
        (size_t) cursor_y * WIDTH + (size_t) (cursor_x - 5)];
    uint16_t intersection = full[
        (size_t) cursor_y * WIDTH + (size_t) cursor_x];
    CHECK(vertical_arm != 0 && vertical_arm == horizontal_arm
          && intersection == vertical_arm);
    return true;
}

static bool test_composite_keeps_guards(void)
{
    enum { WIDTH = 480, HEIGHT = 272, GUARD = 32 };
    static uint16_t guarded[GUARD + WIDTH * HEIGHT + GUARD];
    for (size_t at = 0; at < sizeof(guarded) / sizeof(guarded[0]); at++) {
        guarded[at] = 0x5aa5u;
    }
    uint16_t *frame = guarded + GUARD;
    for (size_t at = 0; at < WIDTH * HEIGHT; at++) frame[at] = 0xffffu;

    PspUiState ui;
    psp_ui_init(&ui);
    psp_ui_set_page(&ui, "Wikipedia", "https://en.wikipedia.org/", true);
    psp_ui_set_scroll(&ui, 200, 2000);
    psp_ui_set_focus(&ui, true, -10, 265, 90, 24);
    ui.screen = PSP_UI_SCREEN_MENU;
    psp_ui_composite(&ui, frame, WIDTH, HEIGHT, WIDTH);
    ui.screen = PSP_UI_SCREEN_OPTIONS;
    ui.browser_ui_scale = 2;
    psp_ui_composite(&ui, frame, WIDTH, HEIGHT, WIDTH);
    ui.screen = PSP_UI_SCREEN_DATA_OPTIONS;
    ui.live_cache_kib = 4096;
    ui.persistent_cache_mb = 4;
    ui.persist_local_storage = true;
    psp_ui_composite(&ui, frame, WIDTH, HEIGHT, WIDTH);
    PspUiTabsView tabs = {
        .count = 5,
        .active_index = 1
    };
    snprintf(tabs.titles[0], sizeof(tabs.titles[0]), "Wikipedia");
    snprintf(tabs.titles[1], sizeof(tabs.titles[1]), "YouTube");
    snprintf(tabs.titles[2], sizeof(tabs.titles[2]), "Hacker News");
    snprintf(tabs.titles[3], sizeof(tabs.titles[3]), "Reddit");
    snprintf(tabs.titles[4], sizeof(tabs.titles[4]), "NYTimes");
    psp_ui_set_tabs(&ui, &tabs);
    ui.screen = PSP_UI_SCREEN_TABS;
    ui.tab_selection = 4;
    psp_ui_composite(&ui, frame, WIDTH, HEIGHT, WIDTH);
    BrowserProfileSuggestion suggestion = {
        .url = "https://en.wikipedia.org/",
        .title = "Wikipedia",
        .bookmark = true
    };
    PspUiTextEntryView text_entry = {
        .description = "SEARCH OR ADDRESS",
        .text = "wiki",
        .suggestions = &suggestion,
        .cursor = 4,
        .suggestion_count = 1,
        .suggestion_selection = 0,
        .cell = 8,
        .shifted = true
    };
    psp_ui_set_text_entry(&ui, &text_entry);
    psp_ui_composite(&ui, frame, WIDTH, HEIGHT, WIDTH);
    psp_ui_clear_text_entry(&ui);
    PspUiFindView find_view = {
        .query = "portable",
        .match_count = 256,
        .selected = 14,
        .truncated = true
    };
    psp_ui_set_find(&ui, &find_view);
    psp_ui_composite(&ui, frame, WIDTH, HEIGHT, WIDTH);
    psp_ui_clear_find(&ui);
    ui.screen = PSP_UI_SCREEN_PAGE;
    ui.cursor_visible = true;
    ui.cursor_x_milli = 240000;
    ui.cursor_y_milli = 136000;
    psp_ui_composite(&ui, frame, WIDTH, HEIGHT, WIDTH);

    for (size_t at = 0; at < GUARD; at++) {
        CHECK(guarded[at] == 0x5aa5u);
        CHECK(guarded[GUARD + WIDTH * HEIGHT + at] == 0x5aa5u);
    }
    CHECK(frame[0] != 0xffffu);
    CHECK(frame[(HEIGHT - 1) * WIDTH] != 0xffffu);
    return true;
}

static bool test_large_toast_uses_full_psp_width(void)
{
    enum { WIDTH = 480, HEIGHT = 272 };
    static uint16_t thirty_four[WIDTH * HEIGHT];
    static uint16_t thirty_five[WIDTH * HEIGHT];
    PspUiState ui;
    psp_ui_init(&ui);
    ui.browser_ui_scale = 2;
    psp_ui_show_status(&ui, "1234567890123456789012345678901234", 60);
    ui.toast_entry_frames = 0;
    psp_ui_composite(&ui, thirty_four, WIDTH, HEIGHT, WIDTH);

    psp_ui_show_status(&ui, "1234567890123456789012345678901234X", 60);
    ui.toast_entry_frames = 0;
    psp_ui_composite(&ui, thirty_five, WIDTH, HEIGHT, WIDTH);

    /* The former fixed 34-character cap made these frames identical and
       clipped the final letter of the network-cancellation hint. */
    CHECK(memcmp(thirty_four, thirty_five, sizeof(thirty_four)) != 0);
    return true;
}

static bool test_options_hide_page_loading_indicator(void)
{
    enum { WIDTH = 480, HEIGHT = 272 };
    static uint16_t idle[WIDTH * HEIGHT];
    static uint16_t loading[WIDTH * HEIGHT];
    for (size_t at = 0; at < WIDTH * HEIGHT; at++) {
        idle[at] = 0x7befu;
        loading[at] = 0x7befu;
    }
    PspUiState ui;
    psp_ui_init(&ui);
    ui.screen = PSP_UI_SCREEN_OPTIONS;
    ui.overlay_animation_frames = 0;
    psp_ui_composite(&ui, idle, WIDTH, HEIGHT, WIDTH);
    psp_ui_set_loading(&ui, true, 650);
    ui.screen = PSP_UI_SCREEN_OPTIONS;
    ui.overlay_animation_frames = 0;
    psp_ui_composite(&ui, loading, WIDTH, HEIGHT, WIDTH);
    CHECK(memcmp(idle + 38u * WIDTH, loading + 38u * WIDTH,
                 (HEIGHT - 38u) * WIDTH * sizeof(*idle)) == 0);
    return true;
}

static bool test_chrome_normalizes_common_unicode_punctuation(void)
{
    enum { WIDTH = 480, HEIGHT = 272 };
    static uint16_t unicode_frame[WIDTH * HEIGHT];
    static uint16_t ascii_frame[WIDTH * HEIGHT];
    PspUiState ui;
    psp_ui_init(&ui);
    psp_ui_clear_chrome_font();
    /*
     * The address bar shows the path when there is one and the title only
     * when there is not, so a URL carrying "/wiki/Animal" made this compare
     * two frames the title never reached. Keep the URL bare.
     */
    psp_ui_set_page(
        &ui, "Animal \xe2\x80\x93 Wikipedia",
        "https://en.wikipedia.org", true);
    psp_ui_composite(
        &ui, unicode_frame, WIDTH, HEIGHT, WIDTH);
    psp_ui_set_page(
        &ui, "Animal - Wikipedia",
        "https://en.wikipedia.org", true);
    psp_ui_composite(
        &ui, ascii_frame, WIDTH, HEIGHT, WIDTH);
    CHECK(memcmp(unicode_frame, ascii_frame, sizeof(unicode_frame)) == 0);
    psp_ui_set_page(
        &ui, "Animal * Wikipedia", "https://en.wikipedia.org", true);
    psp_ui_composite(&ui, unicode_frame, WIDTH, HEIGHT, WIDTH);
    CHECK(memcmp(unicode_frame, ascii_frame, sizeof(unicode_frame)) != 0);
    return true;
}

static bool test_chrome_retains_bounded_unicode_glyphs(void)
{
    enum { WIDTH = 480, HEIGHT = 272 };
    static uint16_t fallback[WIDTH * HEIGHT];
    static uint16_t proportional[WIDTH * HEIGHT];
    PspUiState ui;
    psp_ui_init(&ui);
    psp_ui_clear_chrome_font();
    psp_ui_set_page(
        &ui, "Caf\xc3\xa9 \xea\xb2\x8c\xec\x9e\x84 - Wikipedia",
        "https://en.wikipedia.org/wiki/Cafe", true);
    psp_ui_composite(&ui, fallback, WIDTH, HEIGHT, WIDTH);

    Budget budget;
    budget_init(&budget, 2u * 1024u * 1024u);
    FontSet fonts;
    CHECK(font_set_load(
        &fonts, &budget, TILEFINCH_TEST_SANS_FONT, NULL, NULL, NULL, NULL,
        NULL, NULL, 1024u * 1024u));
    psp_ui_set_chrome_fonts(font_set_face(&fonts, FONT_SANS), NULL);
    psp_ui_composite(&ui, proportional, WIDTH, HEIGHT, WIDTH);
    CHECK(memcmp(fallback, proportional, sizeof(fallback)) != 0);
    psp_ui_clear_chrome_font();
    font_set_destroy(&fonts);
    CHECK(budget.current == 0 && budget_categories_reconcile(&budget));
    return true;
}

/*
 * The chrome's own glyph vocabulary, checked against the faces the device
 * actually ships.
 *
 * chrome_font_glyph() is reached from exactly three places in src/psp_ui.c:
 * ui_ellipsis_advance() asks for '.', and draw_text_with_font() and
 * chrome_text_width_bytes() ask for every codepoint of whatever string the
 * chrome is drawing. Every string literal in src/psp_ui.c, the src/psp_app
 * sources, src/psp_script_main.c and src/danzeff_input.c is ASCII, so the
 * fixed part
 * of that vocabulary is the retained cache range U+0020..U+007E; the variable
 * part is page-derived text, for which draw_text_with_font() names a table of
 * Unicode punctuation it maps to ASCII and falls back to a drawn tile
 * otherwise.
 *
 * This is deliberately loaded from fonts/DejaVuSans-Latin.ttf and its bold
 * sibling -- the subset staged beside the EBOOT -- and not from the full
 * upstream DejaVuSans.ttf that TILEFINCH_TEST_SANS_FONT points at. The
 * previous covering test used the full face and so certified U+25BA, a
 * codepoint no device has ever had.
 */
static bool test_chrome_vocabulary_resolves_in_the_shipped_subset(void)
{
    Budget budget;
    budget_init(&budget, 4u * 1024u * 1024u);
    FontSet fonts;
    CHECK(font_set_load(
        &fonts, &budget, TILEFINCH_TEST_PSP_SANS_FONT, NULL, NULL,
        TILEFINCH_TEST_PSP_SANS_BOLD_FONT, NULL, NULL, NULL,
        2u * 1024u * 1024u));
    const FontFace *faces[2] = {
        font_set_face(&fonts, FONT_SANS),
        font_set_face_variant(&fonts, FONT_SANS, false, true)
    };
    CHECK(faces[0] != NULL && faces[1] != NULL && faces[0] != faces[1]);

    /* Both chrome sizes: 11px at scale 1, 15px at scale 2. */
    static const int heights[2] = {11, 15};
    for (unsigned codepoint = 32u; codepoint <= 126u; codepoint++) {
        for (size_t weight = 0; weight < 2u; weight++) {
            CHECK(font_face_has_codepoint(faces[weight], codepoint));
            for (size_t size = 0; size < 2u; size++) {
                FontGlyph glyph = {0};
                CHECK(font_glyph_load(faces[weight], codepoint,
                                      heights[size], false, &glyph));
                CHECK(glyph.advance > 0);
                CHECK(codepoint == ' ' || glyph.width > 0);
                font_glyph_destroy(faces[weight], &glyph);
            }
        }
    }

    /* The punctuation draw_text_with_font() names, minus U+2212 below. The
       subset carries all of it, so the chrome draws the real character and
       never reaches the ASCII substitute. */
    static const unsigned punctuation[] = {
        0x00a0u, 0x00b7u, 0x2010u, 0x2011u, 0x2012u, 0x2013u, 0x2014u,
        0x2018u, 0x2019u, 0x201cu, 0x201du, 0x2022u, 0x2026u
    };
    for (size_t at = 0;
         at < sizeof(punctuation) / sizeof(punctuation[0]); at++) {
        for (size_t weight = 0; weight < 2u; weight++) {
            CHECK(font_face_has_codepoint(faces[weight], punctuation[at]));
        }
    }

    /* U+2212 MINUS SIGN is the one entry in that table the subset does not
       carry, which is why the table maps it to '-'. U+25BA is the pointer
       the media overlay used to typeset before f8140a5 drew it instead.
       Pinning both as absent keeps this test honest about what the subset
       is, so that widening the subset is a deliberate edit here too. */
    for (size_t weight = 0; weight < 2u; weight++) {
        CHECK(!font_face_has_codepoint(faces[weight], 0x2212u));
        CHECK(!font_face_has_codepoint(faces[weight], 0x25bau));
        CHECK(font_face_has_codepoint(faces[weight], '-'));
    }

    /*
     * And why coverage has to be asked rather than inferred: three unrelated
     * absent codepoints load successfully and produce the identical glyph,
     * because all three are .notdef. In the bold subset that shape is a box,
     * so a missing character reached the panel as tofu; in the regular subset
     * it is empty, so it reached the panel as a blank cell with a real
     * advance. A load result can never tell those from a hit.
     */
    for (size_t weight = 0; weight < 2u; weight++) {
        FontGlyph absent[3] = {{0}, {0}, {0}};
        static const unsigned probes[3] = {0x25bau, 0x25c0u, 0x2212u};
        for (size_t at = 0; at < 3u; at++) {
            CHECK(font_glyph_load(faces[weight], probes[at], 11, false,
                                  &absent[at]));
        }
        for (size_t at = 1; at < 3u; at++) {
            CHECK(absent[at].width == absent[0].width
                  && absent[at].height == absent[0].height
                  && absent[at].advance == absent[0].advance);
            size_t bytes = (size_t) absent[0].width
                           * (size_t) absent[0].height;
            CHECK(bytes == 0
                  || memcmp(absent[at].pixels, absent[0].pixels, bytes) == 0);
        }
        CHECK(absent[0].advance > 0);
        /* Regular's .notdef is empty; bold's is a drawn box. */
        CHECK(weight == 0 ? absent[0].width == 0 : absent[0].width > 0);
        for (size_t at = 0; at < 3u; at++) {
            font_glyph_destroy(faces[weight], &absent[at]);
        }
    }

    /*
     * Which is what the chrome now asks. With the shipped faces installed, a
     * title carrying U+2212 no longer composites the same as one carrying
     * U+25BA: the first takes the '-' substitution, the second the drawn
     * fallback tile. Before coverage was queried, both were .notdef and the
     * two frames were byte-identical.
     */
    enum { WIDTH = 480, HEIGHT = 272 };
    static uint16_t minus_frame[WIDTH * HEIGHT];
    static uint16_t pointer_frame[WIDTH * HEIGHT];
    PspUiState ui;
    psp_ui_init(&ui);
    psp_ui_clear_chrome_font();
    psp_ui_set_chrome_fonts(faces[0], faces[1]);
    psp_ui_set_page(&ui, "Animal \xe2\x88\x92 Wikipedia",
                    "https://en.wikipedia.org", true);
    psp_ui_composite(&ui, minus_frame, WIDTH, HEIGHT, WIDTH);
    psp_ui_set_page(&ui, "Animal \xe2\x96\xba Wikipedia",
                    "https://en.wikipedia.org", true);
    psp_ui_composite(&ui, pointer_frame, WIDTH, HEIGHT, WIDTH);
    CHECK(memcmp(minus_frame, pointer_frame, sizeof(minus_frame)) != 0);
    psp_ui_clear_chrome_font();

    font_set_destroy(&fonts);
    CHECK(budget.current == 0 && budget_categories_reconcile(&budget));
    return true;
}

static bool test_media_controls_and_composite(void)
{
    CHECK(psp_ui_ratio_extent_u64(0, 100, 432) == 0);
    CHECK(psp_ui_ratio_extent_u64(50, 100, 432) == 216);
    CHECK(psp_ui_ratio_extent_u64(100, 100, 432) == 432);
    CHECK(psp_ui_ratio_extent_u64(
              UINT64_C(50000000000), UINT64_C(100000000000), 432)
          >= 215
          && psp_ui_ratio_extent_u64(
                 UINT64_C(50000000000), UINT64_C(100000000000), 432)
                 <= 216);
    enum { WIDTH = 480, HEIGHT = 272 };
    static uint16_t frame[WIDTH * HEIGHT];
    memset(frame, 0, sizeof(frame));
    PspUiMediaState media;
    psp_ui_media_init(&media);
    psp_ui_media_set(&media, true, false, false,
                     UINT64_C(5000000), UINT64_C(20000000),
                     "Example video");
    CHECK(psp_ui_media_state_bytes() <= 288);
    psp_ui_media_set_buffering(
        &media, true, UINT64_C(9000000));
    CHECK(media.buffering
          && media.buffered_until_us == UINT64_C(9000000));

    PspUiInput input = {
        .pressed = PSP_UI_BUTTON_RIGHT,
        .analog_x = 128,
        .analog_y = 128
    };
    PspUiMediaIntent intent = psp_ui_media_update(&media, &input);
    CHECK(intent.action == PSP_UI_MEDIA_ACTION_PREVIEW_SEEK);
    CHECK(intent.seek_time_us == UINT64_C(15000000));
    CHECK(media.seek_preview_active
          && media.seek_preview_time_us == UINT64_C(15000000));
    intent = psp_ui_media_update(&media, &input);
    CHECK(intent.action == PSP_UI_MEDIA_ACTION_PREVIEW_SEEK);
    CHECK(intent.seek_time_us == UINT64_C(20000000));

    /* Seeking is a state-machine service, not a reason to deaden the D-pad.
       The UI moves immediately; the session coalesces physical decode work. */
    PspMediaUiProjection seeking = {
        .mode = PSP_MEDIA_UI_SEEKING,
        .visible = true,
        .controls_enabled = true,
        .play_pause_enabled = true,
        .seek_enabled = true,
        .show_progress = true,
        .preview_active = true
    };
    psp_ui_media_apply_projection(&media, &seeking);
    input.pressed = PSP_UI_BUTTON_LEFT;
    intent = psp_ui_media_update(&media, &input);
    CHECK(intent.action == PSP_UI_MEDIA_ACTION_PREVIEW_SEEK);
    CHECK(intent.seek_time_us == UINT64_C(10000000));
    input.pressed = PSP_UI_BUTTON_CONFIRM;
    intent = psp_ui_media_update(&media, &input);
    CHECK(intent.action == PSP_UI_MEDIA_ACTION_SEEK
          && intent.seek_time_us == UINT64_C(10000000)
          && media.seek_preview_active);

    /* Priming declares Play/Pause available even though it also projects a
       loading surface. The old resolving blanket gate discarded this. */
    PspMediaUiProjection priming = {
        .mode = PSP_MEDIA_UI_PRIMING,
        .visible = true,
        .controls_enabled = true,
        .play_pause_enabled = true,
        .show_progress = true
    };
    psp_ui_media_apply_projection(&media, &priming);
    psp_ui_media_cancel_seek_preview(&media);
    input.pressed = PSP_UI_BUTTON_CONFIRM;
    intent = psp_ui_media_update(&media, &input);
    CHECK(intent.action == PSP_UI_MEDIA_ACTION_PLAY_PAUSE
          && !psp_ui_media_intent_has_predispatch_visual(&intent));

    /* The same rule applies in the other direction. The UI update only makes
       controls visible; Playing becomes Paused when the session consumes the
       intent, so presenting here would expose the stale pause legend. */
    psp_ui_media_set(&media, true, true, false,
                     UINT64_C(5000000), UINT64_C(20000000),
                     "Example video");
    input.pressed = PSP_UI_BUTTON_CONFIRM;
    intent = psp_ui_media_update(&media, &input);
    CHECK(intent.action == PSP_UI_MEDIA_ACTION_PLAY_PAUSE
          && !psp_ui_media_intent_has_predispatch_visual(&intent));

    psp_ui_media_set(&media, true, false, false,
                     UINT64_C(5000000), UINT64_C(20000000),
                     "Example video");
    psp_ui_media_set_seek_preview(&media, UINT64_C(20000000));
    input.pressed = PSP_UI_BUTTON_CONFIRM;
    intent = psp_ui_media_update(&media, &input);
    CHECK(intent.action == PSP_UI_MEDIA_ACTION_SEEK);
    CHECK(intent.seek_time_us == UINT64_C(20000000)
          && !media.seek_preview_active);

    intent = psp_ui_media_activate_at(&media, WIDTH / 2, HEIGHT - 69,
                                      WIDTH, HEIGHT);
    CHECK(intent.action == PSP_UI_MEDIA_ACTION_PREVIEW_SEEK);
    CHECK(intent.seek_time_us > UINT64_C(9000000)
          && intent.seek_time_us < UINT64_C(11000000));
    media.duration_us = UINT64_MAX;
    intent = psp_ui_media_activate_at(&media, WIDTH / 2, HEIGHT - 69,
                                      WIDTH, HEIGHT);
    CHECK(intent.action == PSP_UI_MEDIA_ACTION_PREVIEW_SEEK
          && intent.seek_time_us > UINT64_MAX / 2u - UINT64_C(50000000)
          && intent.seek_time_us < UINT64_MAX / 2u + UINT64_C(50000000));
    media.duration_us = UINT64_C(20000000);
    psp_ui_media_set_seek_preview(&media, UINT64_C(10000000));
    static uint16_t preview[128 * 72];
    for (size_t i = 0; i < sizeof(preview) / sizeof(preview[0]); i++)
        preview[i] = 0x07e0u;
    PspUiMediaPreview preview_view = {
        .pixels = preview, .width = 128, .height = 72, .stride = 128
    };
    psp_ui_media_composite_with_preview(
        &media, &preview_view, frame, WIDTH, HEIGHT, WIDTH);
    CHECK(frame[14 * WIDTH + 14] != 0);
    CHECK(frame[(HEIGHT - 69) * WIDTH + WIDTH / 2] != 0);
    CHECK(frame[60 * WIDTH + 30] == 0x07e0u);
    input.pressed = PSP_UI_BUTTON_CANCEL;
    intent = psp_ui_media_update(&media, &input);
    CHECK(intent.action == PSP_UI_MEDIA_ACTION_CANCEL_SEEK_PREVIEW
          && !media.seek_preview_active);

    psp_ui_media_set(&media, true, false, false,
                     UINT64_C(60000000), UINT64_C(120000000), NULL);
    input = (PspUiInput) {
        .analog_x = 255, .analog_y = 128, .elapsed_ms = 20
    };
    for (unsigned elapsed = 0; elapsed < 1000; elapsed += 20) {
        intent = psp_ui_media_update(&media, &input);
        CHECK(intent.action == PSP_UI_MEDIA_ACTION_NONE
              && intent.visual_changed
              && !psp_ui_media_intent_has_predispatch_visual(&intent));
    }
    CHECK(media.seek_preview_active
          && media.seek_preview_time_us > UINT64_C(89500000)
          && media.seek_preview_time_us < UINT64_C(90500000));
    /* The nub owns its local preview until it is released. An unrelated
       controller projection (for example a refill edge) must not erase it
       for one frame and make the timeline alternate between two positions. */
    PspMediaUiProjection playing_projection = {
        .mode = PSP_MEDIA_UI_PLAYING,
        .visible = true,
        .controls_enabled = true,
        .play_pause_enabled = true,
        .seek_enabled = true,
        .playing = true
    };
    psp_ui_media_apply_projection(&media, &playing_projection);
    CHECK(media.seek_preview_active
          && media.analog_seek_direction == 1);
    PspUiMediaState faster_cadence;
    psp_ui_media_init(&faster_cadence);
    psp_ui_media_set(
        &faster_cadence, true, false, false,
        UINT64_C(60000000), UINT64_C(120000000), NULL);
    PspUiInput faster_input = {
        .analog_x = 255, .analog_y = 128, .elapsed_ms = 10
    };
    for (unsigned elapsed = 0; elapsed < 1000; elapsed += 10)
        (void) psp_ui_media_update(&faster_cadence, &faster_input);
    CHECK(faster_cadence.seek_preview_time_us
          == media.seek_preview_time_us);
    input.analog_x = 128;
    intent = psp_ui_media_update(&media, &input);
    CHECK(intent.action == PSP_UI_MEDIA_ACTION_PREVIEW_SEEK
          && intent.seek_time_us == media.seek_preview_time_us
          && media.analog_seek_direction == 0);
    input.pressed = PSP_UI_BUTTON_CONFIRM;
    intent = psp_ui_media_update(&media, &input);
    CHECK(intent.action == PSP_UI_MEDIA_ACTION_SEEK
          && !media.seek_preview_active);

    /* A short refill is reported by the centre pill. It must not replace the
       bottom legend as well: toggling that whole text row after resume made
       an otherwise-stable timeline band appear to flash. */
    enum { CONTROL_HEIGHT = 78 };
    static uint16_t stable_controls[WIDTH * CONTROL_HEIGHT];
    memset(frame, 0, sizeof(frame));
    psp_ui_media_set(&media, true, true, false,
                     UINT64_C(5000000), UINT64_C(20000000), NULL);
    psp_ui_media_set_buffering(&media, false, UINT64_C(9000000));
    psp_ui_media_composite(&media, frame, WIDTH, HEIGHT, WIDTH);
    memcpy(stable_controls, frame + (HEIGHT - CONTROL_HEIGHT) * WIDTH,
           sizeof(stable_controls));
    memset(frame, 0, sizeof(frame));
    psp_ui_media_set_buffering(&media, true, UINT64_C(9000000));
    psp_ui_media_composite(&media, frame, WIDTH, HEIGHT, WIDTH);
    CHECK(memcmp(stable_controls,
                 frame + (HEIGHT - CONTROL_HEIGHT) * WIDTH,
                 sizeof(stable_controls)) == 0);
    CHECK(frame[(HEIGHT - 10) * WIDTH] == PSP_THEME_HINT_BAR);

    psp_ui_media_set(&media, true, true, false,
                     UINT64_C(5000000), UINT64_C(20000000), NULL);
    psp_ui_media_tick(&media, 3000);
    CHECK(!media.controls_visible);
    psp_ui_media_set_seek_preview(&media, UINT64_C(7000000));
    psp_ui_media_set_buffering(&media, true, UINT64_C(9000000));
    psp_ui_media_set_resolving(&media, "Loading title");
    CHECK(media.visible && media.resolving && !media.failed
          && !media.buffering && !media.seek_preview_active
          && media.analog_seek_direction == 0
          && media.buffered_until_us == 0);
    psp_ui_media_set_resolving_progress(
        &media, "Retrying at 240p", 400u);
    CHECK(media.resolving_progress_per_mille == 400u
          && strcmp(media.status, "Retrying at 240p") == 0);
    intent = psp_ui_media_activate_at(
        &media, WIDTH / 2, HEIGHT / 2, WIDTH, HEIGHT);
    CHECK(intent.action == PSP_UI_MEDIA_ACTION_NONE);
    memset(frame, 0, sizeof(frame));
    psp_ui_media_composite(
        &media, frame, WIDTH, HEIGHT, WIDTH);
    CHECK(frame[147 * WIDTH + 120] != frame[147 * WIDTH + 330]);
    input.pressed = PSP_UI_BUTTON_CONFIRM;
    intent = psp_ui_media_update(&media, &input);
    CHECK(intent.action == PSP_UI_MEDIA_ACTION_NONE);
    media.duration_us = UINT64_C(20000000);
    psp_ui_media_set_seek_preview(&media, UINT64_C(3000000));
    psp_ui_media_set_buffering(&media, true, UINT64_C(9000000));
    media.ended = true;
    psp_ui_media_set_error(&media, "NETWORK ERROR");
    CHECK(media.visible && media.failed && !media.resolving
          && !media.ended && !media.buffering
          && !media.seek_preview_active
          && media.analog_seek_direction == 0
          && media.seek_preview_time_us == 0
          && media.buffered_until_us == 0);
    intent = psp_ui_media_activate_at(
        &media, WIDTH / 2 - 78, HEIGHT / 2 + 27, WIDTH, HEIGHT);
    CHECK(intent.action == PSP_UI_MEDIA_ACTION_RETRY);
    intent = psp_ui_media_activate_at(
        &media, WIDTH / 2 + 78, HEIGHT / 2 + 27, WIDTH, HEIGHT);
    CHECK(intent.action == PSP_UI_MEDIA_ACTION_CLOSE);
    memset(frame, 0, sizeof(frame));
    psp_ui_media_composite(
        &media, frame, WIDTH, HEIGHT, WIDTH);
    CHECK(frame[150 * WIDTH + 100] != 0
          && frame[150 * WIDTH + 260] != 0);
    input.pressed = PSP_UI_BUTTON_CONFIRM;
    intent = psp_ui_media_update(&media, &input);
    CHECK(intent.action == PSP_UI_MEDIA_ACTION_RETRY);
    /*
     * A quarantined firmware decoder cannot be retried at all: the backend
     * refuses every later open for the rest of the process. The panel must
     * stop offering the affordance rather than let CROSS or the pointer
     * start work that can only fail again; Back keeps working, from its own
     * lower row.
     */
    media.retry_unavailable = true;
    intent = psp_ui_media_update(&media, &input);
    CHECK(intent.action == PSP_UI_MEDIA_ACTION_NONE
          && intent.visual_changed);
    intent = psp_ui_media_activate_at(
        &media, WIDTH / 2 - 78, HEIGHT / 2 + 27, WIDTH, HEIGHT);
    CHECK(intent.action == PSP_UI_MEDIA_ACTION_NONE);
    /* Back moved down a row, so the top of the old chip is now inert. */
    intent = psp_ui_media_activate_at(
        &media, WIDTH / 2 + 78, HEIGHT / 2 + 13, WIDTH, HEIGHT);
    CHECK(intent.action == PSP_UI_MEDIA_ACTION_NONE);
    intent = psp_ui_media_activate_at(
        &media, WIDTH / 2 + 78, HEIGHT / 2 + 27, WIDTH, HEIGHT);
    CHECK(intent.action == PSP_UI_MEDIA_ACTION_CLOSE);
    input.pressed = PSP_UI_BUTTON_CANCEL;
    CHECK(psp_ui_media_update(&media, &input).action
          == PSP_UI_MEDIA_ACTION_CLOSE);
    /* Setting a new error restores the ordinary retry affordance: only a
       caller which knows the decoder is quarantined may withdraw it. */
    psp_ui_media_set_error(&media, "NETWORK ERROR");
    CHECK(!media.retry_unavailable);
    input.pressed = PSP_UI_BUTTON_CONFIRM;
    CHECK(psp_ui_media_update(&media, &input).action
          == PSP_UI_MEDIA_ACTION_RETRY);
    return true;
}

/*
 * Back must leave the player from every media state, not only from a
 * playing one. A hardware report had the player stuck on "DECODING FIRST
 * FRAME" with CIRCLE doing nothing, so the mapping from each state to
 * PSP_UI_MEDIA_ACTION_CLOSE is pinned here rather than left to inspection.
 */
static bool test_media_cancel_closes_from_every_state(void)
{
    PspUiInput cancel = {
        .pressed = PSP_UI_BUTTON_CANCEL,
        .analog_x = 128,
        .analog_y = 128
    };
    PspUiMediaState media;

    /* Resolving, before any progress text exists. */
    psp_ui_media_init(&media);
    psp_ui_media_set_resolving(&media, "YouTube video");
    CHECK(media.visible && media.resolving
          && strcmp(media.status, "Loading...") == 0);
    CHECK(psp_ui_media_update(&media, &cancel).action
          == PSP_UI_MEDIA_ACTION_CLOSE);

    /* Decoding the first frame: resolving with progress, no frame yet. */
    psp_ui_media_init(&media);
    psp_ui_media_set_resolving(&media, "YouTube video");
    psp_ui_media_set_resolving_progress(
        &media, "Loading...", 920u);
    CHECK(strcmp(media.status, "Loading...") == 0);
    CHECK(psp_ui_media_update(&media, &cancel).action
          == PSP_UI_MEDIA_ACTION_CLOSE);

    /* Buffering mid-startup keeps the controls reachable. */
    psp_ui_media_init(&media);
    psp_ui_media_set_resolving(&media, "YouTube video");
    psp_ui_media_set_buffering(&media, true, 0);
    CHECK(psp_ui_media_update(&media, &cancel).action
          == PSP_UI_MEDIA_ACTION_CLOSE);

    /* Playing. */
    psp_ui_media_init(&media);
    psp_ui_media_set(
        &media, true, true, false, UINT64_C(1000000),
        UINT64_C(20000000), "Video");
    CHECK(psp_ui_media_update(&media, &cancel).action
          == PSP_UI_MEDIA_ACTION_CLOSE);

    /* Paused. */
    psp_ui_media_init(&media);
    psp_ui_media_set(
        &media, true, false, false, UINT64_C(1000000),
        UINT64_C(20000000), "Video");
    CHECK(psp_ui_media_update(&media, &cancel).action
          == PSP_UI_MEDIA_ACTION_CLOSE);

    /* Ended. */
    psp_ui_media_init(&media);
    psp_ui_media_set(
        &media, true, false, true, UINT64_C(20000000),
        UINT64_C(20000000), "Video");
    CHECK(media.ended);
    CHECK(psp_ui_media_update(&media, &cancel).action
          == PSP_UI_MEDIA_ACTION_CLOSE);

    /* Every terminal failure surface, including the quarantine text and
       the first-frame ladder's two exits. */
    static const char *const failures[] = {
        "VIDEO DECODER NEEDS APP RESTART",
        "VIDEO FIRST FRAME TIMED OUT",
        "VIDEO DECODER MADE NO PROGRESS",
        "VIDEO START CANCELLED",
        "VIDEO COULD NOT RESTART"
    };
    for (size_t i = 0; i < sizeof(failures) / sizeof(failures[0]); i++) {
        psp_ui_media_init(&media);
        psp_ui_media_set_resolving(&media, "YouTube video");
        psp_ui_media_set_error(&media, failures[i]);
        CHECK(media.visible && media.failed && !media.resolving);
        CHECK(strcmp(media.status, failures[i]) == 0);
        CHECK(psp_ui_media_update(&media, &cancel).action
              == PSP_UI_MEDIA_ACTION_CLOSE);
        /* The same surface still offers retry on CROSS. */
        psp_ui_media_show_controls(&media);
        PspUiInput confirm = {
            .pressed = PSP_UI_BUTTON_CONFIRM,
            .analog_x = 128,
            .analog_y = 128
        };
        CHECK(psp_ui_media_update(&media, &confirm).action
              == PSP_UI_MEDIA_ACTION_RETRY);
    }

    /*
     * Shipping builds compile their printf telemetry to nothing, so the
     * failed surface is the only place a device user can read why playback
     * stopped. It carries a headline and a muted reason row, both inside the
     * one bounded status field, and it still closes and retries.
     */
    psp_ui_media_init(&media);
    psp_ui_media_set_resolving(&media, "YouTube video");
    psp_ui_media_set_error_reason(
        &media, "VIDEO FIRST FRAME TIMED OUT", "NETWORK STALLED 512KB IN");
    CHECK(media.visible && media.failed && !media.resolving);
    CHECK(strcmp(media.status,
                 "VIDEO FIRST FRAME TIMED OUT\nNETWORK STALLED 512KB IN")
          == 0);
    CHECK(psp_ui_media_update(&media, &cancel).action
          == PSP_UI_MEDIA_ACTION_CLOSE);
    /* A reason that cannot fit the bounded field is dropped, never truncated
       into the headline. */
    psp_ui_media_init(&media);
    psp_ui_media_set_error_reason(
        &media,
        "VIDEO DECODER MADE NO PROGRESS BUT THE PLAYER KEPT WAITING FOR IT"
        " ANYWAY AND THEN SOME",
        "DECODER STALLED 80618005");
    CHECK(strchr(media.status, '\n') == NULL
          || strstr(media.status, "DECODER STALLED") == NULL);
    /* A long single-line backend failure folds at a word boundary so the
       firmware status code -- the part that names the fault -- survives
       instead of being clipped off the end of one row. */
    psp_ui_media_init(&media);
    psp_ui_media_set_error(
        &media, "PSP AVC sceMpegGetAvcNalAu failed: 0x80618005");
    const char *folded = strchr(media.status, '\n');
    CHECK(folded != NULL
          && (size_t) (folded - media.status) <= 34u
          && strcmp(folded + 1, "failed: 0x80618005") == 0);
    CHECK(media.failed
          && psp_ui_media_update(&media, &cancel).action
                 == PSP_UI_MEDIA_ACTION_CLOSE);
    /* Short labels stay on one row exactly as before. */
    psp_ui_media_init(&media);
    psp_ui_media_set_error(&media, "VIDEO FIRST FRAME TIMED OUT");
    CHECK(strcmp(media.status, "VIDEO FIRST FRAME TIMED OUT") == 0);
    CHECK(psp_ui_media_state_bytes() <= 288);

    /* A seek preview is the one state that spends the first CIRCLE on the
       preview itself. The very next CIRCLE must still close. */
    psp_ui_media_init(&media);
    psp_ui_media_set(
        &media, true, true, false, UINT64_C(1000000),
        UINT64_C(20000000), "Video");
    psp_ui_media_set_seek_preview(&media, UINT64_C(5000000));
    CHECK(media.seek_preview_active);
    CHECK(psp_ui_media_update(&media, &cancel).action
          == PSP_UI_MEDIA_ACTION_CANCEL_SEEK_PREVIEW);
    CHECK(!media.seek_preview_active);
    CHECK(psp_ui_media_update(&media, &cancel).action
          == PSP_UI_MEDIA_ACTION_CLOSE);

    /* Hidden controls must not swallow the press either: CIRCLE closes on
       the first press rather than only revealing the controls. */
    psp_ui_media_init(&media);
    psp_ui_media_set(
        &media, true, true, false, UINT64_C(1000000),
        UINT64_C(20000000), "Video");
    psp_ui_media_tick(&media, 60000);
    CHECK(!media.controls_visible);
    CHECK(psp_ui_media_update(&media, &cancel).action
          == PSP_UI_MEDIA_ACTION_CLOSE);

    /* A closed player takes no input at all. */
    psp_ui_media_init(&media);
    CHECK(!media.visible);
    CHECK(psp_ui_media_update(&media, &cancel).action
          == PSP_UI_MEDIA_ACTION_NONE);
    return true;
}

static bool test_media_title_uses_unicode_fallback_font(void)
{
    enum { WIDTH = 480, HEIGHT = 272 };
    static uint16_t with_font[WIDTH * HEIGHT];
    static uint16_t without_font[WIDTH * HEIGHT];
    Budget budget;
    budget_init(&budget, 2u * 1024u * 1024u);
    FontSet fonts;
    CHECK(font_set_load(
        &fonts, &budget, TILEFINCH_TEST_SANS_FONT, NULL, NULL, NULL, NULL,
        NULL, NULL, 1024u * 1024u));
    PspUiMediaState media;
    psp_ui_media_init(&media);
    psp_ui_media_set(
        &media, true, false, false, 0, UINT64_C(20000000),
        "Fixture artist (\xed\x85\x8c\xec\x8a\xa4\xed\x8a\xb8)");
    psp_ui_media_set_title_font(
        &media, font_set_face(&fonts, FONT_SANS));
    psp_ui_media_composite(
        &media, with_font, WIDTH, HEIGHT, WIDTH);
    psp_ui_media_set_title_font(&media, NULL);
    psp_ui_media_composite(
        &media, without_font, WIDTH, HEIGHT, WIDTH);
    CHECK(memcmp(with_font, without_font, sizeof(with_font)) != 0);
    font_set_destroy(&fonts);
    CHECK(budget.current == 0 && budget_categories_reconcile(&budget));
    return true;
}

/*
 * The play control inherits the geometry of the overlay the watch page draws
 * over a thumbnail (src/youtube_lite.c: `.play-icon` is a 50x38 box at radius
 * 12 centred on left 50% / top 64px; `.play-glyph` centres a light pointer on
 * the same point from its own 52x40 box). Fullscreen adds a one-pixel light
 * outline so the dark control survives a dark decoded frame. What is pinned
 * here is that geometry and, more importantly, that both rounded silhouettes
 * are sampled rather than decided by a single inside/outside test per pixel.
 *
 * Drawing the pointer instead of typesetting it is the other half. The old
 * control asked the chrome face for U+25BA, and this test proved it got an
 * antialiased glyph -- using TILEFINCH_TEST_SANS_FONT, the full upstream
 * DejaVuSans.ttf. The device ships fonts/DejaVuSans-Latin.ttf, a subset
 * without that codepoint, so on real hardware every frame fell through to a
 * 21-row scanline staircase that the test could never see. Compositing with
 * and without a chrome face must now produce the identical control.
 */
static bool test_media_play_control_matches_the_page_overlay(void)
{
    enum { WIDTH = 480, HEIGHT = 272, CX = WIDTH / 2, CY = HEIGHT / 2 };
    enum { BADGE_LEFT = CX - 25, BADGE_TOP = CY - 19, BADGE_RADIUS = 12 };
    static uint16_t frame[WIDTH * HEIGHT];
    static uint16_t typeset[WIDTH * HEIGHT];
    const size_t pixels = sizeof(frame) / sizeof(frame[0]);
    const uint16_t background = 0xffffu;
    PspUiMediaState media;
    psp_ui_media_init(&media);
    psp_ui_media_set(
        &media, true, false, false, 0, UINT64_C(20000000), NULL);

    psp_ui_clear_chrome_font();
    for (size_t at = 0; at < pixels; at++) frame[at] = background;
    psp_ui_media_composite(&media, frame, WIDTH, HEIGHT, WIDTH);

    uint16_t outline = frame[(size_t) CY * WIDTH + BADGE_LEFT];
    uint16_t badge = frame[(size_t) CY * WIDTH + (BADGE_LEFT + 1)];
    uint16_t pointer = frame[(size_t) CY * WIDTH + (CX - 6)];
    CHECK(outline != background && badge != background
          && outline != badge && pointer == outline);

    /* Exactly the page's 50x38: one row or column past each straight edge is
       still the picture. */
    CHECK(frame[(size_t) CY * WIDTH + (BADGE_LEFT - 1)] == background);
    CHECK(frame[(size_t) CY * WIDTH + BADGE_LEFT] == outline);
    CHECK(frame[(size_t) CY * WIDTH + (BADGE_LEFT + 1)] == badge);
    CHECK(frame[(size_t) CY * WIDTH + (BADGE_LEFT + 48)] == badge);
    CHECK(frame[(size_t) CY * WIDTH + (BADGE_LEFT + 49)] == outline);
    CHECK(frame[(size_t) CY * WIDTH + (BADGE_LEFT + 50)] == background);
    CHECK(frame[(size_t) (BADGE_TOP - 1) * WIDTH + CX] == background);
    CHECK(frame[(size_t) BADGE_TOP * WIDTH + CX] == outline);
    CHECK(frame[(size_t) (BADGE_TOP + 1) * WIDTH + CX] == badge);
    CHECK(frame[(size_t) (BADGE_TOP + 36) * WIDTH + CX] == badge);
    CHECK(frame[(size_t) (BADGE_TOP + 37) * WIDTH + CX] == outline);
    CHECK(frame[(size_t) (BADGE_TOP + 38) * WIDTH + CX] == background);

    /* A binary corner test can only ever paint the two endpoint colours; a
       sampled one fills the blend ladder between them. */
    uint16_t levels[16];
    size_t level_count = 0;
    for (int y = BADGE_TOP; y < BADGE_TOP + BADGE_RADIUS; y++) {
        for (int x = BADGE_LEFT; x < BADGE_LEFT + BADGE_RADIUS; x++) {
            uint16_t pixel = frame[(size_t) y * WIDTH + (size_t) x];
            bool known = false;
            for (size_t at = 0; at < level_count; at++)
                if (levels[at] == pixel) known = true;
            if (!known && level_count < 16) levels[level_count++] = pixel;
        }
    }
    CHECK(level_count >= 6);
    CHECK(frame[(size_t) BADGE_TOP * WIDTH + BADGE_LEFT] == background);
    uint16_t arc = frame[(size_t) (BADGE_TOP + 3) * WIDTH + (BADGE_LEFT + 3)];
    CHECK(arc != background && arc != badge);

    /* The pointer is a 15px square triangle carrying one pixel of rightward
       optical bias, so its flat edge stands at CX - 6, not CX - 7. It spans
       exactly 15 rows: the two extreme ones are its vertices and therefore
       partly covered, and one row further out is untouched picture. */
    CHECK(frame[(size_t) CY * WIDTH + (CX - 7)] == badge);
    uint16_t tip = frame[(size_t) (CY - 7) * WIDTH + (CX - 6)];
    CHECK(tip != badge && tip != pointer);
    CHECK(frame[(size_t) (CY + 7) * WIDTH + (CX - 6)] == tip);
    CHECK(frame[(size_t) (CY - 8) * WIDTH + (CX - 6)] == badge);
    CHECK(frame[(size_t) (CY + 8) * WIDTH + (CX - 6)] == badge);
    /* Apex and slopes land between the two, at more than one level, and the
       two slopes agree about the axis they are mirrored in. */
    uint16_t apex = frame[(size_t) CY * WIDTH + (CX + 8)];
    uint16_t slope_inner = frame[(size_t) (CY - 4) * WIDTH + CX];
    uint16_t slope_outer = frame[(size_t) (CY - 4) * WIDTH + (CX + 1)];
    CHECK(apex != badge && apex != pointer);
    CHECK(slope_inner != badge && slope_inner != pointer);
    CHECK(slope_outer != badge && slope_outer != pointer);
    CHECK(slope_inner != slope_outer);
    CHECK(frame[(size_t) (CY + 4) * WIDTH + CX] == slope_inner);
    CHECK(frame[(size_t) (CY + 4) * WIDTH + (CX + 1)] == slope_outer);

    /* Pause is the same envelope: two bars in the pointer's own 15 rows, the
       badge showing between them, so neither state outweighs the other. */
    psp_ui_media_set(
        &media, true, true, false, 0, UINT64_C(20000000), NULL);
    for (size_t at = 0; at < pixels; at++) frame[at] = background;
    psp_ui_media_composite(&media, frame, WIDTH, HEIGHT, WIDTH);
    CHECK(frame[(size_t) CY * WIDTH + (CX - 8)] == badge);
    CHECK(frame[(size_t) CY * WIDTH + (CX - 7)] == pointer);
    CHECK(frame[(size_t) CY * WIDTH + (CX - 4)] == pointer);
    CHECK(frame[(size_t) CY * WIDTH + (CX - 3)] == badge);
    CHECK(frame[(size_t) CY * WIDTH + CX] == badge);
    CHECK(frame[(size_t) CY * WIDTH + (CX + 4)] == pointer);
    CHECK(frame[(size_t) CY * WIDTH + (CX + 7)] == pointer);
    CHECK(frame[(size_t) CY * WIDTH + (CX + 8)] == badge);
    CHECK(frame[(size_t) (CY - 7) * WIDTH + (CX - 6)] == pointer);
    CHECK(frame[(size_t) (CY + 7) * WIDTH + (CX - 6)] == pointer);
    CHECK(frame[(size_t) (CY - 8) * WIDTH + (CX - 6)] == badge);

    Budget budget;
    budget_init(&budget, 2u * 1024u * 1024u);
    FontSet fonts;
    CHECK(font_set_load(
        &fonts, &budget, TILEFINCH_TEST_SANS_FONT, NULL, NULL, NULL, NULL,
        NULL, NULL, 1024u * 1024u));
    psp_ui_set_chrome_fonts(font_set_face(&fonts, FONT_SANS), NULL);
    for (size_t at = 0; at < pixels; at++) typeset[at] = background;
    psp_ui_media_composite(&media, typeset, WIDTH, HEIGHT, WIDTH);
    for (int y = BADGE_TOP - 1; y < BADGE_TOP + 39; y++) {
        for (int x = BADGE_LEFT - 1; x < BADGE_LEFT + 51; x++) {
            size_t at = (size_t) y * WIDTH + (size_t) x;
            CHECK(frame[at] == typeset[at]);
        }
    }
    psp_ui_clear_chrome_font();
    font_set_destroy(&fonts);
    CHECK(budget.current == 0 && budget_categories_reconcile(&budget));

    /* The silhouette moved; the input did not. Everything off the scrubber
       row still toggles, including the badge's own corner. */
    CHECK(psp_ui_media_activate_at(&media, CX, CY, WIDTH, HEIGHT).action
          == PSP_UI_MEDIA_ACTION_PLAY_PAUSE);
    CHECK(psp_ui_media_activate_at(
              &media, BADGE_LEFT, BADGE_TOP, WIDTH, HEIGHT).action
          == PSP_UI_MEDIA_ACTION_PLAY_PAUSE);
    CHECK(psp_ui_media_activate_at(
              &media, CX, HEIGHT - 69, WIDTH, HEIGHT).action
          == PSP_UI_MEDIA_ACTION_PREVIEW_SEEK);
    return true;
}

static bool test_media_status_uses_antialiased_chrome_font(void)
{
    enum { WIDTH = 480, HEIGHT = 272 };
    static uint16_t fallback[WIDTH * HEIGHT];
    static uint16_t antialiased[WIDTH * HEIGHT];
    PspUiMediaState media;
    psp_ui_media_init(&media);
    psp_ui_media_set_resolving(&media, "YouTube video");
    psp_ui_media_set_resolving_progress(
        &media, "Retrying at 240p", 400u);

    psp_ui_clear_chrome_font();
    psp_ui_media_composite(
        &media, fallback, WIDTH, HEIGHT, WIDTH);

    Budget budget;
    budget_init(&budget, 2u * 1024u * 1024u);
    FontSet fonts;
    CHECK(font_set_load(
        &fonts, &budget, TILEFINCH_TEST_SANS_FONT, NULL, NULL, NULL, NULL,
        NULL, NULL, 1024u * 1024u));
    psp_ui_set_chrome_fonts(font_set_face(&fonts, FONT_SANS), NULL);
    psp_ui_media_composite(
        &media, antialiased, WIDTH, HEIGHT, WIDTH);

    bool status_changed = false;
    for (int y = HEIGHT / 2 - 26; y < HEIGHT / 2 + 5; y++) {
        for (int x = WIDTH / 2 - 132; x < WIDTH / 2 + 132; x++) {
            size_t at = (size_t) y * WIDTH + (size_t) x;
            if (fallback[at] != antialiased[at]) status_changed = true;
        }
    }
    CHECK(status_changed);
    psp_ui_clear_chrome_font();
    font_set_destroy(&fonts);
    CHECK(budget.current == 0 && budget_categories_reconcile(&budget));
    return true;
}

static bool test_startup_views_are_bounded_and_distinct(void)
{
    enum {
        WIDTH = 480, HEIGHT = 272, GUARD = 32,
        MARK_SIZE = 48, ENTRANCE_MARK_TOP = 100
    };
    static uint16_t guarded[GUARD + WIDTH * HEIGHT + GUARD];
    static uint16_t splash[WIDTH * HEIGHT];
    static uint16_t entrance[WIDTH * HEIGHT];
    for (size_t at = 0; at < sizeof(guarded) / sizeof(guarded[0]); at++)
        guarded[at] = 0x5aa5u;
    uint16_t *frame = guarded + GUARD;

    psp_ui_startup_composite(
        PSP_UI_STARTUP_SPLASH, "STARTING BROWSER", 80,
        frame, WIDTH, HEIGHT, WIDTH);
    memcpy(splash, frame, sizeof(splash));

    /* The fallback/diagnostic startup surface and the ordinary slot entrance
       use the exact same 48px mark. The stable launcher consumes the same
       primitive list, so all non-XMB boot surfaces share one logo geometry. */
    PspUiBootEntranceView entrance_view = {
        .frame = 0u,
        .wave = false,
        .branch_status = NULL
    };
    psp_ui_boot_entrance_composite(
        &entrance_view, NULL, entrance, WIDTH, HEIGHT, WIDTH);
    for (int y = 0; y < MARK_SIZE; y++) {
        CHECK(memcmp(
            &splash[(size_t) (54 + y) * WIDTH
                    + (WIDTH - MARK_SIZE) / 2],
            &entrance[(size_t) (ENTRANCE_MARK_TOP + y) * WIDTH
                      + (WIDTH - MARK_SIZE) / 2],
            MARK_SIZE * sizeof(uint16_t)) == 0);
    }
    psp_ui_startup_composite(
        PSP_UI_STARTUP_HOMEPAGE, "PREPARING HOME", 620,
        frame, WIDTH, HEIGHT, WIDTH);

    for (size_t at = 0; at < GUARD; at++) {
        CHECK(guarded[at] == 0x5aa5u);
        CHECK(guarded[GUARD + WIDTH * HEIGHT + at] == 0x5aa5u);
    }
    CHECK(memcmp(splash, frame, sizeof(splash)) != 0);
    /* Loading progress now sits under the shared URL bar, so both startup
       views intentionally share their top-left shell pixel. */
    CHECK(splash[40u * WIDTH] != frame[40u * WIDTH]);
    CHECK(frame[100u * WIDTH + 20u] != frame[113u * WIDTH + 20u]);
    return true;
}

static bool test_native_surfaces_own_the_panel(void)
{
    PspUiState ui;
    psp_ui_init(&ui);
    CHECK(!psp_ui_screen_is_native_surface(PSP_UI_SCREEN_PAGE)
          && !psp_ui_screen_is_native_surface(PSP_UI_SCREEN_MENU)
          && psp_ui_screen_is_native_surface(PSP_UI_SCREEN_HOME)
          && psp_ui_screen_is_native_surface(PSP_UI_SCREEN_COLLECTIONS));

    psp_ui_show_home(&ui);
    CHECK(ui.screen == PSP_UI_SCREEN_HOME
          && ui.base_screen == PSP_UI_SCREEN_HOME);

    PspUiInput input = {.analog_x = 128, .analog_y = 128};
    /* Triangle retracts page chrome; there is no page chrome to retract on a
       native surface, so the surface must survive it. */
    input.pressed = PSP_UI_BUTTON_TOOLBAR;
    (void) psp_ui_update(&ui, &input);
    CHECK(ui.screen == PSP_UI_SCREEN_HOME);

    /* An overlay opened over a surface returns to the surface, never to a
       page the user was not looking at. */
    input.pressed = PSP_UI_BUTTON_MENU;
    (void) psp_ui_update(&ui, &input);
    CHECK(ui.screen == PSP_UI_SCREEN_MENU);
    input.pressed = PSP_UI_BUTTON_CANCEL;
    (void) psp_ui_update(&ui, &input);
    CHECK(ui.screen == PSP_UI_SCREEN_HOME);

    /* Circle on HOME is a floor, not a back step. */
    input.pressed = PSP_UI_BUTTON_CANCEL;
    PspUiIntent intent = psp_ui_update(&ui, &input);
    CHECK(ui.screen == PSP_UI_SCREEN_HOME
          && intent.action == PSP_UI_ACTION_NONE
          && ui.toast_frames != 0);

    psp_ui_show_collections(&ui, PSP_UI_COLLECTION_BOOKMARKS);
    CHECK(ui.screen == PSP_UI_SCREEN_COLLECTIONS
          && ui.collections_section == PSP_UI_COLLECTION_BOOKMARKS
          && ui.collections_selection == 0);
    input.pressed = PSP_UI_BUTTON_CANCEL;
    (void) psp_ui_update(&ui, &input);
    CHECK(ui.screen == PSP_UI_SCREEN_PAGE
          && ui.base_screen == PSP_UI_SCREEN_PAGE);

    /* A refresh that shrinks a view can never leave focus past its end. */
    PspUiCollectionsView view = {
        .section = PSP_UI_COLLECTION_HISTORY,
        .count = 2,
        .rows = {{"a", "a.example", "", false},
                 {"b", "b.example", "", false}}
    };
    psp_ui_show_collections(&ui, PSP_UI_COLLECTION_HISTORY);
    psp_ui_set_collections(&ui, &view);
    ui.collections_selection = 1;
    view.count = 1;
    psp_ui_set_collections(&ui, &view);
    CHECK(ui.collections_selection == 0);

    PspUiHomeView home = {.tile_count = 3, .continue_count = 1};
    psp_ui_show_home(&ui);
    psp_ui_set_home(&ui, &home);
    ui.home_selection = 3;
    home.tile_count = 1;
    home.continue_count = 0;
    psp_ui_set_home(&ui, &home);
    CHECK(ui.home_selection == 0);

    psp_ui_leave_native_surface(&ui);
    CHECK(ui.screen == PSP_UI_SCREEN_PAGE
          && ui.base_screen == PSP_UI_SCREEN_PAGE);
    return true;
}

static PspUiHomeView home_fixture(uint8_t tiles, uint8_t entries)
{
    PspUiHomeView home = {.tile_count = tiles, .continue_count = entries};
    for (uint8_t at = 0; at < tiles && at < PSP_UI_HOME_TILE_LIMIT; at++)
        snprintf(home.tiles[at].label, sizeof(home.tiles[at].label),
                 "TILE %u", (unsigned) at);
    for (uint8_t at = 0; at < entries && at < PSP_UI_HOME_CONTINUE_LIMIT;
         at++)
        snprintf(home.continues[at].label,
                 sizeof(home.continues[at].label), "ROW %u", (unsigned) at);
    return home;
}

static bool test_home_traversal_and_activation(void)
{
    PspUiState ui;
    psp_ui_init(&ui);
    PspUiHomeView home = home_fixture(6, 3);
    psp_ui_show_home(&ui);
    psp_ui_set_home(&ui, &home);
    PspUiInput input = {.analog_x = 128, .analog_y = 128};

    /* Rows wrap inside themselves; columns are preserved down the grid. */
    input.pressed = PSP_UI_BUTTON_LEFT;
    (void) psp_ui_update(&ui, &input);
    CHECK(ui.home_selection == 2);
    input.pressed = PSP_UI_BUTTON_RIGHT;
    (void) psp_ui_update(&ui, &input);
    CHECK(ui.home_selection == 0);
    input.pressed = PSP_UI_BUTTON_DOWN;
    (void) psp_ui_update(&ui, &input);
    CHECK(ui.home_selection == 3);
    /* Below the last tile row is the CONTINUE list, not a wrap. */
    input.pressed = PSP_UI_BUTTON_DOWN;
    (void) psp_ui_update(&ui, &input);
    CHECK(ui.home_selection == 6);
    input.pressed = PSP_UI_BUTTON_DOWN;
    (void) psp_ui_update(&ui, &input);
    input.pressed = PSP_UI_BUTTON_DOWN;
    (void) psp_ui_update(&ui, &input);
    CHECK(ui.home_selection == 8);
    /* Past the last row the surface wraps to its first tile. */
    input.pressed = PSP_UI_BUTTON_DOWN;
    (void) psp_ui_update(&ui, &input);
    CHECK(ui.home_selection == 0);
    /* Up from the first tile row reaches the end of the list. */
    input.pressed = PSP_UI_BUTTON_UP;
    (void) psp_ui_update(&ui, &input);
    CHECK(ui.home_selection == 8);
    input.pressed = PSP_UI_BUTTON_UP;
    (void) psp_ui_update(&ui, &input);
    CHECK(ui.home_selection == 7);

    ui.home_selection = 4;
    input.pressed = PSP_UI_BUTTON_CONFIRM;
    PspUiIntent intent = psp_ui_update(&ui, &input);
    CHECK(intent.action == PSP_UI_ACTION_HOME_ACTIVATE
          && intent.list_index == 4
          && ui.screen == PSP_UI_SCREEN_HOME);

    /* A grid that does not fill its last row still wraps within that row. */
    PspUiHomeView ragged = home_fixture(4, 0);
    psp_ui_set_home(&ui, &ragged);
    ui.home_selection = 3;
    input.pressed = PSP_UI_BUTTON_RIGHT;
    (void) psp_ui_update(&ui, &input);
    CHECK(ui.home_selection == 3);
    input.pressed = PSP_UI_BUTTON_UP;
    (void) psp_ui_update(&ui, &input);
    CHECK(ui.home_selection == 0);

    /* An empty surface never activates anything. */
    PspUiHomeView empty = home_fixture(0, 0);
    psp_ui_set_home(&ui, &empty);
    input.pressed = PSP_UI_BUTTON_CONFIRM;
    intent = psp_ui_update(&ui, &input);
    CHECK(intent.action == PSP_UI_ACTION_NONE);
    return true;
}

static bool test_implicit_cursor_handoff(void)
{
    PspUiState ui;
    psp_ui_init(&ui);
    PspUiHomeView home = home_fixture(6, 2);
    psp_ui_show_home(&ui);
    psp_ui_set_home(&ui, &home);
    ui.home_selection = 0;

    /* Analog past the dead zone fades the cursor in and moves it. */
    PspUiInput input = {.analog_x = 255, .analog_y = 255, .elapsed_ms = 16};
    for (int at = 0; at < 40; at++) (void) psp_ui_update(&ui, &input);
    CHECK(ui.cursor_visible
          && ui.cursor_x_milli / 1000 > 240
          && ui.cursor_y_milli / 1000 > 136);

    /* The d-pad press is spent handing the cursor's position to focus: the
       selection snaps to the nearest row and does not also step. */
    input.analog_x = 128;
    input.analog_y = 128;
    input.pressed = PSP_UI_BUTTON_UP;
    PspUiIntent intent = psp_ui_update(&ui, &input);
    CHECK(!ui.cursor_visible
          && intent.action == PSP_UI_ACTION_NONE
          && ui.home_selection != 0);
    uint8_t snapped = ui.home_selection;
    /* The next press moves normally. */
    intent = psp_ui_update(&ui, &input);
    CHECK(ui.home_selection != snapped);

    /* On a page the same press becomes a focus-at request carrying the
       cursor position, not a directional move. */
    PspUiState page;
    psp_ui_init(&page);
    PspUiInput drift = {.analog_x = 255, .analog_y = 255, .elapsed_ms = 16};
    for (int at = 0; at < 10; at++) (void) psp_ui_update(&page, &drift);
    CHECK(page.cursor_visible);
    int expected_x = page.cursor_x_milli / 1000;
    int expected_y = page.cursor_y_milli / 1000;
    drift.analog_x = 128;
    drift.analog_y = 128;
    drift.pressed = PSP_UI_BUTTON_LEFT;
    intent = psp_ui_update(&page, &drift);
    CHECK(intent.action == PSP_UI_ACTION_FOCUS_AT
          && intent.pointer_x == expected_x
          && intent.pointer_y == expected_y
          && !page.cursor_visible
          && !psp_ui_intent_has_predispatch_visual(&intent));
    intent = psp_ui_update(&page, &drift);
    CHECK(intent.action == PSP_UI_ACTION_FOCUS_LEFT);

    /* With the cursor option off there is nothing to hand over. */
    psp_ui_init(&page);
    page.analog_cursor_enabled = false;
    drift.pressed = PSP_UI_BUTTON_LEFT;
    intent = psp_ui_update(&page, &drift);
    CHECK(intent.action == PSP_UI_ACTION_FOCUS_LEFT);
    return true;
}

static bool test_collections_sections_and_deletes(void)
{
    PspUiState ui;
    psp_ui_init(&ui);
    static PspUiCollectionsView view;
    memset(&view, 0, sizeof(view));
    view.section = PSP_UI_COLLECTION_OFFLINE;
    view.count = 12;
    view.empty_message = "NOTHING SAVED YET";
    for (size_t at = 0; at < view.count; at++) {
        view.rows[at].title = "Saved item";
        view.rows[at].detail = "example.org";
        view.rows[at].deletable = at != 5;
    }
    psp_ui_show_collections(&ui, PSP_UI_COLLECTION_OFFLINE);
    psp_ui_set_collections(&ui, &view);
    PspUiInput input = {.analog_x = 128, .analog_y = 128};

    /* The window follows the selection and never runs past the list. */
    for (int at = 0; at < 8; at++) {
        input.pressed = PSP_UI_BUTTON_DOWN;
        (void) psp_ui_update(&ui, &input);
    }
    CHECK(ui.collections_selection == 8
          && ui.collections_first_row == 3);
    input.pressed = PSP_UI_BUTTON_UP;
    for (int at = 0; at < 9; at++) (void) psp_ui_update(&ui, &input);
    CHECK(ui.collections_selection == 11
          && ui.collections_first_row == 6);

    /* L and R switch section and say so, because the rows behind a section
       belong to the frontend and have to be refreshed with it. */
    input.pressed = PSP_UI_BUTTON_PAGE_DOWN;
    PspUiIntent intent = psp_ui_update(&ui, &input);
    CHECK(ui.collections_section == PSP_UI_COLLECTION_BOOKMARKS
          && intent.action == PSP_UI_ACTION_SHOW_BOOKMARKS
          && ui.collections_selection == 0
          && ui.collections_first_row == 0);
    input.pressed = PSP_UI_BUTTON_PAGE_DOWN;
    intent = psp_ui_update(&ui, &input);
    CHECK(ui.collections_section == PSP_UI_COLLECTION_HISTORY
          && intent.action == PSP_UI_ACTION_SHOW_HISTORY);
    input.pressed = PSP_UI_BUTTON_PAGE_DOWN;
    intent = psp_ui_update(&ui, &input);
    CHECK(ui.collections_section == PSP_UI_COLLECTION_OFFLINE
          && intent.action == PSP_UI_ACTION_SHOW_OFFLINE);
    input.pressed = PSP_UI_BUTTON_PAGE_UP;
    intent = psp_ui_update(&ui, &input);
    CHECK(ui.collections_section == PSP_UI_COLLECTION_HISTORY);

    psp_ui_set_collections(&ui, &view);
    ui.collections_selection = 2;
    input.pressed = PSP_UI_BUTTON_CONFIRM;
    intent = psp_ui_update(&ui, &input);
    CHECK(intent.action == PSP_UI_ACTION_COLLECTION_ACTIVATE
          && intent.list_index == 2);

    /* Deleting costs two deliberate presses on the same row. */
    input.pressed = PSP_UI_BUTTON_RELOAD;
    intent = psp_ui_update(&ui, &input);
    CHECK(intent.action == PSP_UI_ACTION_NONE
          && ui.collections_delete_confirmation == 3);
    intent = psp_ui_update(&ui, &input);
    CHECK(intent.action == PSP_UI_ACTION_COLLECTION_DELETE
          && intent.list_index == 2
          && ui.collections_delete_confirmation == 0);

    /* Moving away, or Circle, abandons a pending confirmation instead of
       carrying it to a row the user did not arm. */
    input.pressed = PSP_UI_BUTTON_RELOAD;
    (void) psp_ui_update(&ui, &input);
    CHECK(ui.collections_delete_confirmation == 3);
    input.pressed = PSP_UI_BUTTON_DOWN;
    (void) psp_ui_update(&ui, &input);
    CHECK(ui.collections_delete_confirmation == 0);
    input.pressed = PSP_UI_BUTTON_RELOAD;
    (void) psp_ui_update(&ui, &input);
    CHECK(ui.collections_delete_confirmation == 4);
    input.pressed = PSP_UI_BUTTON_CANCEL;
    (void) psp_ui_update(&ui, &input);
    CHECK(ui.collections_delete_confirmation == 0
          && ui.screen == PSP_UI_SCREEN_COLLECTIONS);
    input.pressed = PSP_UI_BUTTON_CANCEL;
    (void) psp_ui_update(&ui, &input);
    CHECK(ui.screen == PSP_UI_SCREEN_PAGE);

    /* A row that cannot be deleted says so rather than arming a confirm. */
    psp_ui_show_collections(&ui, PSP_UI_COLLECTION_OFFLINE);
    psp_ui_set_collections(&ui, &view);
    ui.collections_selection = 5;
    input.pressed = PSP_UI_BUTTON_RELOAD;
    intent = psp_ui_update(&ui, &input);
    CHECK(intent.action == PSP_UI_ACTION_NONE
          && ui.collections_delete_confirmation == 0
          && ui.toast_frames != 0);

    /* An empty section activates and deletes nothing, and still switches. */
    static PspUiCollectionsView empty;
    memset(&empty, 0, sizeof(empty));
    empty.empty_message = "NO BOOKMARKS YET";
    psp_ui_set_collections(&ui, &empty);
    input.pressed = PSP_UI_BUTTON_CONFIRM;
    intent = psp_ui_update(&ui, &input);
    CHECK(intent.action == PSP_UI_ACTION_NONE);
    input.pressed = PSP_UI_BUTTON_RELOAD;
    intent = psp_ui_update(&ui, &input);
    CHECK(intent.action == PSP_UI_ACTION_NONE);
    input.pressed = PSP_UI_BUTTON_PAGE_DOWN;
    intent = psp_ui_update(&ui, &input);
    CHECK(intent.action == PSP_UI_ACTION_SHOW_BOOKMARKS);

    uint16_t frame[64] = {0};
    psp_ui_composite(&ui, frame, 8, 8, 8);
    return true;
}

static bool test_native_motion_budget(void)
{
    PspUiState ui;
    psp_ui_init(&ui);
    CHECK(!ui.wave_enabled);
    PspUiHomeView home = home_fixture(3, 1);
    psp_ui_show_home(&ui);
    psp_ui_set_home(&ui, &home);

    /* Opening a surface spends the panel-slide budget and lands exactly on
       the static layout, never past it. */
    CHECK(ui.overlay_animation_frames == PSP_THEME_MOTION_PANEL_FRAMES);
    CHECK(psp_ui_motion_pending(&ui));
    PspUiInput input = {.analog_x = 128, .analog_y = 128, .elapsed_ms = 16};
    for (int at = 0; at < PSP_THEME_MOTION_PANEL_FRAMES; at++)
        (void) psp_ui_update(&ui, &input);
    CHECK(ui.overlay_animation_frames == 0);
    CHECK(!psp_ui_motion_pending(&ui));

    psp_ui_show_status(&ui, "SAVED", 60);
    CHECK(ui.toast_entry_frames == PSP_THEME_MOTION_TOAST_FRAMES);
    CHECK(psp_ui_motion_pending(&ui));
    for (int at = 0; at < PSP_THEME_MOTION_TOAST_FRAMES; at++)
        (void) psp_ui_update(&ui, &input);
    CHECK(ui.toast_entry_frames == 0);
    CHECK(!psp_ui_motion_pending(&ui));

    /*
     * Focus settle is a fixed, bounded budget, and it is spent on pixels:
     * the frame while it runs must differ from the frame after it ends, or
     * the counter would be describing motion that does not happen.
     */
    enum { WIDTH = 480, HEIGHT = 272 };
    static uint16_t settling[WIDTH * HEIGHT];
    static uint16_t settled[WIDTH * HEIGHT];
    input.pressed = PSP_UI_BUTTON_RIGHT;
    (void) psp_ui_update(&ui, &input);
    CHECK(ui.focus_settle_frames == PSP_THEME_MOTION_FOCUS_FRAMES);
    CHECK(psp_ui_motion_pending(&ui));
    psp_ui_composite(&ui, settling, WIDTH, HEIGHT, WIDTH);
    input.pressed = 0;
    for (int at = 0; at < PSP_THEME_MOTION_FOCUS_FRAMES; at++)
        (void) psp_ui_update(&ui, &input);
    CHECK(ui.focus_settle_frames == 0);
    CHECK(!psp_ui_motion_pending(&ui));
    psp_ui_composite(&ui, settled, WIDTH, HEIGHT, WIDTH);
    CHECK(memcmp(settling, settled, sizeof(settling)) != 0);

    /* Idle time alone does not dirty HOME now that ambient CPU animation is
       absent. */
    input.pressed = 0;
    input.elapsed_ms = 60;
    PspUiIntent idle_intent = psp_ui_update(&ui, &input);
    CHECK(!idle_intent.visual_changed);
    return true;
}

/*
 * The overlay's declared rows, and the 32-bit path that trusts them.
 *
 * During fullscreen video the panel scans out 8888 so the decoder's bytes
 * reach it unconverted, and the player's chrome -- which is 16-bit code --
 * reaches that buffer by round-tripping only the rows it touches. If the
 * declaration and the composite ever disagree, the consequence is silent:
 * either a strip of the picture is needlessly quantized, or, far worse, a
 * strip of 16-bit chrome is left in a 32-bit buffer and shows as noise.
 *
 * So the declaration is not trusted, it is measured: composite into a poisoned
 * surface and require that every pixel which moved lies inside a declared
 * band. Every state the overlay has is swept, because the panel and the
 * preview move the bands.
 */
static bool media_overlay_bands_cover_every_written_row(
    const PspUiMediaState *media, const PspUiMediaPreview *preview,
    int width, int height, const char *label)
{
    static uint16_t surface[512 * 272];
    const uint16_t poison = 0x1234u;
    for (int at = 0; at < width * height; at++) surface[at] = poison;
    psp_ui_media_composite_with_preview(
        media, preview, surface, width, height, width);
    PspUiRowBand bands[PSP_UI_MEDIA_OVERLAY_BAND_LIMIT];
    size_t count = psp_ui_media_overlay_bands(
        media, width, height, bands, PSP_UI_MEDIA_OVERLAY_BAND_LIMIT);
    /* Sorted, disjoint and inside the panel, or the wrapper's loops are not
       the loops the declaration describes. */
    for (size_t band = 0; band < count; band++) {
        CHECK(bands[band].top >= 0 && bands[band].bottom <= height);
        CHECK(bands[band].top < bands[band].bottom);
        if (band != 0) CHECK(bands[band].top > bands[band - 1u].bottom);
    }
    for (int y = 0; y < height; y++) {
        bool declared = false;
        for (size_t band = 0; band < count; band++) {
            if (y >= bands[band].top && y < bands[band].bottom)
                declared = true;
        }
        if (declared) continue;
        for (int x = 0; x < width; x++) {
            if (surface[(size_t) y * width + x] != poison) {
                fprintf(stderr,
                        "FAIL %s: row %d column %d written outside the "
                        "declared bands\n", label, y, x);
                return false;
            }
        }
    }
    return true;
}

static bool test_media_overlay_bands_and_the_32_bit_wrapper(void)
{
    enum { WIDTH = 480, HEIGHT = 272 };
    PspUiMediaState media;
    psp_ui_media_init(&media);
    /* An invisible or control-free player draws nothing, so it declares
       nothing and the wrapper must not touch the picture at all. */
    PspUiRowBand bands[PSP_UI_MEDIA_OVERLAY_BAND_LIMIT];
    CHECK(psp_ui_media_overlay_bands(
        &media, WIDTH, HEIGHT, bands, PSP_UI_MEDIA_OVERLAY_BAND_LIMIT) == 0);
    psp_ui_media_set(&media, true, true, false,
                     UINT64_C(5000000), UINT64_C(20000000), "Example video");
    media.controls_visible = false;
    CHECK(psp_ui_media_overlay_bands(
        &media, WIDTH, HEIGHT, bands, PSP_UI_MEDIA_OVERLAY_BAND_LIMIT) == 0);
    psp_ui_media_set_buffering(&media, true, UINT64_C(9000000));
    CHECK(!media.controls_visible
          && media_overlay_bands_cover_every_written_row(
                 &media, NULL, WIDTH, HEIGHT, "buffering-pill-only"));
    psp_ui_media_set_buffering(&media, false, UINT64_C(9000000));
    CHECK(psp_ui_media_overlay_bands(
        &media, WIDTH, HEIGHT, bands, PSP_UI_MEDIA_OVERLAY_BAND_LIMIT) == 0);

    psp_ui_media_show_controls(&media);
    CHECK(media_overlay_bands_cover_every_written_row(
        &media, NULL, WIDTH, HEIGHT, "playing"));
    psp_ui_media_set(&media, true, false, false,
                     UINT64_C(5000000), UINT64_C(20000000), "Example video");
    psp_ui_media_show_controls(&media);
    CHECK(media_overlay_bands_cover_every_written_row(
        &media, NULL, WIDTH, HEIGHT, "paused"));
    psp_ui_media_set_buffering(&media, true, UINT64_C(9000000));
    CHECK(media_overlay_bands_cover_every_written_row(
        &media, NULL, WIDTH, HEIGHT, "buffering"));
    psp_ui_media_set_buffering(&media, false, UINT64_C(9000000));

    static uint16_t preview[128 * 72];
    for (size_t at = 0; at < sizeof(preview) / sizeof(preview[0]); at++)
        preview[at] = 0x07e0u;
    PspUiMediaPreview preview_view = {
        .pixels = preview, .width = 128, .height = 72, .stride = 128
    };
    psp_ui_media_set_seek_preview(&media, UINT64_C(10000000));
    CHECK(media_overlay_bands_cover_every_written_row(
        &media, &preview_view, WIDTH, HEIGHT, "seek-preview"));
    psp_ui_media_cancel_seek_preview(&media);

    psp_ui_media_set_resolving(&media, "Example video");
    psp_ui_media_set_resolving_progress(&media, "Opening video", 400u);
    CHECK(media_overlay_bands_cover_every_written_row(
        &media, NULL, WIDTH, HEIGHT, "resolving"));
    psp_ui_media_set_error(&media, "VIDEO OPEN TIMED OUT IN video-demux");
    CHECK(media_overlay_bands_cover_every_written_row(
        &media, NULL, WIDTH, HEIGHT, "failed"));
    media.retry_unavailable = true;
    CHECK(media_overlay_bands_cover_every_written_row(
        &media, NULL, WIDTH, HEIGHT, "failed-quarantined"));
    media.retry_unavailable = false;

    /*
     * And the wrapper itself: the picture outside the bands keeps the
     * decoder's own bytes, the chrome inside them lands, and a colour that
     * survives the narrowing survives the whole trip. Full-scale channels are
     * chosen deliberately -- they are what the replicate-low-bits widening
     * exists to preserve.
     */
    psp_ui_media_set(&media, true, true, false,
                     UINT64_C(5000000), UINT64_C(20000000), "Example video");
    psp_ui_media_show_controls(&media);
    static uint32_t video[512 * 272];
    static uint16_t scratch[512 * 272];
    const uint32_t picture = UINT32_C(0xff0000ff);
    for (int at = 0; at < WIDTH * HEIGHT; at++) video[at] = picture;
    memset(scratch, 0, sizeof(scratch));
    psp_ui_media_composite_8888(
        &media, NULL, video, WIDTH, HEIGHT, WIDTH, scratch);
    size_t count = psp_ui_media_overlay_bands(
        &media, WIDTH, HEIGHT, bands, PSP_UI_MEDIA_OVERLAY_BAND_LIMIT);
    CHECK(count != 0);
    bool overlay_landed = false;
    for (int y = 0; y < HEIGHT; y++) {
        bool declared = false;
        for (size_t band = 0; band < count; band++) {
            if (y >= bands[band].top && y < bands[band].bottom)
                declared = true;
        }
        for (int x = 0; x < WIDTH; x++) {
            uint32_t pixel = video[(size_t) y * WIDTH + x];
            if (!declared) {
                /* Untouched, byte for byte. */
                CHECK(pixel == picture);
            } else if (pixel != picture) {
                overlay_landed = true;
            }
        }
    }
    CHECK(overlay_landed);
    /* A saturated channel round-trips to itself, so the video read through a
       translucent bar is not darkened by the conversion. */
    CHECK((video[(size_t) (HEIGHT / 2 - 1) * WIDTH + 4] & 0x00ffffffu)
          == (picture & 0x00ffffffu));
    /*
     * The transport frame behind the controls changes at video cadence. The
     * bottom chrome itself must not: letting that picture bleed through a
     * quantized 565 overlay made static glyph edges and the scrubber shimmer
     * on the physical panel. Compare the actual 8888 bridge output over two
     * maximally different decoded frames.
     */
    enum {
        CONTROL_HEIGHT = 78,
        TITLE_HEIGHT = 42,
        BADGE_WIDTH = 50,
        BADGE_HEIGHT = 38,
        BADGE_RADIUS = 12
    };
    static uint32_t stable_bottom[WIDTH * CONTROL_HEIGHT];
    static uint32_t stable_title[WIDTH * TITLE_HEIGHT];
    static uint32_t stable_badge[BADGE_WIDTH * BADGE_HEIGHT];
    for (int y = 0; y < TITLE_HEIGHT; y++) {
        memcpy(stable_title + (size_t) y * WIDTH,
               video + (size_t) y * WIDTH,
               WIDTH * sizeof(*video));
    }
    for (int y = 0; y < BADGE_HEIGHT; y++) {
        memcpy(stable_badge + (size_t) y * BADGE_WIDTH,
               video
                   + (size_t) (HEIGHT / 2 - BADGE_HEIGHT / 2 + y) * WIDTH
                   + WIDTH / 2 - BADGE_WIDTH / 2,
               BADGE_WIDTH * sizeof(*video));
    }
    for (int y = HEIGHT - CONTROL_HEIGHT; y < HEIGHT; y++) {
        memcpy(stable_bottom + (size_t) (y - (HEIGHT - CONTROL_HEIGHT)) * WIDTH,
               video + (size_t) y * WIDTH,
               WIDTH * sizeof(*video));
    }
    const uint32_t other_picture = UINT32_C(0xff00ff00);
    for (int at = 0; at < WIDTH * HEIGHT; at++) video[at] = other_picture;
    psp_ui_media_composite_8888(
        &media, NULL, video, WIDTH, HEIGHT, WIDTH, scratch);
    for (int y = 0; y < TITLE_HEIGHT; y++) {
        CHECK(memcmp(stable_title + (size_t) y * WIDTH,
                     video + (size_t) y * WIDTH,
                     WIDTH * sizeof(*video)) == 0);
    }
    /* The rounded badge does not own the pixels outside its silhouette. Its
       authored interior stays byte-identical over radically different
       decoded pictures. Only the partially covered pixels in the four outer
       corner arcs may follow the picture; resolving those samples against a
       fixed dark matte produces the aliased halo visible on hardware. */
    size_t destination_edge_pixels = 0;
    for (int y = 0; y < BADGE_HEIGHT; y++) {
        const uint32_t *saved = stable_badge + (size_t) y * BADGE_WIDTH;
        const uint32_t *current = video
            + (size_t) (HEIGHT / 2 - BADGE_HEIGHT / 2 + y) * WIDTH
            + WIDTH / 2 - BADGE_WIDTH / 2;
        for (int x = 0; x < BADGE_WIDTH; x++) {
            bool first_painted = saved[x] != picture;
            bool second_painted = current[x] != other_picture;
            if (!first_painted && !second_painted) {
                continue;
            } else if (saved[x] != current[x]) {
                bool corner_column =
                    x < BADGE_RADIUS || x >= BADGE_WIDTH - BADGE_RADIUS;
                bool corner_row =
                    y < BADGE_RADIUS || y >= BADGE_HEIGHT - BADGE_RADIUS;
                CHECK(corner_column && corner_row);
                destination_edge_pixels++;
            } else {
                CHECK(saved[x] == current[x]);
            }
        }
    }
    CHECK(destination_edge_pixels != 0u);
    for (int y = HEIGHT - CONTROL_HEIGHT; y < HEIGHT; y++) {
        CHECK(memcmp(
                  stable_bottom
                      + (size_t) (y - (HEIGHT - CONTROL_HEIGHT)) * WIDTH,
                  video + (size_t) y * WIDTH,
                  WIDTH * sizeof(*video)) == 0);
    }
    /* The cooperative seek supervisor freezes the accepted video frame and
       repaints only this opaque band. That acknowledgement must neither
       touch the picture above it nor produce a different control surface. */
    for (int at = 0; at < WIDTH * HEIGHT; at++) video[at] = picture;
    psp_ui_media_composite_controls_8888(
        &media, video, WIDTH, HEIGHT, WIDTH, scratch);
    for (int y = 0; y < HEIGHT - CONTROL_HEIGHT; y++) {
        for (int x = 0; x < WIDTH; x++)
            CHECK(video[(size_t) y * WIDTH + x] == picture);
    }
    for (int y = HEIGHT - CONTROL_HEIGHT; y < HEIGHT; y++) {
        CHECK(memcmp(
                  stable_bottom
                      + (size_t) (y - (HEIGHT - CONTROL_HEIGHT)) * WIDTH,
                  video + (size_t) y * WIDTH,
                  WIDTH * sizeof(*video)) == 0);
    }
    /* Nothing is written without a scratch surface to compose into. */
    for (int at = 0; at < WIDTH * HEIGHT; at++) video[at] = picture;
    psp_ui_media_composite_8888(
        &media, NULL, video, WIDTH, HEIGHT, WIDTH, NULL);
    for (int at = 0; at < WIDTH * HEIGHT; at++) CHECK(video[at] == picture);
    return true;
}

static bool test_media_buffering_transition_frames_are_stable(void)
{
    enum { WIDTH = 480, HEIGHT = 272 };
    static uint32_t video[WIDTH * HEIGHT];
    static uint32_t stable_pill[96 * 16];
    static uint16_t scratch[WIDTH * HEIGHT];
    PspUiMediaState media;
    psp_ui_media_init(&media);
    psp_ui_media_set(&media, true, true, false,
                     UINT64_C(5000000), UINT64_C(20000000), "Example video");
    media.controls_visible = false;

    const uint32_t frames[] = {
        UINT32_C(0xff1010f0), UINT32_C(0xff20e020),
        UINT32_C(0xffe03030), UINT32_C(0xffd0d010)
    };
    /* The off frame is a true pass-through: ending a short refill must not
       leave a stale badge or briefly reveal the full control bar. */
    for (size_t at = 0; at < sizeof(video) / sizeof(video[0]); at++)
        video[at] = frames[0];
    psp_ui_media_set_buffering(&media, false, UINT64_C(9000000));
    psp_ui_media_composite_8888(
        &media, NULL, video, WIDTH, HEIGHT, WIDTH, scratch);
    CHECK(!media.controls_visible);
    for (size_t at = 0; at < sizeof(video) / sizeof(video[0]); at++)
        CHECK(video[at] == frames[0]);

    /* Capture two consecutive buffering frames over maximally different
       pictures. The opaque interior is chrome, not translucent video, and
       the bottom player controls remain absent byte-for-byte. */
    for (unsigned frame = 1; frame <= 2; frame++) {
        for (size_t at = 0; at < sizeof(video) / sizeof(video[0]); at++)
            video[at] = frames[frame];
        psp_ui_media_set_buffering(&media, true, UINT64_C(9000000));
        psp_ui_media_composite_8888(
            &media, NULL, video, WIDTH, HEIGHT, WIDTH, scratch);
        CHECK(!media.controls_visible);
        for (int y = HEIGHT - 78; y < HEIGHT; y++) {
            for (int x = 0; x < WIDTH; x++)
                CHECK(video[(size_t) y * WIDTH + x] == frames[frame]);
        }
        for (int y = 0; y < 16; y++) {
            const uint32_t *source = video
                + (size_t) (HEIGHT / 2 - 8 + y) * WIDTH
                + WIDTH / 2 - 48;
            uint32_t *saved = stable_pill + (size_t) y * 96u;
            if (frame == 1)
                memcpy(saved, source, 96u * sizeof(*saved));
            else
                CHECK(memcmp(saved, source, 96u * sizeof(*saved)) == 0);
        }
    }

    /* A second true->false transition catches stale dirty-band state rather
       than validating only the first opening of the badge. */
    for (size_t at = 0; at < sizeof(video) / sizeof(video[0]); at++)
        video[at] = frames[3];
    psp_ui_media_set_buffering(&media, false, UINT64_C(9000000));
    psp_ui_media_composite_8888(
        &media, NULL, video, WIDTH, HEIGHT, WIDTH, scratch);
    CHECK(!media.controls_visible);
    for (size_t at = 0; at < sizeof(video) / sizeof(video[0]); at++)
        CHECK(video[at] == frames[3]);
    return true;
}

/*
 * A pair of flat colours catches ordinary alpha blending, but decoded video
 * is spatially noisy and changes underneath different glyph pixels every
 * frame.  Sweep a short deterministic motion sequence and require the two
 * opaque control bands to remain byte-identical.  This is the host analogue
 * of watching the player chrome for shimmer on the physical LCD.
 */
static bool test_media_chrome_is_stable_across_motion_sequence(void)
{
    enum {
        WIDTH = 480,
        HEIGHT = 272,
        TITLE_HEIGHT = 42,
        CONTROL_HEIGHT = 78,
        FRAMES = 24
    };
    static uint32_t video[WIDTH * HEIGHT];
    static uint16_t scratch[WIDTH * HEIGHT];
    static uint32_t title[WIDTH * TITLE_HEIGHT];
    static uint32_t controls[WIDTH * CONTROL_HEIGHT];
    PspUiMediaState media;
    psp_ui_media_init(&media);
    psp_ui_media_set(&media, true, true, false,
                     UINT64_C(5000000), UINT64_C(20000000),
                     "Motion stability");
    psp_ui_media_show_controls(&media);

    for (unsigned frame = 0; frame < FRAMES; frame++) {
        for (int y = 0; y < HEIGHT; y++) {
            for (int x = 0; x < WIDTH; x++) {
                unsigned red = (unsigned) (x * 13 + y * 3 + frame * 47);
                unsigned green = (unsigned) (x * 5 + y * 17 + frame * 29);
                unsigned blue = (unsigned) (x * 19 + y * 7 + frame * 61);
                video[(size_t) y * WIDTH + x] = UINT32_C(0xff000000)
                    | (red & 0xffu) << 16
                    | (green & 0xffu) << 8
                    | (blue & 0xffu);
            }
        }
        psp_ui_media_composite_8888(
            &media, NULL, video, WIDTH, HEIGHT, WIDTH, scratch);
        if (frame == 0) {
            memcpy(title, video, sizeof(title));
            memcpy(controls,
                   video + (size_t) (HEIGHT - CONTROL_HEIGHT) * WIDTH,
                   sizeof(controls));
        } else {
            CHECK(memcmp(title, video, sizeof(title)) == 0);
            CHECK(memcmp(
                controls,
                video + (size_t) (HEIGHT - CONTROL_HEIGHT) * WIDTH,
                sizeof(controls)) == 0);
        }
    }
    return true;
}

int main(void)
{
    if (!test_input_mapping_and_menu()
        || !test_time_based_analog_scroll()
        || !test_focus_hold_repeat()
        || !test_page_dark_transform()
        || !test_battery_clock_policy()
        || !test_device_status_clamp_and_clock_format()
        || !test_wifi_status_bars()
        || !test_legacy_collection_url_classification()
        || !test_native_home_url_classification()
        || !test_tab_placeholder_draws_one_letter()
        || !test_analog_cursor_and_drag()
        || !test_autohide_and_loading()
        || !test_navigation_target_replaces_incumbent_address()
        || !test_clamping_and_bounded_state()
        || !test_panels_draw_the_token_ground()
        || !test_page_chrome_holds_its_edge_over_a_light_page()
        || !test_boot_entrance_becomes_home()
        || !test_cursor_fades_in_and_out()
        || !test_composite_keeps_guards()
        || !test_large_toast_uses_full_psp_width()
        || !test_options_hide_page_loading_indicator()
        || !test_startup_views_are_bounded_and_distinct()
        || !test_chrome_normalizes_common_unicode_punctuation()
        || !test_chrome_retains_bounded_unicode_glyphs()
        || !test_chrome_vocabulary_resolves_in_the_shipped_subset()
        || !test_media_controls_and_composite()
        || !test_media_cancel_closes_from_every_state()
        || !test_media_title_uses_unicode_fallback_font()
        || !test_media_play_control_matches_the_page_overlay()
        || !test_media_status_uses_antialiased_chrome_font()
        || !test_media_overlay_bands_and_the_32_bit_wrapper()
        || !test_media_buffering_transition_frames_are_stable()
        || !test_media_chrome_is_stable_across_motion_sequence()
        || !test_native_surfaces_own_the_panel()
        || !test_home_traversal_and_activation()
        || !test_implicit_cursor_handoff()
        || !test_collections_sections_and_deletes()
        || !test_native_motion_budget()) return 1;
    puts("psp-ui-tests: ok");
    return 0;
}
