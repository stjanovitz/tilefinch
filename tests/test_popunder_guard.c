/* Popunder guard (docs/SECURITY_MODEL.md, "Popunder navigations").

   A page answers the user's activation by opening the content in a new
   window and sending its own (opener) document to another site. Tilefinch
   opens no auxiliary window, so without the guard the other site replaces
   the page the user asked for. Each case activates one element through the
   controller, as the X button does, and reads the navigation the page then
   requests. Guard cases must keep navigating as they do without the rule. */

#include "tilefinch/budget.h"
#include "tilefinch/controller.h"
#include "tilefinch/document.h"
#include "tilefinch/js_runtime.h"
#include "tilefinch/navigation.h"

#include <stdio.h>
#include <string.h>

#define MIB (1024u * 1024u)
#define CHECK(condition) do {                                                \
    if (!(condition)) {                                                      \
        fprintf(stderr, "popunder guard test failed at %s:%d: %s\n",       \
                __FILE__, __LINE__, #condition);                             \
        return false;                                                        \
    }                                                                        \
} while (0)

static lxb_dom_node_t *find_id(lxb_dom_node_t *node, const char *wanted)
{
    if (node == NULL) return NULL;
    if (node->type == LXB_DOM_NODE_TYPE_ELEMENT) {
        size_t length = 0;
        const char *id = document_attribute(node, "id", &length);
        if (id != NULL && strlen(wanted) == length
            && memcmp(id, wanted, length) == 0) return node;
    }
    for (lxb_dom_node_t *child = node->first_child; child != NULL;
         child = child->next) {
        lxb_dom_node_t *found = find_id(child, wanted);
        if (found != NULL) return found;
    }
    return NULL;
}

/* The page under test. `listener` is the body of a capture-phase document
   click listener with `e` (the event) and `a` (the closest link) in scope. */
static const char page_prefix[] =
    "<!doctype html><title>Watch</title><body>"
    "<a id=next href='/watch/2'>Next video</a>"
    "<a id=partner href='https://partner.test/page'>Partner</a>"
    "<a id=blank href='https://docs.test/manual' target=_blank>Manual</a>"
    "<a id=spa href='/watch/4'>Router link</a>"
    "<button id=player>Play</button>"
    "<button id=signin>Sign in</button>"
    "<form id=pay method=post action='https://pay.test/checkout'>"
    "<input name=item value=1><button id=buy>Buy</button></form>"
    "<script>document.addEventListener('click',function(e){"
    "var a=e.target.closest('a');";
static const char page_suffix[] = "},true)</script>";

/* The pattern found in the wild: links (except new-window links) and the
   player open the content in a new window, then the opener is sent to an
   ad server after a short delay. */
static const char popunder_delayed[] =
    "if(a&&a.target==='_blank')return;"
    "if(!a&&!e.target.closest('#player'))return;"
    "e.preventDefault();e.stopPropagation();"
    "window.open((a?a.href:location.href)+'?from=1');"
    "setTimeout(function(){location.href='https://ads.test/pop?z=1'},500);";
/* The same pair in the other order, synchronously. */
static const char popunder_sync[] =
    "if(!a)return;e.preventDefault();"
    "location.assign('https://ads.test/pop?z=2');window.open(a.href);";
/* The pair without cancelling the press: the link's own navigation is
   the browser's default action. */
static const char popunder_uncancelled[] =
    "if(!a||a.target==='_blank')return;"
    "window.open(a.href+'?from=1');"
    "location.assign('https://ads.test/pop?z=5');";
static const char popunder_uncancelled_delayed[] =
    "if(!a||a.target==='_blank')return;"
    "window.open(a.href+'?from=1');"
    "setTimeout(function(){location.href='https://ads.test/pop?z=6'},500);";
/* Guards. A same-site router that also offers a share window. */
static const char spa_router[] =
    "if(!a||a.id!=='spa')return;e.preventDefault();"
    "window.open('https://share.test/?u='+encodeURIComponent(a.href));"
    "location.assign(a.href+'?view=1');";
/* An ad window on a link whose own destination is on another site. */
static const char cross_site_link[] =
    "if(!a||a.id!=='partner')return;e.preventDefault();"
    "window.open('https://ads.test/pop?z=3');"
    "setTimeout(function(){location.href=a.href},500);";
/* A sign-in popup that falls back to a redirect (another URL on the
   popup's site) when no window opens. */
static const char signin_fallback[] =
    "if(e.target.id!=='signin')return;"
    "var w=window.open('https://login.test/auth?display=popup');"
    "if(!w)location.href='https://login.test/auth?display=page';";
/* A blank window the page would fill itself, with an in-place fallback. */
static const char blank_window_fallback[] =
    "if(e.target.id!=='signin')return;"
    "var w=window.open('','checkout');"
    "if(!w)location.href='https://pay.test/checkout?token=1';";
/* A popunder from a control nested deeper than the guard's link search,
   below no link at all. */
static const char deep_control[] =
    "if(e.target.id!=='deep')return;e.preventDefault();"
    "window.open('https://content.test/v');"
    "location.href='https://ads.test/pop?z=4';";
/* A receipt window opened while a form posts to another site. */
static const char form_post[] =
    "if(e.target.id!=='buy')return;"
    "window.open('https://pay.test/receipt');";

typedef struct {
    const char *name;
    const char *listener;
    const char *activate_id;
    /* Expected controller default action (NAVIGATE / FORM_SUBMIT / NONE). */
    ControllerActionType action;
    const char *action_url;
    /* Expected script navigation after the page's timers ran, or NULL. */
    const char *navigation;
    /* When nonzero, the page is instead a button #deep at this DOM depth
       (the document is depth 0) under a cross-site <base href>. */
    unsigned deep_depth;
} PopunderCase;

static int deep_page(char *html, size_t size, const PopunderCase *test)
{
    size_t length = (size_t) snprintf(html, size,
        "<!doctype html><title>Watch</title>"
        "<base href='https://elsewhere.test/'><body>");
    unsigned wrappers = test->deep_depth - 3u;
    for (unsigned i = 0; i < wrappers && length < size; i++)
        length += (size_t) snprintf(html + length, size - length, "<div>");
    if (length < size)
        length += (size_t) snprintf(html + length, size - length,
            "<button id=deep>Deep</button>");
    for (unsigned i = 0; i < wrappers && length < size; i++)
        length += (size_t) snprintf(html + length, size - length, "</div>");
    if (length < size)
        length += (size_t) snprintf(html + length, size - length,
            "<script>document.addEventListener('click',function(e){"
            "%s},true)</script>", test->listener);
    return length < size ? (int) length : -1;
}

static bool run_case(const PopunderCase *test)
{
    char html[4096];
    int length = test->deep_depth != 0
        ? deep_page(html, sizeof(html), test)
        : snprintf(html, sizeof(html), "%s%s%s", page_prefix,
                   test->listener, page_suffix);
    CHECK(length > 0 && (size_t) length < sizeof(html));
    Budget budget;
    budget_init(&budget, 32 * MIB);
    CHECK(budget_install_lexbor(&budget));
    static NavigationSession navigation;
    memset(&navigation, 0, sizeof(navigation));
    bool ok = navigation_init(&navigation, &budget, 2);
    if (ok) {
        navigation_enable_scripts(&navigation, 4 * MIB, 1000);
        navigation.defer_script_navigation = true;
        ok = navigation_commit_html(
            &navigation, navigation_begin(&navigation),
            "https://video.test/watch/1", html, (size_t) length, 480,
            NULL, NULL, true);
    }
    BrowserController controller;
    ControllerAction action;
    memset(&action, 0, sizeof(action));
    lxb_dom_node_t *target = ok ? find_id(
        lxb_dom_interface_node(navigation.page.document.html),
        test->activate_id) : NULL;
    /* Layout stops descending before the deep control, so it is pressed
       with a trusted click dispatched straight to it. */
    ok = ok && controller_init(&controller, &navigation)
        && (test->deep_depth != 0
            ? navigation_dispatch_node_pointer(&navigation, target, 0,
                                               0, 0, 0, 0, 0)
            : controller_focus_node(&controller, target)
              && controller_activate(&controller, &action));
    char url[NAVIGATION_URL_LIMIT] = {0};
    bool record_history = false;
    bool navigated = false;
    for (unsigned turn = 0; ok && turn < 8 && !navigated; turn++) {
        navigated = navigation_take_script_navigation(
            &navigation, url, sizeof(url), &record_history);
        if (!navigated) ok = navigation_advance_runtime(&navigation, 100, 8);
    }
    bool action_ok = action.type == test->action
        && (test->action_url == NULL
            || strcmp(action.url, test->action_url) == 0);
    bool navigation_ok = test->navigation == NULL
        ? !navigated : navigated && strcmp(url, test->navigation) == 0;
    if (!ok || !action_ok || !navigation_ok) {
        fprintf(stderr,
                "case %s: ok=%d action=%d action-url=\"%s\" navigated=%d "
                "url=\"%s\" error=%s\n",
                test->name, ok, (int) action.type, action.url, navigated,
                url, navigation.last_error);
    }
    navigation_destroy(&navigation);
    bool released = budget.current == 0;
    budget_uninstall_lexbor(&budget);
    return ok && action_ok && navigation_ok && released;
}

int main(void)
{
    static const PopunderCase cases[] = {
        /* The popunder is refused and the pressed link is followed. */
        { "delayed popunder on a link", popunder_delayed, "next",
          CONTROLLER_ACTION_NONE, NULL, "https://video.test/watch/2" },
        /* No link to follow: the page stays, as the page cancelled the
           press itself. */
        { "delayed popunder on the player", popunder_delayed, "player",
          CONTROLLER_ACTION_NONE, NULL, NULL },
        { "synchronous popunder, navigation first", popunder_sync, "next",
          CONTROLLER_ACTION_NONE, NULL, "https://video.test/watch/2" },
        /* The press is not cancelled: the browser follows the link once,
           and the refused popunder adds no second load. */
        { "uncancelled synchronous popunder", popunder_uncancelled, "next",
          CONTROLLER_ACTION_NAVIGATE, "https://video.test/watch/2", NULL },
        { "uncancelled delayed popunder", popunder_uncancelled_delayed,
          "next", CONTROLLER_ACTION_NAVIGATE, "https://video.test/watch/2",
          NULL },
        /* Guards: these navigate exactly as they do without the rule. */
        { "same-site router", spa_router, "spa",
          CONTROLLER_ACTION_NONE, NULL,
          "https://video.test/watch/4?view=1" },
        { "link whose own destination is cross-site", cross_site_link,
          "partner", CONTROLLER_ACTION_NONE, NULL,
          "https://partner.test/page" },
        { "sign-in window falls back to a redirect", signin_fallback,
          "signin", CONTROLLER_ACTION_CONTROL, NULL,
          "https://login.test/auth?display=page" },
        { "blank window falls back to a redirect", blank_window_fallback,
          "signin", CONTROLLER_ACTION_CONTROL, NULL,
          "https://pay.test/checkout?token=1" },
        { "form post with a receipt window", form_post, "buy",
          CONTROLLER_ACTION_FORM_SUBMIT, "https://pay.test/checkout", NULL },
        { "new-window link opens its destination", popunder_delayed,
          "blank", CONTROLLER_ACTION_NAVIGATE, "https://docs.test/manual",
          NULL },
        /* The link search stops after 64 ancestors without taking the last
           one (here the document) for the pressed link, so nothing is
           followed to the <base href>. */
        { "popunder from a control 64 levels deep", deep_control, "deep",
          CONTROLLER_ACTION_NONE, NULL, NULL, 64 },
        { "popunder from a control 100 levels deep", deep_control, "deep",
          CONTROLLER_ACTION_NONE, NULL, NULL, 100 },
    };
    bool all_ok = true;
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        printf("test: popunder guard: %s\n", cases[i].name);
        if (!run_case(&cases[i])) all_ok = false;
    }
    return all_ok ? 0 : 1;
}
