#ifndef TILEFINCH_BROWSER_TABS_H
#define TILEFINCH_BROWSER_TABS_H

#include <stdbool.h>
#include <stddef.h>

#include "tilefinch/budget.h"
#include "tilefinch/navigation.h"

#define BROWSER_TAB_LIMIT 5u
#define BROWSER_TAB_HISTORY_LIMIT 4u
#define BROWSER_TAB_LABEL_LIMIT 64u
#define BROWSER_TAB_HIBERNATION_FILE_LIMIT (16u * 1024u)
#define BROWSER_TAB_SESSION_FILE_LIMIT \
    (BROWSER_TAB_LIMIT * (BROWSER_TAB_HIBERNATION_FILE_LIMIT + 32u) + 16u)
#define BROWSER_TAB_FIND_QUERY_LIMIT 96u

typedef struct {
    char url[NAVIGATION_URL_LIMIT];
    char title[NAVIGATION_TITLE_LIMIT];
    int scroll_y;
    int focus_kind;
    size_t focus_index;
} BrowserTabHistoryEntry;

typedef struct {
    BrowserTabHistoryEntry history[BROWSER_TAB_HISTORY_LIMIT];
    size_t history_count;
    size_t history_index;
} BrowserTabSnapshot;

typedef struct BrowserTabs BrowserTabs;

/*
 * Tabs deliberately retain navigation facts, not page graphs. Exactly one
 * BrowserEngine remains live; switching reloads through its shared HTTP cache,
 * cookies, and storage, then restores this bounded history/viewport snapshot.
 */
BrowserTabs *browser_tabs_create(
    Budget *budget, const NavigationSession *initial_navigation);
void browser_tabs_destroy(BrowserTabs *tabs);

size_t browser_tabs_count(const BrowserTabs *tabs);
size_t browser_tabs_active_index(const BrowserTabs *tabs);
const BrowserTabSnapshot *browser_tabs_tab(
    const BrowserTabs *tabs, size_t index);
const BrowserTabSnapshot *browser_tabs_active(const BrowserTabs *tabs);
const char *browser_tabs_title(const BrowserTabs *tabs, size_t index);
bool browser_tabs_hibernated(const BrowserTabs *tabs, size_t index);

bool browser_tabs_capture_active(
    BrowserTabs *tabs, const NavigationSession *navigation);
bool browser_tabs_select(BrowserTabs *tabs, size_t index);
bool browser_tabs_add(
    BrowserTabs *tabs, const char *url, const char *title,
    size_t *created_index);
bool browser_tabs_remove(BrowserTabs *tabs, size_t index);
bool browser_tabs_reset(
    BrowserTabs *tabs, size_t index, const char *url, const char *title);
bool browser_tabs_update_active_document(
    BrowserTabs *tabs, const char *url, const char *title);
const char *browser_tabs_active_find_query(const BrowserTabs *tabs);
bool browser_tabs_set_active_find_query(BrowserTabs *tabs,
                                        const char *query);

/* Exactly one inactive tab may occupy the transactional disk slot. The
   in-memory snapshot is released only after a complete file is durable; a
   failed rehydrate leaves the hibernated slot untouched. */
bool browser_tabs_hibernate(
    BrowserTabs *tabs, size_t index, const char *path);
bool browser_tabs_rehydrate(
    BrowserTabs *tabs, size_t index, const char *path);
/* Session snapshots contain only bounded navigation facts. They are written
   transactionally on controlled persistence points and never read unless the
   user's existing restore-last-page option is enabled. */
bool browser_tabs_save_session(
    const BrowserTabs *tabs, const char *path, const char *hibernation_path);
bool browser_tabs_restore_session(BrowserTabs *tabs, const char *path);
size_t browser_tabs_resident_bytes(const BrowserTabs *tabs);

bool browser_tab_history_records(
    const BrowserTabSnapshot *tab,
    NavigationHistoryRecord records[BROWSER_TAB_HISTORY_LIMIT],
    size_t *count, size_t *current_index);
size_t browser_tabs_owned_bytes(void);

#endif
