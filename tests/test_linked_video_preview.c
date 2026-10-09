#include "tilefinch/linked_video_preview.h"
#include "tilefinch/fetch.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <lexbor/dom/interface.h>

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "FAIL %s:%d: %s\n", \
    __FILE__, __LINE__, #x); return false; } } while (0)
#define MIB (1024u * 1024u)

static bool scan_case(Budget *budget, const char *html, const char *expected)
{
    size_t baseline = budget->current;
    size_t length = strlen(html);
    for (size_t chunk = 1; chunk <= 4096; chunk *= 8) {
        LinkedVideoPreviewScanner scan;
        linked_video_preview_scanner_init(&scan, budget);
        for (size_t at = 0; at < length && !scan.stopped; at += chunk) {
            size_t n = length - at < chunk ? length - at : chunk;
            linked_video_preview_scanner_feed(&scan,
                (const unsigned char *) html + at, n);
        }
        CHECK(strcmp(scan.image_url, expected) == 0);
        CHECK(budget->current == baseline);
    }
    return true;
}

static bool scanner_tests(Budget *budget)
{
    const char *good = "<meta property='og:image' content='https://preview.test/p.gif?a=1&amp;b=2'>";
    CHECK(scan_case(budget, good, "https://preview.test/p.gif?a=1&b=2"));
    CHECK(scan_case(budget, "<!doctype html><html><head><!--<meta property=og:image "
        "content=https://bad.test/a>--><title>&lt;meta&gt;</title>"
        "<script>var s='<meta property=og:image content=https://bad.test/b>';</script>"
        "<META NAME=twitter:image CONTENT=https://preview.test/p.gif>",
        "https://preview.test/p.gif"));
    const char *declines[] = {
        "<body><meta property=og:image content=https://bad.test/a>",
        "<div><meta property=og:image content=https://bad.test/a>",
        "<template><meta property=og:image content=https://bad.test/a>",
        "<noscript><meta property=og:image content=https://bad.test/a>",
        "<script><!--<script></script><meta property=og:image content=https://bad.test/a>",
        "<head></head><meta property=og:image content=https://bad.test/a>",
        "<meta property=og:image content=javascript:bad>",
        "<meta property=og:image content=/relative.gif>",
        "<meta property=og:image content='https://'>",
        "<style>body{content:'</stylex><meta property=og:image content=https://bad.test/a>'}"
    };
    for (size_t at = 0; at < sizeof(declines) / sizeof(declines[0]); at++)
        CHECK(scan_case(budget, declines[at], ""));
    char *long_head = malloc(600000u);
    CHECK(long_head != NULL);
    memcpy(long_head, "<style>", 7);
    memset(long_head + 7, ' ', 330000u);
    strcpy(long_head + 330007u, "</style><meta property=og:image content=https://preview.test/p.gif>");
    CHECK(scan_case(budget, long_head, "https://preview.test/p.gif"));
    memset(long_head + 7, ' ', 550000u);
    strcpy(long_head + 550007u, "</style><meta property=og:image content=https://bad.test/a>");
    CHECK(scan_case(budget, long_head, ""));
    memset(long_head, 'x', 6000);
    memcpy(long_head, "<meta content='", 15);
    strcpy(long_head + 6000, "' property=og:image><meta property=og:image content=https://bad.test/a>");
    CHECK(scan_case(budget, long_head, ""));
    free(long_head);
    for (size_t failure = 0; failure < 32; failure++) {
        LinkedVideoPreviewScanner scan;
        size_t baseline = budget->current;
        linked_video_preview_scanner_init(&scan, budget);
        budget_inject_failure_after(budget, failure);
        linked_video_preview_scanner_feed(&scan, (const unsigned char *) good, strlen(good));
        budget_clear_failure_injection(budget);
        CHECK(scan.image_url[0] == '\0'
              || strcmp(scan.image_url, "https://preview.test/p.gif?a=1&b=2") == 0);
        CHECK(budget->current == baseline);
    }
    return true;
}

static bool record(const char *dir, unsigned index, const char *url,
                   const char *effective, const char *type,
                   const void *body, size_t length)
{
    char path[256];
    snprintf(path, sizeof(path), "%s/%04u.meta", dir, index);
    FILE *f = fopen(path, "wb");
    CHECK(f != NULL);
    uint64_t hash = UINT64_C(14695981039346656037);
    for (size_t at = 0; at < length; at++) {
        hash ^= ((const unsigned char *) body)[at];
        hash *= UINT64_C(1099511628211);
    }
    CHECK(fprintf(f, "psp-http-trace=10\ncookie-values=redacted\nmethod=GET\nurl=%s\n"
        "logical-request-url=%s\nsuccess=1\nasync-delay-pumps=1\nexternal-cancel=0\n"
        "transport-timeout=0\nredirect-origin-tainted=0\nerror=\n"
        "request-body-length=0\nrequest-body-hash=cbf29ce484222325\nrequest-content-type=\n"
        "request-cookie-bytes=0\nrequest-has-cf-clearance=0\n"
        "request-extra-header-bytes=0\nrequest-extra-header-shape=\n"
        "request-allow-http-errors=1\nrequest-enforce-cors=0\nrequest-redirect-same-origin-only=0\n"
        "request-cors-cached-response-validated=0\nrequest-if-none-match=\nrequest-if-modified-since=\n"
        "request-referer=\nrequest-origin=\nrequest-accept=\nrequest-sec-fetch-dest=\n"
        "request-sec-fetch-mode=\nrequest-sec-fetch-site=\nrequest-send-client-hints=0\n"
        "request-client-hint-tokens=\nrequest-client-hint-origin=\nrequest-send-low-client-hints=0\n"
        "request-sec-fetch-user=0\nrequest-upgrade-insecure=0\nrequest-user-agent=\n"
        "request-diagnostic-mobile-safari=0\nrequest-credentials=0\nrequest-credential-origin=\n"
        "request-initiator-url=\nrequest-referrer-source=\nrequest-referrer-policy=\n"
        "status=200\nlength=%zu\nresponse-body-hash=%016llx\neffective-url=%s\ncontent-type=%s\n"
        "etag=\nlast-modified=\ncf-mitigated=\naccept-ch=\ncritical-ch=\nserver=fixture\ncf-ray=\n"
        "response-referrer-policy-metadata-valid=1\nresponse-referrer-policy-present=0\n"
        "response-referrer-policy=\nresponse-security-headers-truncated=0\n"
        "set-cookie-count=0\nresponse-header-count=1\nresponse-header-0=content-type: %s\n",
        url, url, length, (unsigned long long) hash, effective, type, type) > 0);
    CHECK(fclose(f) == 0);
    snprintf(path, sizeof(path), "%s/%04u.body", dir, index);
    f = fopen(path, "wb");
    CHECK(f != NULL && fwrite(body, 1, length, f) == length);
    CHECK(fclose(f) == 0);
    return true;
}

static bool make_trace(const char *dir)
{
    char *article = malloc(900000u);
    CHECK(article != NULL);
    memcpy(article, "<style>", 7);
    memset(article + 7, ' ', 330000u);
    const char *meta = "</style><meta property=og:image content=https://preview.test/p.gif></head><body>";
    memcpy(article + 330007u, meta, strlen(meta));
    memset(article + 330007u + strlen(meta), 'x', 900000u - 330007u - strlen(meta));
    CHECK(record(dir, 0, "https://preview.test/article", "https://preview.test/article",
                 "text/html; charset=utf-8", article, 900000u));
    free(article);
    const unsigned char gif[] = { 'G','I','F','8','9','a',1,0,1,0,0x80,0,0,
        255,0,0,0,0,0,0x2c,0,0,0,0,1,0,1,0,0,2,2,0x44,1,0,0x3b };
    CHECK(record(dir, 1, "https://preview.test/p.gif", "https://preview.test/p.gif",
                 "image/gif", gif, sizeof(gif)));
    const char *good = "<meta property=og:image content=https://preview.test/p.gif>";
    CHECK(record(dir, 2, "https://preview.test/bad-type", "https://preview.test/bad-type",
                 "text/htmljunk", good, strlen(good)));
    CHECK(record(dir, 3, "https://preview.test/redirect", "https://elsewhere.test/article",
                 "text/html", good, strlen(good)));
    char path[256]; snprintf(path, sizeof(path), "%s/trace.meta", dir);
    FILE *f = fopen(path, "wb"); CHECK(f != NULL);
    CHECK(fputs("psp-http-trace-clock=1\norigin-ms=1700000000000\n"
        "capture-complete=yes\nrecord-count=4\n", f) >= 0);
    CHECK(fclose(f) == 0);
    return true;
}

static lxb_dom_node_t *video_node(NavigationSession *nav)
{
    for (size_t at = 0; at < nav->page.layout.node_box_count; at++) {
        lxb_dom_node_t *node = nav->page.layout.node_boxes[at].node;
        size_t length = 0;
        const char *name = document_element_name(node, &length);
        if (name != NULL && length == 5 && memcmp(name, "video", 5) == 0) return node;
    }
    return NULL;
}

static bool navigation_case(Budget *budget, const char *dir,
                            const char *markup, bool expected, unsigned attempts,
                            unsigned cancel)
{
    size_t baseline = budget->current;
    char error[256];
    CHECK(fetch_trace_replay_begin_response_keyed(dir, error, sizeof(error)));
    NavigationSession nav;
    CHECK(navigation_init(&nav, budget, 2));
    navigation_enable_external_resources(&nav, 2, 65536, 32768,
        8, MIB, 128 * 1024u, MIB, 1000);
    char html[4096];
    snprintf(html, sizeof(html), "<!doctype html><title>Preview harness</title><style>body{margin:0}video{width:160px;"
        "height:100px}</style>%s", markup);
    uint64_t generation = navigation_begin(&nav);
    CHECK(navigation_commit_static_html(&nav, generation, "https://preview.test/",
        html, strlen(html), 480, NULL, NULL, true));
    /* Trusted static commits intentionally do not adopt author security
       metadata. Install the response policy a network commit would own. */
    if (strstr(markup, "Content-Security-Policy") != NULL) {
        const char *header = strstr(markup, "connect-src") != NULL
            ? "content-security-policy: connect-src 'none'\n"
            : "content-security-policy: img-src 'none'\n";
        CHECK(tilefinch_csp_parse_response_headers(&nav.page.document.content_security_policy,
            "https://preview.test/", header, strlen(header), false));
    }
    lxb_dom_node_t *video = video_node(&nav);
    for (unsigned pump = 0; pump < 700; pump++) {
        bool work = navigation_run_linked_video_preview(&nav);
        if (cancel && nav.page.linked_video_preview != NULL) {
            if (cancel == 2) {
                navigation_cancel_network_work(&nav, "user stopped loading");
                CHECK(!navigation_run_linked_video_preview(&nav));
            } else {
                nav.page.document.content_generation++;
                CHECK(navigation_run_linked_video_preview(&nav));
            }
            CHECK(nav.page.linked_video_preview == NULL);
            break;
        }
        if (!work) break;
    }
    CHECK(nav.page.loaded);
    CHECK(strcmp(nav.page.document.title, "Preview harness") == 0);
    CHECK(nav.page.linked_video_preview_attempts == attempts);
    printf("case %.90s attempts=%u loaded=%zu failed=%zu images=%zu expected=%d\n",
        markup, (unsigned) nav.page.linked_video_preview_attempts,
        nav.page.images.stats.loaded, nav.page.images.stats.failed,
        nav.page.images.count, expected);
    CHECK((video != NULL && image_resource_available(images_find_node(&nav.page.images, video))) == expected);
    CHECK(nav.page.linked_video_preview == NULL);
    if (expected) {
        bool painted = false;
        for (size_t at = 0; at < nav.page.layout.count; at++)
            painted |= nav.page.layout.commands[at].type == DRAW_IMAGE;
        CHECK(painted);
        if (attempts != 0) CHECK(document_attribute(video, "poster", NULL) == NULL);
    }
    navigation_destroy(&nav);
    fetch_trace_end();
    CHECK(budget->current == baseline);
    return true;
}

static bool replacement_releases_active_preview(Budget *budget,
                                                const char *dir)
{
    size_t baseline = budget->current;
    char error[256];
    CHECK(fetch_trace_replay_begin_response_keyed(dir, error, sizeof(error)));
    NavigationSession nav;
    CHECK(navigation_init(&nav, budget, 2));
    navigation_enable_external_resources(&nav, 2, 65536, 32768,
        8, MIB, 128 * 1024u, MIB, 1000);
    static const char first[] =
        "<!doctype html><style>video{width:160px;height:100px}</style>"
        "<a href=/article><video src=/clip.mp4></video></a>";
    CHECK(navigation_commit_static_html(&nav, navigation_begin(&nav),
        "https://preview.test/", first, sizeof(first) - 1u,
        480, NULL, NULL, true));
    for (unsigned pump = 0; pump < 32u && nav.page.linked_video_preview == NULL;
         pump++) CHECK(navigation_run_linked_video_preview(&nav));
    CHECK(nav.page.linked_video_preview != NULL);
    static const char second[] = "<!doctype html><title>Replacement</title>";
    CHECK(navigation_commit_static_html(&nav, navigation_begin(&nav),
        "https://preview.test/next", second, sizeof(second) - 1u,
        480, NULL, NULL, true));
    navigation_destroy(&nav);
    fetch_trace_end();
    CHECK(budget->current == baseline);
    return true;
}

typedef struct { size_t bytes; bool stop; } StreamProbe;
static bool count_body(void *opaque, const unsigned char *data, size_t length)
{
    (void) data;
    StreamProbe *probe = opaque;
    probe->bytes += length;
    return !(probe->stop && probe->bytes >= 8192u);
}

static bool replay_ceiling(Budget *budget, const char *dir, bool stop)
{
    char error[256];
    CHECK(fetch_trace_replay_begin_response_keyed(dir, error, sizeof(error)));
    FetchScheduler *scheduler = fetch_scheduler_create(budget, 1, 32768u);
    CHECK(scheduler != NULL);
    StreamProbe probe = {.stop = stop};
    FetchStreamOptions stream = {.on_body = count_body, .opaque = &probe, .chunk_bytes = 4096u};
    uint64_t id = fetch_scheduler_enqueue_stream(scheduler, "https://preview.test/article",
        NULL, 16384u, 1000, &stream);
    CHECK(id != 0);
    for (unsigned at = 0; at < 64 && !fetch_scheduler_request_complete(scheduler, id); at++)
        fetch_scheduler_pump(scheduler, 1, 0);
    bool success;
    FetchResult result = {.budget = budget};
    CHECK(fetch_scheduler_take(scheduler, id, &success, &result));
    CHECK(!success && probe.bytes == (stop ? 8192u : 16384u));
    CHECK(stop ? !result.response_limit_exceeded : result.response_limit_exceeded);
    fetch_result_destroy(&result);
    fetch_scheduler_destroy(scheduler);
    fetch_trace_end();
    return true;
}

static bool refusal_tests(Budget *budget, const char *dir)
{
    const char *html = "<!doctype html><a href=/article><video src=/clip.mp4 "
        "style='width:160px;height:100px'></video></a>";
    for (size_t refusal = 0; refusal < 48; refusal++) {
        size_t baseline = budget->current;
        char error[256];
        CHECK(fetch_trace_replay_begin_response_keyed(dir, error, sizeof(error)));
        NavigationSession nav;
        CHECK(navigation_init(&nav, budget, 2));
        navigation_enable_external_resources(&nav, 2, 65536, 32768,
            8, MIB, 128 * 1024u, MIB, 1000);
        CHECK(navigation_commit_static_html(&nav, navigation_begin(&nav),
            "https://preview.test/", html, strlen(html), 480, NULL, NULL, true));
        budget_inject_failure_after(budget, refusal);
        for (unsigned at = 0; at < 200; at++)
            if (!navigation_run_linked_video_preview(&nav)) break;
        budget_clear_failure_injection(budget);
        CHECK(nav.page.loaded);
        navigation_destroy(&nav);
        fetch_trace_end();
        CHECK(budget->current == baseline);
    }
    return true;
}

static bool corrupt_tail_test(Budget *budget, const char *dir)
{
    char path[256]; snprintf(path, sizeof(path), "%s/0000.body", dir);
    FILE *f = fopen(path, "r+b"); CHECK(f != NULL);
    CHECK(fseek(f, 899999L, SEEK_SET) == 0 && fputc('!', f) != EOF && fclose(f) == 0);
    char error[256];
    CHECK(fetch_trace_replay_begin_response_keyed(dir, error, sizeof(error)));
    FetchScheduler *scheduler = fetch_scheduler_create(budget, 1, 32768u);
    CHECK(scheduler != NULL);
    StreamProbe probe = {.stop = true};
    FetchStreamOptions stream = {.on_body = count_body, .opaque = &probe, .chunk_bytes = 4096u};
    uint64_t id = fetch_scheduler_enqueue_stream(scheduler, "https://preview.test/article",
        NULL, 16384u, 1000, &stream);
    CHECK(id != 0);
    for (unsigned at = 0; at < 64 && !fetch_scheduler_request_complete(scheduler, id); at++)
        fetch_scheduler_pump(scheduler, 1, 0);
    bool success; FetchResult result = {.budget = budget};
    CHECK(fetch_scheduler_take(scheduler, id, &success, &result));
    CHECK(!success && probe.bytes == 0 && strstr(result.error, "corrupt") != NULL);
    fetch_result_destroy(&result); fetch_scheduler_destroy(scheduler); fetch_trace_end();
    return true;
}

int main(void)
{
    Budget budget; budget_init(&budget, 24u * MIB);
    if (!budget_install_lexbor(&budget)) return 1;
    char dir[] = "/tmp/tilefinch-video-preview-XXXXXX";
    if (mkdtemp(dir) == NULL) return 1;
    bool ok = scanner_tests(&budget) && make_trace(dir)
        && replay_ceiling(&budget, dir, true) && replay_ceiling(&budget, dir, false)
        && navigation_case(&budget, dir, "<a href=/article><video src=/clip.mp4></video></a>", true, 1, false)
        && replacement_releases_active_preview(&budget, dir)
        && navigation_case(&budget, dir, "<a href=/article><video><source src=/clip.mp4></video></a>", true, 1, false)
        && navigation_case(&budget, dir, "<a href=/bad-type><video src=/clip.mp4></video></a>", false, 1, false)
        && navigation_case(&budget, dir, "<a href=/redirect><video src=/clip.mp4></video></a>", false, 1, false)
        && navigation_case(&budget, dir, "<a href=/article><video src=/clip.mp4></video></a>", false, 1, true)
        && navigation_case(&budget, dir, "<a href=/article><video src=/clip.mp4></video></a>", false, 2, 2)
        && navigation_case(&budget, dir, "<a href=https://elsewhere.test/article><video src=/clip.mp4></video></a>", false, 0, false)
        && navigation_case(&budget, dir, "<a href=http://preview.test/article><video src=/clip.mp4></video></a>", false, 0, false)
        && navigation_case(&budget, dir, "<a href='/#anchor'><video src=/clip.mp4></video></a>", false, 0, false)
        && navigation_case(&budget, dir, "<a href=/article download><video src=/clip.mp4></video></a>", false, 0, false)
        && navigation_case(&budget, dir, "<a href=/article style=display:none><video src=/clip.mp4></video></a>", false, 0, false)
        && navigation_case(&budget, dir, "<a href=/article style=visibility:hidden><video src=/clip.mp4></video></a>", false, 0, false)
        && navigation_case(&budget, dir, "<a href=/article style=opacity:0><video src=/clip.mp4></video></a>", false, 0, false)
        && navigation_case(&budget, dir, "<div style=height:800px></div><a href=/article><video src=/clip.mp4></video></a>", false, 0, false)
        && navigation_case(&budget, dir, "<video src=/clip.mp4></video>", false, 0, false)
        && navigation_case(&budget, dir, "<a href=/article><video></video></a>", false, 0, false)
        && navigation_case(&budget, dir, "<meta http-equiv=Content-Security-Policy content=\"connect-src 'none'\"><a href=/article><video src=/clip.mp4></video></a>", false, 1, false)
        && navigation_case(&budget, dir, "<meta http-equiv=Content-Security-Policy content=\"img-src 'none'\"><a href=/article><video src=/clip.mp4></video></a>", false, 1, false)
        && navigation_case(&budget, dir, "<a href=/article><video src=/clip.mp4></video><video src=/clip.mp4></video><video src=/clip.mp4></video></a>", true, 2, false)
        && navigation_case(&budget, dir, "<a href=/article><video src=/clip.mp4 poster='data:image/gif;base64,R0lGODlhAQABAIABAP8AAAAAACH5BAEKAAEALAAAAAABAAEAAAICRAEAOw=='></video></a>", true, 0, false)
        && refusal_tests(&budget, dir) && corrupt_tail_test(&budget, dir);
    char path[256];
    for (unsigned at = 0; at < 4; at++) {
        snprintf(path, sizeof(path), "%s/%04u.meta", dir, at); unlink(path);
        snprintf(path, sizeof(path), "%s/%04u.body", dir, at); unlink(path);
    }
    snprintf(path, sizeof(path), "%s/trace.meta", dir); unlink(path); rmdir(dir);
    ok &= budget.current == 0 && budget_uninstall_lexbor(&budget);
    puts(ok ? "linked video previews PASS" : "linked video previews FAIL");
    return ok ? 0 : 1;
}
