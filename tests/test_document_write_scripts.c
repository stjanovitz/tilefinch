/* document.write() from a parser-executed script feeds the parser at its
   insertion point, as HTML requires and Chromium does: a written inline
   <script> runs inside the write() call, a written external one is a
   pending parsing-blocking script that runs once the writing script has
   returned and before any markup written after it, and the written markup
   continues the document's own input stream. Each case loads a replayed
   document and its scripts; the probe reads the order the page recorded. */
#include "tilefinch/budget.h"
#include "tilefinch/content_blocker.h"
#include "tilefinch/document.h"
#include "tilefinch/fetch.h"
#include "tilefinch/js_runtime.h"
#include "tilefinch/navigation.h"
#include "tilefinch/platform.h"
#include "tilefinch/session.h"
#include "../src/tilefinch_test_faults.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define MIB (1024u * 1024u)
#define PAGE_URL "https://write.test/"
/* The script watchdog timeout every page case gets. */
#define SCRIPT_TIMEOUT_MS 2000
/* A spin that lasts this long on any host, however loaded: wall time, not
   a loop count, bounds it. */
#define SPIN_TEXT(ms_) "for(var t=Date.now();Date.now()-t<" #ms_ ";);"
#define SPIN(ms_) SPIN_TEXT(ms_)

typedef struct {
    const char *url;
    const char *content_type;
    const char *body;
    /* One extra response header line ("name: value"), or NULL. */
    const char *header;
} PageRecord;

static bool write_fixture(char directory[128], const PageRecord *records,
                          size_t count)
{
    snprintf(directory, 128, "%s", "/tmp/tilefinch-document-write-XXXXXX");
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
            "response-header-count=%d\nset-cookie-count=0\n"
            "response-header-0=content-type: %s\n",
            records[i].url, records[i].url, length,
            (unsigned long long) hash, records[i].url,
            records[i].content_type, records[i].header == NULL ? 1 : 2,
            records[i].content_type) > 0;
        if (ok && records[i].header != NULL) {
            ok = fprintf(meta, "response-header-1=%s\n",
                         records[i].header) > 0;
        }
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

typedef struct {
    const char *name;
    const char *html;
    /* A Content-Security-Policy for the document, or NULL. */
    const char *policy;
    const PageRecord *resources;
    size_t resource_count;
    /* The Basic content blocker list is active. */
    bool blocker;
    /* The expected value of globalThis.order.join(','), read after the
       load and after each runtime advance. */
    const char *expected;
    /* The page may also be refused for its size: the case then only
       checks that the load ends and releases everything. */
    bool may_refuse;
    /* For a page whose JavaScript is stopped while it loads (its realm is
       gone afterwards): the expected data-order attribute of the body,
       which REC() keeps equal to order.join(','), and the page's script
       summary. `expected` is then not read. */
    const char *stopped_order;
    const char *stopped_summary;
    /* Checks the body's data-order attribute whether or not the realm
       survived the load (instead of `expected`), or NULL. */
    bool (*recorded_check)(const char *recorded);
    /* The parser-script stage time charged must not pass the load's own
       wall time. */
    bool stage_within_load;
    /* The page's cookies after the load (refused or not), or NULL. */
    const char *cookies;
    /* The script watchdog timeout for the load and its scripts, in
       milliseconds (SCRIPT_TIMEOUT_MS when 0). */
    long timeout_ms;
    /* Loads of another scheduler that nobody pumps during the load hold
       this many ordinary worker descriptors, under the replay lane with
       the PSP's descriptors (as test_browser_engine's stuck-loads case).
       The load must then end within `max_load_us` with exactly
       `stuck_failures` failed scripts.
       The held loads are `stuck_urls` records of the fixture. */
    size_t stuck_loads;
    uint64_t max_load_us;
    size_t stuck_failures;
    /* Hold all but this many bytes of the page Budget once the page realm
       exists (navigation's heavy-policy seam), or 0. */
    size_t leave_free;
    /* Scripts the load refused for memory pressure (checked when not 0). */
    size_t pressure_refusals;
    /* The maximum document bytes (64 KiB when 0). */
    size_t document_limit;
    /* The per-file document script limit (64 KiB when 0); the page's total
       is the larger of it and 256 KiB. */
    size_t script_limit;
    /* The element children of #box after the load, read from the DOM (so
       also when the realm is gone), in BOX_CHILDREN's form, or NULL. */
    const char *box_children;
} PageCase;

static lxb_dom_node_t *find_id(lxb_dom_node_t *node, const char *id)
{
    for (; node != NULL; node = node->next) {
        size_t length = 0;
        const char *value = node->type == LXB_DOM_NODE_TYPE_ELEMENT
            ? document_attribute(node, "id", &length) : NULL;
        if (value != NULL && length == strlen(id)
            && memcmp(value, id, length) == 0) return node;
        lxb_dom_node_t *found = find_id(node->first_child, id);
        if (found != NULL) return found;
    }
    return NULL;
}

/* "TAG#id/TAG/..." for the element children of #box. */
static void box_children(PocDocument *document, char *out, size_t size)
{
    size_t at = 0;
    out[0] = '\0';
    lxb_dom_node_t *box = find_id(document_body_node(document), "box");
    for (lxb_dom_node_t *child = box == NULL ? NULL : box->first_child;
         child != NULL; child = child->next) {
        if (child->type != LXB_DOM_NODE_TYPE_ELEMENT) continue;
        size_t name_length = 0, id_length = 0;
        const lxb_char_t *name = lxb_dom_element_qualified_name(
            lxb_dom_interface_element(child), &name_length);
        const char *id = document_attribute(child, "id", &id_length);
        char tag[32] = {0};
        for (size_t i = 0; i < name_length && i + 1u < sizeof(tag); i++)
            tag[i] = (char) (name[i] >= 'a' && name[i] <= 'z'
                             ? name[i] - 'a' + 'A' : name[i]);
        int written = snprintf(out + at, size - at, "%s%s%s%.*s",
                               at == 0 ? "" : "/", tag,
                               id_length != 0 ? "#" : "",
                               id == NULL ? 0 : (int) id_length,
                               id == NULL ? "" : id);
        if (written < 0 || (size_t) written >= size - at) break;
        at += (size_t) written;
    }
}

#define STUCK_LOAD_LIMIT 5
static const char *const stuck_urls[STUCK_LOAD_LIMIT] = {
    "https://write.test/held-0.png", "https://write.test/held-1.png",
    "https://write.test/held-2.png", "https://write.test/held-3.png",
    "https://write.test/held-4.png",
};

/* Navigation resolves the realm's heavy-script policy right after it
   creates the page realm, before the document's scripts run: the seam
   holds all but `leave_free` bytes of the page Budget there. */
typedef struct {
    NavigationSession *navigation;
    BudgetReservation reservation;
    size_t leave_free;
    bool reserved;
} HeapHold;

static ScriptHeavyPolicy hold_heap_at_realm(void *opaque, const char *url)
{
    (void) url;
    HeapHold *hold = opaque;
    Budget *budget = hold->navigation->budget;
    size_t remaining = budget_remaining(budget);
    if (!hold->reserved && remaining > hold->leave_free)
        hold->reserved = budget_reservation_acquire(
            &hold->reservation, budget, BUDGET_CATEGORY_RESOURCE,
            remaining - hold->leave_free);
    return hold->navigation->heavy_script_policy;
}

static const char probe[] =
    "globalThis.pocSummary=(globalThis.order||['no-order']).join(',')";

static bool run_page_case(const PageCase *page)
{
    PageRecord records[16];
    char header[1024];
    if (page->policy != NULL)
        snprintf(header, sizeof(header), "content-security-policy: %s",
                 page->policy);
    records[0] = (PageRecord) {
        .url = PAGE_URL, .content_type = "text/html; charset=utf-8",
        .body = page->html, .header = page->policy == NULL ? NULL : header
    };
    size_t count = 1;
    for (size_t i = 0; i < page->resource_count && count < 16; i++)
        records[count++] = page->resources[i];
    size_t stuck = page->stuck_loads < STUCK_LOAD_LIMIT
        ? page->stuck_loads : STUCK_LOAD_LIMIT;
    for (size_t i = 0; i < stuck && count < 16; i++)
        records[count++] = (PageRecord) {
            .url = stuck_urls[i], .content_type = "image/png", .body = "png"
        };
    long timeout_ms = page->timeout_ms == 0 ? SCRIPT_TIMEOUT_MS : page->timeout_ms;
    char directory[128] = {0};
    bool written = write_fixture(directory, records, count);
    Budget budget;
    budget_init(&budget, 32u * MIB);
    bool lexbor = budget_install_lexbor(&budget);
    BrowserSession browser = {0};
    NavigationSession navigation = {0};
    ContentBlocker *blocker = NULL;
    bool browser_ready = written && lexbor
        && browser_session_init(&browser, &budget, 256u * 1024u);
    if (browser_ready && page->blocker) {
        blocker = content_blocker_create(&budget);
        browser_ready = blocker != NULL
            && content_blocker_configure(blocker, CONTENT_BLOCKER_BASIC,
                                         NULL);
        browser.content_blocker = blocker;
    }
    bool navigation_ready = browser_ready
        && navigation_init(&navigation, &budget, 4);
    if (navigation_ready) {
        navigation_attach_browser_session(&navigation, &browser);
        navigation_enable_scripts(&navigation, 8u * MIB, timeout_ms);
        size_t script_limit = page->script_limit == 0 ? 64u * 1024u
            : page->script_limit;
        navigation_enable_document_scripts(
            &navigation, 32,
            script_limit > 256u * 1024u ? script_limit : 256u * 1024u,
            script_limit, timeout_ms);
    }
    HeapHold hold = {.navigation = &navigation, .leave_free = page->leave_free};
    if (navigation_ready && page->leave_free != 0) {
        navigation.heavy_policy_resolver = hold_heap_at_realm;
        navigation.heavy_policy_opaque = &hold;
    }
    char error[256] = {0};
    bool replaying = navigation_ready
        && fetch_trace_replay_begin_response_keyed(
               directory, error, sizeof(error));
    TilefinchTestFaults *faults = tilefinch_test_faults();
    FetchScheduler *held = NULL;
    uint64_t held_ids[STUCK_LOAD_LIMIT] = {0};
    bool holding = true;
    if (replaying && stuck != 0) {
        faults->replay_lane_worker_descriptors = true;
        faults->replay_lane_runs_page_hops = true;
        FetchSchedulerDomain *domain =
            fetch_scheduler_domain_create(&budget, stuck);
        held = domain == NULL ? NULL
            : fetch_scheduler_create_in_domain(domain, stuck, stuck * 4096u);
        fetch_scheduler_domain_release(domain);
        holding = held != NULL
            && fetch_scheduler_enable_background_transport(held, true);
        static const FetchRequest image_request = {
            .method = "GET", .accept = "image/png",
            .credentials = FETCH_CREDENTIALS_OMIT
        };
        for (size_t i = 0; holding && i < stuck; i++) {
            held_ids[i] = fetch_scheduler_enqueue(
                held, stuck_urls[i], &image_request, 4096u, 30000);
            holding = held_ids[i] != 0;
        }
    }
    uint64_t generation = replaying && holding
        ? navigation_begin(&navigation) : 0;
    uint64_t load_started_us = tilefinch_platform_monotonic_time_us();
    bool loaded = replaying && holding && navigation_load_url(
        &navigation, generation, PAGE_URL,
        page->document_limit == 0 ? 64u * 1024u : page->document_limit,
        timeout_ms, 480, NULL, NULL, true);
    uint64_t load_us =
        tilefinch_platform_monotonic_time_us() - load_started_us;
    if (hold.reserved) budget_reservation_release(&hold.reservation);
    ScriptResult result = {0};
    bool matched = false;
    if (loaded && page->stopped_order != NULL) {
        size_t length = 0;
        const char *recorded = document_attribute(
            document_body_node(&navigation.page.document), "data-order",
            &length);
        snprintf(result.summary, sizeof(result.summary), "%.*s|%s",
                 recorded == NULL ? 0 : (int) length,
                 recorded == NULL ? "" : recorded,
                 navigation.page.script_result.summary);
        char expected[sizeof(result.summary)];
        snprintf(expected, sizeof(expected), "%s|%s", page->stopped_order,
                 page->stopped_summary);
        matched = navigation.page.runtime == NULL
            && strcmp(result.summary, expected) == 0;
        if (!matched)
            fprintf(stderr, "  stopped page: runtime=%s expected \"%s\"\n",
                    navigation.page.runtime == NULL ? "gone" : "live",
                    expected);
    }
    if (loaded && page->recorded_check != NULL) {
        size_t length = 0;
        const char *recorded = document_attribute(
            document_body_node(&navigation.page.document), "data-order",
            &length);
        snprintf(result.summary, sizeof(result.summary), "%.*s",
                 recorded == NULL ? 0 : (int) length,
                 recorded == NULL ? "" : recorded);
        matched = page->recorded_check(result.summary);
    }
    for (size_t step = 0; loaded && !matched && page->stopped_order == NULL
                              && page->recorded_check == NULL
                              && step < 32; step++) {
        if (step != 0) (void) navigation_advance_runtime(&navigation, 1, 8);
        matched = navigation.page.runtime != NULL
            && script_runtime_evaluate_diagnostic(
                   navigation.page.runtime, probe, "<write-probe>", &result)
            && strcmp(result.summary, page->expected) == 0;
    }
    bool passed = matched || (page->may_refuse && !loaded);
    if (page->box_children != NULL) {
        char children[256];
        box_children(&navigation.page.document, children, sizeof(children));
        if (!loaded || strcmp(children, page->box_children) != 0) {
            fprintf(stderr, "  #box children \"%s\" expected \"%s\"\n",
                    children, page->box_children);
            passed = false;
        }
    }
    if (stuck != 0 && (load_us > page->max_load_us
                       || navigation.script_failed != page->stuck_failures)) {
        fprintf(stderr, "  beside %zu held loads: load took %llu us, "
                "%zu script(s) failed\n", stuck,
                (unsigned long long) load_us, navigation.script_failed);
        passed = false;
    }
    if (page->pressure_refusals != 0
        && navigation.script_skipped_pressure != page->pressure_refusals) {
        fprintf(stderr, "  %zu script(s) refused for memory pressure, "
                "expected %zu\n", navigation.script_skipped_pressure,
                page->pressure_refusals);
        passed = false;
    }
    if (page->leave_free != 0 && !hold.reserved) {
        fprintf(stderr, "  the page Budget was not held\n");
        passed = false;
    }
    if (page->stage_within_load
        && navigation.performance.parser_script_stage_us > load_us) {
        fprintf(stderr, "  parser-script stage charged %llu us in a %llu us "
                "load\n",
                (unsigned long long)
                    navigation.performance.parser_script_stage_us,
                (unsigned long long) load_us);
        passed = false;
    }
    if (page->cookies != NULL) {
        char cookies[256] = {0};
        (void) browser_session_cookie_get(&browser, PAGE_URL, cookies,
                                          sizeof(cookies));
        if (strcmp(cookies, page->cookies) != 0) {
            fprintf(stderr, "  cookies: \"%s\" expected \"%s\"\n",
                    cookies, page->cookies);
            passed = false;
        }
    }
    if (!passed) {
        fprintf(stderr,
                "document.write case \"%s\" failed: loaded=%d\n"
                "  order:    \"%s\"\n  expected: \"%s\"\n"
                "  error=\"%s\" navigation-error=\"%s\" replay-error=\"%s\"\n"
                "  scripts discovered=%zu attempted=%zu loaded=%zu "
                "failed=%zu\n",
                page->name, loaded, result.summary,
                page->expected == NULL ? "" : page->expected,
                result.error, navigation.last_error, error,
                navigation.script_discovered, navigation.script_attempted,
                navigation.script_loaded, navigation.script_failed);
    }
    /* The held loads still finish once their owner pumps. */
    for (size_t i = 0; held != NULL && i < stuck; i++) {
        FetchResult fetched = {0};
        bool success = false;
        for (unsigned turn = 0; held_ids[i] != 0 && turn < 64u
             && !fetch_scheduler_take(held, held_ids[i], &success, &fetched);
             turn++)
            (void) fetch_scheduler_pump(held, stuck, 1);
        fetch_result_destroy(&fetched);
    }
    if (held != NULL) fetch_scheduler_destroy(held);
    faults->replay_lane_worker_descriptors = false;
    faults->replay_lane_runs_page_hops = false;
    if (replaying) fetch_trace_end();
    if (navigation_ready) navigation_destroy(&navigation);
    if (browser_ready || blocker != NULL) {
        browser.content_blocker = NULL;
        content_blocker_destroy(blocker);
    }
    if (written && lexbor) browser_session_destroy(&browser);
    bool clean = budget.current == 0
        && budget_active_allocations(&budget, NULL) == 0;
    if (lexbor) clean = budget_uninstall_lexbor(&budget) && clean;
    if (written) remove_fixture(directory, count);
    if (!clean)
        fprintf(stderr, "document.write case \"%s\" leaked\n", page->name);
    return passed && clean;
}

#define SCRIPT(url_, body_) \
    {.url = (url_), .content_type = "text/javascript", .body = (body_)}

/* Records `s` in order and in the body's data-order attribute, which
   outlives a realm the page's JavaScript was stopped in. */
#define REC \
    "globalThis.order=[];function rec(s){order.push(s);" \
    "document.body.setAttribute('data-order',order.join(','))}"

/* Lists the element children of #box by tag name (and id when present). */
#define BOX_CHILDREN \
    "order.push([...document.getElementById('box').children].map(" \
    "e=>e.tagName+(e.id?'#'+e.id:'')).join('/'));"

/* A written inline script runs inside write(), before write() returns, and
   the markup written after it is parsed before write() returns too. The
   written nodes sit at the insertion point: after the writing script and
   before the markup that follows it in the response. */
static const PageCase inline_case = {
    .name = "written inline script runs inside write()",
    .html =
        "<!doctype html><body><div id=box><script>globalThis.order=[];"
        "document.write('<script>window.x=1;order.push(\"written\")"
        "<\\/script><span id=w></span>');"
        "order.push('returned:'+window.x+':'"
        "+!!document.getElementById('w'));</script><i id=tail></i></div>"
        "<script>" BOX_CHILDREN "</script></body>",
    .expected = "written,returned:1:true,SCRIPT/SCRIPT/SPAN#w/I#tail",
};

/* A written external script blocks the parser but not the writing script:
   it runs after the writing script returns, before the markup written
   after it (here by a second write() while it was pending) and before the
   response's next script. What it writes goes at its own insertion point,
   ahead of that pending markup. */
static const PageRecord external_resources[] = {
    SCRIPT("https://write.test/a.js",
           "window.aRan=1;order.push('a:'+!!document.getElementById('w'));"
           "document.write('<b id=fromA></b>');"),
};

static const PageCase external_case = {
    .name = "written external script blocks the parser",
    .html =
        "<!doctype html><body><div id=box><script>globalThis.order=[];"
        "document.write('<script src=\"/a.js\"><\\/script>');"
        "document.write('<span id=w></span>');"
        "order.push('returned:'+(window.aRan?1:0)+':'"
        "+!!document.getElementById('w'));</script><i id=tail></i></div>"
        "<script>order.push('next:'+!!document.getElementById('w'));"
        BOX_CHILDREN "</script></body>",
    .resources = external_resources,
    .resource_count = 1,
    .expected = "returned:0:false,a:false,next:true,"
        "SCRIPT/SCRIPT/B#fromA/SPAN#w/I#tail",
};

/* An inline script written by a written external script runs inside that
   script's write(); a chain of written external scripts runs in order. */
static const PageRecord chain_resources[] = {
    SCRIPT("https://write.test/one.js",
           "order.push('one');document.write('<script>order.push(\"inner\")"
           "<\\/script><script src=\"/two.js\"><\\/script>');"
           "order.push('one-returned');"),
    SCRIPT("https://write.test/two.js", "order.push('two');"),
};

static const PageCase chain_case = {
    .name = "written scripts write scripts",
    .html =
        "<!doctype html><body><script>globalThis.order=[];"
        "document.write('<script src=\"/one.js\"><\\/script>"
        "<script>order.push(\"tail\")<\\/script>');order.push('outer');"
        "</script><script>order.push('next')</script></body>",
    .resources = chain_resources,
    .resource_count = 2,
    .expected = "outer,one,inner,one-returned,two,tail,next",
};

/* Written markup continues the document's own input stream: an unclosed
   written element keeps the response text that follows the script. */
static const PageCase stream_case = {
    .name = "written markup continues the input stream",
    .html =
        "<!doctype html><body><script>globalThis.order=[];"
        "document.write('<b id=bold>');</script>after</b>"
        "<script>order.push(document.getElementById('bold').textContent)"
        "</script></body>",
    .expected = "after",
};

/* A script written across several write() calls runs when the call that
   writes its end tag parses it. */
static const PageCase split_case = {
    .name = "a written script split across writes",
    .html =
        "<!doctype html><body><script>globalThis.order=[];"
        "document.write('<scr'+'ipt>order.push(');"
        "document.write('\"split\")');order.push('open');"
        "document.write('<\\/script>');order.push('closed');</script>"
        "</body>",
    .expected = "open,split,closed",
};

/* A script that writes scripts forever is stopped by its own watchdog
   deadline, which the scripts it writes share instead of restarting: the
   writer and its written scripts get one script timeout in all. Each
   written script spins for RUNAWAY_SPIN_MS of wall time, so however fast
   or loaded the host, no more than the timeout divided by the spin start
   before the shared deadline. After it, scripts fail at once until three
   consecutive failures (NAVIGATION_PARSER_SCRIPT_FAILURE_LIMIT) stop the
   page's JavaScript; each may count itself before it is interrupted. On a
   desktop 22 or 23 run. When each written script restarted the deadline,
   all 256 the parser runs at most ran in full. The count is kept in the
   DOM, which outlives the realm. */
#define RUNAWAY_SPIN_MS 100
/* The failures the breaker allows, and one for rounding. */
#define RUNAWAY_SLACK 4

static bool runaway_stopped(const char *recorded)
{
    long count = strtol(recorded, NULL, 10);
    const long most = SCRIPT_TIMEOUT_MS / RUNAWAY_SPIN_MS + RUNAWAY_SLACK;
    if (count > 0 && count <= most) return true;
    fprintf(stderr, "  writing loop ran %ld written scripts (at most %ld "
            "fit a %d ms deadline)\n", count, most, SCRIPT_TIMEOUT_MS);
    return false;
}

static const PageCase runaway_case = {
    .name = "a writing loop is stopped",
    .html =
        "<!doctype html><body><script>globalThis.order=[];"
        "globalThis.n=0;for(;;)document.write('<script>"
        "document.body.setAttribute(\"data-order\",++n);"
        SPIN(RUNAWAY_SPIN_MS) "<\\/script>');</script></body>",
    .recorded_check = runaway_stopped,
};

/* A written script's time is part of its writer's: the parser-script
   stage (20 s on a device) is charged once for both. It used to be charged
   for the written script, and again twice through the writer's share of the
   shared execution-time counter, so pages whose written scripts did real
   work hit the stage limit early. Two written scripts spin here, one
   nested in the other's write, each for 60 ms of wall time: well inside
   the script timeout on any host, and the old triple charge still gives
   about three times the load's own time. */
static const PageCase stage_time_case = {
    .name = "written scripts' time is charged once",
    .html =
        "<!doctype html><body><script>globalThis.order=[];"
        "document.write('<script>" SPIN(60)
        "document.write(\"<script>" SPIN(60)
        "order.push(\\\\\"inner\\\\\")<\\\\/script>\");"
        "order.push(\"outer\")<\\/script>');order.push('writer');"
        "</script></body>",
    .expected = "inner,outer,writer",
    .stage_within_load = true,
};

/* Input waiting behind a pending script is bounded: a write that would
   take it past 256 KiB throws RangeError and is dropped; smaller writes
   still wait, and everything runs in order afterwards. */
static const PageRecord cap_resources[] = {
    SCRIPT("https://write.test/a.js", "order.push('a');"),
};

static const PageCase cap_case = {
    .name = "waiting input is capped at 256 KiB",
    .html =
        "<!doctype html><body><script>globalThis.order=[];"
        "document.write('<script src=\"/a.js\"><\\/script>');"
        "var big='<i></i>'.repeat(40000);"
        "try{document.write(big);order.push('no-throw')}"
        "catch(e){order.push(e.name)}"
        "document.write('<b id=small></b>');order.push('small-ok');"
        "</script><script>order.push('next:'+!!document.getElementById("
        "'small')+':'+document.getElementsByTagName('i').length)</script>"
        "</body>",
    .resources = cap_resources,
    .resource_count = 1,
    .expected = "RangeError,small-ok,a,next:true:0",
};

/* The content blocker still applies to a written external script; a
   blocked one does not hold up the parser. */
static const PageRecord blocked_resources[] = {
    SCRIPT("https://securepubads.g.doubleclick.net/tag/js/gpt.js",
           "order.push('ad');"),
    SCRIPT("https://write.test/ok.js", "order.push('ok');"),
};

static const PageCase blocker_case = {
    .name = "content blocker applies to written scripts",
    .html =
        "<!doctype html><body><script>globalThis.order=[];"
        "document.write('<script src=\"https://securepubads.g.doubleclick"
        ".net/tag/js/gpt.js\"><\\/script><script src=\"/ok.js\"><\\/script>"
        "');</script><script>order.push('next')</script></body>",
    .resources = blocked_resources,
    .resource_count = 2,
    .blocker = true,
    .expected = "ok,next",
};

/* CSP applies to written scripts: a written script is parser-inserted, so
   it needs its own nonce. */
static const PageRecord nonce_resources[] = {
    SCRIPT("https://write.test/nonced.js", "order.push('nonced-external');"),
    SCRIPT("https://write.test/bare.js", "order.push('bare-external');"),
};

static const PageCase nonce_case = {
    .name = "CSP nonces apply to written scripts",
    .policy = "script-src 'nonce-N1'",
    .html =
        "<!doctype html><body><script nonce=N1>globalThis.order=[];"
        "document.write('<script nonce=N1>order.push(\"nonced\")<\\/script>"
        "<script>order.push(\"bare\")<\\/script>"
        "<script src=\"/bare.js\"><\\/script>"
        "<script nonce=N1 src=\"/nonced.js\"><\\/script>');</script>"
        "<script nonce=N1>order.push('next')</script></body>",
    .resources = nonce_resources,
    .resource_count = 2,
    .expected = "nonced,nonced-external,next",
};

/* Chromium ignores a write() nested more than 21 deep; the page goes on. */
static const PageCase recursion_case = {
    .name = "nested writes are bounded",
    .html =
        "<!doctype html><body><script>globalThis.order=[];"
        "globalThis.depth=0;function w(){depth++;"
        "document.write('<script>w()<\\/script>')}w();</script>"
        "<script>order.push(depth)</script></body>",
    .expected = "22",
};

/* Written scripts that throw count towards the parser's circuit breaker
   (three failing scripts in a row stop the page's JavaScript). The breaker
   trips while the writing script is still inside write(): the realm must
   live on until that script returns, and only then be stopped. It used to
   be destroyed under the writer (an assertion in QuickJS, or a hang). */
static const PageCase failing_written_case = {
    .name = "written scripts that throw stop JavaScript after the writer",
    .html =
        "<!doctype html><body><script>" REC
        "document.write('<script>rec(\"a\");throw new Error(\"x1\")"
        "<\\/script><script>rec(\"b\");throw new Error(\"x2\")"
        "<\\/script><script>rec(\"c\");throw new Error(\"x3\")"
        "<\\/script><script>rec(\"d\")<\\/script>');"
        "rec('outer-after');var o={};for(var i=0;i<1000;i++)o['k'+i]=[i];"
        "rec('outer-done');</script><script>rec('next')</script></body>",
    .stopped_order = "a,b,c,outer-after,outer-done",
    .stopped_summary =
        "Limited page: JavaScript stopped after repeated failures",
};

/* The failure count runs across the page: two failing response scripts
   and one failing written script trip the breaker inside write(). */
static const PageCase failing_mixed_case = {
    .name = "a failing written script after failing response scripts",
    .html =
        "<!doctype html><body><script>" REC "undefinedFn1()</script>"
        "<script>rec('s2');undefinedFn2()</script><script>"
        "document.write('<script>rec(\"w\");undefinedFn3()<\\/script>');"
        "rec('writer-continues');</script><script>rec('next')</script>"
        "</body>",
    .stopped_order = "s2,w,writer-continues",
    .stopped_summary =
        "Limited page: JavaScript stopped after repeated failures",
};

/* A pending parsing-blocking script destroyed by its writer is forgotten
   (its node is gone), but the markup written behind it still waits at the
   insertion point: it is parsed before the response goes on and before
   markup written after it. It used to be parsed at the end of the feed
   window, wherever the tokenizer was (inside a later script's text). */
static const PageRecord forgotten_resources[] = {
    SCRIPT("https://write.test/a.js", "window.aRan=1;"),
};

static const PageCase forgotten_case = {
    .name = "input behind a destroyed pending script keeps its place",
    .html =
        "<!doctype html><body><div id=box><p id=keep></p><script>"
        "globalThis.order=[];var box=document.getElementById('box');"
        "document.write('<script src=\"/a.js\"><\\/script>"
        "<p id=rest></p>');box.innerHTML='<span id=fresh></span>';"
        "document.write('<i id=late></i>');</script><b id=resp></b></div>"
        "<script>order.push('aRan:'+window.aRan);</script><script>"
        BOX_CHILDREN "</script></body>",
    .resources = forgotten_resources,
    .resource_count = 1,
    .expected = "aRan:undefined,SPAN#fresh/P#rest/I#late/B#resp",
};

/* Child lists and live collections read before a write() see what it
   inserted, in the writing script and in the scripts it writes: a parser
   write moves the realm's DOM version like any other mutation. (The
   bootstrap's childNodes is an array snapshot, so it is read again.) */
static const PageCase lists_case = {
    .name = "lists read before a write see the written nodes",
    .html =
        "<!doctype html><body><div id=box><b></b><script id=A>"
        "globalThis.order=[];var box=document.getElementById('box'),"
        "ch=box.children,"
        "s=document.getElementsByTagName('script');"
        "order.push(box.childNodes.length+':'+ch.length+':'+s.length+':'"
        "+box.lastElementChild.id);"
        "document.write('<i id=q></i><script id=B>order.push(\"B:\""
        "+box.childNodes.length+\":\"+box.children.length+\":\""
        "+box.lastChild.id)<\\/script>');"
        "order.push(box.childNodes.length+':'+ch.length+':'+s.length+':'"
        "+s[s.length-1].id+':'+box.lastElementChild.id);"
        "</script></div></body>",
    .expected = "2:2:1:A,B:4:4:B,4:4:2:B:B",
};

/* A write() the parser cannot take (the page's memory is spent inside
   Lexbor) fails the parser and so the page. The writing script stops
   there, as for a refused allocation, instead of running on (and writing,
   reading the DOM, setting innerHTML) over a failed parser. */
static const PageCase failed_write_case = {
    .name = "a script stops at a write the parser fails",
    .html =
        "<!doctype html><body><div id=first></div><script>"
        "globalThis.order=[];var s='';for(var i=0;i<400;i++)"
        "s+='<div class=a><span>x</span><i>y</i></div>';"
        "for(var r=0;r<4000;r++)document.write(s);"
        "document.cookie='after=1';"
        "document.getElementById('first').innerHTML='<p>x</p>';"
        "order.push('ran-on');</script></body>",
    .expected = "never",
    .may_refuse = true,
    .cookies = "",
};

/* Markup a script writes is styled and laid out for that script's own
   reads: a write marks the realm's layout and computed styles stale, so a
   read rebuilds them (and the sheet a written <style> changed). Before,
   they waited for the next script checkpoint (width auto, offsetWidth 0). */
static const PageCase style_case = {
    .name = "written markup is styled for the writing script",
    .html =
        "<!doctype html><html><head><style>#m0{width:40px}</style></head>"
        "<body><div id=m0></div><script>globalThis.order=[];"
        "order.push('m0:'+getComputedStyle(document.getElementById('m0'))"
        ".width);document.write('<style>#m{width:50px;height:7px}</style>"
        "<div id=m></div>');var m=document.getElementById('m');"
        "order.push('cs:'+getComputedStyle(m).width,"
        "'ow:'+m.offsetWidth+'x'+m.offsetHeight);</script><script>"
        "var m=document.getElementById('m');order.push('later:'"
        "+getComputedStyle(m).width+':'+m.offsetWidth)</script></body>"
        "</html>",
    .expected = "m0:40px,cs:50px,ow:50x7,later:50px:50",
};

/* Basic refuses a written tag-manager script. A written external script
   is a pending parsing-blocking script: it is fetched (here refused) and
   its error event fires once the writer has returned, as in Chromium,
   where the refused request also fails after write() returns. The answer
   (dataLayer.hide.end(), queued and later eventCallbacks) runs once,
   before the first error event; a second refused container on the same
   layer adds nothing. */
#define GTM_WRITTEN \
    "<script src=\"https://www.googletagmanager.com/gtm.js?id=GTM-W\" " \
    "onerror=\"order.push(\\'error:\\'+writing)\"><\\/script>" \
    "<script src=\"https://www.googletagmanager.com/gtm.js?id=GTM-X\" " \
    "onerror=\"order.push(\\'error2:\\'+writing)\"><\\/script>"

#define GTM_LAYER \
    "globalThis.order=[];var writing=false;window.dataLayer=[{event:'q'," \
    "eventCallback:function(){order.push('queued-cb:'+writing)}}];" \
    "dataLayer.hide={end:function(){order.push('end:'+writing)}};"

static const PageCase gtm_case = {
    .name = "a written tag manager is answered once, before its error",
    .html =
        "<!doctype html><body><script>" GTM_LAYER
        "writing=true;document.write('" GTM_WRITTEN "');"
        "order.push('write-returned');writing=false;</script>"
        "<script>dataLayer.push({event:'later',eventCallback:function(){"
        "order.push('later-cb')}});order.push('next')</script></body>",
    .blocker = true,
    .expected = "write-returned,end:false,error:false,error2:false,"
        "queued-cb:false,next,later-cb",
};

/* A page that replaces its data layer after the answer (dataLayer = [])
   leaves the answered array behind: pushes on the new array are not
   answered, as a loaded GTM container, which patched the array it found,
   does not see them either. Pushes through a reference to the old array
   still are. */
static const PageCase replaced_layer_case = {
    .name = "a data layer replaced after the answer is not answered",
    .html =
        "<!doctype html><body><script>" GTM_LAYER
        "document.write('" GTM_WRITTEN "');</script>"
        "<script>var old=dataLayer;window.dataLayer=[];"
        "dataLayer.push({event:'x',eventCallback:function(){"
        "order.push('replaced-cb')}});old.push({event:'y',"
        "eventCallback:function(){order.push('old-cb')}});"
        "order.push('next')</script></body>",
    .blocker = true,
    .expected = "end:false,error:false,error2:false,queued-cb:false,next,"
        "old-cb",
};

/* A written external script uses the blocking lane (900dca6e), as a
   parsed one does (test_browser_engine's
   test_parser_blocking_script_beside_stuck_loads): four page loads of
   another scheduler that nothing pumps, and the document's own stream,
   still arriving, hold every ordinary worker descriptor. The written
   script loads at once and runs before the next response script. On the
   ordinary lane it waits out its timeout and fails. */
static const PageRecord lane_resources[] = {
    {.url = "https://write.test/lane.js", .content_type = "text/javascript",
     .body = "globalThis.laneRan=true;order.push('lane')",
     .header = "cache-control: no-store"},
};

#define LANE_PAD (256u * 1024u)
static char lane_html[LANE_PAD + 1024u];
static char watchdog_lane_html[LANE_PAD + 1024u];

static PageCase lane_case = {
    .name = "a written script beside held loads takes the blocking lane",
    .resources = lane_resources,
    .resource_count = 1,
    .stuck_loads = 4,
    .max_load_us = UINT64_C(1000000),
    .document_limit = 1024u * 1024u,
    .expected = "writer,lane,next:true",
};

/* The watchdog stops a writer while the external script it wrote is
   pending, beside the same held loads. The writer's interruption does
   not carry to the pending script: it is fetched on the lane, runs with
   its own deadline after the writer, and the markup written behind it is
   parsed at the insertion point, before the response's next markup. */
static const PageRecord watchdog_lane_resources[] = {
    {.url = "https://write.test/a.js", .content_type = "text/javascript",
     .body = "window.aRan=1;", .header = "cache-control: no-store"},
};

static PageCase watchdog_lane_case = {
    .name = "a writer stopped by the watchdog leaves its pending script",
    .resources = watchdog_lane_resources,
    .resource_count = 1,
    .timeout_ms = 300,
    .stuck_loads = 4,
    .max_load_us = UINT64_C(1500000),
    /* The writer. */
    .stuck_failures = 1,
    .document_limit = 1024u * 1024u,
    .expected = "spinning,aRan:1,SCRIPT/SCRIPT/P#rest/B#resp",
};

/* The same stop as the third failure in a row: the page's JavaScript is
   stopped after the writer returns, so the pending script is forgotten
   (never fetched), but the markup written behind it still keeps its
   place in the DOM, ahead of the response's. */
static const PageCase watchdog_stopped_case = {
    .name = "a writer stopped as the third failure leaves no input behind",
    .html =
        "<!doctype html><body><script>" REC "undefinedFn1()</script>"
        "<script>rec('s2');undefinedFn2()</script><div id=box><script>"
        "document.write('<script src=\"/a.js\"><\\/script><p id=rest></p>');"
        "rec('spinning');for(;;);</script><b id=resp></b></div>"
        "<script>rec('next')</script></body>",
    .resources = forgotten_resources,
    .resource_count = 1,
    .timeout_ms = 300,
    .stopped_order = "s2,spinning",
    .stopped_summary =
        "Limited page: JavaScript stopped after repeated failures",
    .box_children = "SCRIPT/SCRIPT/P#rest/B#resp",
};

/* A written external script whose planned compile does not fit the
   memory left (script_admit_known_working_set, a38ee032) is refused
   before it compiles, as memory pressure, with an error event; the writer
   has gone on, the markup written behind it and the response's next
   scripts follow in order, and the realm lives on. The page Budget is
   held to 4 MiB free once the realm exists; the script is a 400 KB
   function registry (a planned compile peak of about 1 MB). At this size
   the Budget arm of the admission refuses it: the reachable-heap arm
   refuses only in a narrow band of a nearly full realm heap, below the
   band where such a registry is admitted and then refused at compile
   time (the gap a38ee032 left open). */
static char registry_body[400u * 1024u];
static const PageRecord registry_resources[] = {
    {.url = "https://write.test/big.js", .content_type = "text/javascript",
     .body = registry_body},
};

static const PageCase admission_case = {
    .name = "a written script refused for memory leaves the writer going",
    .html =
        "<!doctype html><body><div id=box><script>globalThis.order=[];"
        "document.write('<script src=\"/big.js\" "
        "onerror=\"order.push(\\'big-error\\')\" "
        "onload=\"order.push(\\'big-load\\')\"><\\/script><p id=rest></p>');"
        "order.push('writer');</script><b id=resp></b></div>"
        "<script>order.push('next:'+!!window.bigRan);" BOX_CHILDREN
        "</script></body>",
    .resources = registry_resources,
    .resource_count = 1,
    .script_limit = 512u * 1024u,
    .leave_free = 4u * MIB,
    .pressure_refusals = 1,
    .expected = "writer,big-error,next:false,SCRIPT/SCRIPT/P#rest/B#resp",
};

static void build_generated_pages(void)
{
    static const char tail[] = "--></body>";
    static const char lane_head[] =
        "<!doctype html><body><p>lane</p><script>globalThis.order=[];"
        "document.write('<script src=\"/lane.js\"><\\/script>');"
        "order.push('writer')</script><script>order.push('next:'"
        "+(globalThis.laneRan===true))</script><p>after</p><!--";
    static const char watchdog_head[] =
        "<!doctype html><body><div id=box><script>globalThis.order=[];"
        "document.write('<script src=\"/a.js\"><\\/script><p id=rest></p>');"
        "order.push('spinning');for(;;);</script><b id=resp></b></div>"
        "<script>order.push('aRan:'+window.aRan);" BOX_CHILDREN
        "</script><!--";
    /* Long enough that the document is still arriving, and so still on
       its own descriptor, when the parser reaches the written script. */
    char *pages[] = {lane_html, watchdog_lane_html};
    const char *heads[] = {lane_head, watchdog_head};
    for (size_t i = 0; i < 2; i++) {
        size_t head = strlen(heads[i]);
        memcpy(pages[i], heads[i], head);
        memset(pages[i] + head, 'x', LANE_PAD);
        memcpy(pages[i] + head + LANE_PAD, tail, sizeof(tail));
    }
    lane_case.html = lane_html;
    watchdog_lane_case.html = watchdog_lane_html;
    size_t at = (size_t) snprintf(registry_body, sizeof(registry_body),
                                  "%s", "globalThis.registry={");
    for (size_t i = 0; at + 200u < sizeof(registry_body); i++)
        at += (size_t) snprintf(
            registry_body + at, sizeof(registry_body) - at,
            "%zu:function(e,t,n){var r=n(%zu),a='label text %zu';"
            "e.exports={id:%zu,size:r.length+a.length}},", i, i / 2u, i, i);
    snprintf(registry_body + at, sizeof(registry_body) - at, "%s",
             "};globalThis.bigRan=true");
}

/* The parser-level half of a failed write: the write reports the failure,
   the streaming parser it left in Lexbor's error state is not cleaned and
   reused by an innerHTML fragment parse (Lexbor would otherwise run the
   fragment on it under the document's DocumentParser), and the response
   is not fed again. */
typedef struct {
    Budget *budget;
    size_t scripts;
    DocumentWriteOutcome outcome;
    bool inner_html_kept_parser;
} FailedWriteProbe;

static bool failed_write_closed(void *opaque, PocDocument *document,
                                lxb_dom_node_t *element)
{
    FailedWriteProbe *probe = opaque;
    if (element->local_name != LXB_TAG_SCRIPT) return true;
    if (probe->scripts++ != 0) return true;
    static char markup[64u * 1024u];
    static const char row[] = "<div class=a><span>x</span><i>y</i></div>";
    size_t length = 0;
    while (length + sizeof(row) <= sizeof(markup)) {
        memcpy(markup + length, row, sizeof(row) - 1u);
        length += sizeof(row) - 1u;
    }
    budget_inject_failure_after(probe->budget, 0);
    probe->outcome = document_parser_write(document, markup, length);
    budget_clear_failure_injection(probe->budget);
    lxb_html_parser_t *streaming = document->html->dom_document.parser;
    lxb_dom_node_t *target = document_body_node(document) == NULL ? NULL
        : document_body_node(document)->first_child;
    bool failed_state = streaming != NULL
        && lxb_html_parser_state(streaming) == LXB_HTML_PARSER_STATE_ERROR;
    (void) document_set_element_inner_html(document, target, "<p>x</p>", 8);
    probe->inner_html_kept_parser = failed_state
        && document->html->dom_document.parser == streaming
        && lxb_html_parser_state(streaming) == LXB_HTML_PARSER_STATE_ERROR;
    return true;
}

static bool run_failed_write_parser_case(void)
{
    static const char html[] =
        "<!doctype html><body><div id=t></div><script>w()</script>"
        "<script>later()</script></body>";
    Budget budget;
    budget_init(&budget, 32u * MIB);
    if (!budget_install_lexbor(&budget)) return false;
    DocumentParser parser;
    FailedWriteProbe probe = {.budget = &budget};
    bool begun = document_parser_begin(&parser, &budget);
    if (begun) {
        (void) document_parser_set_scripting(&parser, true);
        document_parser_set_element_closed_callback(
            &parser, failed_write_closed, &probe);
    }
    bool fed = begun && document_parser_feed(&parser, html, sizeof(html) - 1);
    bool passed = begun && !fed && probe.scripts == 1
        && probe.outcome == DOCUMENT_WRITE_FAILED
        && probe.inner_html_kept_parser;
    if (begun) document_parser_abort(&parser);
    bool clean = budget.current == 0
        && budget_active_allocations(&budget, NULL) == 0;
    clean = budget_uninstall_lexbor(&budget) && clean;
    if (!passed || !clean)
        fprintf(stderr,
                "failed-write parser case: fed=%d scripts=%zu outcome=%d "
                "inner-html-kept-parser=%d clean=%d\n",
                fed, probe.scripts, (int) probe.outcome,
                probe.inner_html_kept_parser, clean);
    return passed && clean;
}

int main(void)
{
    const PageCase *cases[] = {
        &inline_case, &external_case, &chain_case, &stream_case,
        &split_case, &blocker_case, &nonce_case, &recursion_case,
        &runaway_case, &failing_written_case, &failing_mixed_case,
        &forgotten_case, &lists_case, &failed_write_case,
        &style_case, &cap_case, &stage_time_case, &gtm_case,
        &replaced_layer_case, &lane_case, &watchdog_lane_case,
        &watchdog_stopped_case, &admission_case,
    };
    build_generated_pages();
    size_t failures = run_failed_write_parser_case() ? 0 : 1;
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        if (!run_page_case(cases[i])) failures++;
    }
    if (failures != 0) {
        fprintf(stderr, "tilefinch-document-write-tests: %zu failure(s)\n",
                failures);
        return 1;
    }
    puts("tilefinch-document-write-tests: ok");
    return 0;
}
