/* Page-level CSP3 nonce, hash and 'strict-dynamic' behavior. Each case
   loads a replayed document whose response carries the policy, so the
   policy is known before the parser runs, exactly as on a real page; the
   meta cases deliver it (or another) from <meta http-equiv> instead. */
#include "tilefinch/budget.h"
#include "tilefinch/fetch.h"
#include "tilefinch/js_runtime.h"
#include "tilefinch/navigation.h"
#include "tilefinch/session.h"
#include "tilefinch/sha256.h"
#include "tilefinch/style.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "script_test_support.h"

#define MIB (1024u * 1024u)
#define PAGE_URL "https://csp.test/"

typedef struct {
    const char *url;
    const char *content_type;
    const char *body;
    /* One extra response header line ("name: value"), or NULL. */
    const char *header;
    /* A transport error recorded with the (partial) body, or NULL. */
    const char *error;
} PageRecord;

static bool write_fixture(char directory[128], const PageRecord *records,
                          size_t count)
{
    snprintf(directory, 128, "%s", "/tmp/tilefinch-csp-pages-XXXXXX");
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
            "method=GET\nurl=%s\nlogical-request-url=%s\nsuccess=%d\n"
            "async-delay-pumps=0\nexternal-cancel=0\n"
            "transport-timeout=0\nredirect-origin-tainted=0\nerror=%s\n"
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
            records[i].url, records[i].url,
            records[i].error == NULL ? 1 : 0,
            records[i].error == NULL ? "" : records[i].error, length,
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
    const char *policy;
    const char *html;
    const PageRecord *resources;
    size_t resource_count;
    /* Sets pocSummary; evaluated after the load and after each runtime
       advance until it reports `expected`. */
    const char *probe;
    const char *expected;
    /* Optional checks on the loaded session. */
    bool (*check)(NavigationSession *navigation);
} PageCase;

static bool run_page_case(const PageCase *page)
{
    PageRecord records[16];
    char header[1024];
    snprintf(header, sizeof(header), "content-security-policy: %s",
             page->policy == NULL ? "" : page->policy);
    records[0] = (PageRecord) {
        .url = PAGE_URL, .content_type = "text/html; charset=utf-8",
        .body = page->html, .header = page->policy == NULL ? NULL : header
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
        navigation_enable_external_resources(
            &navigation, 4, 64u * 1024u, 32u * 1024u,
            1, 16u * 1024u, 16u * 1024u, 64u * 1024u, 2000);
    }
    char error[256] = {0};
    bool replaying = navigation_ready
        && fetch_trace_replay_begin_response_keyed(
               directory, error, sizeof(error));
    uint64_t generation = replaying ? navigation_begin(&navigation) : 0;
    bool loaded = replaying && navigation_load_url(
        &navigation, generation, PAGE_URL, 64u * 1024u, 2000, 480,
        NULL, NULL, true);
    ScriptResult result = {0};
    bool matched = false;
    for (size_t step = 0; loaded && !matched && step < 64; step++) {
        if (step != 0) (void) navigation_advance_runtime(&navigation, 1, 8);
        matched = page->probe == NULL || (navigation.page.runtime != NULL
            && script_runtime_evaluate_diagnostic(
                   navigation.page.runtime, page->probe, "<csp-probe>",
                   &result)
            && strcmp(result.summary, page->expected) == 0);
    }
    bool checked = matched
        && (page->check == NULL || page->check(&navigation));
    if (!checked) {
        FetchTraceReplayStats stats = {0};
        (void) fetch_trace_replay_stats(&stats);
        fprintf(stderr,
                "replay records=%zu requests=%zu matched=%zu served=%zu "
                "rejected=%zu unmatched=%zu conflicting=%zu invalid=%zu "
                "network-failures=%zu last=\"%s\" status=%ld\n",
                stats.record_count, stats.request_count,
                stats.matched_request_count, stats.served_request_count,
                stats.rejected_request_count, stats.unmatched_request_count,
                stats.conflicting_request_count,
                stats.invalid_route_request_count,
                navigation.page.script_result.network_failures,
                navigation.page.script_result.last_network_url,
                navigation.page.script_result.last_network_status);
        fprintf(stderr, "scripts discovered=%zu attempted=%zu loaded=%zu "
                "failed=%zu cache=%zu quota=%zu xorigin=%zu\n",
                navigation.script_discovered, navigation.script_attempted,
                navigation.script_loaded, navigation.script_failed,
                navigation.script_cache_hits, navigation.script_skipped_quota,
                navigation.script_skipped_cross_origin);
        fprintf(stderr,
                "csp page case \"%s\" failed: loaded=%d matched=%d "
                "summary=\"%s\" expected=\"%s\" error=\"%s\" "
                "navigation-error=\"%s\" replay-error=\"%s\"\n",
                page->name, loaded, matched, result.summary,
                page->expected, result.error, navigation.last_error, error);
    }
    if (replaying) fetch_trace_end();
    if (navigation_ready) navigation_destroy(&navigation);
    if (browser_ready) browser_session_destroy(&browser);
    bool clean = budget.current == 0
        && budget_active_allocations(&budget, NULL) == 0;
    if (lexbor) clean = budget_uninstall_lexbor(&budget) && clean;
    if (written) remove_fixture(directory, count);
    if (!clean) fprintf(stderr, "csp page case \"%s\" leaked\n", page->name);
    return checked && clean;
}

#define SCRIPT(url_, body_) \
    {.url = (url_), .content_type = "text/javascript", .body = (body_)}

/* Nonce-source on external, data: and inline scripts, without
   'strict-dynamic': the host list still applies and a mismatched nonce
   gains nothing. */
static const PageRecord nonce_resources[] = {
    SCRIPT("https://cdn.test/good.js", "globalThis.externalGood='yes';"),
    SCRIPT("https://cdn.test/bad.js", "globalThis.externalBad='yes';"),
    SCRIPT("https://allowed.test/listed.js", "globalThis.listed='yes';"),
    SCRIPT("https://cdn.test/dynamic-good.js",
           "globalThis.dynamicGood='yes';"),
    SCRIPT("https://cdn.test/dynamic-bare.js",
           "globalThis.dynamicBare='yes';"),
};

static const PageCase nonce_case = {
    .name = "nonce external, data and inline scripts",
    .policy = "script-src 'nonce-R4nd0m+/=' https://allowed.test",
    .html =
        "<!doctype html><head>"
        "<script nonce='R4nd0m+/=' src='https://cdn.test/good.js'></script>"
        "<script nonce='R4nd0m' src='https://cdn.test/bad.js'></script>"
        "<script src='https://allowed.test/listed.js'></script>"
        "<script nonce='R4nd0m+/=' src="
        "'data:text/javascript,globalThis.dataGood=%22yes%22'></script>"
        "<script nonce='R4nd0m+/=' id=first>globalThis.inlineGood='yes';"
        "</script>"
        "<script nonce='wrong'>globalThis.inlineBad='yes';</script>"
        "<script nonce='R4nd0m+/='>"
        "var d=document.createElement('script');d.nonce='R4nd0m+/=';"
        "d.src='https://cdn.test/dynamic-good.js';document.head.append(d);"
        "var b=document.createElement('script');"
        "b.src='https://cdn.test/dynamic-bare.js';document.head.append(b);"
        "</script></head><body></body>",
    .resources = nonce_resources,
    .resource_count = sizeof(nonce_resources) / sizeof(nonce_resources[0]),
    .probe =
        "globalThis.pocSummary=[globalThis.externalGood,"
        "globalThis.externalBad,globalThis.listed,globalThis.dataGood,"
        "globalThis.inlineGood,globalThis.inlineBad,globalThis.dynamicGood,"
        "globalThis.dynamicBare].map(v=>v?'1':'0').join('')",
    .expected = "10111010",
};

/* HTML nonce hiding: the content attribute reads back empty, the IDL
   attribute keeps the value, and neither selectors nor serialization see
   it. Setting the IDL attribute does not touch the content attribute. */
static const PageCase hiding_case = {
    .name = "nonce hiding",
    .policy = "script-src 'nonce-H1dd3n'; style-src 'nonce-H1dd3n'",
    .html =
        "<!doctype html><head><script nonce='H1dd3n' id=s>"
        "globalThis.ran='yes';</script>"
        "<style nonce='H1dd3n' id=st>p{color:red}</style></head>"
        "<body><p>x</p></body>",
    .probe =
        "(()=>{const s=document.getElementById('s'),"
        "st=document.getElementById('st'),"
        "n=document.createElement('script');n.nonce='Pr0p';"
        "const late=document.createElement('div');"
        "late.setAttribute('nonce','L4te');document.body.append(late);"
        "globalThis.pocSummary=[globalThis.ran==='yes',"
        "s.getAttribute('nonce')==='',s.hasAttribute('nonce'),"
        "s.nonce==='H1dd3n',st.nonce==='H1dd3n',"
        "st.getAttribute('nonce')==='',"
        "document.querySelector('[nonce^=H]')===null,"
        "!document.head.outerHTML.includes('H1dd3n'),"
        "n.nonce==='Pr0p',n.getAttribute('nonce')===null,"
        "late.getAttribute('nonce')==='',late.nonce==='L4te']"
        ".map(v=>v?'1':'0').join('')})()",
    .expected = "111111111111",
};

/* 'strict-dynamic': script-inserted external scripts need no nonce, but a
   parser-inserted script does even when its host is listed, a script-
   inserted inline script still needs a nonce, and 'unsafe-inline' is
   ignored. */
static const PageRecord strict_dynamic_resources[] = {
    SCRIPT("https://allowed.test/listed.js", "globalThis.listed='yes';"),
    SCRIPT("https://loader.test/chunk.js", "globalThis.chunk='yes';"),
    SCRIPT("https://cdn.test/root.js",
           "var c=document.createElement('script');"
           "c.src='https://loader.test/nested.js';document.head.append(c);"),
    SCRIPT("https://loader.test/nested.js", "globalThis.nested='yes';"),
};

static const PageCase strict_dynamic_case = {
    .name = "strict-dynamic propagation",
    .policy = "script-src 'nonce-S7r1ct' 'strict-dynamic' "
              "https://allowed.test 'self' 'unsafe-inline'",
    .html =
        "<!doctype html><head>"
        "<script src='https://allowed.test/listed.js'></script>"
        "<script>globalThis.bareInline='yes';</script>"
        "<script nonce='S7r1ct' src='https://cdn.test/root.js'></script>"
        "<script nonce='S7r1ct'>"
        "var c=document.createElement('script');"
        "c.src='https://loader.test/chunk.js';document.head.append(c);"
        "var i=document.createElement('script');"
        "i.textContent='globalThis.dynamicInline=\"yes\"';"
        "document.head.append(i);"
        "var j=document.createElement('script');j.nonce='S7r1ct';"
        "j.textContent='globalThis.dynamicInlineNonce=\"yes\"';"
        "document.head.append(j);</script></head><body></body>",
    .resources = strict_dynamic_resources,
    .resource_count = sizeof(strict_dynamic_resources)
        / sizeof(strict_dynamic_resources[0]),
    .probe =
        "globalThis.pocSummary=[globalThis.listed,globalThis.bareInline,"
        "globalThis.chunk,globalThis.nested,globalThis.dynamicInline,"
        "globalThis.dynamicInlineNonce].map(v=>v?'1':'0').join('')",
    .expected = "001101",
};

/* CSP3 hash-source on an external script: every SRI digest listed. */
static const PageRecord hash_resources[] = {
    SCRIPT("https://csp.test/hashed.js", "globalThis.hashed='yes';"),
    SCRIPT("https://csp.test/other.js", "globalThis.hashedOther='yes';"),
};

static const PageCase hash_case = {
    .name = "external script integrity hash",
    .policy = "script-src "
              "'sha256-7nnrcsFgEfuVmv+cLeEXjhqSDEirKreIGPQmEr5FJik='",
    .html =
        "<!doctype html><head><script src='https://csp.test/hashed.js' "
        "integrity='sha256-7nnrcsFgEfuVmv+cLeEXjhqSDEirKreIGPQmEr5FJik='>"
        "</script><script src='https://csp.test/other.js' "
        "integrity='sha256-i280kzbI45cWQw4DI6t1NTagTg90CDjWBOzb6qJj0N8='>"
        "</script></head><body></body>",
    .resources = hash_resources,
    .resource_count = sizeof(hash_resources) / sizeof(hash_resources[0]),
    .probe =
        "globalThis.pocSummary=[globalThis.hashed,globalThis.hashedOther]"
        ".map(v=>v?'1':'0').join('')",
    .expected = "10",
};

/* style-src nonces on <style> and <link rel=stylesheet>. */
static const PageRecord style_resources[] = {
    {.url = "https://cdn.test/good.css", .content_type = "text/css",
     .body = "#c{color:#0000aa}"},
    {.url = "https://cdn.test/bad.css", .content_type = "text/css",
     .body = "#d{color:#00aa00}"},
};

static bool style_colors(NavigationSession *navigation)
{
    static const struct { const char *id; uint32_t color; bool applied; }
    expected[] = {
        {"a", 0x123456u, true}, {"b", 0x654321u, false},
        {"c", 0x0000aau, true}, {"d", 0x00aa00u, false},
    };
    lxb_dom_node_t *root = lxb_dom_interface_node(
        navigation->page.document.html);
    for (size_t i = 0; i < 4; i++) {
        lxb_dom_node_t *node = root;
        for (size_t visited = 0; node != NULL && visited < 4096; visited++) {
            size_t length = 0;
            const char *id = document_attribute(node, "id", &length);
            if (id != NULL && length == 1 && id[0] == expected[i].id[0]
                && document_element_name(node, &length) != NULL
                && length == 1) break;
            if (node->first_child != NULL) { node = node->first_child; continue; }
            while (node != NULL && node->next == NULL) node = node->parent;
            if (node != NULL) node = node->next;
        }
        if (node == NULL) return false;
        ComputedStyle style = style_for_node(
            &navigation->page.stylesheet, node, NULL);
        if ((style.color == expected[i].color) != expected[i].applied) {
            fprintf(stderr, "style #%s color=%06x\n", expected[i].id,
                    style.color);
            return false;
        }
    }
    return true;
}

static const PageCase style_case = {
    .name = "style nonces",
    .policy = "style-src 'nonce-5tyle'",
    .html =
        "<!doctype html><head>"
        "<style nonce='5tyle'>#a{color:#123456}</style>"
        "<style nonce='other'>#b{color:#654321}</style>"
        "<link rel=stylesheet nonce='5tyle' href='https://cdn.test/good.css'>"
        "<link rel=stylesheet nonce='other' href='https://cdn.test/bad.css'>"
        "</head><body><p id=a>a</p><p id=b>b</p><p id=c>c</p>"
        "<p id=d>d</p></body>",
    .resources = style_resources,
    .resource_count = sizeof(style_resources) / sizeof(style_resources[0]),
    .probe = NULL,
    .expected = "",
    .check = style_colors,
};

/* The speculative preload scanner reads the nonce from the tag, so the
   parser's script is fetched early, while a mismatched one never is. */
static const PageRecord preload_resources[] = {
    SCRIPT("https://cdn.test/early.js", "globalThis.early='yes';"),
    SCRIPT("https://cdn.test/refused.js", "globalThis.refused='yes';"),
};

static bool preloads_admitted(NavigationSession *navigation)
{
    FetchTraceReplayStats stats = {0};
    bool ok = navigation->preloads_discovered == 2
        && navigation->preloads_launched == 1
        && navigation->preloads_failed == 0
        && fetch_trace_replay_stats(&stats)
        && stats.rejected_request_count == 0;
    if (!ok) {
        fprintf(stderr,
                "preloads discovered=%zu launched=%zu completed=%zu "
                "failed=%zu rejected=%zu\n",
                navigation->preloads_discovered,
                navigation->preloads_launched,
                navigation->preloads_completed, navigation->preloads_failed,
                stats.rejected_request_count);
    }
    return ok;
}

static const PageCase preload_case = {
    .name = "speculative script preload",
    .policy = "script-src 'nonce-Ear1y'",
    .html =
        "<!doctype html><head>"
        "<script nonce='Ear1y' src='https://cdn.test/early.js'></script>"
        "<script nonce='Late' src='https://cdn.test/refused.js'></script>"
        "</head><body></body>",
    .resources = preload_resources,
    .resource_count = sizeof(preload_resources)
        / sizeof(preload_resources[0]),
    .probe =
        "globalThis.pocSummary=[globalThis.early,globalThis.refused]"
        ".map(v=>v?'1':'0').join('')",
    .expected = "10",
    .check = preloads_admitted,
};

/* HTML fetches a modulepreload with parser metadata "not-parser-inserted",
   so 'strict-dynamic' admits it without a nonce (reddit's module loader
   inserts one per chunk group while the page parses). A classic script
   preload stays parser-inserted and still needs the nonce. */
static const PageRecord modulepreload_resources[] = {
    SCRIPT("https://csp.test/chunk.mjs", "export const chunk='yes';"),
    SCRIPT("https://cdn.test/classic.js", "globalThis.classic='yes';"),
};

static const PageCase modulepreload_case = {
    .name = "modulepreload under strict-dynamic",
    .policy = "script-src 'strict-dynamic' 'nonce-Ear1y'",
    .html =
        "<!doctype html><head>"
        "<link rel=modulepreload href='https://csp.test/chunk.mjs'>"
        "<link rel=preload as=script href='https://cdn.test/classic.js'>"
        "</head><body><script nonce='Ear1y'>globalThis.ran='ok'</script>"
        "</body>",
    .resources = modulepreload_resources,
    .resource_count = sizeof(modulepreload_resources)
        / sizeof(modulepreload_resources[0]),
    .probe = "globalThis.pocSummary=String(globalThis.ran)",
    .expected = "ok",
    .check = preloads_admitted,
};

/* A nonced data: module larger than any URL buffer (Reddit's loader is a
   34 KB data: module): CSP reads only the scheme. */
static char long_data_html[8192];

static PageCase long_data_case = {
    .name = "long nonced data: module",
    .policy = "script-src 'self' 'strict-dynamic' 'nonce-L0ng'",
    .html = long_data_html,
    .probe = "globalThis.pocSummary=String(globalThis.longData)",
    .expected = "yes",
};

/* Review follow-ups. */

/* Inline hash sources of every SRI algorithm, beside 'unsafe-inline' (which
   any hash source disables): the listed scripts run, the other does not. */
static const PageCase inline_sha2_case = {
    .name = "inline sha384/sha512 hashes",
    .policy = "script-src 'unsafe-inline' "
        "'sha384-7Y/9zqY0Wy0Ngt+6q5t/ypIoswvqIanXup7yFD8XAqxqVnDPrfS8sFdcuR1bHaLY' "
        "'sha512-n3k/zw7tyDlzZFEVWBSaPlS52uwmeNOPBYvmci3Fdi8iit8bInTPrpkk36TncfxcPsFYvNps0Ngf76X5ojj5VA=='",
    .html =
        "<!doctype html><head><script>globalThis.h384='yes';</script>"
        "<script>globalThis.h512='yes';</script>"
        "<script>globalThis.unlisted='yes';</script></head><body></body>",
    .probe = "globalThis.pocSummary=[globalThis.h384,globalThis.h512,"
        "globalThis.unlisted].map(v=>v?'1':'0').join('')",
    .expected = "110",
};

/* Dangling markup (an unclosed injected tag swallowing a later legitimate
   script tag) must not lend that tag's nonce to the injected URL: neither
   the parser's element nor the speculative preload scanner may use it. */
static const PageRecord dangling_resources[] = {
    SCRIPT("https://cdn.test/evil.js", "globalThis.evil='yes';"),
    SCRIPT("https://cdn.test/good.js", "globalThis.good='yes';"),
};

static bool evil_never_fetched(NavigationSession *navigation)
{
    (void) navigation;
    /* Record 1 is evil.js (record 0 is the document). */
    if (!fetch_trace_replay_record_was_claimed(1)) return true;
    fprintf(stderr, "dangling-markup URL was fetched\n");
    return false;
}

static const PageCase dangling_case = {
    .name = "dangling markup nonce",
    .policy = "script-src 'nonce-Ear1y'",
    .html =
        "<!doctype html><head><title>x</title>"
        "<script src='https://cdn.test/evil.js' "
        "<script nonce='Ear1y' src='https://cdn.test/good.js'></script>"
        "<script nonce='Ear1y' src='https://cdn.test/good.js'></script>"
        "</head><body></body>",
    .resources = dangling_resources,
    .resource_count = 2,
    .probe = "globalThis.pocSummary=[globalThis.evil,globalThis.good]"
        ".map(v=>v?'1':'0').join('')",
    .expected = "01",
    .check = evil_never_fetched,
};

/* HTML cloning steps copy [[CryptographicNonce]], for parser-hidden and
   IDL-set nonces alike; the clone's attribute stays empty. */
static const PageCase clone_case = {
    .name = "clone keeps the nonce",
    .policy = "script-src 'nonce-C1one'",
    .html =
        "<!doctype html><head></head><body>"
        "<script nonce='C1one' id=src>globalThis.first=1;</script>"
        "<p id=pp nonce='C1one'>x</p>"
        "<script nonce='C1one'>"
        "var m=document.createElement('script');"
        "m.nonce=document.getElementById('src').nonce;"
        "m.textContent='globalThis.viaClone=1';"
        "document.body.append(m.cloneNode(true));</script></body>",
    .probe =
        "(()=>{const s=document.getElementById('src').cloneNode(true),"
        "p=document.getElementById('pp').cloneNode(false);"
        "globalThis.pocSummary=[globalThis.first,globalThis.viaClone,"
        "s.nonce==='C1one',p.nonce==='C1one',p.getAttribute('nonce')==='']"
        ".map(v=>v?'1':'0').join('')})()",
    .expected = "11111",
};

/* Injected markup with many distinct nonces cannot exhaust the slots and
   leave a legitimate nonce readable; an unlisted nonce past the bounded
   table is still hidden (its IDL value is what is given up). */
static char exhaust_html[24576];

static PageCase exhaust_case = {
    .name = "nonce slot exhaustion",
    .policy = "script-src 'nonce-S3cret'",
    .html = exhaust_html,
    .probe =
        "(()=>{const s=document.getElementById('sx'),"
        "l=document.getElementById('last');"
        "globalThis.pocSummary=[globalThis.ranS==='yes',"
        "s.getAttribute('nonce')==='',s.nonce==='S3cret',"
        "document.querySelector('[nonce^=S]')===null,"
        "l.getAttribute('nonce')==='',"
        "document.querySelector('[nonce^=z]')===null]"
        ".map(v=>v?'1':'0').join('')})()",
    .expected = "111111",
};

/* Module graphs inherit the root's nonce and parser metadata (HTML
   descendant script fetch options) but not its integrity. */
static const PageRecord module_resources[] = {
    SCRIPT("https://csp.test/root.mjs",
           "import './dep.mjs'; globalThis.root='yes';"),
    SCRIPT("https://csp.test/dep.mjs", "export const dep='yes';"),
};

static const PageCase module_nonce_case = {
    .name = "nonced module imports",
    .policy = "script-src 'nonce-M0d'",
    .html =
        "<!doctype html><head><script type=module nonce='M0d' "
        "src='https://csp.test/root.mjs'></script></head><body></body>",
    .resources = module_resources,
    .resource_count = 2,
    .probe = "globalThis.pocSummary=String(globalThis.root)",
    .expected = "yes",
};

static const PageRecord integrity_root_resources[] = {
    SCRIPT("https://csp.test/iroot.mjs",
           "globalThis.root='yes';import('./dep.mjs').then(()=>"
           "{globalThis.dep='yes'},()=>{globalThis.depBlocked='yes'});"),
    SCRIPT("https://csp.test/dep.mjs", "export const dep='yes';"),
};

static const PageCase module_integrity_case = {
    .name = "integrity-only parser module root under strict-dynamic",
    .policy = "script-src 'sha256-Xrgom04kJSihdiP8BSAUhc8YUsNGiD0Vf2OOC6J11Lo=' "
              "'strict-dynamic'",
    .html =
        "<!doctype html><head><script type=module "
        "src='https://csp.test/iroot.mjs' "
        "integrity='sha256-Xrgom04kJSihdiP8BSAUhc8YUsNGiD0Vf2OOC6J11Lo='>"
        "</script></head><body></body>",
    .resources = integrity_root_resources,
    .resource_count = 2,
    .probe = "globalThis.pocSummary=[globalThis.root,globalThis.dep,"
        "globalThis.depBlocked].map(v=>v?'1':'0').join('')",
    .expected = "101",
};

/* 'strict-dynamic' trusts script-created elements only: markup a trusted
   script hands to a parser (innerHTML, insertAdjacentHTML, document.write,
   DOMParser) gains nothing, nor does moving or cloning a blocked parser
   script, nor an event-handler attribute ('unsafe-inline' is ignored). */
static const PageRecord gadget_resources[] = {
    SCRIPT("https://cdn.test/blocked.js", "globalThis.gBlocked='yes';"),
    SCRIPT("https://cdn.test/ih.js", "globalThis.gInner='yes';"),
    SCRIPT("https://cdn.test/iah.js", "globalThis.gAdjacent='yes';"),
    SCRIPT("https://cdn.test/dw.js", "globalThis.gWrite='yes';"),
    SCRIPT("https://cdn.test/dp.js", "globalThis.gAdopt='yes';"),
    SCRIPT("https://cdn.test/imp.js", "globalThis.gImport='yes';"),
    SCRIPT("https://cdn.test/ok.js", "globalThis.gOk='yes';"),
};

static const PageCase gadget_case = {
    .name = "strict-dynamic gadgets",
    .policy = "script-src 'nonce-N0nce' 'strict-dynamic' 'unsafe-inline'",
    .html =
        "<!doctype html><head></head><body>"
        "<script id=parserBlocked src='https://cdn.test/blocked.js'></script>"
        "<script>globalThis.gBare='yes';</script><div id=box></div>"
        "<script nonce='N0nce'>"
        "var d=document.createElement('div');"
        "d.innerHTML='<script src=\"https://cdn.test/ih.js\"><\\/script>';"
        "document.body.append(d);"
        "document.body.insertAdjacentHTML('beforeend',"
        "'<script src=\"https://cdn.test/iah.js\"><\\/script>');"
        "document.write('<script src=\"https://cdn.test/dw.js\"><\\/script>');"
        "try{var p=new DOMParser().parseFromString("
        "'<script src=\"https://cdn.test/dp.js\"><\\/script>','text/html');"
        "document.body.append(document.adoptNode(p.querySelector('script')));"
        "var q=new DOMParser().parseFromString("
        "'<script src=\"https://cdn.test/imp.js\"><\\/script>','text/html');"
        "document.body.append(document.importNode(q.querySelector('script'),"
        "true));}catch(e){}"
        "var b=document.getElementById('parserBlocked'),"
        "box=document.getElementById('box');"
        "box.append(b);box.append(b.cloneNode(true));"
        "document.body.setAttribute('onclick',\"globalThis.gHandler='yes'\");"
        "document.body.click();"
        "var o=document.createElement('script');"
        "o.src='https://cdn.test/ok.js';document.body.append(o);"
        "</script></body>",
    .resources = gadget_resources,
    .resource_count = sizeof(gadget_resources) / sizeof(gadget_resources[0]),
    .probe = "globalThis.pocSummary=[globalThis.gOk,globalThis.gBlocked,"
        "globalThis.gBare,globalThis.gInner,globalThis.gAdjacent,"
        "globalThis.gWrite,globalThis.gAdopt,globalThis.gImport,"
        "globalThis.gHandler].map(v=>v?'1':'0').join('')",
    .expected = "100000000",
};

/* The integrity grant admits only bytes SRI then verifies: a data: URL or a
   response whose digest is not the listed one never runs. */
static const PageRecord integrity_resources[] = {
    SCRIPT("https://csp.test/evil.js", "globalThis.e1='yes';"),
    SCRIPT("https://csp.test/evil2.js", "globalThis.e2='yes';"),
};

#define LISTED_INLINE "sha256-FSkkVFoJiDF7R9fbPdbLE4jE9T85j7FKDVfvsQKYGIk="
static const PageCase integrity_mismatch_case = {
    .name = "integrity grant with data: and mismatched bodies",
    .policy = "script-src '" LISTED_INLINE "'",
    .html =
        "<!doctype html><head>"
        "<script>globalThis.inl='yes';</script>"
        "<script src=\"data:text/javascript,globalThis.d1='yes'\" "
        "integrity='" LISTED_INLINE "'></script>"
        "<script type=module src=\"data:text/javascript,globalThis.d2='yes'\" "
        "integrity='" LISTED_INLINE "'></script>"
        "<script src='https://csp.test/evil.js' integrity='" LISTED_INLINE
        "'></script>"
        "<script src='https://csp.test/evil2.js' integrity='" LISTED_INLINE
        "' crossorigin></script></head><body></body>",
    .resources = integrity_resources,
    .resource_count = 2,
    .probe = "globalThis.pocSummary=[globalThis.inl,globalThis.d1,"
        "globalThis.d2,globalThis.e1,globalThis.e2]"
        ".map(v=>v?'1':'0').join('')",
    .expected = "10000",
};

/* Subresource Integrity for script-inserted scripts (NW-02), with
   'strict-dynamic' admitting every one by URL so SRI alone decides: a
   matching body runs; a mismatched classic or module body, a cross-origin
   no-CORS request with integrity, and a fresh cache hit with the wrong
   integrity do not run and fire error; crossorigin makes a classic script
   a CORS request whose verified body runs. */
#define GOOD_DIGEST "sha256-+l0kaPhyoxZAQ5pIvvZMYhSGIck8E9whs9rxBf/V/VQ="
#define X_DIGEST "sha256-fHJJEyzIBDpxfkWuSQAuwJ8zrKb+eLNNhzo0Flk4SfA="
#define X2_DIGEST "sha256-4Djvmj3/KCgkHe1xiDP9BBBgf7xted6PwbI863wRNBk="
static const PageRecord dynamic_sri_resources[] = {
    SCRIPT("https://csp.test/good.js", "globalThis.g1='yes';"),
    SCRIPT("https://csp.test/bad.js", "globalThis.b1='yes';"),
    SCRIPT("https://csp.test/badm.js", "globalThis.b2='yes';"),
    SCRIPT("https://cdn.test/x.js", "globalThis.x1='yes';"),
    {.url = "https://cdn.test/x2.js", .content_type = "text/javascript",
     .body = "globalThis.x2='yes';",
     .header = "access-control-allow-origin: *"},
    {.url = "https://csp.test/cached.js", .content_type = "text/javascript",
     .body = "globalThis.c1=(globalThis.c1||0)+1;",
     .header = "cache-control: max-age=600"},
};

/* Four refusals, and the cross-origin no-CORS one never reaches the
   network. */
static bool dynamic_sri_counted(NavigationSession *navigation)
{
    FetchTraceReplayStats stats = {0};
    bool counted = navigation->page.script_result
            .dynamic_scripts_integrity_rejected == 4u
        && fetch_trace_replay_stats(&stats) && stats.request_count == 6u;
    if (!counted)
        fprintf(stderr, "dynamic SRI refusals=%zu requests=%zu\n",
                navigation->page.script_result
                    .dynamic_scripts_integrity_rejected,
                stats.request_count);
    return counted;
}

static const PageCase dynamic_sri_case = {
    .name = "SRI on script-inserted scripts",
    .policy = "script-src 'nonce-N1' 'strict-dynamic'",
    .html =
        "<!doctype html><head></head><body><script nonce='N1'>"
        "globalThis.errs=[];"
        "function add(src,integrity,type,cors){"
        "var s=document.createElement('script');s.src=src;"
        "if(integrity)s.setAttribute('integrity',integrity);"
        "if(type)s.setAttribute('type',type);"
        "if(cors)s.setAttribute('crossorigin','anonymous');"
        "s.onerror=function(){globalThis.errs.push(src)};"
        "document.head.append(s);return s;}"
        "add('https://csp.test/good.js','" GOOD_DIGEST "');"
        "add('https://csp.test/bad.js','" GOOD_DIGEST "');"
        "add('https://csp.test/badm.js','" GOOD_DIGEST "','module');"
        "add('https://cdn.test/x.js','" X_DIGEST "');"
        "add('https://cdn.test/x2.js','" X2_DIGEST "',null,true);"
        "add('https://csp.test/cached.js').onload=function(){"
        "add('https://csp.test/cached.js','" GOOD_DIGEST "');};"
        "</script></body>",
    .resources = dynamic_sri_resources,
    .resource_count = sizeof(dynamic_sri_resources)
        / sizeof(dynamic_sri_resources[0]),
    .probe = "globalThis.pocSummary=[globalThis.g1,globalThis.b1,"
        "globalThis.b2,globalThis.x1,globalThis.x2]"
        ".map(v=>v?'1':'0').join('')+':'+(globalThis.c1||0)+':'"
        "+globalThis.errs.length",
    .expected = "10001:1:4",
    .check = dynamic_sri_counted,
};

/* The CSP integrity grant admits a script-inserted script only on bytes
   SRI then verifies: a same-origin URL no host source lists runs when its
   body has the listed digest, and another URL claiming the same digest
   with different bytes does not. */
#define Y_DIGEST "sha256-TBC04toWrDLa8W6lvG8EGref/nnBt8kz+pAhCOSQNuc="
static const PageRecord dynamic_grant_resources[] = {
    SCRIPT("https://csp.test/y.js", "globalThis.y1='yes';"),
    SCRIPT("https://csp.test/w.js", "globalThis.w1='yes';"),
    SCRIPT("https://anywhere.test/z.js", "globalThis.z1='yes';"),
};

static const PageCase dynamic_grant_case = {
    .name = "CSP integrity grant on script-inserted scripts",
    .policy = "script-src 'nonce-N1' '" Y_DIGEST "'",
    .html =
        "<!doctype html><head></head><body><script nonce='N1'>"
        "globalThis.errs=0;"
        "for(const src of['https://csp.test/y.js','https://csp.test/w.js',"
        "'https://anywhere.test/z.js']){"
        "var s=document.createElement('script');s.src=src;"
        "s.setAttribute('integrity','" Y_DIGEST "');"
        "s.onerror=function(){globalThis.errs++};document.head.append(s);}"
        "</script></body>",
    .resources = dynamic_grant_resources,
    .resource_count = 3,
    .probe = "globalThis.pocSummary=[globalThis.y1,globalThis.w1,"
        "globalThis.z1].map(v=>v?'1':'0').join('')+':'+globalThis.errs",
    .expected = "100:2",
};

/* Range.createContextualFragment scripts are script-inserted: under
   'strict-dynamic' one a trusted script inserts loads without a nonce, an
   inline one still needs its nonce, and an untrusted script never gets to
   insert one at all. */
static const PageRecord fragment_resources[] = {
    SCRIPT("https://cdn.test/frag.js", "globalThis.fragExternal='yes';"),
    SCRIPT("https://cdn.test/frag2.js", "globalThis.fragUntrusted='yes';"),
};

static const PageCase fragment_case = {
    .name = "contextual fragment scripts under strict-dynamic",
    .policy = "script-src 'nonce-Fr4g' 'strict-dynamic'",
    .html =
        "<!doctype html><head></head><body>"
        "<script>document.body.append(document.createRange()"
        ".createContextualFragment('<script src=\"https://cdn.test/frag2.js\">"
        "<\\/script>'));</script>"
        "<script nonce='Fr4g'>document.body.append(document.createRange()"
        ".createContextualFragment("
        "'<script src=\"https://cdn.test/frag.js\"><\\/script>"
        "<script>globalThis.fragInline=1<\\/script>"
        "<script nonce=\"Fr4g\">globalThis.fragNonced=1<\\/script>'));"
        "</script></body>",
    .resources = fragment_resources,
    .resource_count = 2,
    .probe = "globalThis.pocSummary=[globalThis.fragExternal,"
        "globalThis.fragInline,globalThis.fragNonced,"
        "globalThis.fragUntrusted].map(v=>v?'1':'0').join('')",
    .expected = "1010",
};

/* A nonced cross-host stylesheet over the per-file byte cap is applied up
   to its last complete rule (truncated-stylesheet handling): its grant
   still admits it, and an unnonced one is still refused. The record holds
   what a capped transfer receives: the 32 KiB prefix and the quota error. */
static char oversized_css[32u * 1024u + 1u];

static const PageRecord oversized_resources[] = {
    {.url = "https://cdn.test/big.css", .content_type = "text/css",
     .body = oversized_css, .error = "response quota exceeded"},
    {.url = "https://cdn.test/other.css", .content_type = "text/css",
     .body = "#d{color:#00aa00}"},
};

static const PageCase oversized_style_case = {
    .name = "nonced stylesheet over the byte cap",
    .policy = "style-src 'nonce-B1g'",
    .html =
        "<!doctype html><head>"
        "<link rel=stylesheet nonce='B1g' href='https://cdn.test/big.css'>"
        "<link rel=stylesheet href='https://cdn.test/other.css'>"
        "</head><body><p id=a>a</p><p id=b>b</p><p id=c>c</p>"
        "<p id=d>d</p></body>",
    .resources = oversized_resources,
    .resource_count = 2,
    .check = style_colors,
};

/* ---- the persistent compiled-script tier behind SRI and CSP ---- */

/* Module records restored from the card still pass the page's admission:
   the tier sits after fetch, CSP and SRI. One cache directory across four
   browser starts: a visit with the right integrity writes the root and its
   import (the import past the module table's ceiling, so through the write
   queue); a page whose integrity names other bytes, and a page whose policy
   refuses the root, neither run the module nor read a pack, though the
   served bytes match the packs exactly; the right integrity then restores
   both modules from the card and compiles nothing. */
static char disk_dep[24576];
static const char disk_root[] =
    "import {dep} from './ddep.mjs';globalThis.root='yes';"
    "globalThis.dep=dep;";

static void base64_sha256(const char *text, char out[64])
{
    static const char alphabet[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    uint8_t digest[TILEFINCH_SHA256_DIGEST_BYTES];
    (void) tilefinch_sha256_digest((const uint8_t *) text, strlen(text),
                                   digest);
    size_t at = 0;
    for (size_t i = 0; i < sizeof(digest); i += 3) {
        uint32_t chunk = (uint32_t) digest[i] << 16;
        if (i + 1 < sizeof(digest)) chunk |= (uint32_t) digest[i + 1] << 8;
        if (i + 2 < sizeof(digest)) chunk |= digest[i + 2];
        out[at++] = alphabet[(chunk >> 18) & 63u];
        out[at++] = alphabet[(chunk >> 12) & 63u];
        out[at++] = i + 1 < sizeof(digest) ? alphabet[(chunk >> 6) & 63u]
                                           : '=';
        out[at++] = i + 2 < sizeof(digest) ? alphabet[chunk & 63u] : '=';
    }
    out[at] = '\0';
}

typedef struct {
    char summary[64];
    size_t compiles, module_hits, disk_hits, stores, queued;
    BrowserScriptDiskStats stats;
} DiskVisit;

static bool disk_visit(const char *cache, const char *policy,
                       const char *integrity, DiskVisit *visit)
{
    memset(visit, 0, sizeof(*visit));
    char html[512], header[256];
    snprintf(html, sizeof(html),
             "<!doctype html><head><script type=module "
             "src='https://csp.test/droot.mjs' integrity='sha256-%s'>"
             "</script></head><body></body>", integrity);
    snprintf(header, sizeof(header), "content-security-policy: %s", policy);
    PageRecord records[] = {
        {.url = PAGE_URL, .content_type = "text/html; charset=utf-8",
         .body = html, .header = header},
        SCRIPT("https://csp.test/droot.mjs", disk_root),
        SCRIPT("https://csp.test/ddep.mjs", disk_dep),
    };
    size_t count = sizeof(records) / sizeof(records[0]);
    char directory[128] = {0};
    bool written = write_fixture(directory, records, count);
    Budget budget;
    budget_init(&budget, 32u * MIB);
    bool lexbor = budget_install_lexbor(&budget);
    BrowserSession browser = {0};
    NavigationSession navigation = {0};
    bool browser_ready = written && lexbor
        && browser_session_init(&browser, &budget, 256u * 1024u);
    bool tier = browser_ready
        && browser_session_script_disk_configure(&browser, cache, true, 0);
    /* Room for the root's record, not the import's. */
    if (tier) browser_session_module_bytecode_set_limit(&browser, 8u * 1024u);
    bool navigation_ready = tier
        && navigation_init(&navigation, &budget, 4);
    if (navigation_ready) {
        navigation_attach_browser_session(&navigation, &browser);
        navigation_enable_scripts(&navigation, 8u * MIB, 2000);
        navigation_enable_document_scripts(
            &navigation, 16, 256u * 1024u, 64u * 1024u, 2000);
        navigation_enable_external_resources(
            &navigation, 4, 64u * 1024u, 32u * 1024u,
            1, 16u * 1024u, 16u * 1024u, 64u * 1024u, 2000);
    }
    char error[256] = {0};
    bool replaying = navigation_ready
        && fetch_trace_replay_begin_response_keyed(
               directory, error, sizeof(error));
    uint64_t generation = replaying ? navigation_begin(&navigation) : 0;
    bool loaded = replaying && navigation_load_url(
        &navigation, generation, PAGE_URL, 64u * 1024u, 2000, 480,
        NULL, NULL, true);
    for (size_t step = 0; loaded && step < 32; step++)
        (void) navigation_advance_runtime(&navigation, 1, 8);
    ScriptResult result = {0};
    bool probed = loaded && navigation.page.runtime != NULL
        && script_runtime_evaluate_diagnostic(
               navigation.page.runtime,
               "globalThis.pocSummary=String(globalThis.root)+':'+"
               "String(globalThis.dep)", "<disk-probe>", &result);
    if (probed) {
        snprintf(visit->summary, sizeof(visit->summary), "%s",
                 result.summary);
        const ScriptResult *page = &navigation.page.script_result;
        visit->compiles = page->module_compile_count;
        visit->module_hits = page->module_bytecode_cache_hits;
        visit->disk_hits = page->script_bytecode_disk_hits;
        visit->stores = page->module_bytecode_cache_stores;
        visit->queued = page->module_bytecode_cache_disk_queued;
    }
    if (replaying) fetch_trace_end();
    if (navigation_ready) navigation_destroy(&navigation);
    /* Idle work writes what the visit queued or stored. */
    for (unsigned turn = 0; tier && turn < 1000
             && browser_session_script_disk_maintenance(&browser); turn++) {
    }
    if (tier) browser_session_script_disk_stats(&browser, &visit->stats);
    if (browser_ready) browser_session_destroy(&browser);
    bool clean = budget.current == 0
        && budget_active_allocations(&budget, NULL) == 0;
    if (lexbor) clean = budget_uninstall_lexbor(&budget) && clean;
    if (written) remove_fixture(directory, count);
    if (!probed || !clean)
        fprintf(stderr, "disk visit (%s): probed=%d clean=%d error=%s\n",
                policy, probed, clean, error);
    return probed && clean;
}

static bool module_disk_tier_behind_sri_and_csp(void)
{
    int used = snprintf(disk_dep, sizeof(disk_dep),
                        "function big(n){var s=[];");
    for (int i = 0; used < 11000; i++)
        used += snprintf(disk_dep + used, sizeof(disk_dep) - (size_t) used,
                         "s.push(n*%d+%d);", i, i * 7);
    snprintf(disk_dep + used, sizeof(disk_dep) - (size_t) used,
             "return s}export const dep='d'+big(2).length;");
    char cache[512];
    if (!script_test_make_scratch_root(cache, sizeof(cache), "csp-disk"))
        return false;
    char right[64], wrong[64];
    base64_sha256(disk_root, right);
    base64_sha256("globalThis.root='other';", wrong);
    static const char allow[] = "script-src 'self'";
    DiskVisit first = {0}, mismatch = {0}, refused = {0}, restored = {0};
    bool ok = disk_visit(cache, allow, right, &first)
        && strncmp(first.summary, "yes:d", 5) == 0
        && first.stores == 1 && first.queued == 1
        && first.stats.writes == 2;
    ok = ok && disk_visit(cache, allow, wrong, &mismatch)
        && strcmp(mismatch.summary, "undefined:undefined") == 0
        && mismatch.disk_hits == 0 && mismatch.stats.reads == 0
        && mismatch.stats.promoted == 0 && mismatch.stats.direct_reads == 0;
    ok = ok && disk_visit(cache, "script-src 'none'", right, &refused)
        && strcmp(refused.summary, "undefined:undefined") == 0
        && refused.disk_hits == 0 && refused.stats.reads == 0;
    ok = ok && disk_visit(cache, allow, right, &restored)
        && strcmp(restored.summary, first.summary) == 0
        && restored.compiles == 0 && restored.disk_hits == 2
        && restored.stats.direct_reads >= 1;
    script_test_remove_tree(cache);
    if (!ok) {
        const DiskVisit *visits[] = {&first, &mismatch, &refused, &restored};
        for (size_t i = 0; i < 4; i++)
            fprintf(stderr, "disk visit %zu: summary=%s compiles=%zu "
                    "hits=%zu stores=%zu disk-queued=%zu disk-hits=%zu "
                    "reads=%zu promoted=%zu direct=%zu writes=%zu\n", i,
                    visits[i]->summary, visits[i]->compiles,
                    visits[i]->module_hits, visits[i]->stores,
                    visits[i]->queued, visits[i]->disk_hits,
                    visits[i]->stats.reads, visits[i]->stats.promoted,
                    visits[i]->stats.direct_reads, visits[i]->stats.writes);
        fprintf(stderr, "module disk tier behind SRI and CSP failed\n");
    }
    return ok;
}

/* <meta http-equiv="Content-Security-Policy"> in the head, no header: it
   applies from where the parser inserts it. Scripts before it run (a
   deferred and an async one too, as their fetches began first); after it,
   unlisted inline and external scripts are refused, eval and inline
   handlers are off, and fetch() is refused. */
static const PageRecord meta_resources[] = {
    SCRIPT("https://cdn.test/deferred.js", "globalThis.deferred='1';"),
    SCRIPT("https://cdn.test/async.js", "globalThis.async='1';"),
    SCRIPT("https://cdn.test/ext.js", "globalThis.ext='1';"),
    SCRIPT("https://allowed.test/ok.js", "globalThis.ok='1';"),
    {.url = "https://cdn.test/data.json", .content_type = "application/json",
     .body = "{}"},
};

static const PageCase meta_case = {
    .name = "meta policy applies from its insertion",
    .policy = NULL,
    .html =
        "<!doctype html><head>"
        "<script>globalThis.before='1';globalThis.e1=String(eval('1'));"
        "</script>"
        "<script defer src='https://cdn.test/deferred.js'></script>"
        "<script async src='https://cdn.test/async.js'></script>"
        "<meta http-equiv=Content-Security-Policy content=\"script-src "
        "'nonce-M3ta' https://allowed.test; connect-src 'none'\">"
        "<script>globalThis.inline='1';</script>"
        "<script src='https://cdn.test/ext.js'></script>"
        "<script src='https://allowed.test/ok.js'></script>"
        "<script nonce='M3ta'>globalThis.nonced='1';"
        "try{eval('1');globalThis.e2='ran';}catch(e){globalThis.e2=e.name;}"
        "const d=document.createElement('div');"
        "d.setAttribute('onclick','globalThis.h=1');document.head.append(d);"
        "d.click();"
        "fetch('https://cdn.test/data.json').then(()=>globalThis.f='ok',"
        "()=>globalThis.f='refused');</script>"
        "</head><body></body>",
    .resources = meta_resources,
    .resource_count = sizeof(meta_resources) / sizeof(meta_resources[0]),
    .probe = "globalThis.pocSummary=[globalThis.before,globalThis.deferred,"
        "globalThis.async,globalThis.inline,globalThis.ext,globalThis.ok,"
        "globalThis.nonced].map(v=>v?'1':'0').join('')+','+globalThis.e1+','"
        "+globalThis.e2+','+globalThis.h+','+globalThis.f",
    .expected = "1110011,1,TypeError,undefined,refused",
};

/* A meta outside the head is not the http-equiv state: one the parser puts
   in the body and one a script appends there change nothing. */
static const PageCase meta_body_case = {
    .name = "meta policy in the body is ignored",
    .policy = NULL,
    .html =
        "<!doctype html><head></head><body>"
        "<meta http-equiv=Content-Security-Policy content=\"script-src "
        "'none'\"><script>globalThis.ran='1';"
        "const m=document.createElement('meta');"
        "m.setAttribute('http-equiv','Content-Security-Policy');"
        "m.setAttribute('content',\"script-src 'none'\");"
        "document.body.append(m);"
        "const s=document.createElement('script');"
        "s.textContent='globalThis.ran2=1';document.body.append(s);"
        "globalThis.e=String(eval('3'));</script></body>",
    .probe = "globalThis.pocSummary=[globalThis.ran,globalThis.ran2,"
        "globalThis.e].join(',')",
    .expected = "1,1,3",
};

/* A script-inserted meta in the head applies from its insertion; one in a
   detached tree does not. */
static const PageCase meta_inserted_case = {
    .name = "script-inserted meta policy",
    .policy = NULL,
    .html =
        "<!doctype html><head><script>"
        "const x=document.createElement('div');"
        "x.innerHTML=\"<meta http-equiv=Content-Security-Policy "
        "content=\\\"script-src 'none'\\\">\";"
        "globalThis.e0=String(eval('2'));"
        "const m=document.createElement('meta');"
        "m.setAttribute('http-equiv','Content-Security-Policy');"
        "m.setAttribute('content',\"script-src 'self'\");"
        "document.head.append(m);"
        "try{eval('1');globalThis.e1='ran';}catch(e){globalThis.e1=e.name;}"
        "const s=document.createElement('script');"
        "s.textContent='globalThis.after=1';document.head.append(s);"
        "const d=document.createElement('div');"
        "d.setAttribute('onclick','globalThis.h=1');document.head.append(d);"
        "d.click();</script>"
        "<script>globalThis.parsed=1;</script></head><body></body>",
    .probe = "globalThis.pocSummary=[globalThis.e0,globalThis.e1,"
        "globalThis.after,globalThis.h,globalThis.parsed].join(',')",
    .expected = "2,TypeError,,,",
};

/* A page that replaces its head and puts a meta policy in the new one gets
   the policy; one put in the replaced, detached head changes nothing. The
   parser's cached head pointer still names the old head. */
static const PageCase meta_replaced_head_case = {
    .name = "meta policy in a replacement head",
    .policy = NULL,
    .html =
        "<!doctype html><head><title>t</title></head><body><script>"
        "const old=document.querySelector('head');"
        "const h=document.createElement('head');"
        "document.documentElement.replaceChild(h,old);"
        "const stale=document.createElement('meta');"
        "stale.setAttribute('http-equiv','Content-Security-Policy');"
        "stale.setAttribute('content',\"script-src 'none'\");"
        "old.append(stale);"
        "globalThis.e0=String(eval('2'));"
        "const m=document.createElement('meta');"
        "m.setAttribute('http-equiv','Content-Security-Policy');"
        "m.setAttribute('content',\"script-src 'self'\");"
        "h.append(m);"
        "try{eval('1');globalThis.e1='ran';}catch(e){globalThis.e1=e.name;}"
        "</script><script>globalThis.parsed=1;</script></body>",
    .probe = "globalThis.pocSummary=[globalThis.e0,globalThis.e1,"
        "globalThis.parsed].join(',')",
    .expected = "2,TypeError,",
};

/* The same through documentElement.innerHTML, which detaches the parsed
   head along with everything else. */
static const PageCase meta_reparsed_head_case = {
    .name = "meta policy after documentElement.innerHTML",
    .policy = NULL,
    .html =
        "<!doctype html><head><title>t</title></head><body><script>"
        "document.documentElement.innerHTML="
        "'<head><title>u</title></head><body><p>x</p></body>';"
        "globalThis.e0=String(eval('2'));"
        "const m=document.createElement('meta');"
        "m.setAttribute('http-equiv','Content-Security-Policy');"
        "m.setAttribute('content',\"script-src 'self'\");"
        "document.querySelector('head').append(m);"
        "try{eval('1');globalThis.e1='ran';}catch(e){globalThis.e1=e.name;}"
        "</script></body>",
    .probe = "globalThis.pocSummary=[globalThis.e0,globalThis.e1].join(',')",
    .expected = "2,TypeError",
};

/* A head script removes the head while the parser still has it open. The
   parser keeps inserting into that detached head, the metas after it too
   (one through the after-head mode's head pointer): none is the document's
   head, so neither policy applies. */
static const PageCase meta_removed_head_parse_case = {
    .name = "meta policy in a head removed while parsing",
    .policy = NULL,
    .html =
        "<!doctype html><head><script>"
        "document.querySelector('head').remove();</script>"
        "<meta http-equiv=Content-Security-Policy content=\"script-src "
        "'none'\"><title>t</title></head>"
        "<meta http-equiv=Content-Security-Policy content=\"script-src "
        "'none'\"><body><script>globalThis.ran='1';</script></body>",
    .probe = "globalThis.pocSummary=[globalThis.ran,"
        "document.querySelector('head')===null].join(',')",
    .expected = "1,true",
};

/* A header policy and a meta policy are both enforced: a script runs only
   if each allows it. */
static const PageRecord combined_resources[] = {
    SCRIPT("https://cdn.test/c.js", "globalThis.cdn='1';"),
    SCRIPT("https://csp.test/self.js", "globalThis.own='1';"),
};

static const PageCase meta_combined_case = {
    .name = "header and meta policies combine",
    .policy = "script-src 'self' https://cdn.test 'unsafe-inline'",
    .html =
        "<!doctype html><head><script>globalThis.before='1';</script>"
        "<meta http-equiv=Content-Security-Policy content=\"script-src "
        "https://cdn.test 'nonce-C0mb'\">"
        "<script src='https://cdn.test/c.js'></script>"
        "<script src='/self.js'></script>"
        "<script>globalThis.inline='1';</script>"
        "<script nonce='C0mb'>globalThis.nonced='1';</script>"
        "</head><body></body>",
    .resources = combined_resources,
    .resource_count = 2,
    .probe = "globalThis.pocSummary=[globalThis.before,globalThis.cdn,"
        "globalThis.own,globalThis.inline,globalThis.nonced]"
        ".map(v=>v?'1':'0').join('')",
    .expected = "11001",
};

/* The speculative scanner stops at a meta policy: it runs ahead of the
   parser, which has not applied the policy yet, so a script after the meta
   is neither preloaded nor run. */
static const PageRecord meta_preload_resources[] = {
    SCRIPT("https://cdn.test/before.js", "globalThis.before='1';"),
    SCRIPT("https://cdn.test/after.js", "globalThis.after='1';"),
};

static bool meta_preload_stopped(NavigationSession *navigation)
{
    bool ok = navigation->preloads_discovered == 1
        && navigation->preloads_launched == 1;
    if (!ok)
        fprintf(stderr, "meta preloads discovered=%zu launched=%zu\n",
                navigation->preloads_discovered,
                navigation->preloads_launched);
    return ok;
}

static const PageCase meta_preload_case = {
    .name = "preload scan stops at a meta policy",
    .policy = NULL,
    .html =
        "<!doctype html><head>"
        "<script src='https://cdn.test/before.js'></script>"
        "<meta http-equiv=Content-Security-Policy content=\"script-src "
        "https://allowed.test\">"
        "<script src='https://cdn.test/after.js'></script>"
        "</head><body></body>",
    .resources = meta_preload_resources,
    .resource_count = 2,
    .probe = "globalThis.pocSummary=[globalThis.before,globalThis.after]"
        ".map(v=>v?'1':'0').join('')",
    .expected = "10",
    .check = meta_preload_stopped,
};

/* Inline event handlers follow a later meta policy that refuses them even
   while eval stays allowed: one compiled before the meta runs, one after
   it does not. */
static const PageCase meta_handler_case = {
    .name = "meta policy refuses later inline handlers",
    .policy = NULL,
    .html =
        "<!doctype html><head><script>"
        "const a=document.createElement('div');"
        "a.setAttribute('onclick','globalThis.h1=1');a.click();</script>"
        "<meta http-equiv=Content-Security-Policy content=\"script-src "
        "'nonce-H4nd' 'unsafe-eval'\">"
        "<script nonce='H4nd'>const b=document.createElement('div');"
        "b.setAttribute('onclick','globalThis.h2=1');b.click();"
        "globalThis.e=String(eval('5'));</script></head><body></body>",
    .probe = "globalThis.pocSummary=[globalThis.h1,globalThis.h2,"
        "globalThis.e].join(',')",
    .expected = "1,,5",
};

/* A connected inert element did not prepare a script request. Giving it an
   executable type after a meta policy must use that policy, not acquire an
   exemption merely because its src existed earlier. */
static const PageRecord meta_prepare_resources[] = {
    SCRIPT("https://cdn.test/prepared.js", "globalThis.prepared='ran';"),
    SCRIPT("https://cdn.test/blocked.js", "globalThis.finished='ran';"),
};

static const PageCase meta_inert_activation_case = {
    .name = "inert source does not acquire a prepared CSP grant",
    .html =
        "<!doctype html><head>"
        "<script id=held type=application/json src='https://cdn.test/blocked.js'></script>"
        "<script>const h=document.getElementById('held');"
        "const m=document.createElement('meta');"
        "m.httpEquiv='Content-Security-Policy';"
        "m.content=\"script-src 'self' 'unsafe-inline' 'unsafe-eval'\";"
        "document.head.append(m);h.onerror=()=>globalThis.finished='blocked';"
        "h.type='text/javascript';"
        "h.remove();document.head.append(h);</script></head><body></body>",
    .resources = meta_prepare_resources,
    .resource_count = sizeof(meta_prepare_resources) / sizeof(meta_prepare_resources[0]),
    .probe = "globalThis.pocSummary=globalThis.finished||'pending'",
    .expected = "blocked",
};

/* Preparing one URL cannot exempt a different URL, including a relative
   source reinterpreted by a later base element. */
static const PageCase meta_changed_source_case = {
    .name = "prepared CSP grant is bound to its source URL",
    .html =
        "<!doctype html><head><script defer id=held src='https://cdn.test/prepared.js'></script>"
        "<meta http-equiv=Content-Security-Policy content=\"script-src 'self' 'unsafe-inline' 'unsafe-eval'\">"
        "<script>const h=document.getElementById('held');"
        "h.onerror=()=>globalThis.finished='blocked';"
        "h.src='https://cdn.test/blocked.js';</script></head><body></body>",
    .resources = meta_prepare_resources,
    .resource_count = sizeof(meta_prepare_resources) / sizeof(meta_prepare_resources[0]),
    .probe = "globalThis.pocSummary=globalThis.finished||'pending'",
    .expected = "blocked",
};

static const PageCase meta_changed_base_case = {
    .name = "prepared CSP grant is bound to the original base resolution",
    .html =
        "<!doctype html><head><script defer id=held src='blocked.js'></script>"
        "<meta http-equiv=Content-Security-Policy content=\"script-src 'self' 'unsafe-inline' 'unsafe-eval'\">"
        "<script>document.getElementById('held').onerror=()=>globalThis.finished='blocked';"
        "const b=document.createElement('base');b.href='https://cdn.test/';"
        "document.head.append(b);</script></head><body></body>",
    .resources = meta_prepare_resources,
    .resource_count = sizeof(meta_prepare_resources) / sizeof(meta_prepare_resources[0]),
    .probe = "globalThis.pocSummary=globalThis.finished||'pending'",
    .expected = "blocked",
};

static const PageCase meta_nomodule_activation_case = {
    .name = "nomodule removal does not prepare a request before its policy",
    .html =
        "<!doctype html><head><script defer nomodule id=held src='https://cdn.test/blocked.js'></script>"
        "<meta http-equiv=Content-Security-Policy content=\"script-src 'self' 'unsafe-inline' 'unsafe-eval'\">"
        "<script>const h=document.getElementById('held');"
        "h.onerror=()=>globalThis.finished='blocked';h.removeAttribute('nomodule');"
        "</script></head><body></body>",
    .resources = meta_prepare_resources,
    .resource_count = sizeof(meta_prepare_resources) / sizeof(meta_prepare_resources[0]),
    .probe = "globalThis.pocSummary=globalThis.finished||'pending'",
    .expected = "blocked",
};

static const PageCase meta_prepared_header_case = {
    .name = "prepared meta grant never exempts an earlier header policy",
    .policy = "script-src 'self' 'unsafe-inline' 'unsafe-eval'",
    .html =
        "<!doctype html><head><script defer id=held src='https://cdn.test/blocked.js'></script>"
        "<meta http-equiv=Content-Security-Policy content=\"script-src * 'unsafe-inline' 'unsafe-eval'\">"
        "<script>document.getElementById('held').onerror=()=>globalThis.finished='blocked';"
        "</script></head><body></body>",
    .resources = meta_prepare_resources,
    .resource_count = sizeof(meta_prepare_resources) / sizeof(meta_prepare_resources[0]),
    .probe = "globalThis.pocSummary=globalThis.finished||'pending'",
    .expected = "blocked",
};

static const PageCase meta_prepared_data_case = {
    .name = "prepared data URL keeps its later meta exemption",
    .html =
        "<!doctype html><head><script defer src=\"data:text/javascript,globalThis.finished='ran'\"></script>"
        "<meta http-equiv=Content-Security-Policy content=\"script-src 'self'\">"
        "</head><body></body>",
    .probe = "globalThis.pocSummary=globalThis.finished||'pending'",
    .expected = "ran",
};

static bool meta_prepared_long_data(void)
{
    char html[4096];
    static const char prefix[] =
        "<!doctype html><head><script defer src=\"data:text/javascript,"
        "globalThis.finished='ran';/*";
    static const char suffix[] =
        "*/\"></script><meta http-equiv=Content-Security-Policy "
        "content=\"script-src 'self'\"></head><body></body>";
    size_t at = sizeof(prefix) - 1u;
    memcpy(html, prefix, at);
    memset(html + at, 'x', 3000u);
    at += 3000u;
    memcpy(html + at, suffix, sizeof(suffix));
    const PageCase page = {
        .name = "prepared data URL past the network URL bound",
        .html = html,
        .probe = "globalThis.pocSummary=globalThis.finished||'pending'",
        .expected = "ran",
    };
    return run_page_case(&page);
}

int main(void)
{
    if (!meta_prepared_long_data()) return 1;
    int used = snprintf(
        long_data_html, sizeof(long_data_html),
        "<!doctype html><head><script type=module nonce='L0ng' src='"
        "data:text/javascript,globalThis.longData=%%22yes%%22;/*");
    for (int i = 0; i < 4000; i++) long_data_html[used++] = 'x';
    snprintf(long_data_html + used, sizeof(long_data_html) - (size_t) used,
             "*/'></script></head><body></body>");
    used = snprintf(exhaust_html, sizeof(exhaust_html),
                    "<!doctype html><head></head><body>");
    for (int i = 0; i < 300; i++)
        used += snprintf(exhaust_html + used,
                         sizeof(exhaust_html) - (size_t) used,
                         "<i nonce=z%d></i>", i);
    snprintf(exhaust_html + used, sizeof(exhaust_html) - (size_t) used,
             "<b id=last nonce=zLast></b>"
             "<script nonce='S3cret' id=sx>globalThis.ranS='yes';</script>"
             "</body>");
    used = snprintf(oversized_css, sizeof(oversized_css),
                    "#a{color:#123456}#c{color:#0000aa}/*");
    while ((size_t) used < sizeof(oversized_css) - 1u)
        oversized_css[used++] = 'x';
    oversized_css[used] = '\0';
    const PageCase *cases[] = {
        &nonce_case, &hiding_case, &strict_dynamic_case, &hash_case,
        &style_case, &preload_case, &modulepreload_case, &long_data_case, &inline_sha2_case,
        &dangling_case, &clone_case, &exhaust_case, &module_nonce_case,
        &module_integrity_case, &gadget_case, &integrity_mismatch_case,
        &fragment_case, &oversized_style_case, &dynamic_sri_case,
        &dynamic_grant_case, &meta_case, &meta_body_case,
        &meta_inserted_case, &meta_replaced_head_case,
        &meta_reparsed_head_case, &meta_removed_head_parse_case,
        &meta_combined_case, &meta_preload_case,
        &meta_handler_case, &meta_inert_activation_case,
        &meta_changed_source_case,
        &meta_changed_base_case, &meta_nomodule_activation_case,
        &meta_prepared_header_case, &meta_prepared_data_case,
    };
    size_t failures = 0;
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        if (!run_page_case(cases[i])) failures++;
    }
    if (!module_disk_tier_behind_sri_and_csp()) failures++;
    if (failures != 0) {
        fprintf(stderr, "tilefinch-csp-page-tests: %zu failure(s)\n",
                failures);
        return 1;
    }
    puts("csp page tests passed");
    return 0;
}
