#include "tilefinch/psp_local_route.h"

#include <stdio.h>
#include <string.h>

#define CHECK(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "CHECK failed at %s:%d: %s\n", \
                __FILE__, __LINE__, #condition); \
        return 1; \
    } \
} while (0)

enum { A = PSP_ROUTE_ORIGIN_ACTIVATION, H = PSP_ROUTE_ORIGIN_HISTORY,
       R = PSP_ROUTE_ORIGIN_RELOAD, T = PSP_ROUTE_ORIGIN_TAB };

typedef struct {
    const char *url;
    PspLocalRouteKind kind;
    bool root_alias;
    PspUiCollectionSection section;
    PspProfilePageKind profile_page;
    bool offline_root;
    /* Indexed by PspRouteOrigin: activation, history, reload, tab. */
    PspLocalOpen open[4];
} RouteCase;

#define WEB PSP_LOCAL_OPEN_WEB
#define HOME PSP_LOCAL_OPEN_NATIVE_HOME
#define COLL PSP_LOCAL_OPEN_COLLECTIONS
#define SHOTS PSP_LOCAL_OPEN_SCREENSHOTS
#define PROF PSP_LOCAL_OPEN_PROFILE_PAGE
#define OFF PSP_LOCAL_OPEN_OFFLINE
#define NONE PSP_LOCAL_OPEN_UNAVAILABLE
#define SEC_OFF PSP_UI_COLLECTION_OFFLINE

/* Each URL's classification and what activation, Back/Forward, Reload and
   tab restore do with it. */
static const RouteCase cases[] = {
    {NULL, PSP_LOCAL_ROUTE_WEB, false, SEC_OFF, PSP_PROFILE_PAGE_NONE, false,
     {WEB, WEB, WEB, WEB}},
    {"https://example.com/", PSP_LOCAL_ROUTE_WEB, false, SEC_OFF,
     PSP_PROFILE_PAGE_NONE, false, {WEB, WEB, WEB, WEB}},
    /* Lookalike hosts are remote. */
    {"https://tilefinch.local.example/home", PSP_LOCAL_ROUTE_WEB, false,
     SEC_OFF, PSP_PROFILE_PAGE_NONE, false, {WEB, WEB, WEB, WEB}},
    {"https://tilefinch.local/home", PSP_LOCAL_ROUTE_NATIVE_HOME, false,
     SEC_OFF, PSP_PROFILE_PAGE_NONE, false, {HOME, HOME, HOME, HOME}},
    {"https://tilefinch.local/home/", PSP_LOCAL_ROUTE_NATIVE_HOME, false,
     SEC_OFF, PSP_PROFILE_PAGE_NONE, false, {HOME, HOME, HOME, HOME}},
    /* Decision: the bare origin is HOME at every origin (only Back/Forward
       routed it; the others loaded a host nothing serves). */
    {"https://tilefinch.local", PSP_LOCAL_ROUTE_NATIVE_HOME, true, SEC_OFF,
     PSP_PROFILE_PAGE_NONE, false, {HOME, HOME, HOME, HOME}},
    {"https://tilefinch.local/", PSP_LOCAL_ROUTE_NATIVE_HOME, true, SEC_OFF,
     PSP_PROFILE_PAGE_NONE, false, {HOME, HOME, HOME, HOME}},
    {"https://tilefinch.local/home/more", PSP_LOCAL_ROUTE_INTERNAL_UNKNOWN,
     false, SEC_OFF, PSP_PROFILE_PAGE_NONE, false, {WEB, NONE, WEB, WEB}},
    /* Decision: Reload shows native COLLECTIONS for the legacy addresses,
       as Back/Forward and tab restore do (it used to regenerate the
       retired HTML page, or load the trailing-slash spelling). */
    {"https://tilefinch.local/bookmarks", PSP_LOCAL_ROUTE_COLLECTION, false,
     PSP_UI_COLLECTION_BOOKMARKS, PSP_PROFILE_PAGE_BOOKMARKS, false,
     {WEB, COLL, COLL, COLL}},
    {"https://tilefinch.local/history", PSP_LOCAL_ROUTE_COLLECTION, false,
     PSP_UI_COLLECTION_HISTORY, PSP_PROFILE_PAGE_HISTORY, false,
     {WEB, COLL, COLL, COLL}},
    {"https://tilefinch.local/history/", PSP_LOCAL_ROUTE_COLLECTION, false,
     PSP_UI_COLLECTION_HISTORY, PSP_PROFILE_PAGE_NONE, false,
     {WEB, COLL, COLL, COLL}},
    /* Decision: the /offline root is the offline store's library page at
       every origin (tab restore used to open native COLLECTIONS). */
    {"https://tilefinch.local/offline", PSP_LOCAL_ROUTE_OFFLINE, false,
     SEC_OFF, PSP_PROFILE_PAGE_NONE, true, {OFF, OFF, OFF, OFF}},
    {"https://tilefinch.local/offline/", PSP_LOCAL_ROUTE_OFFLINE, false,
     SEC_OFF, PSP_PROFILE_PAGE_NONE, true, {OFF, OFF, OFF, OFF}},
    {"https://tilefinch.local/offline/youtube?id=abc", PSP_LOCAL_ROUTE_OFFLINE,
     false, SEC_OFF, PSP_PROFILE_PAGE_NONE, false, {OFF, OFF, OFF, OFF}},
    {"https://tilefinch.local/offlineish", PSP_LOCAL_ROUTE_INTERNAL_UNKNOWN,
     false, SEC_OFF, PSP_PROFILE_PAGE_NONE, false, {WEB, NONE, WEB, WEB}},
    /* Decision: a restored tab reopens the screenshot list (it used to
       load the address, which no host serves). */
    {"https://tilefinch.local/screenshots", PSP_LOCAL_ROUTE_SCREENSHOTS, false,
     SEC_OFF, PSP_PROFILE_PAGE_NONE, false, {WEB, SHOTS, SHOTS, SHOTS}},
    {"https://tilefinch.local/my-home", PSP_LOCAL_ROUTE_PROFILE_PAGE, false,
     SEC_OFF, PSP_PROFILE_PAGE_HOMEPAGE, false, {WEB, PROF, PROF, PROF}},
    {"https://tilefinch.local:443/home", PSP_LOCAL_ROUTE_INTERNAL_UNKNOWN,
     false, SEC_OFF, PSP_PROFILE_PAGE_NONE, false, {WEB, NONE, WEB, WEB}},
    {"tilefinch://home", PSP_LOCAL_ROUTE_INTERNAL_ACTION, false, SEC_OFF,
     PSP_PROFILE_PAGE_NONE, false, {WEB, WEB, WEB, WEB}},
    {"tilefinch://address", PSP_LOCAL_ROUTE_INTERNAL_ACTION, false, SEC_OFF,
     PSP_PROFILE_PAGE_NONE, false, {WEB, WEB, WEB, WEB}},
};

static int test_route_table(void)
{
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        const RouteCase *c = &cases[i];
        PspLocalRoute route = psp_local_route_classify(c->url);
        if (route.kind != c->kind || route.root_alias != c->root_alias
            || route.offline_root != c->offline_root
            || route.profile_page != c->profile_page
            || (route.kind == PSP_LOCAL_ROUTE_COLLECTION
                && route.section != c->section)) {
            fprintf(stderr, "classify %s: kind=%d alias=%d section=%d "
                    "profile=%d offline-root=%d\n",
                    c->url == NULL ? "(null)" : c->url, (int) route.kind,
                    (int) route.root_alias, (int) route.section,
                    (int) route.profile_page, (int) route.offline_root);
            return 1;
        }
        for (int origin = A; origin <= T; origin++) {
            PspLocalOpen open =
                psp_local_route_open(&route, (PspRouteOrigin) origin);
            if (open != c->open[origin]) {
                fprintf(stderr, "open %s origin=%d: %d, expected %d\n",
                        c->url == NULL ? "(null)" : c->url, origin,
                        (int) open, (int) c->open[origin]);
                return 1;
            }
        }
    }
    CHECK(psp_local_route_open(NULL, PSP_ROUTE_ORIGIN_TAB) == WEB);
    return 0;
}

/* Everything the classifier calls local is on the local origin or is an
   internal action, and everything on the local origin is local: HOME's
   Continue rows skip exactly these. */
static int test_local_kinds_match_the_local_origin(void)
{
    static const char *const urls[] = {
        "https://tilefinch.local", "https://tilefinch.local?q=1",
        "https://tilefinch.local:443/x", "https://tilefinch.local#top",
        "https://tilefinch.local:444/home", "https://tilefinch.localhost/",
        "tilefinch://retry", "http://tilefinch.local/home",
        "https://example.com/tilefinch.local/home",
    };
    for (size_t i = 0; i < sizeof(urls) / sizeof(urls[0]); i++) {
        bool local = psp_local_route_classify(urls[i]).kind
            != PSP_LOCAL_ROUTE_WEB;
        bool expected = psp_ui_internal_url(urls[i])
            || strncmp(urls[i], "tilefinch://", 12u) == 0;
        if (local != expected) {
            fprintf(stderr, "local %s: %d, expected %d\n", urls[i],
                    (int) local, (int) expected);
            return 1;
        }
    }
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        if (cases[i].url == NULL) continue;
        bool local = cases[i].kind != PSP_LOCAL_ROUTE_WEB;
        CHECK(local == (psp_ui_internal_url(cases[i].url)
                        || strncmp(cases[i].url, "tilefinch://", 12u) == 0));
    }
    return 0;
}

static int test_building_blocks(void)
{
    CHECK(psp_internal_action_url("tilefinch://retry", "retry"));
    CHECK(psp_internal_action_url("tilefinch://retry/", "retry"));
    CHECK(!psp_internal_action_url("tilefinch://retry/x", "retry"));
    CHECK(!psp_internal_action_url("tilefinch://retryx", "retry"));
    CHECK(!psp_internal_action_url(NULL, "retry"));
    CHECK(psp_offline_url("https://tilefinch.local/offline"));
    CHECK(psp_offline_url("https://tilefinch.local/offline/video?id=1"));
    CHECK(!psp_offline_url("https://tilefinch.local/offlinex"));
    CHECK(psp_profile_page_kind("https://tilefinch.local/my-home")
          == PSP_PROFILE_PAGE_HOMEPAGE);
    CHECK(psp_profile_page_kind("https://tilefinch.local/bookmarks/")
          == PSP_PROFILE_PAGE_NONE);
    return 0;
}

int main(void)
{
    CHECK(test_route_table() == 0);
    CHECK(test_building_blocks() == 0);
    CHECK(test_local_kinds_match_the_local_origin() == 0);
    puts("psp local route tests passed");
    return 0;
}
