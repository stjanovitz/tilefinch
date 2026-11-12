/* Forms, secondary documents and DOM-behaviour probes. Each probe is a
   JavaScript file in tests/fixtures/web-forms-dom/ that runs against a
   page body given here and sets globalThis.pocSummary to "OK" or to the
   list of failed expectations. Probes that cross a task boundary keep the
   runtime's owner loop turning until they report. */
#include "tilefinch/budget.h"
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

static char *read_probe(const char *name)
{
    char path[1024];
    int written = snprintf(path, sizeof(path),
                           "%s/tests/fixtures/web-forms-dom/%s.js",
                           TILEFINCH_TEST_SOURCE_DIR, name);
    if (written < 0 || (size_t) written >= sizeof(path)) return NULL;
    FILE *file = fopen(path, "rb");
    if (file == NULL) return NULL;
    char *source = NULL;
    if (fseek(file, 0, SEEK_END) == 0) {
        long end = ftell(file);
        if (end > 0 && end < 1024 * 1024 && fseek(file, 0, SEEK_SET) == 0) {
            source = malloc((size_t) end + 1u);
            if (source != NULL
                && fread(source, 1, (size_t) end, file) == (size_t) end) {
                source[end] = '\0';
            } else {
                free(source);
                source = NULL;
            }
        }
    }
    fclose(file);
    return source;
}

/* Runs probe `name` in a fresh realm whose document body is `body`. */
static int run_probe(const char *name, const char *body)
{
    printf("test: %s\n", name);
    char *probe = read_probe(name);
    if (probe == NULL) {
        fprintf(stderr, "%s: probe file missing\n", name);
        return 1;
    }
    static char page[64 * 1024];
    int written = snprintf(page, sizeof(page),
                           "<!doctype html><html><head><title>probe</title>"
                           "</head><body>%s</body></html>", body);
    if (written < 0 || (size_t) written >= sizeof(page)) {
        free(probe);
        return 1;
    }
    Budget budget;
    budget_init(&budget, 24u * MIB);
    if (!budget_install_lexbor(&budget)) {
        free(probe);
        return 1;
    }
    PocDocument document;
    if (!document_parse(&document, &budget, page, strlen(page), 17)) {
        free(probe);
        return 1;
    }
    BrowserSession session;
    if (!browser_session_init(&session, &budget, 64u * 1024u)) {
        document_destroy(&document);
        free(probe);
        return 1;
    }
    ScriptResult result = {0};
    ScriptRuntime *runtime = script_runtime_create_deferred(
        &document, &budget, 12u * MIB, 3000,
        "https://forms.test/probe", &session, &result);
    bool ran = runtime != NULL
        && script_runtime_evaluate_diagnostic(runtime, probe, name, &result);
    for (unsigned turn = 0; ran && turn < 32u
         && strcmp(result.summary, "OK") != 0
         && strncmp(result.summary, "FAIL", 4) != 0; turn++)
        ran = script_runtime_advance(runtime, 16u, 16u, &result);
    bool ok = ran && result.success && strcmp(result.summary, "OK") == 0;
    if (!ok)
        fprintf(stderr, "%s: ran=%d success=%d summary=%s error=%s\n", name,
                ran, result.success, result.summary, result.error);
    if (runtime != NULL) script_runtime_destroy(runtime);
    document_destroy(&document);
    browser_session_destroy(&session);
    free(probe);
    if (budget.current != 0) {
        fprintf(stderr, "%s: %zu bytes left owned\n", name, budget.current);
        ok = false;
    }
    if (!budget_uninstall_lexbor(&budget)) ok = false;
    return ok ? 0 : 1;
}

/* Runs probe `name` on a committed, laid-out 480 px page whose body is
   `body`: for geometry, which the bare realm above does not have. */
static int run_layout_probe(const char *name, const char *body)
{
    printf("test: %s\n", name);
    char *probe = read_probe(name);
    if (probe == NULL) {
        fprintf(stderr, "%s: probe file missing\n", name);
        return 1;
    }
    static char page[64 * 1024];
    int written = snprintf(page, sizeof(page),
                           "<!doctype html><html><head><title>probe</title>"
                           "<meta name=viewport content='width=device-width'>"
                           "</head><body>%s</body></html>", body);
    Budget budget;
    budget_init(&budget, 32u * MIB);
    if (written < 0 || (size_t) written >= sizeof(page)
        || !budget_install_lexbor(&budget)) {
        free(probe);
        return 1;
    }
    static NavigationSession session;
    bool ok = navigation_init(&session, &budget, 2);
    if (ok) {
        navigation_enable_scripts(&session, 8u * MIB, 1000);
        ok = navigation_commit_html(&session, navigation_begin(&session),
                                    "https://forms.test/layout", page,
                                    (size_t) written, 480, NULL, NULL, true)
            && script_runtime_evaluate_diagnostic(
                   session.page.runtime, probe, name,
                   &session.page.script_result)
            && strcmp(session.page.script_result.summary, "OK") == 0;
        if (!ok)
            fprintf(stderr, "%s: summary=%s error=%s load=%s\n", name,
                    session.page.script_result.summary,
                    session.page.script_result.error, session.last_error);
        navigation_destroy(&session);
    }
    free(probe);
    if (budget.current != 0) {
        fprintf(stderr, "%s: %zu bytes left owned\n", name, budget.current);
        ok = false;
    }
    if (!budget_uninstall_lexbor(&budget)) ok = false;
    return ok ? 0 : 1;
}

/* Writes a one-response replay fixture for `url` serving `body` as HTML
   into a new temporary directory, so a probe page is fed through the
   streaming parser (its scripts run parser-blocking, as on a real load). */
static bool write_page_fixture(char directory[128], const char *url,
                               const char *body)
{
    snprintf(directory, 128, "%s", "/tmp/tilefinch-forms-dom-XXXXXX");
    if (mkdtemp(directory) == NULL) return false;
    size_t length = strlen(body);
    uint64_t hash = UINT64_C(14695981039346656037);
    for (size_t at = 0; at < length; at++) {
        hash ^= (unsigned char) body[at];
        hash *= UINT64_C(1099511628211);
    }
    char path[192];
    snprintf(path, sizeof(path), "%s/0000.body", directory);
    FILE *file = fopen(path, "wb");
    bool ok = file != NULL && fwrite(body, 1, length, file) == length;
    if (file != NULL) ok = fclose(file) == 0 && ok;
    snprintf(path, sizeof(path), "%s/0000.meta", directory);
    file = ok ? fopen(path, "wb") : NULL;
    ok = file != NULL && fprintf(
        file,
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
        "effective-url=%s\ncontent-type=text/html\netag=\n"
        "last-modified=\ncf-mitigated=\naccept-ch=\ncritical-ch=\n"
        "server=fixture\ncf-ray=\n"
        "response-referrer-policy-metadata-valid=1\n"
        "response-referrer-policy-present=0\n"
        "response-referrer-policy=\n"
        "response-security-headers-truncated=0\n"
        "response-header-count=1\nset-cookie-count=0\n"
        "response-header-0=content-type: text/html\n",
        url, url, length, (unsigned long long) hash, url) > 0;
    if (file != NULL) ok = fclose(file) == 0 && ok;
    snprintf(path, sizeof(path), "%s/trace.meta", directory);
    file = ok ? fopen(path, "wb") : NULL;
    ok = file != NULL
        && fprintf(file, "psp-http-trace-clock=1\norigin-ms=1000\n") > 0;
    if (file != NULL) ok = fclose(file) == 0 && ok;
    return ok;
}

static void remove_page_fixture(const char *directory)
{
    static const char *const names[] = {"0000.body", "0000.meta", "trace.meta"};
    char path[192];
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
        snprintf(path, sizeof(path), "%s/%s", directory, names[i]);
        (void) unlink(path);
    }
    (void) rmdir(directory);
}

/* Runs probe `name` after a streamed load of a page whose body is `body`:
   the page's own scripts run as the parser reaches them. */
static int run_stream_probe(const char *name, const char *body)
{
    printf("test: %s\n", name);
    char *probe = read_probe(name);
    if (probe == NULL) {
        fprintf(stderr, "%s: probe file missing\n", name);
        return 1;
    }
    static char page[64 * 1024];
    static const char url[] = "https://forms.test/stream";
    int written = snprintf(page, sizeof(page),
                           "<!doctype html><html><head><title>probe</title>"
                           "</head><body>%s</body></html>", body);
    char directory[128] = {0};
    bool fixture = written > 0 && (size_t) written < sizeof(page)
        && write_page_fixture(directory, url, page);
    Budget budget;
    budget_init(&budget, 32u * MIB);
    bool lexbor = fixture && budget_install_lexbor(&budget);
    BrowserSession browser = {0};
    static NavigationSession session;
    memset(&session, 0, sizeof(session));
    bool browser_ready = lexbor
        && browser_session_init(&browser, &budget, 64u * 1024u);
    bool ready = browser_ready && navigation_init(&session, &budget, 2);
    char error[256] = {0};
    if (ready) {
        navigation_attach_browser_session(&session, &browser);
        navigation_enable_scripts(&session, 8u * MIB, 1000);
        navigation_enable_document_scripts(
            &session, 8, 64u * 1024u, 64u * 1024u, 1000);
    }
    bool replaying = ready && fetch_trace_replay_begin_response_keyed(
        directory, error, sizeof(error));
    bool loaded = replaying && navigation_load_url(
        &session, navigation_begin(&session), url, 64u * 1024u, 1000, 480,
        NULL, NULL, true);
    bool ok = loaded && session.page.runtime != NULL
        && script_runtime_evaluate_diagnostic(
               session.page.runtime, probe, name,
               &session.page.script_result)
        && strcmp(session.page.script_result.summary, "OK") == 0;
    if (!ok)
        fprintf(stderr, "%s: loaded=%d summary=%s error=%s load=%s "
                "replay=%s\n", name, loaded,
                session.page.script_result.summary,
                session.page.script_result.error, session.last_error, error);
    if (replaying) fetch_trace_end();
    if (ready) navigation_destroy(&session);
    if (browser_ready) browser_session_destroy(&browser);
    free(probe);
    if (lexbor && budget.current != 0) {
        fprintf(stderr, "%s: %zu bytes left owned\n", name, budget.current);
        ok = false;
    }
    if (lexbor && !budget_uninstall_lexbor(&budget)) ok = false;
    if (fixture) remove_page_fixture(directory);
    return ok ? 0 : 1;
}

int main(int argc, char **argv)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    static const struct {
        const char *name;
        const char *body;
    } probes[] = {
        {"custom-element-connection", "<div id=host></div>"},
        {"control-state-capacity", "<div id=host></div>"},
        {"secondary-documents", "<div id=host></div>"},
        {"document-names", "<div id=host></div>"},
        {"page-document-children", "<div id=host></div>"},
        {"dom0-forms", "<div id=host></div>"},
        {"control-defaults", "<div id=host></div>"},
        {"focus-rendering", "<div id=host></div>"},
        {"range-text", "<div id=host></div>"},
        {"engine-markers", "<div id=host></div>"},
        {"offset-rounding", "<div id=host></div>"},
    };
    static const struct {
        const char *name;
        const char *body;
    } layout_probes[] = {
        {"offset-geometry",
         "<style>body{margin:8px}div{margin:0}</style>"
         "<div style='height:600px'></div><div id=target>t</div>"
         "<div id=fixed style='position:fixed;top:20px;left:0'>f</div>"
         "<div id=odd style='width:100.6px;height:49.6px'></div>"
         "<div style='height:2000px'></div>"},
    };
    static const struct {
        const char *name;
        const char *body;
    } stream_probes[] = {
        {"removed-body",
         "<div id=a>before</div><script>globalThis.removedBody = document.body;"
         "document.body.remove();globalThis.afterRemove = String(document.body)"
         "</script><p id=after>after</p><script>globalThis.late = 1</script>"},
    };
    int failures = 0;
    for (size_t at = 0; at < sizeof(probes) / sizeof(probes[0]); at++) {
        if (argc == 2 && strcmp(argv[1], probes[at].name) != 0) continue;
        failures += run_probe(probes[at].name, probes[at].body);
    }
    for (size_t at = 0; at < sizeof(layout_probes) / sizeof(layout_probes[0]);
         at++) {
        if (argc == 2 && strcmp(argv[1], layout_probes[at].name) != 0)
            continue;
        failures += run_layout_probe(layout_probes[at].name,
                                     layout_probes[at].body);
    }
    for (size_t at = 0; at < sizeof(stream_probes) / sizeof(stream_probes[0]);
         at++) {
        if (argc == 2 && strcmp(argv[1], stream_probes[at].name) != 0)
            continue;
        failures += run_stream_probe(stream_probes[at].name,
                                     stream_probes[at].body);
    }
    if (failures != 0) return 1;
    puts("web forms and DOM tests passed");
    return 0;
}
