/* Pages that wait on a script the content blocker refuses. Basic blocking
   refuses tag managers and A/B testing clients (paywall SDKs load unless
   a Custom list refuses them); a page that
   hides itself until such a client answers (the anti-flicker snippets) or
   navigates from a tag-manager callback must still show itself and follow
   its links. Each case loads a replayed document and its scripts under one
   blocker configuration and reads what the page recorded after the load
   and a few short runtime turns (timers advance by those turns only). */
#include "tilefinch/budget.h"
#include "tilefinch/content_blocker.h"
#include "tilefinch/document.h"
#include "tilefinch/fetch.h"
#include "tilefinch/js_runtime.h"
#include "tilefinch/navigation.h"
#include "tilefinch/session.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define MIB (1024u * 1024u)
#define PAGE_URL "https://news.example/"

typedef struct {
    const char *url;
    const char *content_type;
    const char *body;
} PageRecord;

static bool write_fixture(char directory[128], const PageRecord *records,
                          size_t count)
{
    snprintf(directory, 128, "%s", "/tmp/tilefinch-blocker-pages-XXXXXX");
    if (mkdtemp(directory) == NULL) return false;
    bool ok = true;
    for (size_t i = 0; ok && i < count; i++) {
        char path[192];
        size_t length = strlen(records[i].body);
        snprintf(path, sizeof(path), "%s/%04zu.body", directory, i);
        FILE *body = fopen(path, "wb");
        ok = body != NULL && fwrite(records[i].body, 1, length, body) == length;
        if (body != NULL) ok = fclose(body) == 0 && ok;
        snprintf(path, sizeof(path), "%s/%04zu.meta", directory, i);
        FILE *meta = ok ? fopen(path, "wb") : NULL;
        uint64_t hash = UINT64_C(14695981039346656037);
        for (size_t at = 0; at < length; at++) {
            hash ^= (unsigned char) records[i].body[at];
            hash *= UINT64_C(1099511628211);
        }
        ok = meta != NULL && fprintf(
            meta,
            "psp-http-trace=10\ncookie-values=redacted\n"
            "method=GET\nurl=%s\nlogical-request-url=%s\nsuccess=1\n"
            "async-delay-pumps=0\nexternal-cancel=0\n"
            "transport-timeout=0\nredirect-origin-tainted=0\nerror=\n"
            "request-body-length=0\n"
            "request-body-hash=cbf29ce484222325\nrequest-content-type=\n"
            "request-cookie-bytes=0\nrequest-has-cf-clearance=0\n"
            "request-extra-header-bytes=0\nrequest-extra-header-shape=\n"
            "request-allow-http-errors=1\nrequest-enforce-cors=0\n"
            "request-redirect-same-origin-only=0\n"
            "request-cors-cached-response-validated=0\n"
            "request-if-none-match=\nrequest-if-modified-since=\n"
            "request-referer=\nrequest-origin=\nrequest-accept=\n"
            "request-sec-fetch-dest=\nrequest-sec-fetch-mode=\n"
            "request-sec-fetch-site=\nrequest-send-client-hints=0\n"
            "request-client-hint-tokens=\nrequest-client-hint-origin=\n"
            "request-send-low-client-hints=1\nrequest-sec-fetch-user=0\n"
            "request-upgrade-insecure=0\nrequest-user-agent=\n"
            "request-diagnostic-mobile-safari=0\nrequest-credentials=0\n"
            "request-credential-origin=\nrequest-initiator-url=\n"
            "request-referrer-source=\nrequest-referrer-policy=\n"
            "status=200\nlength=%zu\nresponse-body-hash=%016llx\n"
            "effective-url=%s\ncontent-type=%s\netag=\nlast-modified=\n"
            "cf-mitigated=\naccept-ch=\ncritical-ch=\nserver=fixture\n"
            "cf-ray=\nresponse-referrer-policy-metadata-valid=1\n"
            "response-referrer-policy-present=0\n"
            "response-referrer-policy=\n"
            "response-security-headers-truncated=0\n"
            "response-header-count=1\nset-cookie-count=0\n"
            "response-header-0=content-type: %s\n",
            records[i].url, records[i].url, length,
            (unsigned long long) hash, records[i].url,
            records[i].content_type, records[i].content_type) > 0;
        if (meta != NULL) ok = fclose(meta) == 0 && ok;
    }
    char path[192];
    snprintf(path, sizeof(path), "%s/trace.meta", directory);
    FILE *clock = ok ? fopen(path, "wb") : NULL;
    ok = clock != NULL
        && fprintf(clock, "psp-http-trace-clock=1\norigin-ms=1000\n") > 0;
    if (clock != NULL) ok = fclose(clock) == 0 && ok;
    return ok;
}

static void remove_fixture(const char *directory, size_t count)
{
    char path[192];
    for (size_t i = 0; i < count; i++) {
        snprintf(path, sizeof(path), "%s/%04zu.body", directory, i);
        (void) unlink(path);
        snprintf(path, sizeof(path), "%s/%04zu.meta", directory, i);
        (void) unlink(path);
    }
    snprintf(path, sizeof(path), "%s/trace.meta", directory);
    (void) unlink(path);
    (void) rmdir(directory);
}

typedef enum {
    BLOCKER_OFF,
    BLOCKER_BASIC,
    /* Basic, with the page's site allowed. */
    BLOCKER_BASIC_SITE_ALLOWED,
    /* A custom list that refuses only the tag manager's host. */
    BLOCKER_CUSTOM_TAG_MANAGER,
    /* A custom list that refuses only Piano's Tinypass host. */
    BLOCKER_CUSTOM_PAYWALL
} BlockerSetup;

typedef struct {
    const char *name;
    const char *html;
    const PageRecord *resources;
    size_t resource_count;
    BlockerSetup setup;
    /* Virtual milliseconds each runtime turn after the load advances the
       page's timers by (and the number of turns): short against the
       snippets' own timeouts unless a case tests the timeout. */
    unsigned turn_ms;
    unsigned turns;
    /* The expected value of globalThis.order.join(','), read after the
       turns. */
    const char *expected;
} PageCase;

static const char probe[] =
    "globalThis.pocSummary=(globalThis.order||['no-order']).join(',')";

static bool configure_blocker(ContentBlocker *blocker, BlockerSetup setup,
                              char custom_path[128])
{
    switch (setup) {
    case BLOCKER_OFF:
        return content_blocker_configure(blocker, CONTENT_BLOCKER_OFF, NULL);
    case BLOCKER_BASIC:
        return content_blocker_configure(blocker, CONTENT_BLOCKER_BASIC,
                                         NULL);
    case BLOCKER_BASIC_SITE_ALLOWED: {
        const char *const sites[] = {"news.example"};
        return content_blocker_configure(blocker, CONTENT_BLOCKER_BASIC,
                                         NULL)
            && content_blocker_set_allowed_sites(blocker, sites, 1u);
    }
    case BLOCKER_CUSTOM_TAG_MANAGER:
    case BLOCKER_CUSTOM_PAYWALL: {
        snprintf(custom_path, 128, "%s", "/tmp/tilefinch-sdk-list-XXXXXX");
        int descriptor = mkstemp(custom_path);
        if (descriptor < 0) return false;
        const char *list = setup == BLOCKER_CUSTOM_PAYWALL
            ? "||tinypass.com^\n" : "||googletagmanager.com^\n";
        size_t length = strlen(list);
        bool written = write(descriptor, list, length) == (ssize_t) length;
        written = close(descriptor) == 0 && written;
        return written && content_blocker_configure(
            blocker, CONTENT_BLOCKER_CUSTOM, custom_path);
    }
    }
    return false;
}

static bool run_page_case(const PageCase *page)
{
    PageRecord records[16];
    records[0] = (PageRecord) {
        .url = PAGE_URL, .content_type = "text/html; charset=utf-8",
        .body = page->html
    };
    size_t count = 1;
    for (size_t i = 0; i < page->resource_count && count < 16; i++)
        records[count++] = page->resources[i];
    char directory[128] = {0};
    char custom_path[128] = {0};
    bool written = write_fixture(directory, records, count);
    Budget budget;
    budget_init(&budget, 32u * MIB);
    bool lexbor = budget_install_lexbor(&budget);
    BrowserSession browser = {0};
    NavigationSession navigation = {0};
    ContentBlocker *blocker = NULL;
    bool browser_ready = written && lexbor
        && browser_session_init(&browser, &budget, 256u * 1024u);
    if (browser_ready) {
        blocker = content_blocker_create(&budget);
        browser_ready = blocker != NULL
            && configure_blocker(blocker, page->setup, custom_path);
        browser.content_blocker = blocker;
    }
    bool navigation_ready = browser_ready
        && navigation_init(&navigation, &budget, 4);
    if (navigation_ready) {
        navigation_attach_browser_session(&navigation, &browser);
        navigation_enable_scripts(&navigation, 8u * MIB, 2000);
        navigation_enable_document_scripts(
            &navigation, 32, 256u * 1024u, 64u * 1024u, 2000);
    }
    char error[256] = {0};
    bool replaying = navigation_ready
        && fetch_trace_replay_begin_response_keyed(
               directory, error, sizeof(error));
    uint64_t generation = replaying ? navigation_begin(&navigation) : 0;
    bool loaded = replaying && navigation_load_url(
        &navigation, generation, PAGE_URL, 64u * 1024u, 2000, 480,
        NULL, NULL, true);
    for (unsigned turn = 0; loaded && turn < page->turns; turn++)
        (void) navigation_advance_runtime(&navigation, page->turn_ms, 8);
    ScriptResult result = {0};
    bool matched = loaded && navigation.page.runtime != NULL
        && script_runtime_evaluate_diagnostic(
               navigation.page.runtime, probe, "<blocker-probe>", &result)
        && strcmp(result.summary, page->expected) == 0;
    if (!matched) {
        fprintf(stderr,
                "blocker page case \"%s\" failed: loaded=%d\n"
                "  order:    \"%s\"\n  expected: \"%s\"\n"
                "  error=\"%s\" navigation-error=\"%s\" replay-error=\"%s\"\n",
                page->name, loaded, result.summary, page->expected,
                result.error, navigation.last_error, error);
    }
    if (replaying) fetch_trace_end();
    if (navigation_ready) navigation_destroy(&navigation);
    browser.content_blocker = NULL;
    content_blocker_destroy(blocker);
    if (written && lexbor) browser_session_destroy(&browser);
    if (custom_path[0] != '\0') (void) unlink(custom_path);
    bool clean = budget.current == 0
        && budget_active_allocations(&budget, NULL) == 0;
    if (lexbor) clean = budget_uninstall_lexbor(&budget) && clean;
    if (written) remove_fixture(directory, count);
    if (!clean)
        fprintf(stderr, "blocker page case \"%s\" leaked\n", page->name);
    return matched && clean;
}

#define SCRIPT(url_, body_) \
    {.url = (url_), .content_type = "text/javascript", .body = (body_)}

#define REC "<script>globalThis.order=[];function rec(s){order.push(s)}</script>"

/* An anti-flicker snippet of the Google Optimize kind: hide the page with a
   class, register `dataLayer.hide` whose end() reveals it, and reveal it
   on a long timeout regardless. */
#define GOOGLE_HIDE \
    "<style>.async-hide{opacity:0!important}</style>" \
    "<script>(function(){var root=document.documentElement;" \
    "root.className+=' async-hide';var hide={end:function(){" \
    "root.className=root.className.replace(/ ?async-hide/,'');" \
    "rec('end')}};(window.dataLayer=window.dataLayer||[]).hide=hide;" \
    "setTimeout(function(){if(hide.end){hide.end();hide.end=null}},60000)" \
    "})()</script>"

/* What the loaded container would do: mark that it ran and end the
   hiding. */
static const PageRecord gtm_container[] = {
    SCRIPT("https://www.googletagmanager.com/gtm.js?id=GTM-TEST",
           "rec('gtm-ran');var h=window.dataLayer.hide;"
           "if(h&&h.end){h.end();h.end=null}"),
};

/* After the turns: whether the page is still hidden, by class and by its
   computed opacity. */
#define HIDDEN_STATE \
    "<script>addEventListener('load',function(){setTimeout(function(){" \
    "rec('class='+(/async-hide/.test(document.documentElement.className)" \
    "?'hidden':'shown'));rec('opacity='+getComputedStyle(" \
    "document.documentElement).opacity)},0)})</script>"

#define GOOGLE_PAGE \
    "<!doctype html><html><head>" REC GOOGLE_HIDE \
    "<script async src=\"https://www.googletagmanager.com/gtm.js?id=GTM-TEST\"" \
    " onerror=\"rec('error')\" onload=\"rec('load')\"></script>" \
    HIDDEN_STATE "</head><body><p>Story</p></body></html>"

/* gtag: the page queues a conversion whose callback navigates before the
   library loads, and a GTM push with a callback after the load. Each
   callback must run exactly once. */
#define GTAG_PAGE \
    "<!doctype html><html><head>" REC \
    "<script async src=\"https://www.googletagmanager.com/gtag/js?id=G-TEST\"" \
    " onerror=\"rec('error')\"></script>" \
    "<script>window.dataLayer=window.dataLayer||[];" \
    "function gtag(){dataLayer.push(arguments)}gtag('js',new Date());" \
    "gtag('config','G-TEST');" \
    "gtag('event','conversion',{send_to:'G-TEST',event_callback:" \
    "function(){rec('gtag-callback')}});" \
    "addEventListener('load',function(){dataLayer.push({event:'click'," \
    "eventCallback:function(){rec('gtm-callback')},eventTimeout:2000})})" \
    "</script></head><body><a href=\"/next\">Next</a></body></html>"

static const PageRecord gtag_library[] = {
    SCRIPT("https://www.googletagmanager.com/gtag/js?id=G-TEST",
           "rec('gtag-ran')"),
};

/* A GTM container on a renamed data layer (`l=`). */
#define NAMED_LAYER_PAGE \
    "<!doctype html><html><head>" REC \
    "<script>window.tagLayer=[{event:'start',eventCallback:" \
    "function(){rec('named-callback')}}];window.dataLayer=[{event:'other'," \
    "eventCallback:function(){rec('default-callback')}}];</script>" \
    "<script src=\"https://www.googletagmanager.com/gtm.js?id=GTM-TEST&amp;l=tagLayer\"" \
    "></script></head><body></body></html>"

/* Adobe Target's pre-hiding snippet before a Launch library. */
#define ADOBE_PAGE \
    "<!doctype html><html><head>" REC \
    "<script>(function(){var style=document.createElement('style');" \
    "style.id='at-body-style';style.textContent='body{opacity:0!important}';" \
    "document.head.appendChild(style);setTimeout(function(){" \
    "var s=document.getElementById('at-body-style');if(s)s.remove()},60000)" \
    "})()</script>" \
    "<script src=\"https://assets.adobedtm.com/launch-TEST.min.js\"></script>" \
    "<script>addEventListener('load',function(){rec(document.getElementById(" \
    "'at-body-style')?'hidden':'shown');rec('opacity='+getComputedStyle(" \
    "document.body).opacity)})</script>" \
    "</head><body><p>Story</p></body></html>"

/* VWO SmartCode that inserts its library and keeps the page hidden until
   finish() (no onerror handler of its own here). */
#define VWO_PAGE \
    "<!doctype html><html><head>" REC \
    "<script>window._vwo_code={done:false,finish:function(){" \
    "if(!this.done){this.done=true;rec('finish')}}};" \
    "var s=document.createElement('script');" \
    "s.src='https://dev.visualwebsiteoptimizer.com/j.php?a=1';" \
    "document.head.appendChild(s)</script></head><body></body></html>"

/* An anti-flicker snippet that only has its timeout (Optimizely and AB
   Tasty style): it reveals the page on its own once the timer runs. */
#define TIMEOUT_PAGE \
    "<!doctype html><html><head>" REC \
    "<style>html.optly-hide{opacity:0}</style>" \
    "<script>document.documentElement.className='optly-hide';" \
    "setTimeout(function(){document.documentElement.className='';" \
    "rec('revealed')},300)</script>" \
    "<script src=\"https://cdn.optimizely.com/js/12345.js\"" \
    " onerror=\"rec('error')\"></script>" \
    "</head><body><p>Story</p></body></html>"

static const PageRecord optimizely_client[] = {
    SCRIPT("https://cdn.optimizely.com/js/12345.js", "rec('optimizely-ran')"),
};

/* A paywall SDK queue: the page keeps working without it. */
#define PIANO_PAGE \
    "<!doctype html><html><head>" REC \
    "<script>window.tp=window.tp||[];tp.push(['init',function(){" \
    "rec('piano-init')}])</script>" \
    "<script async src=\"https://cdn.tinypass.com/api/tinypass.min.js\"" \
    " onerror=\"rec('error')\"></script>" \
    "<script>addEventListener('load',function(){rec('page-ready')})</script>" \
    "</head><body><p>Story</p></body></html>"

static const PageRecord piano_sdk[] = {
    SCRIPT("https://cdn.tinypass.com/api/tinypass.min.js",
           "rec('piano-ran');var q=window.tp;for(var i=0;i<q.length;i++)"
           "if(q[i][0]==='init')q[i][1]()"),
};

/* An Optimize snippet and pushes on a renamed data layer: the answer
   follows the container's `l=` name, and leaves the default layer alone. */
#define NAMED_HIDE_PAGE \
    "<!doctype html><html><head>" REC \
    "<script>window.tagLayer=[];tagLayer.hide={end:function(){" \
    "rec('end')}};window.dataLayer=[];dataLayer.hide={end:function(){" \
    "rec('default-end')}}</script>" \
    "<script src=\"https://www.googletagmanager.com/gtm.js?id=GTM-TEST&amp;l=tagLayer\"" \
    " onerror=\"rec('error')\"></script>" \
    "<script>tagLayer.push({event:'click',eventCallback:function(){" \
    "rec('named-pushed')}});dataLayer.push({event:'click'," \
    "eventCallback:function(){rec('default-pushed')}})</script>" \
    "</head><body></body></html>"

/* A gtag event whose callback the page also guards with its own
   event_timeout fallback (Google's documented pattern): the callback runs
   once, early, and the fallback finds it already called. */
#define GTAG_TIMEOUT_PAGE \
    "<!doctype html><html><head>" REC \
    "<script async src=\"https://www.googletagmanager.com/gtag/js?id=G-TEST\"" \
    " onerror=\"rec('error')\"></script>" \
    "<script>window.dataLayer=window.dataLayer||[];" \
    "function gtag(){dataLayer.push(arguments)}" \
    "function withTimeout(callback,ms){var called=false;" \
    "function once(){if(!called){called=true;callback()}}" \
    "setTimeout(function(){rec(called?'fallback-unused':'fallback-used');" \
    "once()},ms);return once}" \
    "addEventListener('load',function(){gtag('event','purchase'," \
    "{event_callback:withTimeout(function(){rec('callback')},2000)," \
    "event_timeout:2000})})</script></head><body></body></html>"

/* Three refused Google scripts on one data layer (two GTM containers and
   gtag.js): one answer for the realm, so the hiding ends once and each
   callback, queued or pushed later, runs once. */
#define TWO_CONTAINERS_PAGE \
    "<!doctype html><html><head>" REC \
    "<script>var ends=0;window.dataLayer=[{event:'queued'," \
    "eventCallback:function(){rec('queued')}}];" \
    "dataLayer.hide={end:function(){rec('end'+(++ends))}}</script>" \
    "<script src=\"https://www.googletagmanager.com/gtm.js?id=GTM-A\"" \
    " onerror=\"rec('error-a')\"></script>" \
    "<script src=\"https://www.googletagmanager.com/gtm.js?id=GTM-B\"" \
    " onerror=\"rec('error-b')\"></script>" \
    "<script src=\"https://www.googletagmanager.com/gtag/js?id=G-C\"" \
    " onerror=\"rec('error-c')\"></script>" \
    "<script>dataLayer.push({event:'click',eventCallback:function(){" \
    "rec('pushed')}})</script></head><body></body></html>"

/* A second container on its own data layer waits on that layer. */
#define TWO_LAYERS_PAGE \
    "<!doctype html><html><head>" REC \
    "<script>window.dataLayer=[{event:'a',eventCallback:function(){" \
    "rec('default-queued')}}];window.otherLayer=[{event:'b'," \
    "eventCallback:function(){rec('other-queued')}}]</script>" \
    "<script src=\"https://www.googletagmanager.com/gtm.js?id=GTM-A\"" \
    " onerror=\"rec('error-a')\"></script>" \
    "<script src=\"https://www.googletagmanager.com/gtm.js?id=GTM-B&amp;l=otherLayer\"" \
    " onerror=\"rec('error-b')\"></script>" \
    "<script>otherLayer.push({event:'click',eventCallback:function(){" \
    "rec('other-pushed')}})</script></head><body></body></html>"

/* The tag-manager URL requested other than by a classic <script>: a
   fetch() and a dynamic import() are refused (they reject) and have no
   element, so they are not answered and the page stays hidden until its
   own timeout, as where the request fails in Chromium. (import() rejects
   with QuickJS's ReferenceError where Chromium has a TypeError.) A module
   script element refused for the host is answered before its error event
   like a classic one. */
#define NON_SCRIPT_REQUEST_PAGE \
    "<!doctype html><html><head>" REC GOOGLE_HIDE \
    "<script>fetch('https://www.googletagmanager.com/gtm.js?id=GTM-TEST')" \
    ".then(function(){rec('fetch-ok')},function(e){rec('fetch-'+e.name)});" \
    "import('https://www.googletagmanager.com/gtm.js?id=GTM-TEST')" \
    ".then(function(){rec('import-ok')},function(e){rec('import-'+e.name)})" \
    "</script>" HIDDEN_STATE "</head><body><p>Story</p></body></html>"

#define MODULE_REQUEST_PAGE \
    "<!doctype html><html><head>" REC GOOGLE_HIDE \
    "<script type=module src=\"https://www.googletagmanager.com/gtm.js?id=GTM-TEST\"" \
    " onerror=\"rec('module-error')\" onload=\"rec('module-load')\"></script>" \
    HIDDEN_STATE "</head><body><p>Story</p></body></html>"

/* An empty external script is a script that does nothing: it runs and
   fires load, as in Chromium, in every form (parser-blocking, data: URL,
   async, defer, inserted, module, and a module's static and dynamic
   imports). It used to fire error (classic) or nothing at all (a module
   root), and an empty import failed its importer. */
#define EMPTY_SCRIPTS_PAGE \
    "<!doctype html><html><head>" REC \
    "<script src=\"/empty.js\" onload=\"rec('blocking')\"" \
    " onerror=\"rec('blocking-error')\"></script>" \
    "<script src=\"data:text/javascript,\" onload=\"rec('data')\"" \
    " onerror=\"rec('data-error')\"></script>" \
    "<script async src=\"/empty-async.js\" onload=\"rec('async')\"" \
    " onerror=\"rec('async-error')\"></script>" \
    "<script defer src=\"/empty-defer.js\" onload=\"rec('defer')\"" \
    " onerror=\"rec('defer-error')\"></script>" \
    "<script type=module src=\"/empty.mjs\" onload=\"rec('module')\"" \
    " onerror=\"rec('module-error')\"></script>" \
    "<script type=module src=\"/imports.mjs\"></script>" \
    "<script>var s=document.createElement('script');s.src='/empty-dyn.js';" \
    "s.onload=function(){rec('inserted')};" \
    "s.onerror=function(){rec('inserted-error')};" \
    "document.head.appendChild(s)</script></head><body></body></html>"

static const PageRecord empty_scripts[] = {
    SCRIPT("https://news.example/empty.js", ""),
    SCRIPT("https://news.example/empty-async.js", ""),
    SCRIPT("https://news.example/empty-defer.js", ""),
    SCRIPT("https://news.example/empty.mjs", ""),
    SCRIPT("https://news.example/imports.mjs",
           "import './dep.mjs';rec('import');import('./dyn.mjs')"
           ".then(function(){rec('dynamic-import')},"
           "function(e){rec('dynamic-import-'+e.name)})"),
    SCRIPT("https://news.example/dep.mjs", ""),
    SCRIPT("https://news.example/dyn.mjs", ""),
    SCRIPT("https://news.example/empty-dyn.js", ""),
};

/* Off, a tag-manager container whose body is empty answers nothing (the
   page stays hidden until its own timeout), like any container that loads
   without ending the hiding: it fires load, not error. Measurements that
   stand a 1-byte body in for a refused SDK see the same page as with an
   empty one. */
static const PageRecord empty_container[] = {
    SCRIPT("https://www.googletagmanager.com/gtm.js?id=GTM-TEST", ""),
};

static const PageRecord one_byte_container[] = {
    SCRIPT("https://www.googletagmanager.com/gtm.js?id=GTM-TEST", ";"),
};

#define COUNT(records_) (sizeof(records_) / sizeof((records_)[0]))

int main(void)
{
    static const PageCase cases[] = {
        /* Basic refuses the container: the page shows itself at once
           instead of after its 60 s timeout, and gets the error event. */
        {"google anti-flicker, basic", GOOGLE_PAGE, gtm_container, 1,
         BLOCKER_BASIC, 1, 4, "end,error,class=shown,opacity=1"},
        /* The same answer whichever rule refused it. */
        {"google anti-flicker, custom list", GOOGLE_PAGE, gtm_container, 1,
         BLOCKER_CUSTOM_TAG_MANAGER, 1, 4,
         "end,error,class=shown,opacity=1"},
        /* Off and an allowed site load the container, which answers. */
        {"google anti-flicker, off", GOOGLE_PAGE, gtm_container, 1,
         BLOCKER_OFF, 1, 4, "gtm-ran,end,load,class=shown,opacity=1"},
        {"google anti-flicker, site allowed", GOOGLE_PAGE, gtm_container, 1,
         BLOCKER_BASIC_SITE_ALLOWED, 1, 4,
         "gtm-ran,end,load,class=shown,opacity=1"},
        /* gtag callbacks run once each, queued before or pushed after. */
        {"gtag callbacks, basic", GTAG_PAGE, gtag_library, 1, BLOCKER_BASIC,
         1, 4, "error,gtag-callback,gtm-callback"},
        {"gtag callbacks, off", GTAG_PAGE, gtag_library, 1, BLOCKER_OFF,
         1, 4, "gtag-ran"},
        {"named data layer", NAMED_LAYER_PAGE, NULL, 0, BLOCKER_BASIC, 1, 4,
         "named-callback"},
        {"adobe target pre-hiding", ADOBE_PAGE, NULL, 0, BLOCKER_BASIC, 1, 4,
         "shown,opacity=1"},
        {"vwo smartcode", VWO_PAGE, NULL, 0, BLOCKER_BASIC, 1, 4, "finish"},
        /* A snippet with only a timeout reveals the page when it fires. */
        {"timeout anti-flicker, before", TIMEOUT_PAGE, optimizely_client, 1,
         BLOCKER_BASIC, 100, 2, "error"},
        {"timeout anti-flicker, after", TIMEOUT_PAGE, optimizely_client, 1,
         BLOCKER_BASIC, 100, 4, "error,revealed"},
        {"timeout anti-flicker, off", TIMEOUT_PAGE, optimizely_client, 1,
         BLOCKER_OFF, 100, 4, "optimizely-ran,revealed"},
        /* Basic loads a paywall SDK, which answers its queue. */
        {"paywall sdk, basic", PIANO_PAGE, piano_sdk, 1, BLOCKER_BASIC, 1, 4,
         "piano-ran,piano-init,page-ready"},
        /* A paywall SDK a Custom list refuses gets no answer: its queue
           stays unanswered and the page stays as it was sent. */
        {"paywall sdk, custom list", PIANO_PAGE, piano_sdk, 1,
         BLOCKER_CUSTOM_PAYWALL, 1, 4, "error,page-ready"},
        {"paywall sdk, off", PIANO_PAGE, piano_sdk, 1, BLOCKER_OFF, 1, 4,
         "piano-ran,piano-init,page-ready"},
        /* The answer on real snippet shapes. */
        {"named layer anti-flicker and push", NAMED_HIDE_PAGE, NULL, 0,
         BLOCKER_BASIC, 1, 4, "end,error,named-pushed"},
        {"gtag event_callback with event_timeout", GTAG_TIMEOUT_PAGE, NULL,
         0, BLOCKER_BASIC, 1000, 4, "error,callback,fallback-unused"},
        {"second container, one layer", TWO_CONTAINERS_PAGE, NULL, 0,
         BLOCKER_BASIC, 1, 4, "end1,error-a,queued,error-b,error-c,pushed"},
        {"second container, own layer", TWO_LAYERS_PAGE, NULL, 0,
         BLOCKER_BASIC, 1, 4,
         "error-a,default-queued,error-b,other-queued,other-pushed"},
        {"gtag callbacks, custom list", GTAG_PAGE, gtag_library, 1,
         BLOCKER_CUSTOM_TAG_MANAGER, 1, 4, "error,gtag-callback,gtm-callback"},
        {"gtag callbacks, site allowed", GTAG_PAGE, gtag_library, 1,
         BLOCKER_BASIC_SITE_ALLOWED, 1, 4, "gtag-ran"},
        /* An allowed site is never answered, even when the script fails
           to load for another reason (here it is not in the replay). */
        {"google anti-flicker, site allowed, load fails", GOOGLE_PAGE, NULL,
         0, BLOCKER_BASIC_SITE_ALLOWED, 1, 4, "error,class=hidden,opacity=0"},
        {"tag manager by fetch and import()", NON_SCRIPT_REQUEST_PAGE,
         gtm_container, 1, BLOCKER_BASIC, 1, 4,
         "fetch-TypeError,import-ReferenceError,class=hidden,opacity=0"},
        {"tag manager as a module script", MODULE_REQUEST_PAGE, NULL, 0,
         BLOCKER_BASIC, 1, 4, "end,module-error,class=shown,opacity=1"},
        {"empty external scripts load", EMPTY_SCRIPTS_PAGE, empty_scripts,
         COUNT(empty_scripts), BLOCKER_OFF, 1, 8,
         "blocking,data,async,defer,module,import,dynamic-import,inserted"},
        {"google anti-flicker, empty container, off", GOOGLE_PAGE,
         empty_container, 1, BLOCKER_OFF, 1, 4,
         "load,class=hidden,opacity=0"},
        {"google anti-flicker, 1-byte container, off", GOOGLE_PAGE,
         one_byte_container, 1, BLOCKER_OFF, 1, 4,
         "load,class=hidden,opacity=0"},
        /* Refused, the empty container is answered as any other. */
        {"google anti-flicker, empty container, basic", GOOGLE_PAGE,
         empty_container, 1, BLOCKER_BASIC, 1, 4,
         "end,error,class=shown,opacity=1"},
    };
    int failures = 0;
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++)
        if (!run_page_case(&cases[i])) failures++;
    if (failures != 0) {
        fprintf(stderr, "content-blocker-page-tests: %d failed\n", failures);
        return 1;
    }
    puts("content-blocker-page-tests: ok");
    return 0;
}
