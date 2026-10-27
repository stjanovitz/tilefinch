#ifndef TILEFINCH_PSP_LOCAL_ROUTE_H
#define TILEFINCH_PSP_LOCAL_ROUTE_H

/*
 * One definition of a local destination for the PSP frontend.
 *
 * "Is this URL native HOME, a collection, the screenshot list, a profile
 * page, offline, an internal action or the web?" used to be answered
 * separately by each navigation ladder (page activation, Back/Forward,
 * Reload, tab restore), each with its own coverage. The classifier answers
 * it once; psp_local_route_open() then says what each origin does with the
 * answer, so a difference between ladders is a row in one table instead of
 * a missing branch in one of them. SDK-free and host-tested.
 */

#include "tilefinch/psp_ui.h"

#include <stdbool.h>

typedef enum {
    PSP_PROFILE_PAGE_NONE = 0,
    PSP_PROFILE_PAGE_BOOKMARKS,
    PSP_PROFILE_PAGE_HISTORY,
    PSP_PROFILE_PAGE_HOMEPAGE
} PspProfilePageKind;

typedef enum {
    /* Not a local destination: an ordinary navigation. */
    PSP_LOCAL_ROUTE_WEB = 0,
    /* https://tilefinch.local/home, or the bare origin (root_alias). */
    PSP_LOCAL_ROUTE_NATIVE_HOME,
    /* A pre-upgrade collections address: /bookmarks or /history. */
    PSP_LOCAL_ROUTE_COLLECTION,
    PSP_LOCAL_ROUTE_SCREENSHOTS,
    /* The generated custom homepage (/my-home). */
    PSP_LOCAL_ROUTE_PROFILE_PAGE,
    /* /offline and everything under it, the library root included. */
    PSP_LOCAL_ROUTE_OFFLINE,
    /* tilefinch://name, which only a page's own link can emit. */
    PSP_LOCAL_ROUTE_INTERNAL_ACTION,
    /* Any other address on the local origin: no surface answers it. */
    PSP_LOCAL_ROUTE_INTERNAL_UNKNOWN
} PspLocalRouteKind;

typedef struct {
    PspLocalRouteKind kind;
    /* NATIVE_HOME spelled as the bare origin, with or without '/'. */
    bool root_alias;
    /* COLLECTION, and the OFFLINE library root (the third legacy
       collections address). */
    PspUiCollectionSection section;
    /* PROFILE_PAGE, and the exact /bookmarks and /history spellings, which
       the profile page generator also answers. */
    PspProfilePageKind profile_page;
    /* OFFLINE: /offline or /offline/ exactly. */
    bool offline_root;
} PspLocalRoute;

PspLocalRoute psp_local_route_classify(const char *url);

/* Where a navigation ladder starts. Boot keeps its own ladder in main(). */
typedef enum {
    PSP_ROUTE_ORIGIN_ACTIVATION = 0,
    PSP_ROUTE_ORIGIN_HISTORY,
    PSP_ROUTE_ORIGIN_RELOAD,
    PSP_ROUTE_ORIGIN_TAB
} PspRouteOrigin;

typedef enum {
    /* Load it as an ordinary document (or, for an internal action, let the
       activation ladder interpret it). */
    PSP_LOCAL_OPEN_WEB = 0,
    PSP_LOCAL_OPEN_NATIVE_HOME,
    /* Native COLLECTIONS on route.section. */
    PSP_LOCAL_OPEN_COLLECTIONS,
    PSP_LOCAL_OPEN_SCREENSHOTS,
    /* The generated page for route.profile_page. */
    PSP_LOCAL_OPEN_PROFILE_PAGE,
    /* The offline store's route for the URL. */
    PSP_LOCAL_OPEN_OFFLINE,
    /* A local address no surface answers: say so, load nothing. */
    PSP_LOCAL_OPEN_UNAVAILABLE
} PspLocalOpen;

PspLocalOpen psp_local_route_open(
    const PspLocalRoute *route, PspRouteOrigin origin);

/* The classifier's building blocks, kept for their existing callers. */
bool psp_offline_url(const char *url);
bool psp_internal_action_url(const char *url, const char *name);
PspProfilePageKind psp_profile_page_kind(const char *url);

#endif
