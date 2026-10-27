#include "tilefinch/psp_local_route.h"

#include "tilefinch/browser_profile.h"

#include <stdio.h>
#include <string.h>

bool psp_offline_url(const char *url)
{
    static const char root[] = "https://tilefinch.local/offline";
    return url != NULL && strncmp(url, root, sizeof(root) - 1u) == 0
        && (url[sizeof(root) - 1u] == '\0'
            || url[sizeof(root) - 1u] == '/');
}

bool psp_internal_action_url(const char *url, const char *name)
{
    if (url == NULL || name == NULL) return false;
    char expected[64];
    int length = snprintf(
        expected, sizeof(expected), "tilefinch://%s", name);
    if (length <= 0 || (size_t) length >= sizeof(expected)) return false;
    size_t expected_length = (size_t) length;
    return strncmp(url, expected, expected_length) == 0
        && (url[expected_length] == '\0'
            || (url[expected_length] == '/'
                && url[expected_length + 1u] == '\0'));
}

PspProfilePageKind psp_profile_page_kind(const char *url)
{
    if (url == NULL) return PSP_PROFILE_PAGE_NONE;
    if (strcmp(url, BROWSER_PROFILE_HOMEPAGE_URL) == 0)
        return PSP_PROFILE_PAGE_HOMEPAGE;
    if (strcmp(url, "https://tilefinch.local/bookmarks") == 0)
        return PSP_PROFILE_PAGE_BOOKMARKS;
    if (strcmp(url, "https://tilefinch.local/history") == 0)
        return PSP_PROFILE_PAGE_HISTORY;
    return PSP_PROFILE_PAGE_NONE;
}

static bool psp_local_root_alias(const char *url)
{
    return strcmp(url, "https://tilefinch.local") == 0
        || strcmp(url, "https://tilefinch.local/") == 0;
}

PspLocalRoute psp_local_route_classify(const char *url)
{
    PspLocalRoute route = {
        .kind = PSP_LOCAL_ROUTE_WEB,
        .section = PSP_UI_COLLECTION_OFFLINE,
        .profile_page = PSP_PROFILE_PAGE_NONE
    };
    if (url == NULL) return route;
    if (strncmp(url, "tilefinch://", 12u) == 0) {
        route.kind = PSP_LOCAL_ROUTE_INTERNAL_ACTION;
        return route;
    }
    if (psp_ui_native_home_url(url) || psp_local_root_alias(url)) {
        route.kind = PSP_LOCAL_ROUTE_NATIVE_HOME;
        route.root_alias = !psp_ui_native_home_url(url);
        return route;
    }
    /* Offline before the legacy collections: /offline is both, and the
       offline store's library page is the live owner of that address. */
    if (psp_offline_url(url)) {
        route.kind = PSP_LOCAL_ROUTE_OFFLINE;
        route.offline_root = psp_ui_legacy_collection_url(url, NULL);
        return route;
    }
    PspUiCollectionSection section = PSP_UI_COLLECTION_OFFLINE;
    if (psp_ui_legacy_collection_url(url, &section)) {
        route.kind = PSP_LOCAL_ROUTE_COLLECTION;
        route.section = section;
        route.profile_page = psp_profile_page_kind(url);
        return route;
    }
    if (strcmp(url, "https://tilefinch.local/screenshots") == 0) {
        route.kind = PSP_LOCAL_ROUTE_SCREENSHOTS;
        return route;
    }
    route.profile_page = psp_profile_page_kind(url);
    if (route.profile_page != PSP_PROFILE_PAGE_NONE) {
        route.kind = PSP_LOCAL_ROUTE_PROFILE_PAGE;
        return route;
    }
    if (psp_ui_internal_url(url))
        route.kind = PSP_LOCAL_ROUTE_INTERNAL_UNKNOWN;
    return route;
}

/*
 * What each ladder does with a classified destination. Rows that differ by
 * origin are pinned by tests/test_psp_local_route.c, so a difference between
 * ladders is a visible decision here rather than a missing branch in one of
 * them:
 *
 *  - Page activation keeps only HOME and OFFLINE local: a page link to
 *    another local address is an ordinary navigation.
 *  - An unknown local address is refused only by Back/Forward, which must
 *    not move the cursor onto a page nothing can show; the other ladders
 *    load it and report the failure the way any unreachable host does.
 */
PspLocalOpen psp_local_route_open(
    const PspLocalRoute *route, PspRouteOrigin origin)
{
    if (route == NULL) return PSP_LOCAL_OPEN_WEB;
    switch (route->kind) {
    case PSP_LOCAL_ROUTE_NATIVE_HOME:
        /* Decision (10-26): the bare origin is HOME at every origin, not
           only for Back/Forward; elsewhere it used to load a host nothing
           serves. */
        return PSP_LOCAL_OPEN_NATIVE_HOME;
    case PSP_LOCAL_ROUTE_OFFLINE:
        /* Decision (10-26): the /offline root is the offline store's live
           library page at every origin; tab restore used to swap it for
           native COLLECTIONS as though it were a retired address. */
        return PSP_LOCAL_OPEN_OFFLINE;
    case PSP_LOCAL_ROUTE_COLLECTION:
        /* Decision (10-26): Reload shows native COLLECTIONS like
           Back/Forward and tab restore; it used to regenerate the retired
           HTML page for the exact spelling and load the address for the
           trailing-slash one. */
        return origin == PSP_ROUTE_ORIGIN_ACTIVATION
            ? PSP_LOCAL_OPEN_WEB : PSP_LOCAL_OPEN_COLLECTIONS;
    case PSP_LOCAL_ROUTE_SCREENSHOTS:
        /* Decision (10-26): a restored tab regenerates the list as
           Back/Forward and Reload do; it used to load the address. */
        return origin == PSP_ROUTE_ORIGIN_ACTIVATION
            ? PSP_LOCAL_OPEN_WEB : PSP_LOCAL_OPEN_SCREENSHOTS;
    case PSP_LOCAL_ROUTE_PROFILE_PAGE:
        return origin == PSP_ROUTE_ORIGIN_ACTIVATION
            ? PSP_LOCAL_OPEN_WEB : PSP_LOCAL_OPEN_PROFILE_PAGE;
    case PSP_LOCAL_ROUTE_INTERNAL_UNKNOWN:
        return origin == PSP_ROUTE_ORIGIN_HISTORY
            ? PSP_LOCAL_OPEN_UNAVAILABLE : PSP_LOCAL_OPEN_WEB;
    case PSP_LOCAL_ROUTE_INTERNAL_ACTION:
    case PSP_LOCAL_ROUTE_WEB:
    default:
        return PSP_LOCAL_OPEN_WEB;
    }
}
