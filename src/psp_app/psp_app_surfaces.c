/* Native chrome surfaces for the PSP browser: tab rows and thumbnails plus
 * the HOME and COLLECTIONS row backings. Their shared domain formatter stays
 * private to this translation unit.
 */
#include "psp_app_internal.h"

#define PSP_TAB_INDEX_NONE ((size_t) -1)



static void psp_tabs_domain(
    const char *url, char *domain, size_t capacity)
{
    if (domain == NULL || capacity == 0) return;
    domain[0] = '\0';
    if (url == NULL) return;
    const char *at = url;
    if (strncmp(at, "https://", 8u) == 0) at += 8;
    else if (strncmp(at, "http://", 7u) == 0) at += 7;
    else if (strncmp(at, "tilefinch://", 12u) == 0) {
        snprintf(domain, capacity, "%s", "Tilefinch");
        return;
    }
    const char *end = at;
    while (*end != '\0' && *end != '/' && *end != '?' && *end != '#')
        end++;
    size_t length = (size_t) (end - at);
    if (length >= capacity) length = capacity - 1u;
    memcpy(domain, at, length);
    domain[length] = '\0';
}

void psp_tabs_sync_ui(
    PspUiState *ui, const BrowserTabs *tabs, PspUiTabsView *view)
{
    if (ui == NULL || view == NULL) return;
    view->count = 0;
    view->active_index = 0;
    view->hibernated_mask = 0;
    view->can_create = false;
    memset(view->titles, 0, sizeof(view->titles));
    memset(view->domains, 0, sizeof(view->domains));
    if (tabs == NULL) {
        view->count = 1;
        snprintf(view->titles[0], sizeof(view->titles[0]), "%.*s",
                 (int) sizeof(view->titles[0]) - 1,
                 ui->title[0] == '\0' ? "CURRENT PAGE" : ui->title);
        psp_tabs_domain(ui->url, view->domains[0], sizeof(view->domains[0]));
    } else {
        size_t count = browser_tabs_count(tabs);
        if (count > PSP_UI_TAB_LIMIT) count = PSP_UI_TAB_LIMIT;
        view->count = (uint8_t) count;
        view->active_index = (uint8_t) browser_tabs_active_index(tabs);
        view->can_create = count < BROWSER_TAB_LIMIT;
        for (size_t index = 0; index < count; index++) {
            const BrowserTabSnapshot *tab =
                browser_tabs_tab(tabs, index);
            const char *title = browser_tabs_title(tabs, index);
            if (browser_tabs_hibernated(tabs, index))
                view->hibernated_mask |= (uint8_t) (1u << index);
            if (tab != NULL && tab->history_count != 0
                && tab->history_index < tab->history_count
                && tab->history[tab->history_index].title[0] != '\0') {
                title = tab->history[tab->history_index].title;
            }
            const char *url = tab != NULL && tab->history_count != 0
                    && tab->history_index < tab->history_count
                ? tab->history[tab->history_index].url : NULL;
            snprintf(view->titles[index], sizeof(view->titles[index]),
                     "%s", title == NULL || title[0] == '\0'
                                ? "UNTITLED" : title);
            psp_tabs_domain(
                url, view->domains[index], sizeof(view->domains[index]));
        }
    }
    psp_ui_set_tabs(ui, view);
}

void psp_tabs_capture_thumbnail(
    PspUiTabsView *view, size_t index, const uint16_t *frame,
    int width, int height, int stride)
{
    if (view == NULL || index >= PSP_UI_TAB_LIMIT || frame == NULL
        || width <= 0 || height <= 0 || stride < width) return;
    uint16_t *thumbnail = view->thumbnails[index];
    for (int y = 0; y < PSP_UI_TAB_THUMBNAIL_HEIGHT; y++) {
        int source_y = y * height / PSP_UI_TAB_THUMBNAIL_HEIGHT;
        for (int x = 0; x < PSP_UI_TAB_THUMBNAIL_WIDTH; x++) {
            int source_x = x * width / PSP_UI_TAB_THUMBNAIL_WIDTH;
            thumbnail[(size_t) y * PSP_UI_TAB_THUMBNAIL_WIDTH
                      + (size_t) x] =
                frame[(size_t) source_y * (size_t) stride
                      + (size_t) source_x];
        }
    }
    view->thumbnail_valid_mask |= (uint8_t) (1u << index);
}

void psp_tabs_remove_thumbnail(PspUiTabsView *view, size_t index)
{
    if (view == NULL || index >= PSP_UI_TAB_LIMIT) return;
    for (size_t at = index; at + 1u < PSP_UI_TAB_LIMIT; at++) {
        memcpy(
            view->thumbnails[at], view->thumbnails[at + 1u],
            sizeof(view->thumbnails[at]));
    }
    memset(
        view->thumbnails[PSP_UI_TAB_LIMIT - 1u], 0,
        sizeof(view->thumbnails[0]));
    uint8_t lower = (uint8_t) (
        view->thumbnail_valid_mask & ((1u << index) - 1u));
    uint8_t upper = (uint8_t) (
        (view->thumbnail_valid_mask >> (index + 1u)) << index);
    view->thumbnail_valid_mask = (uint8_t) (lower | upper);
}

void psp_tabs_invalidate_thumbnail(
    PspUiTabsView *view, size_t index)
{
    if (view == NULL || index >= PSP_UI_TAB_LIMIT) return;
    view->thumbnail_valid_mask &= (uint8_t) ~(1u << index);
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
        if (requested_index == previous)
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

/*
 * Frontend backing for the native chrome surfaces.
 *
 * The UI layer holds only presentation strings; every row here also records
 * where it came from, so activating one resolves against live profile, tab,
 * and library state instead of a URL copied at refresh time. Nothing in this
 * file allocates, and none of it runs per frame: the views are rebuilt only
 * when the data behind them changes.
 */




static const struct {
    const char *label;
    const char *detail;
    const char *url;
} psp_home_builtin[] = {
    {"WIKIPEDIA", "Articles and search", "https://en.wikipedia.org/"},
    {"YOUTUBE", "Browse and play video", "https://m.youtube.com/"}
};

#define PSP_HOME_BUILTIN_COUNT \
    (sizeof(psp_home_builtin) / sizeof(psp_home_builtin[0]))

static void psp_home_set_row(
    PspHomeSurface *surface, size_t row, PspHomeTargetKind kind,
    size_t target)
{
    if (surface == NULL || row >= PSP_HOME_ROW_LIMIT) return;
    surface->kind[row] = (uint8_t) kind;
    surface->index[row] = (uint8_t) target;
}

static bool psp_home_local_url(const char *url)
{
    static const char local[] = "https://tilefinch.local/";
    return url == NULL || url[0] == '\0'
        || strncmp(url, local, sizeof(local) - 1u) == 0
        || strncmp(url, "tilefinch://", 12u) == 0;
}

/*
 * Rebuild both halves of HOME. Tiles are the profile's bookmarks when it has
 * any and the two built-in cards when it does not. There is deliberately no
 * SEARCH tile: Start already opens the omnibox and the hint line says so, so
 * a tile would spend one of six slots on an affordance the surface already
 * advertises. CONTINUE mirrors the open tabs, skipping chrome pages, which
 * are not documents a user asked for.
 */
void psp_home_sync_ui(
    PspUiState *ui, PspHomeSurface *surface, const BrowserProfile *profile,
    const BrowserTabs *tabs, bool engine_ready)
{
    if (ui == NULL || surface == NULL) return;
    memset(surface, 0, sizeof(*surface));
    surface->view.engine_ready = engine_ready;

    size_t bookmarks = profile == NULL
        ? 0u : browser_profile_bookmark_count(profile);
    size_t tile = 0;
    if (bookmarks == 0) {
        for (size_t at = 0;
             at < PSP_HOME_BUILTIN_COUNT && tile < PSP_UI_HOME_TILE_LIMIT;
             at++) {
            snprintf(surface->view.tiles[tile].label,
                     sizeof(surface->view.tiles[tile].label), "%s",
                     psp_home_builtin[at].label);
            snprintf(surface->view.tiles[tile].detail,
                     sizeof(surface->view.tiles[tile].detail), "%s",
                     psp_home_builtin[at].detail);
            psp_home_set_row(surface, tile, PSP_HOME_TARGET_BUILTIN, at);
            tile++;
        }
    } else {
        for (size_t at = 0;
             at < bookmarks && tile < PSP_UI_HOME_TILE_LIMIT; at++) {
            const BrowserProfilePage *page =
                browser_profile_bookmark(profile, at);
            if (page == NULL || page->url[0] == '\0') continue;
            char domain[PSP_UI_HOME_DETAIL_CAPACITY];
            psp_tabs_domain(page->url, domain, sizeof(domain));
            snprintf(surface->view.tiles[tile].label,
                     sizeof(surface->view.tiles[tile].label), "%.*s",
                     (int) sizeof(surface->view.tiles[tile].label) - 1,
                     page->title[0] == '\0' ? domain : page->title);
            snprintf(surface->view.tiles[tile].detail,
                     sizeof(surface->view.tiles[tile].detail), "%s", domain);
            psp_home_set_row(surface, tile, PSP_HOME_TARGET_BOOKMARK, at);
            tile++;
        }
    }
    surface->view.tile_count = (uint8_t) tile;

    size_t entries = 0;
    size_t tab_count = tabs == NULL ? 0u : browser_tabs_count(tabs);
    for (size_t at = 0;
         at < tab_count && entries < PSP_UI_HOME_CONTINUE_LIMIT; at++) {
        const BrowserTabSnapshot *tab = browser_tabs_tab(tabs, at);
        const char *url = tab != NULL && tab->history_count != 0
                && tab->history_index < tab->history_count
            ? tab->history[tab->history_index].url : NULL;
        if (psp_home_local_url(url)) continue;
        const char *title = browser_tabs_title(tabs, at);
        char domain[PSP_UI_HOME_DETAIL_CAPACITY];
        psp_tabs_domain(url, domain, sizeof(domain));
        snprintf(surface->view.continues[entries].label,
                 sizeof(surface->view.continues[entries].label), "%.*s",
                 (int) sizeof(surface->view.continues[entries].label) - 1,
                 title == NULL || title[0] == '\0' ? domain : title);
        snprintf(surface->view.continues[entries].detail,
                 sizeof(surface->view.continues[entries].detail), "%s",
                 domain);
        psp_home_set_row(
            surface, PSP_UI_HOME_TILE_LIMIT + entries,
            PSP_HOME_TARGET_TAB, at);
        entries++;
    }
    surface->view.continue_count = (uint8_t) entries;
    psp_ui_set_home(ui, &surface->view);
}

/*
 * Show native HOME, refreshed against live profile and tab state.
 *
 * Every arrival at HOME from a navigation does the same three things in the
 * same order, and getting one of them wrong is invisible until a stale tile
 * or an out-of-date tab strip shows up on screen. Naming the sequence once
 * is what lets psp_route_native_home below be a one-liner at seven call
 * sites.
 *
 * Deliberately not included: evicting the voice model. Only the submit arm
 * does that, because only there is HOME reached from a path that may have
 * just run the on-screen keyboard; the other arrivals keep the model
 * resident exactly as they did before.
 */
void psp_show_native_home(PspApp *app)
{
    if (app == NULL) return;
    psp_home_sync_ui(&app->process->presentation.ui, &app->process->presentation.home_surface, app->browser->profile, app->browser->tabs, true);
    psp_ui_show_home(&app->process->presentation.ui);
    psp_tabs_sync_ui(&app->process->presentation.ui, app->browser->tabs, &app->process->presentation.tab_view);
}

/*
 * The single gate between "a navigation names the built-in start page" and
 * "the native HOME surface is showing".
 *
 * The engine's start-page generator is gone, so psp_begin_page_load() on
 * that URL resolves a host nothing serves and lands the user on an error
 * page. Seven sites can hand it one: a submitted form or link, history
 * back/forward, reload, a tab open/switch/close destination, a typed
 * address, a bookmark or history row, and the network gate. Two of them
 * used to classify nothing and loaded the URL. Pairing the test with the
 * routing here means an eighth site gets both by calling one function
 * instead of by remembering a rule.
 *
 * Returns true when the surface is now showing, in which case the caller
 * must not start a page load.
 */
bool psp_route_native_home(PspApp *app, const char *url)
{
    if (app == NULL || !psp_ui_native_home_url(url)) return false;
    psp_show_native_home(app);
    return true;
}

/*
 * Rows are indexed tiles-then-CONTINUE, but the parallel target arrays are
 * indexed by slot so a short tile list does not shift the CONTINUE targets.
 */
static size_t psp_home_target_slot(
    const PspHomeSurface *surface, size_t row)
{
    if (surface == NULL) return PSP_HOME_ROW_LIMIT;
    size_t tiles = surface->view.tile_count;
    if (row < tiles) return row;
    size_t entry = row - tiles;
    if (entry >= surface->view.continue_count) return PSP_HOME_ROW_LIMIT;
    return PSP_UI_HOME_TILE_LIMIT + entry;
}

PspHomeTargetKind psp_home_target_kind(
    const PspHomeSurface *surface, size_t row, size_t *target)
{
    size_t slot = psp_home_target_slot(surface, row);
    if (slot >= PSP_HOME_ROW_LIMIT) return PSP_HOME_TARGET_NONE;
    if (target != NULL) *target = surface->index[slot];
    return (PspHomeTargetKind) surface->kind[slot];
}

/*
 * COLLECTIONS. Row strings are borrowed straight from the profile and the
 * offline index, so the surface always shows what those actually hold; only
 * the trailing detail is formatted, and it is formatted into the row.
 */

static void psp_collections_format_bytes(
    char *out, size_t capacity, uint64_t bytes)
{
    if (bytes >= UINT64_C(1048576)) {
        /* The trailing field is 14 bytes; clamp the whole-MB digits so the
           formatted size always fits instead of truncating mid-number. */
        unsigned long long whole = bytes / UINT64_C(1048576);
        if (whole > UINT64_C(99999)) whole = UINT64_C(99999);
        snprintf(out, capacity, "%llu.%llu MB", whole,
                 (unsigned long long)
                     ((bytes % UINT64_C(1048576)) / UINT64_C(104858)));
    } else if (bytes >= 1024u) {
        snprintf(out, capacity, "%llu KB",
                 (unsigned long long) (bytes / 1024u));
    } else {
        snprintf(out, capacity, "%llu B", (unsigned long long) bytes);
    }
}

static const char *psp_collections_offline_state(OfflineItemState state)
{
    switch (state) {
        case OFFLINE_ITEM_QUEUED: return "QUEUED";
        case OFFLINE_ITEM_DOWNLOADING: return "SAVING";
        case OFFLINE_ITEM_PAUSED: return "PAUSED";
        case OFFLINE_ITEM_FAILED: return "FAILED";
        case OFFLINE_ITEM_READY:
        default: return "";
    }
}

void psp_collections_sync_ui(
    PspUiState *ui, PspCollectionsSurface *surface,
    const BrowserProfile *profile, const OfflineLibrary *library,
    PspUiCollectionSection section)
{
    if (ui == NULL || surface == NULL) return;
    memset(surface, 0, sizeof(*surface));
    surface->view.section = (uint8_t) section;
    size_t rows = 0;
    if (section == PSP_UI_COLLECTION_OFFLINE) {
        surface->view.empty_message = "NOTHING SAVED FOR OFFLINE YET";
        size_t count = library == NULL ? 0u : library->count;
        for (size_t at = 0;
             at < count && rows < PSP_UI_COLLECTIONS_ROW_LIMIT; at++) {
            const OfflineLibraryItem *item = &library->items[at];
            surface->view.rows[rows].title = item->title;
            surface->view.rows[rows].detail = item->source_url;
            surface->view.rows[rows].deletable = true;
            const char *state = psp_collections_offline_state(item->state);
            if (state[0] != '\0') {
                snprintf(surface->view.rows[rows].trailing,
                         sizeof(surface->view.rows[rows].trailing), "%s",
                         state);
            } else {
                psp_collections_format_bytes(
                    surface->view.rows[rows].trailing,
                    sizeof(surface->view.rows[rows].trailing),
                    item->content_bytes + item->audio_bytes);
            }
            surface->id[rows] = item->id;
            rows++;
        }
    } else if (section == PSP_UI_COLLECTION_BOOKMARKS) {
        surface->view.empty_message = "NO BOOKMARKS YET";
        size_t count = profile == NULL
            ? 0u : browser_profile_bookmark_count(profile);
        for (size_t at = 0;
             at < count && rows < PSP_UI_COLLECTIONS_ROW_LIMIT; at++) {
            const BrowserProfilePage *page =
                browser_profile_bookmark(profile, at);
            if (page == NULL || page->url[0] == '\0') continue;
            surface->view.rows[rows].title =
                page->title[0] == '\0' ? page->url : page->title;
            surface->view.rows[rows].detail = page->url;
            /* Removing a bookmark is a page verb, not a list verb: the menu
               owns add/remove so one row cannot mean two things. */
            surface->view.rows[rows].deletable = false;
            surface->id[rows] = (uint32_t) at;
            rows++;
        }
    } else {
        surface->view.empty_message = "NO HISTORY YET";
        size_t count = profile == NULL
            ? 0u : browser_profile_history_count(profile);
        for (size_t at = 0;
             at < count && rows < PSP_UI_COLLECTIONS_ROW_LIMIT; at++) {
            const BrowserProfilePage *page =
                browser_profile_history(profile, at);
            if (page == NULL || page->url[0] == '\0') continue;
            surface->view.rows[rows].title =
                page->title[0] == '\0' ? page->url : page->title;
            surface->view.rows[rows].detail = page->url;
            surface->view.rows[rows].deletable = true;
            if (page->visits > 1u)
                snprintf(surface->view.rows[rows].trailing,
                         sizeof(surface->view.rows[rows].trailing),
                         "%u VISITS", (unsigned) page->visits);
            surface->id[rows] = (uint32_t) at;
            rows++;
        }
    }
    surface->view.count = (uint16_t) rows;
    psp_ui_set_collections(ui, &surface->view);
}

const char *psp_collections_row_url(
    const PspCollectionsSurface *surface, size_t row)
{
    if (surface == NULL || row >= surface->view.count) return NULL;
    return surface->view.rows[row].detail;
}

/* The section a UI action refers to, so one handler serves all three. */
PspUiCollectionSection psp_collections_action_section(
    PspUiAction action, PspUiCollectionSection current)
{
    switch (action) {
        case PSP_UI_ACTION_SHOW_BOOKMARKS: return PSP_UI_COLLECTION_BOOKMARKS;
        case PSP_UI_ACTION_SHOW_HISTORY: return PSP_UI_COLLECTION_HISTORY;
        case PSP_UI_ACTION_SHOW_OFFLINE: return PSP_UI_COLLECTION_OFFLINE;
        default: return current;
    }
}

/* Resolves a tile to a live URL, or NULL when the row is not a navigation. */
const char *psp_home_target_url(
    const PspHomeSurface *surface, size_t row,
    const BrowserProfile *profile)
{
    size_t target = 0;
    switch (psp_home_target_kind(surface, row, &target)) {
        case PSP_HOME_TARGET_BUILTIN:
            return target < PSP_HOME_BUILTIN_COUNT
                ? psp_home_builtin[target].url : NULL;
        case PSP_HOME_TARGET_BOOKMARK: {
            const BrowserProfilePage *page =
                browser_profile_bookmark(profile, target);
            return page == NULL || page->url[0] == '\0' ? NULL : page->url;
        }
        case PSP_HOME_TARGET_SEARCH:
        case PSP_HOME_TARGET_TAB:
        case PSP_HOME_TARGET_NONE:
        default:
            return NULL;
    }
}
