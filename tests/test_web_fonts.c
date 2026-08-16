#include "tilefinch/budget.h"
#include "tilefinch/content_blocker.h"
#include "tilefinch/document.h"
#include "tilefinch/fetch.h"
#include "tilefinch/font.h"
#include "tilefinch/platform.h"
#include "tilefinch/request_context.h"
#include "tilefinch/resources.h"
#include "tilefinch/session.h"
#include "tilefinch/style.h"
#include "tilefinch/user_agent.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#ifndef TILEFINCH_TEST_SOURCE_DIR
#define TILEFINCH_TEST_SOURCE_DIR "."
#endif

#define KIB (1024u)
#define MIB (1024u * KIB)
#define CHECK(condition) do {                                                \
    if (!(condition)) {                                                      \
        fprintf(stderr, "web-font test failed at %s:%d: %s\n",           \
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
        if (id != NULL && length == strlen(wanted)
            && memcmp(id, wanted, length) == 0) return node;
    }
    for (lxb_dom_node_t *child = node->first_child; child != NULL;
         child = child->next) {
        lxb_dom_node_t *found = find_id(child, wanted);
        if (found != NULL) return found;
    }
    return NULL;
}

typedef struct {
    size_t stylesheet_parse_calls;
    size_t font_scan_calls;
    size_t font_fixup_calls;
} FontParsePhases;

static bool count_font_parse_phase(void *opaque, const char *phase,
                                   size_t completed_work_units)
{
    (void) completed_work_units;
    FontParsePhases *phases = opaque;
    if (phases == NULL || phase == NULL) return true;
    if (strcmp(phase, "stylesheet-parse") == 0) {
        phases->stylesheet_parse_calls++;
    } else if (strcmp(phase, "stylesheet-font-scan") == 0) {
        phases->font_scan_calls++;
    } else if (strcmp(phase, "stylesheet-font-fixup") == 0) {
        phases->font_fixup_calls++;
    }
    return true;
}

static bool test_inline_discovery_and_canonicalization(void)
{
    static const char html[] =
        "<!doctype html><style>"
        ".target{font-family:GDS\\20 Transport,serif;font-weight:700}"
        "/* @font-face{font-family:Comment Fake;src:url(comment.ttf)} */"
        ".noise::before{content:\"@font-face{font-family:String Fake;"
        "src:url(string.ttf)}\"}"
        "@keyframes fake{from{opacity:0}"
        "@font-face{font-family:Keyframe Fake;src:url(keyframe.ttf)}}"
        "@media (min-width:900px){@font-face{font-family:Media Fake;"
        "src:url(media.ttf)}}"
        "@supports (definitely-unsupported-property:value){"
        "@font-face{font-family:Supports Fake;src:url(supports.ttf)}}"
        "@container card (min-width:1px){"
        "@font-face{font-family:Container Fake;src:url(container.ttf)}}"
        "@media screen and (max-width:600px){"
        "@font-face{font-family:\"GDS Trans\\70 ort\";font-weight:400;"
        "font-style:normal;src:local(\"GDS Transport\"),"
        "url(regular.woff2) format(\"woff2\"),"
        "url(\"DATA:font/ttf;base64,AAAA\") format(\"truetype\"),"
        "url(../fonts/regular.woff) format(\"woff\"),"
        "url(./fallback.ttf) format(\"truetype\"),"
        "url(ignored.svg) format(\"svg\")}}"
        "@supports (display:flex){@layer fonts{"
        "@font-face{font-family:gds transport;font-weight:700;"
        "src:url(./bold.woff) format(woff)}}}"
        "</style><div id=target class=target>target</div>";
    Budget budget;
    budget_init(&budget, 8u * MIB);
    CHECK(budget_install_lexbor(&budget));
    PocDocument document = {0};
    Stylesheet sheet = {0};
    CHECK(document_parse(&document, &budget, html, sizeof(html) - 1u, 19));
    lxb_dom_node_t *target = find_id(
        lxb_dom_interface_node(document.html), "target");
    FontParsePhases phases = {0};
    TilefinchPlatformServices services = {
        .context = &phases,
        .cooperate = count_font_parse_phase
    };
    tilefinch_platform_set_services(&services);
    CHECK(target != NULL && stylesheet_build(&sheet, &budget, &document, 480));
    tilefinch_platform_set_services(NULL);
    CHECK(phases.stylesheet_parse_calls != 0);
    CHECK(phases.font_scan_calls == 0);
    CHECK(phases.font_fixup_calls != 0);

    StylesheetWebFontStats stats = {0};
    CHECK(stylesheet_web_font_stats(&sheet, &stats));
    CHECK(stats.declarations_discovered == 2);
    CHECK(stats.sources_selected == 3);
    CHECK(stats.unsupported_sources == 0);
    CHECK(stats.duplicate_sources == 0);
    CHECK(stats.skipped_family_limit == 0);
    CHECK(stats.skipped_source_limit == 0);
    CHECK(stylesheet_web_font_source_count(&sheet) == 3);

    StylesheetWebFontSource source = {0};
    CHECK(stylesheet_web_font_source(&sheet, 0, &source));
    CHECK(source.family_slot == 0 && !source.bold);
    CHECK(strcmp(source.reference, "../fonts/regular.woff") == 0);
    CHECK(source.source_base_url == NULL);
    CHECK(source.source_referrer_policy == NULL);
    CHECK(stylesheet_web_font_source(&sheet, 1, &source));
    CHECK(source.family_slot == 0 && !source.bold);
    CHECK(strcmp(source.reference, "./fallback.ttf") == 0);
    CHECK(stylesheet_web_font_source(&sheet, 2, &source));
    CHECK(source.family_slot == 0 && source.bold);
    CHECK(strcmp(source.reference, "./bold.woff") == 0);
    CHECK(!stylesheet_web_font_source(&sheet, 3, &source));

    ComputedStyle computed = style_for_node(&sheet, target, NULL);
    CHECK(font_family_is_web(computed.font_family));
    CHECK(font_family_web_slot(computed.font_family) == 0);
    CHECK(font_family_web_fallback(computed.font_family) == FONT_SERIF);
    CHECK(computed.font_bold && computed.font_weight == 700);

    stylesheet_destroy(&sheet);
    document_destroy(&document);
    CHECK(budget.current == 0);
    CHECK(budget_uninstall_lexbor(&budget));
    return true;
}

static bool test_external_base_and_source_order(void)
{
    static const char html[] =
        "<!doctype html><div id=target class=target>target</div>";
    static const char css[] =
        ".target{font-family:'aux sans',monospace}"
        "@font-face{font-family:\"AUX   Sans\";font-weight:normal;"
        "src:local(Aux),url(../fonts/aux.woff) format(woff),"
        "url(fallback.ttf) format(truetype)}";
    static const char base[] = "https://cdn.example/assets/css/site.css";
    Budget budget;
    budget_init(&budget, 8u * MIB);
    CHECK(budget_install_lexbor(&budget));
    PocDocument document = {0};
    Stylesheet sheet = {0};
    CHECK(document_parse(&document, &budget, html, sizeof(html) - 1u, 13));
    lxb_dom_node_t *target = find_id(
        lxb_dom_interface_node(document.html), "target");
    CHECK(target != NULL && stylesheet_build(&sheet, &budget, &document, 480));
    CHECK(stylesheet_add_css_from(&sheet, css, sizeof(css) - 1u, base));
    CHECK(stylesheet_add_css_from(&sheet, css, sizeof(css) - 1u, base));

    StylesheetWebFontStats stats = {0};
    CHECK(stylesheet_web_font_stats(&sheet, &stats));
    CHECK(stats.declarations_discovered == 2);
    CHECK(stats.sources_selected == 2);
    CHECK(stats.duplicate_sources == 2);
    CHECK(stylesheet_web_font_source_count(&sheet) == 2);
    StylesheetWebFontSource source = {0};
    CHECK(stylesheet_web_font_source(&sheet, 0, &source));
    CHECK(!source.bold && source.family_slot == 0);
    CHECK(strcmp(source.reference, "../fonts/aux.woff") == 0);
    CHECK(source.source_base_url != NULL
          && strcmp(source.source_base_url, base) == 0);
    CHECK(source.source_referrer_policy != NULL
          && source.source_referrer_policy[0] == '\0');
    CHECK(stylesheet_web_font_source(&sheet, 1, &source));
    CHECK(strcmp(source.reference, "fallback.ttf") == 0);
    CHECK(source.source_base_url != NULL
          && strcmp(source.source_base_url, base) == 0);
    CHECK(source.source_referrer_policy != NULL
          && source.source_referrer_policy[0] == '\0');

    ComputedStyle computed = style_for_node(&sheet, target, NULL);
    CHECK(font_family_is_web(computed.font_family));
    CHECK(font_family_web_slot(computed.font_family) == 0);
    CHECK(font_family_web_fallback(computed.font_family) == FONT_MONOSPACE);

    stylesheet_destroy(&sheet);
    document_destroy(&document);
    CHECK(budget.current == 0);
    CHECK(budget_uninstall_lexbor(&budget));
    return true;
}

static bool test_single_pass_forward_reference_reinterns(void)
{
    static const char html[] =
        "<!doctype html><style>"
        ".a{font-family:Forward Face,sans-serif}"
        ".b{font-family:Forward Face,sans-serif}"
        "</style><style>"
        "@font-face{font-family:Forward Face;src:url(forward.ttf)}"
        "</style><div id=a class=a>a</div><div id=b class=b>b</div>";
    Budget budget;
    budget_init(&budget, 8u * MIB);
    CHECK(budget_install_lexbor(&budget));
    PocDocument document = {0};
    Stylesheet sheet = {0};
    CHECK(document_parse(&document, &budget, html, sizeof(html) - 1u, 17));
    CHECK(stylesheet_build(&sheet, &budget, &document, 480));
    CHECK(sheet.count == 2 && sheet.declaration_count == 1);
    CHECK(stylesheet_web_font_source_count(&sheet) == 1);
    lxb_dom_node_t *a = find_id(lxb_dom_interface_node(document.html), "a");
    lxb_dom_node_t *b = find_id(lxb_dom_interface_node(document.html), "b");
    ComputedStyle a_style = style_for_node(&sheet, a, NULL);
    ComputedStyle b_style = style_for_node(&sheet, b, NULL);
    CHECK(font_family_is_web(a_style.font_family)
          && a_style.font_family == b_style.font_family);
    stylesheet_destroy(&sheet);
    document_destroy(&document);
    CHECK(budget.current == 0);
    CHECK(budget_uninstall_lexbor(&budget));
    return true;
}

static bool test_external_referrer_policy_identity(void)
{
    static const char html[] = "<!doctype html><div>target</div>";
    static const char css[] =
        "@font-face{font-family:Policy Face;"
        "src:url(font.woff) format(woff)}";
    static const char allocation_failure_css[] =
        "@font-face{font-family:Policy Allocation Failure;"
        "src:url(failure.woff) format(woff)}";
    static const char base[] = "https://cdn.example/css/site.css";
    Budget budget;
    budget_init(&budget, 8u * MIB);
    CHECK(budget_install_lexbor(&budget));
    PocDocument document = {0};
    Stylesheet sheet = {0};
    CHECK(STYLE_WEB_FONT_REFERRER_POLICY_CAPACITY == 40);
    CHECK(document_parse(&document, &budget, html, sizeof(html) - 1u, 13));
    CHECK(stylesheet_build(&sheet, &budget, &document, 480));
    CHECK(stylesheet_add_css_from_context(
        &sheet, css, sizeof(css) - 1u, base, "No-Referrer"));
    CHECK(stylesheet_add_css_from_context(
        &sheet, css, sizeof(css) - 1u, base, "unsafe-url"));
    CHECK(stylesheet_add_css_from_context(
        &sheet, css, sizeof(css) - 1u, base, "NO-REFERRER"));

    StylesheetWebFontStats stats = {0};
    CHECK(stylesheet_web_font_stats(&sheet, &stats));
    CHECK(stats.declarations_discovered == 3);
    CHECK(stats.sources_selected == 2);
    CHECK(stats.duplicate_sources == 1);
    CHECK(stats.skipped_source_limit == 0);
    CHECK(stylesheet_web_font_source_count(&sheet) == 2);
    StylesheetWebFontSource source = {0};
    CHECK(stylesheet_web_font_source(&sheet, 0, &source));
    CHECK(source.source_base_url != NULL
          && strcmp(source.source_base_url, base) == 0);
    CHECK(source.source_referrer_policy != NULL
          && strcmp(source.source_referrer_policy, "no-referrer") == 0);
    CHECK(stylesheet_web_font_source(&sheet, 1, &source));
    CHECK(source.source_base_url != NULL
          && strcmp(source.source_base_url, base) == 0);
    CHECK(source.source_referrer_policy != NULL
          && strcmp(source.source_referrer_policy, "unsafe-url") == 0);

    /* With the fixed policy stored inline, this permits exactly the source
       reference allocation and then fails the external-base allocation.
       The partially retained reference must be rolled back and the source
       must not become reachable with document-relative semantics. */
    size_t before_failed_source = budget.current;
    budget_inject_failure_after(&budget, 1);
    CHECK(stylesheet_add_css_from_context(
        &sheet, allocation_failure_css, sizeof(allocation_failure_css) - 1u,
        base, "origin"));
    budget_clear_failure_injection(&budget);
    CHECK(budget.current == before_failed_source);
    CHECK(stylesheet_web_font_source_count(&sheet) == 2);
    CHECK(stylesheet_web_font_stats(&sheet, &stats));
    CHECK(stats.declarations_discovered == 4);
    CHECK(stats.sources_selected == 2);
    CHECK(stats.duplicate_sources == 1);
    CHECK(stats.skipped_source_limit == 1);

    stylesheet_destroy(&sheet);
    document_destroy(&document);
    CHECK(budget.current == 0);
    CHECK(budget_uninstall_lexbor(&budget));
    return true;
}

static void store_be32(unsigned char *destination, uint32_t value)
{
    destination[0] = (unsigned char) (value >> 24);
    destination[1] = (unsigned char) (value >> 16);
    destination[2] = (unsigned char) (value >> 8);
    destination[3] = (unsigned char) value;
}

static uint16_t read_be16(const unsigned char *source)
{
    return (uint16_t) ((uint16_t) source[0] << 8 | source[1]);
}

static uint32_t read_be32(const unsigned char *source)
{
    return (uint32_t) source[0] << 24 | (uint32_t) source[1] << 16
        | (uint32_t) source[2] << 8 | source[3];
}

static void store_be16(unsigned char *destination, uint16_t value)
{
    destination[0] = (unsigned char) (value >> 8);
    destination[1] = (unsigned char) value;
}

static bool aligned_four(size_t value, size_t *aligned)
{
    if (aligned == NULL || value > SIZE_MAX - 3u) return false;
    *aligned = (value + 3u) & ~(size_t) 3u;
    return true;
}

/* Build a standards-shaped, uncompressed WOFF 1.0 wrapper around the
   permissively licensed Tilefinch sfnt at runtime.  This exercises the exact
   page-font decoder without retaining another binary fixture in the repo. */
static bool build_uncompressed_woff(const unsigned char *sfnt,
                                    size_t sfnt_length,
                                    unsigned char **woff,
                                    size_t *woff_length)
{
    if (sfnt == NULL || woff == NULL || woff_length == NULL
        || sfnt_length < 12 || read_be32(sfnt) != UINT32_C(0x00010000)) {
        return false;
    }
    *woff = NULL;
    *woff_length = 0;
    uint16_t table_count = read_be16(sfnt + 4);
    if (table_count == 0 || table_count > 64
        || (size_t) table_count > (sfnt_length - 12u) / 16u) return false;
    size_t directory_bytes = (size_t) table_count * 20u;
    size_t output_size = 44u + directory_bytes;
    size_t reconstructed_size = 12u + (size_t) table_count * 16u;
    for (size_t table = 0; table < table_count; table++) {
        const unsigned char *record = sfnt + 12u + table * 16u;
        size_t source_offset = read_be32(record + 8);
        size_t source_length = read_be32(record + 12);
        size_t padded = 0;
        if (source_offset > sfnt_length
            || source_length > sfnt_length - source_offset
            || !aligned_four(source_length, &padded)
            || output_size > SIZE_MAX - padded
            || reconstructed_size > SIZE_MAX - padded) return false;
        output_size += padded;
        reconstructed_size += padded;
    }
    if (output_size > UINT32_MAX || reconstructed_size > UINT32_MAX
        || output_size > 4u * MIB) return false;
    unsigned char *output = calloc(1, output_size);
    if (output == NULL) return false;
    memcpy(output, "wOFF", 4);
    store_be32(output + 4, read_be32(sfnt));
    store_be32(output + 8, (uint32_t) output_size);
    store_be16(output + 12, table_count);
    store_be32(output + 16, (uint32_t) reconstructed_size);
    store_be16(output + 20, 1);
    size_t destination_offset = 44u + directory_bytes;
    for (size_t table = 0; table < table_count; table++) {
        const unsigned char *record = sfnt + 12u + table * 16u;
        unsigned char *destination = output + 44u + table * 20u;
        size_t source_offset = read_be32(record + 8);
        size_t source_length = read_be32(record + 12);
        size_t padded = 0;
        if (!aligned_four(source_length, &padded)) {
            free(output);
            return false;
        }
        store_be32(destination, read_be32(record));
        store_be32(destination + 4, (uint32_t) destination_offset);
        store_be32(destination + 8, (uint32_t) source_length);
        store_be32(destination + 12, (uint32_t) source_length);
        store_be32(destination + 16, read_be32(record + 4));
        memcpy(output + destination_offset, sfnt + source_offset,
               source_length);
        destination_offset += padded;
    }
    if (destination_offset != output_size) {
        free(output);
        return false;
    }
    *woff = output;
    *woff_length = output_size;
    return true;
}

static bool read_file(const char *path, unsigned char **data, size_t *length)
{
    *data = NULL;
    *length = 0;
    FILE *file = fopen(path, "rb");
    if (file == NULL || fseek(file, 0, SEEK_END) != 0) {
        if (file != NULL) fclose(file);
        return false;
    }
    long size = ftell(file);
    if (size <= 0 || (unsigned long) size > 2u * MIB
        || fseek(file, 0, SEEK_SET) != 0) {
        fclose(file);
        return false;
    }
    unsigned char *bytes = malloc((size_t) size);
    if (bytes == NULL) {
        fclose(file);
        return false;
    }
    bool ok = fread(bytes, 1, (size_t) size, file) == (size_t) size;
    bool closed = fclose(file) == 0;
    if (!ok || !closed) {
        free(bytes);
        return false;
    }
    *data = bytes;
    *length = (size_t) size;
    return true;
}

static void remove_async_font_replay(const char *directory)
{
    char path[512];
    const char *files[] = {"0000.body", "0000.meta", "trace.meta"};
    for (size_t i = 0; i < sizeof(files) / sizeof(files[0]); i++) {
        if (snprintf(path, sizeof(path), "%s/%s", directory, files[i]) > 0) {
            (void) unlink(path);
        }
    }
    (void) rmdir(directory);
}

static uint64_t async_font_body_hash(const unsigned char *data,
                                     size_t length)
{
    uint64_t value = UINT64_C(14695981039346656037);
    for (size_t i = 0; i < length; i++) {
        value ^= data[i];
        value *= UINT64_C(1099511628211);
    }
    return value;
}

static bool write_async_font_replay(
    const unsigned char *font, size_t font_length,
    char directory[128])
{
    if (font == NULL || font_length == 0 || directory == NULL) return false;
    snprintf(directory, 128, "%s", "/tmp/tilefinch-font-async-XXXXXX");
    if (mkdtemp(directory) == NULL) return false;
    char body_path[192], meta_path[192], trace_path[192];
    bool paths = snprintf(body_path, sizeof(body_path), "%s/0000.body",
                          directory) > 0
        && snprintf(meta_path, sizeof(meta_path), "%s/0000.meta",
                    directory) > 0
        && snprintf(trace_path, sizeof(trace_path), "%s/trace.meta",
                    directory) > 0;
    FILE *body = paths ? fopen(body_path, "wb") : NULL;
    bool body_written = body != NULL
        && fwrite(font, 1, font_length, body) == font_length;
    bool body_closed = body != NULL && fclose(body) == 0;
    FILE *meta = body_written && body_closed ? fopen(meta_path, "wb") : NULL;
    bool meta_written = meta != NULL && fprintf(
        meta,
        "psp-http-trace=8\n"
        "cookie-values=redacted\n"
        "method=GET\n"
        "url=https://font-async.test/font.ttf\n"
        "success=1\n"
        "async-delay-pumps=2\n"
        "external-cancel=0\n"
        "transport-timeout=0\n"
        "redirect-origin-tainted=0\n"
        "error=\n"
        "request-body-length=0\n"
        "request-body-hash=cbf29ce484222325\n"
        "request-content-type=\n"
        "request-cookie-bytes=0\n"
        "request-has-cf-clearance=0\n"
        "request-extra-header-bytes=0\n"
        "request-extra-header-shape=\n"
        "request-allow-http-errors=1\n"
        "request-enforce-cors=1\n"
        "request-redirect-same-origin-only=0\n"
        "request-cors-cached-response-validated=0\n"
        "request-if-none-match=\n"
        "request-if-modified-since=\n"
        "request-referer=\n"
        "request-origin=https://font-async.test\n"
        "request-accept=font/woff,application/font-woff;q=0.9,*/*;q=0.1\n"
        "request-sec-fetch-dest=font\n"
        "request-sec-fetch-mode=cors\n"
        "request-sec-fetch-site=same-origin\n"
        "request-send-client-hints=0\n"
        "request-client-hint-tokens=\n"
        "request-client-hint-origin=\n"
        "request-send-low-client-hints=0\n"
        "request-sec-fetch-user=0\n"
        "request-upgrade-insecure=0\n"
        "request-user-agent=" TILEFINCH_BROWSER_USER_AGENT "\n"
        "request-diagnostic-mobile-safari=0\n"
        "request-credentials=2\n"
        "request-credential-origin=https://font-async.test\n"
        "request-initiator-url=https://font-async.test/page\n"
        "request-referrer-source=https://font-async.test/page\n"
        "request-referrer-policy=no-referrer\n"
        "status=200\n"
        "length=%zu\n"
        "response-body-hash=%016llx\n"
        "effective-url=https://font-async.test/font.ttf\n"
        "content-type=font/ttf\n"
        "etag=\nlast-modified=\ncf-mitigated=\naccept-ch=\ncritical-ch=\n"
        "server=fixture\ncf-ray=\n"
        "response-header-count=2\nset-cookie-count=0\n"
        "response-header-0=content-type: font/ttf\n"
        "response-header-1=access-control-allow-origin: "
        "https://font-async.test\n",
        font_length,
        (unsigned long long) async_font_body_hash(font, font_length)) > 0;
    bool meta_closed = meta != NULL && fclose(meta) == 0;
    FILE *trace = meta_written && meta_closed
        ? fopen(trace_path, "wb") : NULL;
    bool trace_written = trace != NULL && fprintf(
        trace,
        "psp-http-trace-clock=1\n"
        "origin-ms=1000\n"
        "capture-complete=yes\n"
        "record-count=1\n") > 0;
    bool trace_closed = trace != NULL && fclose(trace) == 0;
    if (!trace_written || !trace_closed) {
        remove_async_font_replay(directory);
        return false;
    }
    return true;
}

static bool test_async_font_loader_is_fallback_first(void)
{
    static const char html[] =
        "<!doctype html><style>"
        "@font-face{font-family:AsyncFace;"
        "src:url(https://font-async.test/font.ttf) format(truetype)}"
        ".target{font-family:AsyncFace,sans-serif}"
        "</style><div class=target>fallback first</div>";
    unsigned char *font = NULL;
    size_t font_length = 0;
    CHECK(read_file(TILEFINCH_TEST_SOURCE_DIR
                    "/fonts/TilefinchSans-Regular.ttf",
                    &font, &font_length));
    char directory[128] = {0};
    CHECK(write_async_font_replay(font, font_length, directory));

    Budget budget;
    budget_init(&budget, 8u * MIB);
    CHECK(budget_install_lexbor(&budget));
    PocDocument document = {0};
    Stylesheet sheet = {0};
    CHECK(document_parse(&document, &budget, html, sizeof(html) - 1u, 17));
    CHECK(stylesheet_build(&sheet, &budget, &document, 480));
    FontFace *face = stylesheet_web_font_face(&sheet, 0, false);
    CHECK(face != NULL && !face->loaded);

    char error[256] = {0};
    CHECK(fetch_trace_replay_begin_response_keyed(
        directory, error, sizeof(error)));
    FetchScheduler *scheduler = fetch_scheduler_create(
        &budget, 1, 256u * KIB);
    CHECK(scheduler != NULL);
    ExternalFontLoader loader = {0};
    ExternalFontStats stats = {0};
    CHECK(fonts_external_loader_begin(&loader, &sheet, 1000, &stats));
    /* Discovery is CPU-only: beginning the page font pipeline neither
       reserves a response body nor starts network work. */
    CHECK(fonts_external_loader_pending(&loader)
          && fetch_scheduler_pending(scheduler) == 0
          && stats.attempted == 0 && !face->loaded);

    bool loaded = false;
    CHECK(fonts_external_loader_step(
        &loader, &sheet, &budget, "https://font-async.test/page",
        "https://font-async.test/page", "no-referrer", 1,
        256u * KIB, 256u * KIB, 1u * MIB, scheduler, NULL, NULL, &stats, 0,
        &loaded));
    CHECK(!loaded && !face->loaded && stats.attempted == 1
          && fetch_scheduler_pending(scheduler) == 1);
    for (size_t slice = 0;
         slice < 8 && fonts_external_loader_pending(&loader); slice++) {
        CHECK(fonts_external_loader_step(
            &loader, &sheet, &budget, "https://font-async.test/page",
            "https://font-async.test/page", "no-referrer", 1,
            256u * KIB, 256u * KIB, 1u * MIB, scheduler, NULL, NULL, &stats, 0,
            &loaded));
    }
#ifdef TILEFINCH_TEST_HAVE_FREETYPE
    if (!loaded || !face->loaded || stats.loaded_faces != 1
        || stats.failed != 0) {
        FetchTraceReplayStats replay = {0};
        (void) fetch_trace_replay_stats(&replay);
        fprintf(stderr,
                "async-font loaded=%d face=%d attempted=%zu loaded-faces=%zu "
                "failed=%zu unsupported=%zu pending=%d scheduler=%zu "
                "replay matched=%zu rejected=%zu unmatched=%zu invalid=%zu\n",
                loaded, face->loaded, stats.attempted, stats.loaded_faces,
                stats.failed, stats.unsupported,
                fonts_external_loader_pending(&loader),
                fetch_scheduler_pending(scheduler),
                replay.matched_request_count, replay.rejected_request_count,
                replay.unmatched_request_count,
                replay.invalid_route_request_count);
    }
    CHECK(loaded && face->loaded && stats.loaded_faces == 1
          && stats.failed == 0);
#else
    CHECK(!loaded && !face->loaded && stats.loaded_faces == 0
          && stats.failed == 1 && stats.unsupported == 1);
#endif
    CHECK(!fonts_external_loader_pending(&loader)
          && fetch_scheduler_pending(scheduler) == 0);

    font_face_destroy(face);
    ExternalFontLoader cancelled_loader = {0};
    ExternalFontStats cancelled_stats = {0};
    CHECK(fonts_external_loader_begin(
        &cancelled_loader, &sheet, 1000, &cancelled_stats));
    loaded = false;
    CHECK(fonts_external_loader_step(
        &cancelled_loader, &sheet, &budget,
        "https://font-async.test/page",
        "https://font-async.test/page", "no-referrer", 1,
        256u * KIB, 256u * KIB, 1u * MIB, scheduler, NULL, NULL,
        &cancelled_stats, 0, &loaded));
    CHECK(fonts_external_loader_pending(&cancelled_loader)
          && cancelled_stats.attempted == 1 && !loaded);
    fonts_external_loader_cancel(
        &cancelled_loader, scheduler, &cancelled_stats);
    CHECK(!fonts_external_loader_pending(&cancelled_loader)
          && fetch_scheduler_pending(scheduler) == 0
          && cancelled_stats.deadline_cancelled == 0);

    fetch_scheduler_destroy(scheduler);
    fetch_trace_end();
    stylesheet_destroy(&sheet);
    document_destroy(&document);
    CHECK(budget.current == 0);
    CHECK(budget_uninstall_lexbor(&budget));
    remove_async_font_replay(directory);
    free(font);
    return true;
}

static bool test_blocked_font_settles_without_backpressure_retry(void)
{
    static const char html[] =
        "<!doctype html><style>"
        "@font-face{font-family:BlockedFace;"
        "src:url(https://doubleclick.net/font.ttf) format(truetype)}"
        ".target{font-family:BlockedFace,sans-serif}"
        "</style><div class=target>fallback</div>";
    static const char document_url[] = "https://publisher.example/page";
    Budget budget;
    budget_init(&budget, 4u * MIB);
    CHECK(budget_install_lexbor(&budget));
    BrowserSession session = {0};
    CHECK(browser_session_init(&session, &budget, 256u * KIB));
    ContentBlocker *blocker = content_blocker_create(&budget);
    CHECK(blocker != NULL
          && content_blocker_configure(
                 blocker, CONTENT_BLOCKER_BASIC, NULL));
    session.content_blocker = blocker;
    PocDocument document = {0};
    Stylesheet sheet = {0};
    CHECK(document_parse(&document, &budget, html, sizeof(html) - 1u, 17));
    CHECK(stylesheet_build(&sheet, &budget, &document, 480));
    FetchScheduler *scheduler = fetch_scheduler_create(
        &budget, 1, 256u * KIB);
    CHECK(scheduler != NULL);
    ExternalFontLoader loader = {0};
    ExternalFontStats stats = {0};
    bool loaded = false;
    CHECK(fonts_external_loader_begin(&loader, &sheet, 1000, &stats));
    CHECK(fonts_external_loader_step(
        &loader, &sheet, &budget, document_url, document_url,
        "no-referrer", 1, 256u * KIB, 256u * KIB, 1u * MIB,
        scheduler, &session, NULL, &stats, 0, &loaded));
    ContentBlockerMetrics metrics = {0};
    CHECK(!loaded && !fonts_external_loader_pending(&loader)
          && fetch_scheduler_pending(scheduler) == 0
          && stats.sources_discovered == 1 && stats.attempted == 1
          && stats.failed == 1 && stats.loaded_faces == 0
          && content_blocker_metrics(blocker, &metrics)
          && metrics.requests_blocked == 1);
    fetch_scheduler_destroy(scheduler);
    stylesheet_destroy(&sheet);
    document_destroy(&document);
    session.content_blocker = NULL;
    content_blocker_destroy(blocker);
    browser_session_destroy(&session);
    CHECK(budget.current == 0);
    CHECK(budget_uninstall_lexbor(&budget));
    return true;
}

static bool rejected_without_retention(Budget *budget,
                                       const unsigned char *data,
                                       size_t length,
                                       size_t maximum_backend_bytes)
{
    size_t baseline = budget->current;
    FontFace face = {0};
    bool rejected = !font_face_load_encoded(
        &face, budget, data, length, maximum_backend_bytes);
    bool empty = !face.loaded && face.data == NULL
                 && face.implementation == NULL
                 && face.backend == FONT_FACE_BACKEND_NONE;
    font_face_destroy(&face);
    return rejected && empty && budget->current == baseline;
}

static bool rejected_woff_prefix(Budget *budget, unsigned char *woff,
                                 size_t woff_length, size_t prefix_length,
                                 size_t maximum_backend_bytes)
{
    if (prefix_length >= woff_length || prefix_length > UINT32_MAX) {
        return false;
    }
    uint32_t declared = read_be32(woff + 8);
    if (prefix_length >= 44) {
        store_be32(woff + 8, (uint32_t) prefix_length);
    }
    bool rejected = rejected_without_retention(
        budget, woff, prefix_length, maximum_backend_bytes);
    store_be32(woff + 8, declared);
    return rejected;
}

static bool test_woff_truncation_boundaries(Budget *budget,
                                            unsigned char *woff,
                                            size_t woff_length,
                                            size_t maximum_backend_bytes)
{
    CHECK(woff_length >= 44 && read_be32(woff) == UINT32_C(0x774f4646));
    uint16_t table_count = read_be16(woff + 12);
    CHECK(table_count != 0
          && 44u + (size_t) table_count * 20u < woff_length);
    for (size_t prefix = 0; prefix < 44; prefix++) {
        CHECK(rejected_woff_prefix(budget, woff, woff_length, prefix,
                                   maximum_backend_bytes));
    }
    /* Both sides of every directory-record boundary, including the first
       byte of table storage after the complete directory. */
    for (size_t entry = 0; entry <= table_count; entry++) {
        size_t boundary = 44u + entry * 20u;
        if (boundary != 0 && boundary - 1u < woff_length) {
            CHECK(rejected_woff_prefix(
                budget, woff, woff_length, boundary - 1u,
                maximum_backend_bytes));
        }
        if (boundary < woff_length) {
            CHECK(rejected_woff_prefix(
                budget, woff, woff_length, boundary,
                maximum_backend_bytes));
        }
    }
    /* Give FreeType a truthful shortened container length at the start and
       just before the end of each table, not merely a stale full-size WOFF
       header that the outer loader can reject by itself. */
    for (size_t table = 0; table < table_count; table++) {
        const unsigned char *record = woff + 44u + table * 20u;
        size_t offset = read_be32(record + 4);
        size_t compressed_length = read_be32(record + 8);
        CHECK(offset < woff_length
              && compressed_length <= woff_length - offset);
        size_t boundaries[3] = {
            offset,
            offset + (compressed_length != 0),
            offset + compressed_length - (compressed_length != 0)
        };
        for (size_t candidate = 0; candidate < 3; candidate++) {
            size_t prefix = boundaries[candidate];
            if (prefix >= 44 && prefix < woff_length) {
                CHECK(rejected_woff_prefix(
                    budget, woff, woff_length, prefix,
                    maximum_backend_bytes));
            }
        }
    }
    CHECK(rejected_woff_prefix(budget, woff, woff_length, woff_length - 1u,
                               maximum_backend_bytes));
    return true;
}

#ifdef TILEFINCH_TEST_HAVE_FREETYPE
typedef struct {
    int width;
    int metric_height;
    int line_height;
    int ascent;
    int glyph_width;
    int glyph_height;
    int glyph_advance;
    uint64_t glyph_hash;
} FontProbe;

static bool probe_web_font(Budget *budget, const unsigned char *encoded,
                           size_t encoded_length,
                           size_t maximum_backend_bytes, FontProbe *probe)
{
    FontFace face = {0};
    FontGlyph glyph = {0};
    FontProbe measured = {0};
    bool loaded = font_face_load_encoded(
        &face, budget, encoded, encoded_length, maximum_backend_bytes);
    bool ok = loaded && face.loaded
        && face.backend == FONT_FACE_BACKEND_FREETYPE_WEB;
    if (ok) {
        measured.width = font_text_width(
            &face, "Tilefinch AV", 12, 17, false);
        measured.metric_height = font_metric_height(&face, 17);
        measured.line_height = font_line_height(&face, 17);
        measured.ascent = font_ascent(&face, 17);
        ok = measured.width > 0 && measured.metric_height > 0
             && measured.line_height >= measured.metric_height
             && measured.ascent > 0
             && font_glyph_load(&face, 'A', 17, false, &glyph)
             && glyph.pixels != NULL && glyph.budget == budget
             && glyph.width > 0 && glyph.height > 0;
    }
    if (ok) {
        measured.glyph_width = glyph.width;
        measured.glyph_height = glyph.height;
        measured.glyph_advance = glyph.advance_fixed;
        measured.glyph_hash = UINT64_C(1469598103934665603);
        size_t pixels = (size_t) glyph.width * (size_t) glyph.height;
        for (size_t i = 0; i < pixels; i++) {
            measured.glyph_hash ^= glyph.pixels[i];
            measured.glyph_hash *= UINT64_C(1099511628211);
        }
    }
    font_glyph_destroy(&face, &glyph);
    font_face_destroy(&face);
    if (ok && probe != NULL) *probe = measured;
    return ok;
}

static bool font_probes_equal(const FontProbe *left, const FontProbe *right)
{
    return left->width == right->width
        && left->metric_height == right->metric_height
        && left->line_height == right->line_height
        && left->ascent == right->ascent
        && left->glyph_width == right->glyph_width
        && left->glyph_height == right->glyph_height
        && left->glyph_advance == right->glyph_advance
        && left->glyph_hash == right->glyph_hash;
}

static bool sweep_font_allocation_failures(const unsigned char *encoded,
                                           size_t encoded_length,
                                           size_t maximum_backend_bytes)
{
    Budget clean;
    budget_init(&clean, 4u * MIB);
    size_t allocations_before = clean.allocation_count;
    FontProbe probe = {0};
    CHECK(probe_web_font(&clean, encoded, encoded_length,
                         maximum_backend_bytes, &probe));
    size_t successful_allocations = clean.allocation_count
                                    - allocations_before;
    size_t largest = 0;
    CHECK(clean.current == 0
          && budget_active_allocations(&clean, &largest) == 0
          && successful_allocations != 0);

    size_t sweep_limit = successful_allocations + 32u;
    if (sweep_limit > 256u) sweep_limit = 256u;
    bool reached_uninjected_success = false;
    for (size_t failure_at = 0; failure_at <= sweep_limit; failure_at++) {
        Budget fault;
        budget_init(&fault, 4u * MIB);
        budget_inject_failure_after(&fault, failure_at);
        FontProbe ignored = {0};
        bool completed = probe_web_font(
            &fault, encoded, encoded_length, maximum_backend_bytes, &ignored);
        bool injected = fault.injected_failure_count != 0;
        budget_clear_failure_injection(&fault);
        largest = 0;
        CHECK(fault.current == 0
              && budget_active_allocations(&fault, &largest) == 0);
        if (!injected) {
            CHECK(completed);
            reached_uninjected_success = true;
            break;
        }
    }
    CHECK(reached_uninjected_success);
    return true;
}
#endif

static bool test_bounded_font_loader(void)
{
    unsigned char *bytes = NULL;
    size_t length = 0;
    CHECK(read_file(TILEFINCH_TEST_SOURCE_DIR "/fonts/TilefinchSans-Regular.ttf",
                    &bytes, &length));
    CHECK(length > 64 && bytes[0] == 0 && bytes[1] == 1
          && bytes[2] == 0 && bytes[3] == 0);
    unsigned char *woff = NULL;
    size_t woff_length = 0;
    CHECK(build_uncompressed_woff(bytes, length, &woff, &woff_length));
    CHECK(woff != NULL && woff_length >= 44
          && read_be32(woff) == UINT32_C(0x774f4646)
          && read_be32(woff + 4) == UINT32_C(0x00010000)
          && read_be32(woff + 8) == woff_length
          && read_be32(woff + 16) <= 1u * MIB);
    Budget budget;
    budget_init(&budget, 4u * MIB);
    const size_t maximum_backend_bytes = 1u * MIB;

    for (size_t truncated = 0; truncated < 64; truncated++) {
        CHECK(rejected_without_retention(
            &budget, bytes, truncated, maximum_backend_bytes));
    }
    CHECK(rejected_without_retention(&budget, bytes, length, length - 1u));
    CHECK(test_woff_truncation_boundaries(
        &budget, woff, woff_length, maximum_backend_bytes));

    unsigned char malformed[64] = {0};
    memcpy(malformed, "wOFF", 4);
    store_be32(malformed + 4, UINT32_C(0x00010000));
    store_be32(malformed + 8, 44);
    store_be32(malformed + 16, 64);
    CHECK(rejected_without_retention(&budget, malformed, 44,
                                     maximum_backend_bytes));
    store_be32(malformed + 8, 45);
    CHECK(rejected_without_retention(&budget, malformed, 44,
                                     maximum_backend_bytes));
    store_be32(malformed + 8, 44);
    store_be32(malformed + 16, (uint32_t) maximum_backend_bytes + 1u);
    CHECK(rejected_without_retention(&budget, malformed, 44,
                                     maximum_backend_bytes));
    memcpy(malformed, "wOF2", 4);
    CHECK(rejected_without_retention(&budget, malformed, sizeof(malformed),
                                     maximum_backend_bytes));
    memcpy(malformed, "OTTO", 4);
    CHECK(rejected_without_retention(&budget, malformed, sizeof(malformed),
                                     maximum_backend_bytes));
    memcpy(malformed, "ttcf", 4);
    CHECK(rejected_without_retention(&budget, malformed, sizeof(malformed),
                                     maximum_backend_bytes));
    store_be32(malformed, UINT32_C(0x00010000));
    CHECK(rejected_without_retention(&budget, malformed, sizeof(malformed),
                                     maximum_backend_bytes));

    budget_inject_failure_after(&budget, 0);
    CHECK(rejected_without_retention(
        &budget, bytes, length, maximum_backend_bytes));
    budget_clear_failure_injection(&budget);

    size_t baseline = budget.current;
    FontFace face = {0};
#ifdef TILEFINCH_TEST_HAVE_FREETYPE
    CHECK(font_face_load_encoded(&face, &budget, bytes, length,
                                 maximum_backend_bytes));
    CHECK(face.loaded && face.backend == FONT_FACE_BACKEND_FREETYPE_WEB);
    CHECK(face.data != NULL && face.data != bytes
          && face.data_length == length && face.implementation != NULL);
    unsigned char *retained_data = face.data;
    void *retained_implementation = face.implementation;
    CHECK(!font_face_load_encoded(&face, &budget, bytes, length,
                                  maximum_backend_bytes));
    CHECK(face.data == retained_data
          && face.implementation == retained_implementation);
    CHECK(font_text_width(&face, "Tilefinch", 9, 16, false) > 0
          && font_metric_height(&face, 19) == 22
          && font_line_height(&face, 19) == 23
          && font_line_baseline(&face, 40, 43)
                 == font_ascent(&face, 40) - 2);
    FontGlyph glyph = {0};
    CHECK(font_glyph_load(&face, 'A', 16, false, &glyph));
    CHECK(glyph.pixels != NULL && glyph.budget == &budget
          && glyph.width > 0 && glyph.height > 0);
    font_glyph_destroy(&face, &glyph);
    font_face_destroy(&face);
    font_face_destroy(&face);
    CHECK(budget.current == baseline);
    FontProbe raw_probe = {0}, woff_probe = {0};
    CHECK(probe_web_font(&budget, bytes, length, maximum_backend_bytes,
                         &raw_probe));
    CHECK(probe_web_font(&budget, woff, woff_length, maximum_backend_bytes,
                         &woff_probe));
    CHECK(font_probes_equal(&raw_probe, &woff_probe));

    unsigned char *fractional_metric_bytes = NULL;
    size_t fractional_metric_length = 0;
    FontFace fractional_metric_face = {0};
    CHECK(read_file(TILEFINCH_TEST_SOURCE_DIR
                    "/fonts/DejaVuSans-Oblique-Latin.ttf",
                    &fractional_metric_bytes, &fractional_metric_length)
          && font_face_load_encoded(
              &fractional_metric_face, &budget, fractional_metric_bytes,
              fractional_metric_length, maximum_backend_bytes)
          /* The 2384-unit hhea box at 19px is 22.1171875px.  CSS layout
             needs the scalable ceiling (23), not FreeType's grid-fitted
             nearest-pixel size metric (22). */
          && font_metric_height(&fractional_metric_face, 19) == 23);
    font_face_destroy(&fractional_metric_face);
    free(fractional_metric_bytes);
    CHECK(budget.current == baseline);

    CHECK(sweep_font_allocation_failures(
        bytes, length, maximum_backend_bytes));
    CHECK(sweep_font_allocation_failures(
        woff, woff_length, maximum_backend_bytes));
#else
    CHECK(!font_face_load_encoded(&face, &budget, bytes, length,
                                  maximum_backend_bytes));
    CHECK(!font_face_load_encoded(&face, &budget, woff, woff_length,
                                  maximum_backend_bytes));
#endif
    font_face_destroy(&face);
    font_face_destroy(&face);
    CHECK(budget.current == baseline);
    free(woff);
    free(bytes);
    return true;
}

static bool test_font_request_destination(void)
{
    CHECK(TILEFINCH_DESTINATION_OTHER == 6);
    CHECK(TILEFINCH_DESTINATION_FONT == 7);
    TilefinchRequestContext request = {
        .target_url = "https://fonts.example/font.woff",
        .initiator_url = "https://document.example/page",
        .top_level_url = "https://document.example/page",
        .method = "GET",
        .mode = TILEFINCH_REQUEST_MODE_CORS,
        .credentials = TILEFINCH_CREDENTIALS_SAME_ORIGIN,
        .destination = TILEFINCH_DESTINATION_FONT
    };
    CHECK(tilefinch_request_context_valid(&request));
    CHECK(strcmp(tilefinch_request_fetch_destination(&request), "font") == 0);
    CHECK(strcmp(tilefinch_request_fetch_mode(&request), "cors") == 0);
    CHECK(strcmp(tilefinch_request_fetch_site(&request), "cross-site") == 0);
    CHECK(!tilefinch_request_sends_credentials(&request));
    request.target_url = "https://document.example/font.woff";
    CHECK(tilefinch_request_same_origin(&request));
    CHECK(tilefinch_request_sends_credentials(&request));
    CHECK(strcmp(tilefinch_request_fetch_destination(&request), "font") == 0);
    return true;
}

static bool test_cache_response_url_provenance(void)
{
    static const char request_url[] = "https://document.example/font.woff";
    static const unsigned char payload[] = {0, 1, 2, 3};
    Budget budget;
    budget_init(&budget, 512u * KIB);
    BrowserSession session;
    CHECK(browser_session_init(&session, &budget, 64u * KIB));
    CHECK(browser_session_cache_put_http(
        &session, request_url, payload, sizeof(payload), NULL, NULL,
        "font/woff", "max-age=60", NULL, 100));
    const BrowserCacheEntry *entry = browser_session_cache_lookup(
        &session, request_url);
    CHECK(entry != NULL && !entry->response_url_known
          && entry->response_url == NULL
          && strcmp(browser_cache_entry_response_url(entry), request_url)
                 == 0);
    CHECK(!browser_session_cache_set_response_url(
        &session, request_url, "data:font/woff;base64,AAAA"));
    entry = browser_session_cache_lookup(&session, request_url);
    CHECK(entry != NULL && !entry->response_url_known);

    /* Even a no-redirect response must affirmatively mark the provenance;
       the request-key fallback alone is not CORS evidence. */
    CHECK(browser_session_cache_set_response_url(
        &session, request_url, request_url));
    entry = browser_session_cache_lookup(&session, request_url);
    CHECK(entry != NULL && entry->response_url_known
          && entry->response_url == NULL
          && strcmp(browser_cache_entry_response_url(entry), request_url)
                 == 0);

    char redirected[] = "https://static.document.example/fonts/final.woff";
    CHECK(browser_session_cache_set_response_url(
        &session, request_url, redirected));
    redirected[8] = 'X';
    entry = browser_session_cache_lookup(&session, request_url);
    CHECK(entry != NULL && entry->response_url_known
          && entry->response_url != NULL
          && strcmp(browser_cache_entry_response_url(entry),
                    "https://static.document.example/fonts/final.woff") == 0);
    CHECK(browser_session_cache_revalidate(
        &session, request_url, "max-age=120", NULL, 200));
    entry = browser_session_cache_lookup(&session, request_url);
    CHECK(entry != NULL && entry->response_url_known
          && strcmp(browser_cache_entry_response_url(entry),
                    "https://static.document.example/fonts/final.woff") == 0);

    browser_session_destroy(&session);
    CHECK(budget.current == 0);
    return true;
}

static bool test_external_fonts_bypass_generic_cache(void)
{
    static const char html[] =
        "<!doctype html><style>"
        "@font-face{font-family:CachedFace;"
        "src:url(https://document.example/cached.ttf) format(truetype)}"
        ".target{font-family:CachedFace,sans-serif}"
        "</style><div class=target>cached</div>";
    static const char document_url[] = "https://document.example/page";
    static const char font_url[] = "https://document.example/cached.ttf";
    unsigned char *bytes = NULL;
    size_t length = 0;
    CHECK(read_file(TILEFINCH_TEST_SOURCE_DIR "/fonts/TilefinchSans-Regular.ttf",
                    &bytes, &length));

    Budget budget;
    budget_init(&budget, 8u * MIB);
    CHECK(budget_install_lexbor(&budget));
    BrowserSession session;
    CHECK(browser_session_init(&session, &budget, 512u * KIB));
    uint64_t now = tilefinch_platform_monotonic_time_ns();
    CHECK(browser_session_cache_put_http(
        &session, font_url, bytes, length, NULL, NULL, "font/ttf",
        "max-age=60", NULL, now));
    CHECK(browser_session_cache_set_response_url(
        &session, font_url, font_url));
    size_t cache_hits = session.cache_hits;
    size_t cache_misses = session.cache_misses;

    PocDocument document = {0};
    Stylesheet sheet = {0};
    CHECK(document_parse(&document, &budget, html, sizeof(html) - 1u, 7));
    CHECK(stylesheet_build(&sheet, &budget, &document, 480));
    CHECK(stylesheet_web_font_source_count(&sheet) == 1);

    /* If fonts consulted the generic cache, this valid cached TTF would load
       and the injected transport failure would remain unconsumed. */
    fetch_inject_failure_once(FETCH_INJECT_TLS);
    ExternalFontStats stats = {0};
    CHECK(fonts_load_external(
        &sheet, &budget, document_url, document_url, "no-referrer",
        1, 256u * KIB, 128u * KIB, 1u * MIB, 1000, NULL, &session, NULL,
        &stats));
    fetch_inject_failure_once(FETCH_INJECT_NONE);
    CHECK(stats.sources_discovered == 1 && stats.attempted == 1
          && stats.loaded_faces == 0 && stats.failed == 1
          && stats.cache_hits == 0 && stats.encoded_bytes == 0);
    CHECK(session.cache_hits == cache_hits
          && session.cache_misses == cache_misses);
    FontFace *face = stylesheet_web_font_face(&sheet, 0, false);
    CHECK(face != NULL && !face->loaded && face->data == NULL
          && face->implementation == NULL);

    stylesheet_destroy(&sheet);
    document_destroy(&document);
    browser_session_destroy(&session);
    free(bytes);
    CHECK(budget.current == 0);
    CHECK(budget_uninstall_lexbor(&budget));
    return true;
}

int main(void)
{
    if (!test_bounded_font_loader()
        || !test_async_font_loader_is_fallback_first()
        || !test_blocked_font_settles_without_backpressure_retry()
        || !test_inline_discovery_and_canonicalization()
        || !test_single_pass_forward_reference_reinterns()
        || !test_external_base_and_source_order()
        || !test_external_referrer_policy_identity()
        || !test_font_request_destination()
        || !test_cache_response_url_provenance()
        || !test_external_fonts_bypass_generic_cache()) return 1;
    puts("web-font-tests status=PASS");
    return 0;
}
