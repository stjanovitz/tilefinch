#ifndef TILEFINCH_PSP_TABS_H
#define TILEFINCH_PSP_TABS_H

#include "tilefinch/browser_engine.h"
#include "tilefinch/browser_tabs.h"
#include "tilefinch/psp_ui.h"

/* A pending selection owns facts about the previous and target tabs, not
   the live document. Native Home may show a retained tab with no matching
   document loaded, so selecting that same tab can still require navigation. */
typedef struct {
    bool pending;
    bool added_target;
    bool hibernate_previous_after_success;
    size_t previous_index;
    size_t target_index;
    size_t close_after_success;
} PspTabTransition;

typedef enum {
    PSP_TAB_REQUEST_REFUSED = 0,
    PSP_TAB_REQUEST_CAPTURE_FAILED,
    PSP_TAB_REQUEST_HIBERNATION_FAILED,
    PSP_TAB_REQUEST_COMPLETE,
    PSP_TAB_REQUEST_LOAD
} PspTabRequestResult;

bool psp_tabs_document_visible(const PspUiState *ui);
PspTabRequestResult psp_tabs_request(
    BrowserTabs *tabs, BrowserEngine *engine,
    bool capture_current_document, PspUiAction action, size_t requested_index,
    const char *homepage_url, const char *hibernation_path,
    PspTabTransition *transition, char destination[NAVIGATION_URL_LIMIT]);
bool psp_tabs_finish(
    BrowserTabs *tabs, BrowserEngine *engine, PspTabTransition *transition,
    bool succeeded, bool hibernation_enabled, const char *hibernation_path);
/* The one boot notice for a profile or tab session that could not be read
   (saving is then refused, browser_profile_save_blocked), or NULL. */
#define PSP_TABS_PROFILE_UNREAD_NOTICE "SETTINGS UNREADABLE - NOT SAVING"
#define PSP_TABS_SESSION_UNREAD_NOTICE "LAST TABS UNREADABLE - NOT SAVING"
_Static_assert(sizeof(PSP_TABS_SESSION_UNREAD_NOTICE) - 1u
                   <= PSP_UI_LARGE_TOAST_CHARACTER_LIMIT,
               "the unread notice must fit the large PSP toast");
const char *psp_tabs_unread_notice(const BrowserProfile *profile,
                                   const BrowserTabs *tabs);
bool psp_tabs_restore_startup(BrowserTabs *tabs, BrowserEngine *engine,
                              const char *path);
bool psp_tabs_finish_native_home(
    BrowserTabs *tabs, BrowserEngine *engine, PspTabTransition *transition, bool hibernation_enabled,
    const char *hibernation_path);

#endif
