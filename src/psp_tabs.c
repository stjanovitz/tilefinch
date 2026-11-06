#include "tilefinch/psp_tabs.h"
#include "tilefinch/site_adapter.h"

#include <stdio.h>
#include <string.h>

#ifdef __PSP__
#include "tilefinch/psp_log.h"
#define printf psp_log_printf
#endif

#define PSP_TAB_INDEX_NONE ((size_t) -1)

bool psp_tabs_document_visible(const PspUiState *ui)
{
    /* Menus preserve base_screen: opening one over Home must not make the
       hidden backing document authoritative for a retained tab. */
    return ui != NULL && !ui->captive_portal_active
        && !psp_ui_screen_is_native_surface((PspUiScreen) ui->base_screen);
}

static const BrowserTabHistoryEntry *psp_tabs_target_entry(
    const BrowserTabs *tabs)
{
    const BrowserTabSnapshot *tab = browser_tabs_active(tabs);
    if (tab == NULL || tab->history_count == 0
        || tab->history_index >= tab->history_count) return NULL;
    return &tab->history[tab->history_index];
}

PspTabRequestResult psp_tabs_request(
    BrowserTabs *tabs, const NavigationSession *navigation,
    bool capture_current_document,
    PspUiAction action, size_t requested_index, const char *homepage_url,
    const char *hibernation_path, PspTabTransition *transition,
    char destination[NAVIGATION_URL_LIMIT])
{
    if (tabs == NULL || navigation == NULL || transition == NULL
        || destination == NULL || transition->pending) {
        return PSP_TAB_REQUEST_REFUSED;
    }
    if (capture_current_document
        && !browser_tabs_capture_active(tabs, navigation))
        return PSP_TAB_REQUEST_REFUSED;

    size_t previous = browser_tabs_active_index(tabs);
    size_t target = previous;
    size_t close_after = PSP_TAB_INDEX_NONE;
    bool added = false;
    if (action == PSP_UI_ACTION_SWITCH_TAB) {
        if (requested_index >= browser_tabs_count(tabs))
            return PSP_TAB_REQUEST_REFUSED;
        if (requested_index == previous && capture_current_document)
            return PSP_TAB_REQUEST_COMPLETE;
        target = requested_index;
        if (browser_tabs_hibernated(tabs, target)
            && !browser_tabs_rehydrate(
                   tabs, target, hibernation_path))
            return PSP_TAB_REQUEST_HIBERNATION_FAILED;
        if (!browser_tabs_select(tabs, target))
            return PSP_TAB_REQUEST_REFUSED;
    } else if (action == PSP_UI_ACTION_NEW_TAB) {
        if (!browser_tabs_add(
                tabs, homepage_url, "New Tab", &target))
            return PSP_TAB_REQUEST_REFUSED;
        added = true;
    } else if (action == PSP_UI_ACTION_CLOSE_TAB) {
        size_t count = browser_tabs_count(tabs);
        if (requested_index >= count || count <= 1u)
            return PSP_TAB_REQUEST_REFUSED;
        if (requested_index != previous) {
            bool hibernated = browser_tabs_hibernated(
                tabs, requested_index);
            bool removed = browser_tabs_remove(tabs, requested_index);
            if (removed && hibernated) (void) remove(hibernation_path);
            return removed ? PSP_TAB_REQUEST_COMPLETE
                           : PSP_TAB_REQUEST_REFUSED;
        }
        target = previous + 1u < count ? previous + 1u : previous - 1u;
        close_after = previous;
        if (browser_tabs_hibernated(tabs, target)
            && !browser_tabs_rehydrate(
                   tabs, target, hibernation_path))
            return PSP_TAB_REQUEST_HIBERNATION_FAILED;
        if (!browser_tabs_select(tabs, target))
            return PSP_TAB_REQUEST_REFUSED;
    } else {
        return PSP_TAB_REQUEST_REFUSED;
    }

    const BrowserTabHistoryEntry *entry = psp_tabs_target_entry(tabs);
    if (entry == NULL || entry->url[0] == '\0') {
        if (added) (void) browser_tabs_remove(tabs, target);
        (void) browser_tabs_select(tabs, previous);
        return PSP_TAB_REQUEST_REFUSED;
    }
    snprintf(destination, NAVIGATION_URL_LIMIT, "%s", entry->url);
    *transition = (PspTabTransition) {
        .pending = true,
        .added_target = added,
        .hibernate_previous_after_success = added,
        .previous_index = previous,
        .target_index = target,
        .close_after_success = close_after
    };
    return PSP_TAB_REQUEST_LOAD;
}

bool psp_tabs_finish(
    BrowserTabs *tabs, BrowserEngine *engine,
    PspTabTransition *transition, bool succeeded,
    bool hibernation_enabled, const char *hibernation_path)
{
    if (tabs == NULL || engine == NULL || transition == NULL
        || !transition->pending) return false;

    if (!succeeded) {
        if (transition->added_target) {
            (void) browser_tabs_remove(tabs, transition->target_index);
        }
        (void) browser_tabs_select(tabs, transition->previous_index);
        memset(transition, 0, sizeof(*transition));
        return true;
    }

    BrowserFrontendSnapshot snapshot = {0};
    (void) browser_engine_frontend_snapshot(engine, &snapshot);
    const NavigationSession *navigation = snapshot.navigation;
    const char *url = navigation == NULL
        ? NULL : navigation->page.document_url;
    const char *title = navigation == NULL
        ? NULL : navigation->page.document.title;
    if (url == NULL || url[0] == '\0') {
        const BrowserTabHistoryEntry *entry =
            psp_tabs_target_entry(tabs);
        url = entry == NULL ? NULL : entry->url;
    }
    if (title == NULL) title = "";
    if (!browser_tabs_update_active_document(tabs, url, title)) {
        memset(transition, 0, sizeof(*transition));
        return false;
    }

    const BrowserTabSnapshot *tab = browser_tabs_active(tabs);
    NavigationHistoryRecord records[BROWSER_TAB_HISTORY_LIMIT];
    size_t count = 0;
    size_t current = 0;
    if (!browser_tab_history_records(tab, records, &count, &current)
        || !browser_engine_replace_history(
               engine, records, count, current)) {
        /*
         * The page commit is already authoritative. Do not leave the tab
         * controller wedged in a pending transition if the small history
         * copy is refused under extreme memory pressure; retain a truthful
         * one-entry tab and let the user continue from the committed page.
         */
        size_t active = browser_tabs_active_index(tabs);
        (void) browser_tabs_reset(tabs, active, url, title);
        if (transition->close_after_success != PSP_TAB_INDEX_NONE)
            (void) browser_tabs_remove(
                tabs, transition->close_after_success);
        memset(transition, 0, sizeof(*transition));
        return false;
    }
    const BrowserTabHistoryEntry *entry =
        &tab->history[tab->history_index];
    (void) browser_engine_restore_view(
        engine, entry->scroll_y, entry->focus_kind, entry->focus_index);
    if (transition->close_after_success != PSP_TAB_INDEX_NONE) {
        (void) browser_tabs_remove(
            tabs, transition->close_after_success);
    }
    (void) browser_engine_frontend_snapshot(engine, &snapshot);
    navigation = snapshot.navigation;
    (void) browser_tabs_capture_active(tabs, navigation);
    if (hibernation_enabled
        && transition->hibernate_previous_after_success
        && transition->previous_index < browser_tabs_count(tabs)
        && transition->previous_index != browser_tabs_active_index(tabs)) {
        bool hibernated = browser_tabs_hibernate(
            tabs, transition->previous_index, hibernation_path);
        printf(
            "tilefinch-tabs: hibernate index=%zu result=%s resident=%zuB\n",
            transition->previous_index, hibernated ? "ok" : "kept-live",
            browser_tabs_resident_bytes(tabs));
    }
    memset(transition, 0, sizeof(*transition));
    return true;
}

bool psp_tabs_finish_native_home(
    BrowserTabs *tabs, PspTabTransition *transition,
    bool hibernation_enabled, const char *hibernation_path)
{
    if (tabs == NULL || transition == NULL || !transition->pending)
        return false;
    if (!browser_tabs_update_active_document(
            tabs, TILEFINCH_HOMEPAGE_URL, "Home")) {
        if (transition->added_target)
            (void) browser_tabs_remove(tabs, transition->target_index);
        (void) browser_tabs_select(tabs, transition->previous_index);
        memset(transition, 0, sizeof(*transition));
        return false;
    }
    if (transition->close_after_success != PSP_TAB_INDEX_NONE) {
        (void) browser_tabs_remove(
            tabs, transition->close_after_success);
    }
    if (hibernation_enabled
        && transition->hibernate_previous_after_success
        && transition->previous_index < browser_tabs_count(tabs)
        && transition->previous_index != browser_tabs_active_index(tabs)) {
        bool hibernated = browser_tabs_hibernate(
            tabs, transition->previous_index, hibernation_path);
        printf(
            "tilefinch-tabs: hibernate index=%zu result=%s resident=%zuB\n",
            transition->previous_index, hibernated ? "ok" : "kept-live",
            browser_tabs_resident_bytes(tabs));
    }
    memset(transition, 0, sizeof(*transition));
    return true;
}

