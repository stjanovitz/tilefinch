#include "psp_ui_menu.h"

#include "tilefinch/content_blocker.h"
#include "psp_ui_theme.h"

static void menu_open_overlay(PspUiState *ui, PspUiScreen screen)
{
    if (ui == NULL) return;
    ui->screen = screen;
    ui->overlay_animation_frames = PSP_THEME_MOTION_PANEL_FRAMES;
    ui->overlay_motion = 0u;
    ui->chrome_visible = true;
}

static void menu_open_child(PspUiState *ui, PspUiScreen screen)
{
    menu_open_overlay(ui, screen);
    if (ui != NULL) ui->overlay_motion = 1u;
}

static void menu_open_parent(PspUiState *ui, PspUiScreen screen)
{
    menu_open_overlay(ui, screen);
    if (ui != NULL) ui->overlay_motion = 2u;
}

static void menu_close(PspUiState *ui)
{
    if (ui != NULL) ui->screen = (PspUiScreen) ui->base_screen;
}

static bool menu_site_available(const PspUiState *ui)
{
    char site[CONTENT_BLOCKER_HOST_LIMIT];
    return ui != NULL && content_blocker_site_from_url(ui->url, site);
}

static PspUiAction menu_root_action(size_t selection)
{
    static const PspUiAction actions[PSP_UI_MENU_ITEM_COUNT] = {
        PSP_UI_ACTION_HOME,
        PSP_UI_ACTION_NONE, /* Tabs */
        PSP_UI_ACTION_NONE, /* Page tools */
        PSP_UI_ACTION_SHOW_OFFLINE,
        PSP_UI_ACTION_NONE, /* Settings */
        PSP_UI_ACTION_NONE, /* Help */
        PSP_UI_ACTION_EXIT
    };
    return selection < PSP_UI_MENU_ITEM_COUNT
        ? actions[selection] : PSP_UI_ACTION_NONE;
}

static bool menu_handle_escape(
    PspUiState *ui, uint32_t pressed, PspUiIntent *intent)
{
    if ((pressed & PSP_UI_BUTTON_MENU) == 0u) return false;
    ui->cursor_visible = false;
    if (ui->screen == PSP_UI_SCREEN_PAGE
        || psp_ui_screen_is_native_surface(ui->screen)) {
        menu_open_overlay(ui, PSP_UI_SCREEN_MENU);
    } else {
        if (ui->screen == PSP_UI_SCREEN_DIAGNOSTIC_QR)
            intent->action = PSP_UI_ACTION_CLOSE_DIAGNOSTIC_QR;
        else if (ui->screen == PSP_UI_SCREEN_UPDATE_VERSIONS)
            intent->update_versions_closed = true;
        else if (ui->screen == PSP_UI_SCREEN_FIND) {
            psp_ui_clear_find(ui);
            intent->action = PSP_UI_ACTION_FIND_CLOSE;
        }
        if (ui->screen == PSP_UI_SCREEN_PAGE_TOOLS
            || ui->screen == PSP_UI_SCREEN_SITE_CONTROLS
            || ui->screen == PSP_UI_SCREEN_PAGE_INFORMATION)
            ui->menu_selection = UI_MENU_ROW_PAGE_TOOLS;
        else if (ui->screen == PSP_UI_SCREEN_HELP
                 || ui->screen == PSP_UI_SCREEN_HELP_DETAIL
                 || ui->screen == PSP_UI_SCREEN_DIAGNOSTIC_QR)
            ui->menu_selection = UI_MENU_ROW_HELP;
        else if (ui->screen == PSP_UI_SCREEN_OPTIONS
                 || ui->screen == PSP_UI_SCREEN_OPTION_ITEMS
                 || ui->screen == PSP_UI_SCREEN_EXPERIMENTAL_OPTIONS
                 || ui->screen == PSP_UI_SCREEN_GLYPH_OPTIONS
                 || ui->screen == PSP_UI_SCREEN_UPDATE
                 || ui->screen == PSP_UI_SCREEN_UPDATE_VERSIONS
                 || ui->screen == PSP_UI_SCREEN_DATA_OPTIONS)
            ui->menu_selection = UI_MENU_ROW_SETTINGS;
        else if (ui->screen == PSP_UI_SCREEN_TABS)
            ui->menu_selection = UI_MENU_ROW_TABS;
        menu_close(ui);
    }
    intent->visual_changed = true;
    return true;
}

static void menu_update_site_controls(
    PspUiState *ui, uint32_t pressed, PspUiIntent *intent)
{
    if (pressed & PSP_UI_BUTTON_UP) {
        ui->menu_selection = (uint8_t) (
            (ui->menu_selection + UI_SITE_CONTROLS_ITEM_COUNT - 1u)
            % UI_SITE_CONTROLS_ITEM_COUNT);
        intent->visual_changed = true;
    } else if (pressed & PSP_UI_BUTTON_DOWN) {
        ui->menu_selection = (uint8_t) (
            (ui->menu_selection + 1u) % UI_SITE_CONTROLS_ITEM_COUNT);
        intent->visual_changed = true;
    } else if (pressed & PSP_UI_BUTTON_CONFIRM) {
        switch (ui->menu_selection) {
            case 0:
                if (ui->javascript_enabled) {
                    ui->site_javascript_enabled =
                        !ui->site_javascript_enabled;
                    intent->setting.id = PSP_UI_SETTING_SITE_JAVASCRIPT;
                    intent->setting.value.boolean =
                        ui->site_javascript_enabled;
                }
                break;
            case 1:
                if (ui->content_blocker_mode != CONTENT_BLOCKER_OFF
                    && menu_site_available(ui)) {
                    ui->content_blocker_site_allowed =
                        !ui->content_blocker_site_allowed;
                    intent->setting.id =
                        PSP_UI_SETTING_CONTENT_BLOCKER_SITE_ALLOWED;
                    intent->setting.value.boolean =
                        ui->content_blocker_site_allowed;
                }
                break;
            case 2:
                if (menu_site_available(ui)) {
                    ui->cookie_banner_hidden = !ui->cookie_banner_hidden;
                    intent->setting.id =
                        PSP_UI_SETTING_COOKIE_BANNER_HIDDEN;
                    intent->setting.value.boolean =
                        ui->cookie_banner_hidden;
                }
                break;
            case 3:
                ui->reader_site_always = !ui->reader_site_always;
                intent->action = PSP_UI_ACTION_TOGGLE_READER_SITE;
                break;
            case 4:
                ui->third_party_cookie_site_allowed =
                    !ui->third_party_cookie_site_allowed;
                intent->setting.id =
                    PSP_UI_SETTING_THIRD_PARTY_COOKIES_SITE;
                intent->setting.value.boolean =
                    ui->third_party_cookie_site_allowed;
                break;
            case 5:
                ui->mixed_content_site_allowed =
                    !ui->mixed_content_site_allowed;
                intent->setting.id = PSP_UI_SETTING_MIXED_CONTENT_SITE;
                intent->setting.value.boolean =
                    ui->mixed_content_site_allowed;
                break;
        }
        intent->visual_changed = true;
    } else if (pressed & PSP_UI_BUTTON_CANCEL) {
        ui->menu_selection = 5u;
        menu_open_parent(ui, PSP_UI_SCREEN_PAGE_TOOLS);
        intent->visual_changed = true;
    }
}

static void menu_update_page_tools(
    PspUiState *ui, uint32_t pressed, PspUiIntent *intent)
{
    if (pressed & PSP_UI_BUTTON_UP) {
        ui->menu_selection = (uint8_t) (
            (ui->menu_selection + UI_PAGE_TOOLS_ITEM_COUNT - 1u)
            % UI_PAGE_TOOLS_ITEM_COUNT);
        intent->visual_changed = true;
    } else if (pressed & PSP_UI_BUTTON_DOWN) {
        ui->menu_selection = (uint8_t) (
            (ui->menu_selection + 1u) % UI_PAGE_TOOLS_ITEM_COUNT);
        intent->visual_changed = true;
    } else if (pressed & PSP_UI_BUTTON_CONFIRM) {
        switch (ui->menu_selection) {
            case 0: intent->action = PSP_UI_ACTION_OPEN_FIND; break;
            case 1: intent->action = PSP_UI_ACTION_TOGGLE_READER; break;
            case 2: intent->action = PSP_UI_ACTION_TOGGLE_BOOKMARK; break;
            case 3: intent->action = PSP_UI_ACTION_SAVE_FOR_LATER; break;
            case 4: intent->action = PSP_UI_ACTION_SCREENSHOT; break;
            case 5:
                ui->menu_selection = 0u;
                menu_open_child(ui, PSP_UI_SCREEN_SITE_CONTROLS);
                intent->visual_changed = true;
                return;
            case 6:
                menu_open_child(ui, PSP_UI_SCREEN_PAGE_INFORMATION);
                intent->visual_changed = true;
                return;
        }
        ui->menu_selection = UI_MENU_ROW_PAGE_TOOLS;
        menu_close(ui);
        intent->visual_changed = true;
    } else if (pressed & PSP_UI_BUTTON_CANCEL) {
        ui->menu_selection = UI_MENU_ROW_PAGE_TOOLS;
        menu_open_parent(ui, PSP_UI_SCREEN_MENU);
        intent->visual_changed = true;
    }
}

static void menu_update_help(
    PspUiState *ui, uint32_t pressed, PspUiIntent *intent)
{
    if (pressed & PSP_UI_BUTTON_UP) {
        ui->menu_selection = (uint8_t) (
            (ui->menu_selection + UI_HELP_ITEM_COUNT - 1u)
            % UI_HELP_ITEM_COUNT);
        intent->visual_changed = true;
    } else if (pressed & PSP_UI_BUTTON_DOWN) {
        ui->menu_selection = (uint8_t) (
            (ui->menu_selection + 1u) % UI_HELP_ITEM_COUNT);
        intent->visual_changed = true;
    } else if (pressed & PSP_UI_BUTTON_CONFIRM) {
        if (ui->menu_selection == 0u) {
            ui->status[0] = '\0';
            ui->toast_frames = 0u;
            menu_open_child(ui, PSP_UI_SCREEN_DIAGNOSTIC_QR);
        } else {
            menu_open_child(ui, PSP_UI_SCREEN_HELP_DETAIL);
        }
        intent->visual_changed = true;
    } else if (pressed & PSP_UI_BUTTON_CANCEL) {
        ui->menu_selection = UI_MENU_ROW_HELP;
        menu_open_parent(ui, PSP_UI_SCREEN_MENU);
        intent->visual_changed = true;
    }
}

static void menu_update_settings(
    PspUiState *ui, uint32_t pressed, PspUiIntent *intent)
{
    if (pressed & (PSP_UI_BUTTON_UP | PSP_UI_BUTTON_PAGE_UP)) {
        ui->options_group_selection =
            (ui->options_group_selection + UI_SETTINGS_GROUP_COUNT - 1u)
            % UI_SETTINGS_GROUP_COUNT;
        intent->visual_changed = true;
    } else if (pressed & (PSP_UI_BUTTON_DOWN | PSP_UI_BUTTON_PAGE_DOWN)) {
        ui->options_group_selection =
            (ui->options_group_selection + 1u) % UI_SETTINGS_GROUP_COUNT;
        intent->visual_changed = true;
    } else if (pressed & PSP_UI_BUTTON_CONFIRM) {
        if (ui->options_group_selection == 6u) {
            menu_open_child(ui, PSP_UI_SCREEN_EXPERIMENTAL_OPTIONS);
            ui->experimental_options_selection = 0u;
            intent->voice_component_probe_requested = true;
        } else {
            ui->options_selection = psp_ui_menu_option_first_in_group(
                ui->options_group_selection);
            menu_open_child(ui, PSP_UI_SCREEN_OPTION_ITEMS);
        }
        intent->visual_changed = true;
    } else if (pressed & PSP_UI_BUTTON_CANCEL) {
        menu_open_parent(ui, PSP_UI_SCREEN_MENU);
        intent->visual_changed = true;
    }
}

static void menu_update_tabs(
    PspUiState *ui, uint32_t pressed, PspUiIntent *intent)
{
    /* Preserve the pre-extraction global toolbar behavior for this one menu
       screen, which used to be reached after the toolbar branch. */
    if (pressed & PSP_UI_BUTTON_TOOLBAR) {
        ui->cursor_visible = false;
        ui->chrome_visible = !ui->chrome_visible;
        ui->screen = PSP_UI_SCREEN_PAGE;
        intent->visual_changed = true;
        return;
    }
    size_t tab_count = ui->tabs == NULL ? 1u : ui->tabs->count;
    if (tab_count == 0u || tab_count > PSP_UI_TAB_LIMIT) tab_count = 1u;
    bool can_create = ui->tabs != NULL && ui->tabs->can_create
        && tab_count < PSP_UI_TAB_LIMIT;
    size_t row_count = tab_count + (can_create ? 1u : 0u);
    if (pressed & (PSP_UI_BUTTON_UP | PSP_UI_BUTTON_LEFT)) {
        ui->tab_selection = (uint8_t) (
            (ui->tab_selection + row_count - 1u) % row_count);
        intent->visual_changed = true;
    } else if (pressed & (PSP_UI_BUTTON_DOWN | PSP_UI_BUTTON_RIGHT)) {
        ui->tab_selection = (uint8_t) (
            (ui->tab_selection + 1u) % row_count);
        intent->visual_changed = true;
    } else if (pressed & PSP_UI_BUTTON_CONFIRM) {
        menu_close(ui);
        if (ui->tab_selection < tab_count) {
            intent->action = PSP_UI_ACTION_SWITCH_TAB;
            intent->tab_index = ui->tab_selection;
        } else {
            intent->action = PSP_UI_ACTION_NEW_TAB;
        }
        intent->visual_changed = true;
    } else if (pressed & PSP_UI_BUTTON_RELOAD) {
        if (ui->tab_selection < tab_count && tab_count > 1u) {
            intent->action = PSP_UI_ACTION_CLOSE_TAB;
            intent->tab_index = ui->tab_selection;
            menu_close(ui);
        }
        intent->visual_changed = true;
    } else if (pressed & PSP_UI_BUTTON_CANCEL) {
        menu_open_parent(ui, PSP_UI_SCREEN_MENU);
        intent->visual_changed = true;
    }
}

static void menu_update_root(
    PspUiState *ui, uint32_t pressed, PspUiIntent *intent)
{
    /* These two shortcuts preceded the root-menu branch before extraction. */
    if (pressed & PSP_UI_BUTTON_TOOLBAR) {
        ui->cursor_visible = false;
        ui->chrome_visible = !ui->chrome_visible;
        ui->screen = PSP_UI_SCREEN_PAGE;
        intent->visual_changed = true;
        return;
    }
    if (pressed & PSP_UI_BUTTON_ADDRESS) {
        ui->cursor_visible = false;
        menu_close(ui);
        ui->chrome_visible = true;
        intent->action = PSP_UI_ACTION_OPEN_ADDRESS;
        intent->visual_changed = true;
        return;
    }
    if (pressed & (PSP_UI_BUTTON_UP | PSP_UI_BUTTON_LEFT)) {
        ui->menu_selection = (uint8_t) (
            (ui->menu_selection + PSP_UI_MENU_ITEM_COUNT - 1u)
            % PSP_UI_MENU_ITEM_COUNT);
        intent->visual_changed = true;
    } else if (pressed & (PSP_UI_BUTTON_DOWN | PSP_UI_BUTTON_RIGHT)) {
        ui->menu_selection = (uint8_t) (
            (ui->menu_selection + 1u) % PSP_UI_MENU_ITEM_COUNT);
        intent->visual_changed = true;
    } else if (pressed & PSP_UI_BUTTON_CONFIRM) {
        if (ui->menu_selection == UI_MENU_ROW_TABS) {
            menu_open_child(ui, PSP_UI_SCREEN_TABS);
            ui->tab_selection = ui->tabs == NULL ? 0u : ui->tabs->active_index;
        } else if (ui->menu_selection == UI_MENU_ROW_PAGE_TOOLS) {
            ui->menu_selection = 0u;
            menu_open_child(ui, PSP_UI_SCREEN_PAGE_TOOLS);
        } else if (ui->menu_selection == UI_MENU_ROW_SETTINGS) {
            menu_open_child(ui, PSP_UI_SCREEN_OPTIONS);
            ui->options_group_selection = 0u;
        } else if (ui->menu_selection == UI_MENU_ROW_HELP) {
            ui->menu_selection = 0u;
            menu_open_child(ui, PSP_UI_SCREEN_HELP);
        } else {
            menu_close(ui);
            intent->action = menu_root_action(ui->menu_selection);
        }
        intent->visual_changed = true;
    } else if (pressed & PSP_UI_BUTTON_CANCEL) {
        menu_close(ui);
        intent->visual_changed = true;
    }
}

bool psp_ui_menu_update(
    PspUiState *ui, uint32_t pressed, PspUiIntent *intent)
{
    if (ui == NULL || intent == NULL) return false;
    if (menu_handle_escape(ui, pressed, intent)) return true;
    switch (ui->screen) {
        case PSP_UI_SCREEN_PAGE_INFORMATION:
            if (pressed & PSP_UI_BUTTON_CANCEL) {
                ui->menu_selection = 6u;
                menu_open_parent(ui, PSP_UI_SCREEN_PAGE_TOOLS);
                intent->visual_changed = true;
            }
            return true;
        case PSP_UI_SCREEN_HELP_DETAIL:
            if (pressed & PSP_UI_BUTTON_CANCEL) {
                menu_open_parent(ui, PSP_UI_SCREEN_HELP);
                intent->visual_changed = true;
            }
            return true;
        case PSP_UI_SCREEN_SITE_CONTROLS:
            menu_update_site_controls(ui, pressed, intent);
            return true;
        case PSP_UI_SCREEN_PAGE_TOOLS:
            menu_update_page_tools(ui, pressed, intent);
            return true;
        case PSP_UI_SCREEN_HELP:
            menu_update_help(ui, pressed, intent);
            return true;
        case PSP_UI_SCREEN_OPTIONS:
            menu_update_settings(ui, pressed, intent);
            return true;
        case PSP_UI_SCREEN_TABS:
            menu_update_tabs(ui, pressed, intent);
            return true;
        case PSP_UI_SCREEN_MENU:
            menu_update_root(ui, pressed, intent);
            return true;
        default:
            return false;
    }
}
