#ifndef TILEFINCH_PSP_UI_MENU_H
#define TILEFINCH_PSP_UI_MENU_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "tilefinch/psp_ui.h"

/* Internal menu geometry and routing constants. They live beside the
   controller rather than in the public device-facing UI contract. */
#define UI_MENU_VISIBLE_ROWS 7u
#define UI_MENU_ROW_HOME 0u
#define UI_MENU_ROW_TABS 1u
#define UI_MENU_ROW_PAGE_TOOLS 2u
#define UI_MENU_ROW_LIBRARY 3u
#define UI_MENU_ROW_SETTINGS 4u
#define UI_MENU_ROW_HELP 5u
#define UI_MENU_ROW_EXIT 6u
#define UI_PAGE_TOOLS_ITEM_COUNT 7u
#define UI_SITE_CONTROLS_ITEM_COUNT 6u
#define UI_HELP_ITEM_COUNT 5u
#define UI_SETTINGS_GROUP_COUNT 7u

/* Returns true only for screens whose input authority belongs to the menu
   controller. The global Menu button is also routed here from any screen. */
static inline bool psp_ui_menu_owns_screen(PspUiScreen screen)
{
    switch (screen) {
        case PSP_UI_SCREEN_MENU:
        case PSP_UI_SCREEN_PAGE_TOOLS:
        case PSP_UI_SCREEN_SITE_CONTROLS:
        case PSP_UI_SCREEN_PAGE_INFORMATION:
        case PSP_UI_SCREEN_HELP:
        case PSP_UI_SCREEN_HELP_DETAIL:
        case PSP_UI_SCREEN_OPTIONS:
        case PSP_UI_SCREEN_TABS:
            return true;
        default:
            return false;
    }
}
bool psp_ui_menu_update(
    PspUiState *ui, uint32_t pressed, PspUiIntent *intent);

/* Settings metadata still drives the existing painter and detailed option
   executor. Exposing this single query lets the category controller open the
   correct row without duplicating the option-order table. */
size_t psp_ui_menu_option_first_in_group(size_t group);

#endif
