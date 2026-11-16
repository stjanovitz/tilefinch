/* HTML "report an exception" for script evaluation: an exception a
   classic or module script throws while it runs (or a SyntaxError while it
   compiles) fires a cancelable ErrorEvent at the Window before anything is
   logged, exactly as an exception thrown by a timer or event callback
   does. window.onerror gets (message, filename, lineno, colno, error) and
   cancels the default report by returning true; a listener cancels it with
   preventDefault(). A classic script fetched cross-origin without CORS
   reports a muted error ("Script error.", no location, null error). Each
   case loads a replayed document and reads the log its scripts kept, then
   the default reports: the host log (TILEFINCH_TRACE_CONSOLE's
   "page-console[error]" lines on stderr). The page's own console.error is
   not the default report, so it must never see one. */
#include "tilefinch/budget.h"
#include "tilefinch/fetch.h"
#include "tilefinch/js_runtime.h"
#include "tilefinch/navigation.h"
#include "tilefinch/session.h"
#include "../src/tilefinch_test_faults.h"

#include <fcntl.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define MIB (1024u * 1024u)
#define PAGE_URL "https://errors.test/"

typedef struct {
    const char *url;
    const char *content_type;
    const char *body;
    /* One extra response header line ("name: value"), or NULL. */
    const char *header;
    /* The HTTP status; 0 is 200. */
    int status;
} PageRecord;

static bool write_fixture(char directory[128], const PageRecord *records,
                          size_t count)
{
    snprintf(directory, 128, "%s", "/tmp/tilefinch-script-errors-XXXXXX");
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
            "status=%d\nlength=%zu\nresponse-body-hash=%016llx\n"
            "effective-url=%s\ncontent-type=%s\netag=\nlast-modified=\n"
            "cf-mitigated=\naccept-ch=\ncritical-ch=\nserver=fixture\n"
            "cf-ray=\nresponse-referrer-policy-metadata-valid=1\n"
            "response-referrer-policy-present=0\n"
            "response-referrer-policy=\n"
            "response-security-headers-truncated=0\n"
            "response-header-count=%d\nset-cookie-count=0\n"
            "response-header-0=content-type: %s\n",
            records[i].url, records[i].url,
            records[i].status == 0 ? 200 : records[i].status, length,
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
    snprintf(path, sizeof(path), "%s/console.log", directory);
    (void) unlink(path);
    (void) rmdir(directory);
}

typedef struct {
    const char *name;
    const char *html;
    const PageRecord *resources;
    size_t resource_count;
    /* The page's log.join(','), read after the load and after each
       runtime advance until it matches, then " >> " and the host log's
       "page-console[error]" lines (their first lines) joined by ','. */
    const char *expected;
    /* Sets pocSummary instead of `probe`, or NULL. */
    const char *probe;
    /* The document's extra response header ("name: value"), or NULL. */
    const char *page_header;
    /* Once the log matches, advance the runtime this many more times and
       require it unchanged (nothing else, such as a second fallback copy,
       runs later). */
    size_t settle;
    /* Refuse this (1-based) heap allocation of the page's first inline
       event handler compile (0: none). */
    unsigned refuse_handler_allocation;
} PageCase;

static const char probe[] =
    "globalThis.pocSummary=(globalThis.log||['no-log']).join(',')";

static bool run_page_case(const PageCase *page)
{
    PageRecord records[16];
    records[0] = (PageRecord) {
        .url = PAGE_URL, .content_type = "text/html; charset=utf-8",
        .body = page->html, .header = page->page_header
    };
    size_t count = 1;
    for (size_t i = 0; i < page->resource_count && count < 16; i++)
        records[count++] = page->resources[i];
    char directory[128] = {0};
    bool written = write_fixture(directory, records, count);
    Budget budget;
    budget_init(&budget, 32u * MIB);
    bool lexbor = budget_install_lexbor(&budget);
    BrowserSession browser = {0};
    NavigationSession navigation = {0};
    bool browser_ready = written && lexbor
        && browser_session_init(&browser, &budget, 256u * 1024u);
    bool navigation_ready = browser_ready
        && navigation_init(&navigation, &budget, 4);
    if (navigation_ready) {
        navigation_attach_browser_session(&navigation, &browser);
        navigation_enable_scripts(&navigation, 8u * MIB, 2000);
        navigation_enable_document_scripts(
            &navigation, 16, 256u * 1024u, 64u * 1024u, 2000);
    }
    /* The host log goes to stderr while the page loads and runs. */
    char console_path[192];
    snprintf(console_path, sizeof(console_path), "%s/console.log",
             directory);
    fflush(stderr);
    int saved_stderr = written ? dup(2) : -1;
    int console_fd = saved_stderr < 0 ? -1
        : open(console_path, O_CREAT | O_TRUNC | O_WRONLY, 0600);
    if (console_fd >= 0) {
        (void) dup2(console_fd, 2);
        (void) close(console_fd);
    }
    char error[256] = {0};
    bool replaying = navigation_ready
        && fetch_trace_replay_begin_response_keyed(
               directory, error, sizeof(error));
    uint64_t generation = replaying ? navigation_begin(&navigation) : 0;
    /* Script deadlines count work, not wall time, so a loaded machine
       cannot turn an out-of-memory stop into a timeout. */
    tilefinch_test_faults()->watchdog_poll_ms = 1u;
    tilefinch_test_faults()->inline_handler_refuse_allocation =
        page->refuse_handler_allocation;
    bool loaded = replaying && navigation_load_url(
        &navigation, generation, PAGE_URL, 64u * 1024u, 2000, 480,
        NULL, NULL, true);
    ScriptResult result = {0};
    const char *bar = strstr(page->expected, " >> ");
    size_t log_length = bar == NULL ? strlen(page->expected)
        : (size_t) (bar - page->expected);
    bool matched = false;
    for (size_t step = 0; loaded && !matched && step < 48; step++) {
        if (step != 0) (void) navigation_advance_runtime(&navigation, 1, 8);
        matched = navigation.page.runtime != NULL
            && script_runtime_evaluate_diagnostic(
                   navigation.page.runtime,
                   page->probe == NULL ? probe : page->probe,
                   "<error-probe>", &result)
            && strlen(result.summary) == log_length
            && memcmp(result.summary, page->expected, log_length) == 0;
    }
    for (size_t step = 0; matched && step < page->settle; step++) {
        (void) navigation_advance_runtime(&navigation, 1, 8);
        matched = script_runtime_evaluate_diagnostic(
                navigation.page.runtime,
                page->probe == NULL ? probe : page->probe,
                "<error-probe>", &result)
            && strlen(result.summary) == log_length
            && memcmp(result.summary, page->expected, log_length) == 0;
    }
    fflush(stderr);
    if (saved_stderr >= 0) {
        (void) dup2(saved_stderr, 2);
        (void) close(saved_stderr);
    }
    /* summary >> first line of each host-log entry, after its prefix. */
    static const char prefix[] = "page-console[error] ";
    char reported[sizeof(result.summary) + 2048];
    size_t used = (size_t) snprintf(reported, sizeof(reported), "%s >> ",
                                    result.summary);
    bool first = true;
    FILE *console = fopen(console_path, "rb");
    char line[1024];
    while (console != NULL && fgets(line, sizeof(line), console) != NULL) {
        if (strncmp(line, prefix, sizeof(prefix) - 1) != 0) continue;
        line[strcspn(line, "\n")] = '\0';
        int added = snprintf(reported + used, sizeof(reported) - used,
                             "%s%s", first ? "" : ",",
                             line + sizeof(prefix) - 1);
        if (added < 0 || (size_t) added >= sizeof(reported) - used) break;
        used += (size_t) added;
        first = false;
    }
    if (console != NULL) fclose(console);
    matched = matched && strcmp(reported, page->expected) == 0;
    if (!matched) {
        fprintf(stderr,
                "script error case \"%s\" failed: loaded=%d\n"
                "  reported: \"%s\"\n  expected: \"%s\"\n"
                "  error=\"%s\" navigation-error=\"%s\" replay-error=\"%s\"\n"
                "  scripts discovered=%zu attempted=%zu loaded=%zu "
                "failed=%zu\n",
                page->name, loaded, reported, page->expected,
                result.error, navigation.last_error, error,
                navigation.script_discovered, navigation.script_attempted,
                navigation.script_loaded, navigation.script_failed);
    }
    if (replaying) fetch_trace_end();
    if (navigation_ready) navigation_destroy(&navigation);
    tilefinch_test_faults()->watchdog_poll_ms = 0;
    tilefinch_test_faults()->inline_handler_refuse_allocation = 0;
    if (browser_ready) browser_session_destroy(&browser);
    bool clean = budget.current == 0
        && budget_active_allocations(&budget, NULL) == 0;
    if (lexbor) clean = budget_uninstall_lexbor(&budget) && clean;
    if (written) remove_fixture(directory, count);
    if (!clean)
        fprintf(stderr, "script error case \"%s\" leaked\n", page->name);
    return matched && clean;
}

#define SCRIPT(url_, body_) \
    {.url = (url_), .content_type = "text/javascript", .body = (body_)}

/* The first script of every page, on the document's first line (so an
   inline script's line numbers are the document's, as Chromium counts
   them): log/rec, a page console.error that would log "page-console:"
   entries if the default report reached it, and where()/shape()
   describing a report. Every other script stays on that line too unless a
   case says otherwise. */
#define PRELUDE                                                             \
    "<!doctype html><body><script>globalThis.log=[];"                       \
    "globalThis.rec=s=>log.push(s);"                                        \
    "console.error=(...a)=>rec('page-console:'+a.join(' '));"              \
    "globalThis.where=(m,f,l,c,e)=>[m,f===location.href?'doc':f,l,"         \
    "c>0?'col':c,e===undefined?'undef':e===null?'null'"                     \
    ":e===globalThis.thrown?'same':e instanceof SyntaxError?'syntax'"       \
    ":String(e)].join('|');"                                                \
    "globalThis.shape=e=>[e instanceof ErrorEvent,e.isTrusted,"             \
    "e.cancelable,where(e.message,e.filename,e.lineno,e.colno,e.error),"    \
    "e.defaultPrevented].join('|');"

#define ONERROR "onerror=(m,f,l,c,e)=>{rec('on:'+where(m,f,l,c,e))};"

/* An initial inline script that throws: onerror's five arguments, then a
   registered listener's trusted, cancelable ErrorEvent with the same
   exception object, then the default report; the next script runs. */
static const PageCase inline_case = {
    .name = "initial inline classic script",
    .html =
        PRELUDE ONERROR
        "addEventListener('error',e=>rec('ev:'+shape(e)));rec('s1');\n"
        "throw globalThis.thrown=new Error('inline-boom')</script>"
        "<script>rec('next')</script></body>",
    .expected =
        "s1,on:inline-boom|doc|2|col|same,"
        "ev:true|true|true|inline-boom|doc|2|col|same|false,next"
        " >> Uncaught script Error: inline-boom",
};

/* onerror returning true cancels the default report (and a later listener
   sees the event canceled); returning false does not; a listener's
   preventDefault() cancels it too. */
static const PageCase cancel_case = {
    .name = "return true and preventDefault cancel the report",
    .html =
        PRELUDE "onerror=()=>{rec('on');return true};"
        "addEventListener('error',e=>rec('ev:'+e.message+':'"
        "+e.defaultPrevented));throw new Error('cancel-a')</script>"
        "<script>rec('ok1')</script>"
        "<script>onerror=()=>{rec('on-false');return false};"
        "throw new Error('keep-b')</script>"
        "<script>rec('ok2')</script>"
        "<script>onerror=null;addEventListener('error',e=>e.preventDefault());"
        "throw new Error('cancel-c')</script>"
        "<script>rec('next')</script></body>",
    .expected =
        "on,ev:cancel-a:true,ok1,on-false,ev:keep-b:false,ok2,"
        "ev:cancel-c:false,next >> Uncaught script Error: keep-b",
};

/* A handler that throws is reported to the console without firing another
   error event (no recursion); the rest of the listeners still run, and
   the original exception is still reported. */
static const PageCase handler_throws_case = {
    .name = "error handlers that throw",
    .html =
        PRELUDE "let calls=0;onerror=()=>{calls++;rec('on');"
        "throw new Error('handler-boom')};"
        "addEventListener('error',()=>{throw new Error('listener-boom')});"
        "addEventListener('error',e=>rec('ev:'+e.message));"
        "throw new Error('outer-boom')</script>"
        "<script>rec('next:'+calls)</script></body>",
    .expected =
        "on,ev:outer-boom,next:1 >> Uncaught error handler Error: handler-boom,"
        "Uncaught event error Error: listener-boom,"
        "Uncaught script Error: outer-boom",
};

/* External classic scripts report their own URL and line; a SyntaxError
   reports where the compile stopped with the SyntaxError itself. A
   cross-origin script fetched without CORS reports a muted error; with
   crossorigin (and a CORS response) it reports everything. */
static const PageRecord external_resources[] = {
    SCRIPT("https://errors.test/a.js",
           "rec('a');\nthrow globalThis.thrown=new Error('ext-boom')"),
    SCRIPT("https://errors.test/syntax.js", "rec('never');\nvar = 1;"),
    SCRIPT("https://other.test/x.js",
           "rec('x');throw globalThis.thrown=new Error('secret-boom')"),
    {.url = "https://other.test/y.js", .content_type = "text/javascript",
     .body = "rec('y');throw globalThis.thrown=new Error('cors-boom')",
     .header = "access-control-allow-origin: *"},
};

static const PageCase external_case = {
    .name = "external classic scripts, SyntaxError and muted errors",
    .html =
        PRELUDE ONERROR "</script>"
        "<script src='https://errors.test/a.js'></script>"
        "<script>rec('ok1')</script>"
        "<script src='https://errors.test/syntax.js'></script>"
        "<script>rec('ok2')</script>"
        "<script src='https://other.test/x.js'></script>"
        "<script>rec('ok3')</script>"
        "<script src='https://other.test/y.js' crossorigin></script>"
        "<script>rec('next')</script></body>",
    .resources = external_resources,
    .resource_count = sizeof(external_resources)
        / sizeof(external_resources[0]),
    .expected =
        "a,on:ext-boom|https://errors.test/a.js|2|col|same,ok1,"
        "on:variable name expected|https://errors.test/syntax.js|2|col|syntax,"
        "ok2,x,on:Script error.||0|0|null,ok3,"
        "y,on:cors-boom|https://other.test/y.js|1|col|same,next"
        " >> Uncaught script Error: ext-boom,"
        "Uncaught script SyntaxError: variable name expected,"
        "Uncaught script Error: secret-boom,"
        "Uncaught script Error: cors-boom",
};

/* An inline SyntaxError reports the line inside the document. */
/* Error listeners may inspect, replace and clear the handler attribute.
   Suppressing its duplicate invocation must not mutate that live record. */
static const PageCase handler_mutation_case = {
    .name = "error listeners preserve handler attribute changes",
    .html =
        PRELUDE
        "globalThis.first=(m)=>{rec('first:'+m);return true};"
        "globalThis.second=(m)=>{rec('second:'+m);return true};"
        "onerror=first;let n=0;addEventListener('error',e=>{"
        "rec('visible:'+(onerror===(n===0?first:second)));"
        "onerror=n++===0?second:null;e.preventDefault()});"
        "</script><script>throw new Error('one')</script>"
        "<script>rec('replaced:'+(onerror===second));"
        "throw new Error('two')</script>"
        "<script>rec('cleared:'+(onerror===null))</script></body>",
    .expected = "first:one,visible:true,replaced:true,second:two,"
                "visible:true,cleared:true >> ",
};

static const PageCase inline_syntax_case = {
    .name = "inline SyntaxError",
    .html =
        PRELUDE ONERROR "</script><script>rec('never');\n"
        "var = 1;</script><script>rec('next')</script></body>",
    .expected =
        "on:variable name expected|doc|2|col|syntax,next"
        " >> Uncaught script SyntaxError: variable name expected",
};

/* Module scripts, external and inline, report their evaluation error and
   a parse error the same way. */
static const PageRecord module_resources[] = {
    SCRIPT("https://errors.test/m.js",
           "rec('m');\nthrow globalThis.thrown=new Error('module-boom')"),
};

static const PageCase module_case = {
    .name = "module scripts",
    .html =
        PRELUDE ONERROR "</script>"
        "<script type=module src='https://errors.test/m.js'></script>"
        "<script type=module>rec('im');\n"
        "throw globalThis.thrown=new Error('inline-module-boom')</script>"
        "<script type=module>rec('never');\n+++;</script></body>",
    .resources = module_resources,
    .resource_count = 1,
    .expected =
        "m,on:module-boom|https://errors.test/m.js|2|col|same,im,"
        "on:inline-module-boom|doc|2|col|same,"
        "on:unexpected token in expression: ';'|doc|2|col|syntax"
        " >> Uncaught script Error: module-boom,"
        "Uncaught script Error: inline-module-boom,"
        "Uncaught script SyntaxError: unexpected token in expression: ';'",
};

/* Scripts document.write() and appendChild() insert report their errors;
   the write() and appendChild() calls return normally. */
static const PageRecord inserted_resources[] = {
    SCRIPT("https://errors.test/dyn.js",
           "rec('dyn');throw globalThis.thrown=new Error('dyn-boom')"),
};

static const PageCase inserted_case = {
    .name = "document.write and appendChild scripts",
    .html =
        PRELUDE ONERROR
        "document.write('<script>rec(\"w\");throw globalThis.thrown="
        "new Error(\"write-boom\")<\\/script>');rec('after-write');</script>"
        "<script>const s=document.createElement('script');"
        "s.textContent='rec(\"d\");throw globalThis.thrown="
        "new Error(\"append-boom\")';document.body.appendChild(s);"
        "rec('after-append');const x=document.createElement('script');"
        "x.src='https://errors.test/dyn.js';document.body.appendChild(x);"
        "</script></body>",
    .resources = inserted_resources,
    .resource_count = 1,
    .expected =
        "w,on:write-boom|doc|1|col|same,after-write,"
        "d,on:append-boom|doc|1|col|same,after-append,"
        "dyn,on:dyn-boom|https://errors.test/dyn.js|1|col|same"
        " >> Uncaught script Error: write-boom,"
        "Uncaught script Error: append-boom,"
        "Uncaught script Error: dyn-boom",
};

/* A Worker's top-level exception goes to the worker global's onerror
   first, then (not canceled there) to the Worker object's error event,
   whose error is null; the Window's onerror sees nothing once the Worker
   object's handler cancels it. The worker's message and the Worker's
   error event are separate tasks, so the log is read sorted. */
static const PageCase worker_case = {
    .name = "worker top-level exception",
    .html =
        PRELUDE "onerror=m=>{rec('window:'+m)};"
        "const src='self.onerror=(m,f,l,c,e)=>{postMessage(\"scope:\"+m+\":\""
        "+(e instanceof Error))};throw new Error(\"worker-boom\")';"
        "const url=URL.createObjectURL(new Blob([src]));"
        "const w=new Worker(url);w.onmessage=e=>rec(e.data);"
        "w.onerror=e=>{rec('worker:'+(e instanceof ErrorEvent)+':'+e.message"
        "+':'+(e.filename===url)+':'+e.error);e.preventDefault()};"
        "</script></body>",
    .expected =
        "scope:worker-boom:true,worker:true:worker-boom:true:null >> ",
    .probe = "globalThis.pocSummary=(globalThis.log||['no-log']).slice()"
        ".sort().join(',')",
};

/* A timer callback's exception takes the same report: an ErrorEvent, and
   onerror's true cancels the host log (callbacks report no location). */
static const PageCase timer_case = {
    .name = "timer callback exceptions share the report",
    .html =
        PRELUDE "let n=0;onerror=(m,f,l,c,e)=>{rec('on:'+where(m,f,l,c,e));"
        "return ++n===1};addEventListener('error',e=>rec('ev:'+shape(e)));"
        "setTimeout(()=>{throw globalThis.thrown=new Error('timer-a')},0);"
        "setTimeout(()=>{throw globalThis.thrown=new Error('timer-b')},5);"
        "</script></body>",
    .expected =
        "on:timer-a||0|0|same,ev:true|true|true|timer-a||0|0|same|true,"
        "on:timer-b||0|0|same,ev:true|true|true|timer-b||0|0|same|false"
        " >> Uncaught timer callback Error: timer-b",
};

/* A module Worker's top-level exception reaches the Worker object too. */
static const PageCase module_worker_case = {
    .name = "module worker top-level exception",
    .html =
        PRELUDE
        "const src='throw new Error(\"module-worker-boom\")';"
        "const url=URL.createObjectURL(new Blob([src],"
        "{type:'text/javascript'}));"
        "const w=new Worker(url,{type:'module'});"
        "w.onerror=e=>{rec('worker:'+e.message);e.preventDefault()};"
        "</script></body>",
    .expected = "worker:module-worker-boom >> ",
};

/* The script element's own event (HTML "execute the script element"):
   `error` only when there is no script to run (a failed fetch, an HTTP
   error, a CSP, integrity or MIME refusal, or one of the browser's own
   refusals); otherwise `load`, after the script ran, even when it threw
   or did not compile: that exception goes to the Window's onerror first,
   as in Chromium, and then its microtasks. A page's onerror fallback to a second copy of a library
   therefore runs only when the first copy never ran. */
#define QUIET_ONERROR "onerror=m=>{rec('on:'+m);return true};"
#define EXT_EVENTS(name_, url_, attributes_)                                 \
    "<script src='" url_ "'" attributes_ " onload=\"rec('L:" name_ "')\""    \
    " onerror=\"rec('E:" name_ "')\"></script>"
#define EXT(name_, url_) EXT_EVENTS(name_, url_, "")
#define MOD(name_, url_) EXT_EVENTS(name_, url_, " type=module")
/* A script that runs cleanly, between the browser's refusals: three of
   those in a row stop the page's parser scripts (its failure breaker). */
#define INLINE_OK "<script>rec('i')</script>"
#define MISSING(url_)                                                        \
    {.url = (url_), .content_type = "text/javascript",                       \
     .body = "rec('missing-body-ran')", .status = 404}

static const PageRecord parser_event_resources[] = {
    SCRIPT("https://errors.test/ok.js", "rec('ok')"),
    SCRIPT("https://errors.test/throw.js",
           "rec('t');throw new Error('t-boom')"),
    SCRIPT("https://errors.test/syntax.js", "rec('never');\nvar = 1;"),
    MISSING("https://errors.test/missing.js"),
    SCRIPT("https://errors.test/sri.js", "rec('sri-ran')"),
    SCRIPT("https://other.test/muted.js", "throw new Error('secret')"),
    SCRIPT("https://errors.test/micro.js",
           "Promise.resolve().then(()=>rec('micro'));throw new Error('m-boom')"),
};

static const PageCase parser_events_case = {
    .name = "parser-inserted classic script element events",
    .html =
        PRELUDE QUIET_ONERROR "</script>"
        EXT("ok", "https://errors.test/ok.js")
        EXT("throw", "https://errors.test/throw.js")
        INLINE_OK
        EXT("syntax", "https://errors.test/syntax.js")
        INLINE_OK
        EXT("missing", "https://errors.test/missing.js")
        EXT_EVENTS("sri", "https://errors.test/sri.js",
                   " integrity='sha256-"
                   "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA='")
        EXT("muted", "https://other.test/muted.js")
        EXT("micro", "https://errors.test/micro.js")
        "<script>rec('end')</script></body>",
    .resources = parser_event_resources,
    .resource_count = sizeof(parser_event_resources)
        / sizeof(parser_event_resources[0]),
    .expected =
        "ok,L:ok,t,on:t-boom,L:throw,i,on:variable name expected,L:syntax,i,"
        "E:missing,E:sri,on:Script error.,L:muted,on:m-boom,micro,L:micro,"
        "end >> ",
};

/* async and defer scripts: each entry names the window error (if any)
   that preceded its element event; the order across scripts is not
   fixed, so the log is read sorted. */
#define LAST_ONERROR "onerror=m=>{globalThis.last=m;return true};"
#define SEEN "<'+(globalThis.last||'-')+'>');globalThis.last=''"
#define EXT_SEEN(name_, url_, attributes_)                                   \
    "<script src='" url_ "'" attributes_                                     \
    " onload=\"rec('L:" name_ SEEN "\""                                      \
    " onerror=\"rec('E:" name_ SEEN "\"></script>"
#define SORTED_PROBE                                                         \
    "globalThis.pocSummary=(globalThis.log||['no-log']).slice()"             \
    ".sort().join(',')"

static const PageRecord deferred_event_resources[] = {
    SCRIPT("https://errors.test/athrow.js", "throw new Error('async-boom')"),
    MISSING("https://errors.test/a404.js"),
    SCRIPT("https://errors.test/dthrow.js", "throw new Error('defer-boom')"),
    SCRIPT("https://errors.test/dsyntax.js", "var = 1;"),
};

static const PageCase deferred_events_case = {
    .name = "async and defer script element events",
    .html =
        PRELUDE LAST_ONERROR "</script>"
        EXT_SEEN("athrow", "https://errors.test/athrow.js", " async")
        EXT_SEEN("a404", "https://errors.test/a404.js", " async")
        EXT_SEEN("dthrow", "https://errors.test/dthrow.js", " defer")
        EXT_SEEN("dsyntax", "https://errors.test/dsyntax.js", " defer")
        "</body>",
    .resources = deferred_event_resources,
    .resource_count = sizeof(deferred_event_resources)
        / sizeof(deferred_event_resources[0]),
    .expected =
        "E:a404<->,L:athrow<async-boom>,"
        "L:dsyntax<variable name expected>,L:dthrow<defer-boom> >> ",
    .probe = SORTED_PROBE,
};

/* A script CSP refuses gets `error` and no window error. */
static const PageRecord csp_event_resources[] = {
    SCRIPT("https://other.test/csp.js", "rec('csp-ran')"),
};

static const PageCase csp_events_case = {
    .name = "CSP-refused script element event",
    .html =
        PRELUDE QUIET_ONERROR "</script>"
        EXT("csp", "https://other.test/csp.js")
        "<script>rec('end')</script></body>",
    .resources = csp_event_resources,
    .resource_count = 1,
    .expected = "E:csp,end >> ",
    .page_header =
        "content-security-policy: script-src 'self' 'unsafe-inline'",
};

/* Module scripts: a module that throws, or whose graph has a parse error,
   fires `load`; one whose graph could not be fetched (an HTTP error, a
   failed import, a non-JavaScript MIME type) fires `error`. A second
   element for a settled module gets the same event without running it
   again. */
static const PageRecord module_event_resources[] = {
    SCRIPT("https://errors.test/mt.js",
           "rec('mt');throw new Error('mt-boom')"),
    SCRIPT("https://errors.test/ms.js", "rec('never');\n+++;"),
    MISSING("https://errors.test/m404.js"),
    SCRIPT("https://errors.test/mdep.js",
           "import './gone.js';rec('never')"),
    MISSING("https://errors.test/gone.js"),
    SCRIPT("https://errors.test/mdsyn.js",
           "import './bad.js';rec('never')"),
    SCRIPT("https://errors.test/bad.js", "+++;"),
    {.url = "https://errors.test/mmime.js", .content_type = "text/plain",
     .body = "rec('never')"},
};

static const PageCase module_events_case = {
    .name = "module script element events",
    .html =
        PRELUDE QUIET_ONERROR "</script>"
        MOD("mt", "https://errors.test/mt.js")
        MOD("ms", "https://errors.test/ms.js")
        MOD("m404", "https://errors.test/m404.js")
        MOD("mdep", "https://errors.test/mdep.js")
        MOD("mdsyn", "https://errors.test/mdsyn.js")
        MOD("mmime", "https://errors.test/mmime.js")
        MOD("mt2", "https://errors.test/mt.js")
        MOD("m404b", "https://errors.test/m404.js")
        "<script type=module>rec('end')</script></body>",
    .resources = module_event_resources,
    .resource_count = sizeof(module_event_resources)
        / sizeof(module_event_resources[0]),
    .expected =
        "mt,on:mt-boom,L:mt,on:unexpected token in expression: ';',L:ms,"
        "E:m404,E:mdep,on:unexpected token in expression: ';',L:mdsyn,"
        "E:mmime,L:mt2,E:m404b,end >> ",
};

/* appendChild() scripts, classic and module, read sorted. */
static const PageRecord dynamic_event_resources[] = {
    SCRIPT("https://errors.test/dt.js", "throw new Error('dt-boom')"),
    SCRIPT("https://errors.test/ds.js", "var = 1;"),
    MISSING("https://errors.test/d404.js"),
    SCRIPT("https://errors.test/dmt.js", "throw new Error('dmt-boom')"),
    SCRIPT("https://errors.test/dms.js", "+++;"),
    MISSING("https://errors.test/dm404.js"),
};

static const PageCase dynamic_events_case = {
    .name = "appendChild script element events",
    .html =
        PRELUDE LAST_ONERROR
        "const seen=(k,n)=>()=>{rec(k+':'+n+'<'+(globalThis.last||'-')+'>');"
        "globalThis.last=''};"
        "const add=(n,module)=>{const s=document.createElement('script');"
        "if(module)s.type='module';s.src='https://errors.test/'+n+'.js';"
        "s.onload=seen('L',n);s.onerror=seen('E',n);"
        "document.body.appendChild(s)};"
        "add('dt');add('ds');add('d404');"
        "add('dmt',1);add('dms',1);add('dm404',1);</script></body>",
    .resources = dynamic_event_resources,
    .resource_count = sizeof(dynamic_event_resources)
        / sizeof(dynamic_event_resources[0]),
    .expected =
        "E:d404<->,E:dm404<->,L:dms<unexpected token in expression: ';'>,"
        "L:dmt<dmt-boom>,L:ds<variable name expected>,L:dt<dt-boom> >> ",
    .probe = SORTED_PROBE,
};

/* External scripts document.write() inserts. */
static const PageRecord written_event_resources[] = {
    SCRIPT("https://errors.test/wt.js", "rec('wt');throw new Error('wt-boom')"),
    MISSING("https://errors.test/w404.js"),
};

static const PageCase written_events_case = {
    .name = "document.write external script element events",
    .html =
        PRELUDE QUIET_ONERROR
        "globalThis.L=n=>rec('L:'+n);globalThis.E=n=>rec('E:'+n);"
        "document.write(`<script src=\"https://errors.test/wt.js\" "
        "onload=\"L('wt')\" onerror=\"E('wt')\"><\\/script>`);"
        "document.write(`<script src=\"https://errors.test/w404.js\" "
        "onload=\"L('w404')\" onerror=\"E('w404')\"><\\/script>`);"
        "rec('after')</script><script>rec('end')</script></body>",
    .resources = written_event_resources,
    .resource_count = sizeof(written_event_resources)
        / sizeof(written_event_resources[0]),
    .expected = "after,wt,on:wt-boom,L:wt,E:w404,end >> ",
};

/* The usual CDN fallback: try the local copy when the CDN copy fails to
   load (onerror) or ran without defining the library (onload). */
#define FALLBACK_PAGE                                                        \
    PRELUDE "onerror=()=>true;globalThis.fb=()=>{rec('fb');"                 \
    "const s=document.createElement('script');"                              \
    "s.src='https://errors.test/lib-local.js';document.head.appendChild(s)};" \
    "</script><script src='https://cdn.test/lib.js' "                         \
    "onload='window.Lib||fb()' onerror='fb()'></script></body>"
#define LOCAL_LIB SCRIPT("https://errors.test/lib-local.js",                \
                         "rec('local:'+(window.Lib?'twice':'once'));"        \
                         "window.Lib={}")

static const PageRecord fallback_throw_resources[] = {
    SCRIPT("https://cdn.test/lib.js", "rec('cdn');throw new Error('broken')"),
    LOCAL_LIB,
};

static const PageCase fallback_throw_case = {
    .name = "CDN fallback: the CDN copy throws before defining the library",
    .html = FALLBACK_PAGE,
    .resources = fallback_throw_resources,
    .resource_count = 2,
    .expected = "cdn,fb,local:once >> ",
    .settle = 8,
};

static const PageRecord fallback_missing_resources[] = {
    MISSING("https://cdn.test/lib.js"),
    LOCAL_LIB,
};

static const PageCase fallback_missing_case = {
    .name = "CDN fallback: the CDN copy is missing",
    .html = FALLBACK_PAGE,
    .resources = fallback_missing_resources,
    .resource_count = 2,
    .expected = "fb,local:once >> ",
    .settle = 8,
};

/* A library whose own setup throws after it defined itself must not be
   loaded a second time. */
static const PageRecord fallback_late_throw_resources[] = {
    SCRIPT("https://cdn.test/lib.js",
           "rec('cdn');window.Lib={};throw new Error('plugin')"),
    LOCAL_LIB,
};

static const PageCase fallback_late_throw_case = {
    .name = "CDN fallback: the CDN copy throws after defining the library",
    .html = FALLBACK_PAGE,
    .resources = fallback_late_throw_resources,
    .resource_count = 2,
    .expected = "cdn >> ",
    .settle = 8,
};

/* The browser's own refusals fire `error` without a window error: a
   script over the per-file limit, and one that runs out of memory (the
   realm then runs no more scripts). The arrays are block-scoped, as in the
   null case below, so the refused script's memory is released before the
   element's error handler is compiled: kept in a global, whether that
   handler still fit depended on how many bytes the bootstrap left in the
   realm, and an out-of-memory handler reported to window.onerror instead. */
static char oversized_body[80u * 1024u];

static const PageRecord refusal_resources[] = {
    SCRIPT("https://errors.test/big.js", oversized_body),
    SCRIPT("https://errors.test/oom.js",
           "rec('oom');{const a=[];for(;;)a.push(new Array(65536).fill(1))}"),
};

static const PageCase refusal_events_case = {
    .name = "browser refusals fire the element's error event",
    .html =
        PRELUDE QUIET_ONERROR "</script>"
        EXT("big", "https://errors.test/big.js")
        INLINE_OK
        EXT("oom", "https://errors.test/oom.js")
        "</body>",
    .resources = refusal_resources,
    .resource_count = 2,
    .expected = "E:big,i,oom,E:oom >> ",
};

/* The out-of-memory refusal again, when the heap also refuses the
   element's error handler as it compiles (here the first allocation of the
   first compile; with the arrays in a global, whether it fit depended on
   the bytes the bootstrap left). That refusal is the browser's too: it is
   not reported to window.onerror, and the handler is compiled again when
   it is next needed, here in the same dispatch. */
static const PageCase refusal_handler_case = {
    .name = "an error handler the heap refuses to compile",
    .html =
        PRELUDE QUIET_ONERROR "</script>"
        "<script src='https://errors.test/oom.js'"
        " onerror=\"rec('E:oom')\"></script>"
        "</body>",
    .resources = &refusal_resources[1],
    .resource_count = 1,
    .expected = "oom,E:oom >> ",
    .refuse_handler_allocation = 1,
};

/* A script the watchdog interrupts did not finish: `error`, and no
   window error. */
static const PageRecord interrupt_resources[] = {
    SCRIPT("https://errors.test/spin.js", "rec('spin');for(;;){}"),
};

static const PageCase interrupt_events_case = {
    .name = "an interrupted script fires the element's error event",
    .html =
        PRELUDE QUIET_ONERROR "</script>"
        EXT("spin", "https://errors.test/spin.js")
        "<script>rec('end')</script></body>",
    .resources = interrupt_resources,
    .resource_count = 1,
    .expected = "spin,E:spin,end >> ",
};

/* QuickJS throws null for a refused allocation when it cannot allocate
   even its "out of memory" error. After the heap refused an allocation
   in the same run, a null is that refusal: `error`, and no window error
   (here the script catches the refusal and its null stands in for the
   one QuickJS would throw). A null a page throws with no refusal is the
   page's own error: the window error event reports it, and `load`. */
static const PageRecord null_resources[] = {
    SCRIPT("https://errors.test/null-oom.js",
           "rec('n-oom');try{const a=[];"
           "for(;;)a.push(new Array(65536).fill(1))}catch(e){}throw null"),
    SCRIPT("https://errors.test/null.js", "rec('n');throw null"),
};

static const PageCase null_events_case = {
    .name = "a null after a refused allocation is out of memory",
    .html =
        PRELUDE QUIET_ONERROR "</script>"
        EXT("n-oom", "https://errors.test/null-oom.js")
        INLINE_OK
        EXT("n", "https://errors.test/null.js")
        "</body>",
    .resources = null_resources,
    .resource_count = 2,
    .expected = "n-oom,E:n-oom,i,n,on:null,L:n >> ",
};

int main(void)
{
    if (setenv("TILEFINCH_TRACE_CONSOLE", "1", 1) != 0) return 1;
    /* A comment over the 64 KiB per-file limit run_page_case sets. */
    memset(oversized_body, ' ', sizeof(oversized_body) - 1);
    memcpy(oversized_body, "rec('big-ran')//", 16);
    const PageCase *cases[] = {
        &inline_case, &cancel_case, &handler_throws_case, &external_case,
        &handler_mutation_case, &inline_syntax_case, &module_case,
        &inserted_case, &worker_case,
        &timer_case, &module_worker_case, &parser_events_case,
        &deferred_events_case, &csp_events_case, &module_events_case,
        &dynamic_events_case, &written_events_case, &fallback_throw_case,
        &fallback_missing_case, &fallback_late_throw_case,
        &refusal_events_case, &refusal_handler_case, &interrupt_events_case,
        &null_events_case,
    };
    size_t failures = 0;
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        if (!run_page_case(cases[i])) failures++;
    }
    if (failures != 0) {
        fprintf(stderr, "tilefinch-script-error-event-tests: %zu failure(s)\n",
                failures);
        return 1;
    }
    puts("tilefinch-script-error-event-tests: ok");
    return 0;
}
