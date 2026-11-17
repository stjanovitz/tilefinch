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
#define UI_PAGE_TOOLS_ITEM_COUNT 9u
static inline size_t psp_ui_page_tools_count(const PspUiState *ui)
{
    return UI_PAGE_TOOLS_ITEM_COUNT + (ui->youtube_signin_continue ? 1u : 0u)
        + (ui->microphone_live ? 1u : 0u)
        + (ui->basic_default_page ? 1u : 0u);
}
/* The page tools row ROW stands for: the fixed rows, with Try full page
   right after Basic view on a page opened in Basic view by default, then
   Continue to YouTube and Stop microphone when they show. */
#define UI_PAGE_TOOLS_ROW_YOUTUBE UI_PAGE_TOOLS_ITEM_COUNT
#define UI_PAGE_TOOLS_ROW_MICROPHONE (UI_PAGE_TOOLS_ITEM_COUNT + 1u)
#define UI_PAGE_TOOLS_ROW_FULL_PAGE (UI_PAGE_TOOLS_ITEM_COUNT + 2u)
#define UI_PAGE_TOOLS_ROW_LIMIT (UI_PAGE_TOOLS_ITEM_COUNT + 3u)
/* Display row of Try full page: the one after Basic view. */
#define UI_PAGE_TOOLS_FULL_PAGE_AT 3u
static inline size_t psp_ui_page_tools_row(const PspUiState *ui, size_t row)
{
    if (ui->basic_default_page) {
        if (row == UI_PAGE_TOOLS_FULL_PAGE_AT)
            return UI_PAGE_TOOLS_ROW_FULL_PAGE;
        if (row > UI_PAGE_TOOLS_FULL_PAGE_AT) row--;
    }
    if (row == UI_PAGE_TOOLS_ITEM_COUNT && !ui->youtube_signin_continue)
        return UI_PAGE_TOOLS_ROW_MICROPHONE;
    return row;
}
#define UI_SITE_CONTROLS_ITEM_COUNT 10u
#define UI_HELP_ITEM_COUNT 6u
#define UI_SETTINGS_GROUP_COUNT 7u
#define UI_SITE_INFO_ACTION_COUNT 4u
#define UI_SITE_INFO_ROW_SITE_STORAGE 1u
/* data_clear_confirmation while "Exit to XMB" waits for its second X. No
   screen's row confirmations reach this value. */
#define UI_MENU_EXIT_CONFIRMATION 0xE0u

/* Returns true only for screens whose input authority belongs to the menu
   controller. The global Menu button is also routed here from any screen. */
static inline bool psp_ui_menu_owns_screen(PspUiScreen screen)
{
    switch (screen) {
        case PSP_UI_SCREEN_MENU:
        case PSP_UI_SCREEN_PAGE_TOOLS:
        case PSP_UI_SCREEN_SITE_CONTROLS:
        case PSP_UI_SCREEN_PAGE_INFORMATION:
        case PSP_UI_SCREEN_OFFLINE_APP_PREVIEW:
        case PSP_UI_SCREEN_FILE_DOWNLOAD:
        case PSP_UI_SCREEN_ZIP_INSTALL:
        case PSP_UI_SCREEN_FAILURE_RECOVERY:
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

/* The painter and controller consume the same ordered recovery choices.
   A null labels array asks only for the bounded row count. */
size_t psp_ui_menu_failure_labels(
    const PspUiState *ui, const char **labels, size_t capacity);

#endif
