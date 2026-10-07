/* libFuzzer target: HTTP response-header consumers.

   The input is a raw response header block. Each "name: value" line is
   normalized exactly the way the libcurl header callback in
   src/fetch/response_stream.inc (receive_header) stores it into the bounded
   response snapshot, and is then fed to everything that parses header text:
   security metadata (CORS/CORP/nosniff/HSTS/Referrer-Policy), CSP and
   frame-ancestors/X-Frame-Options, Set-Cookie and cookie dates, HSTS,
   Content-Range, Accept-CH/Critical-CH, SRI integrity metadata, request
   header validation, and the WebSocket close/text payload validators. */
#include <ctype.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "tilefinch/budget.h"
#include "tilefinch/content_security_policy.h"
#include "tilefinch/document.h"
#include "tilefinch/fetch.h"
#include "tilefinch/media_http.h"
#include "tilefinch/resource_integrity.h"
#include "tilefinch/session.h"
#include "tilefinch/url.h"

#define MIB (1024u * 1024u)

static const char response_url[] = "https://www.site.test/dir/page.html";

static const char inline_page[] =
    "<script nonce=abc>alert(1)</script>"
    "<style nonce='r4nd0m'>p{color:red}</style><p>x</p>";

static bool name_is(const char *name, size_t length, const char *wanted)
{
    return strlen(wanted) == length && strncasecmp(name, wanted, length) == 0;
}

static lxb_dom_node_t *first_element(lxb_dom_node_t *node, const char *tag)
{
    for (; node != NULL; node = node->next) {
        size_t length = 0;
        const char *name = document_element_name(node, &length);
        if (name != NULL && name_is(name, length, tag)) return node;
        lxb_dom_node_t *found = first_element(node->first_child, tag);
        if (found != NULL) return found;
    }
    return NULL;
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    if (size > 64 * 1024) return 0;
    Budget budget;
    budget_init(&budget, 24u * MIB);
    if (!budget_install_lexbor(&budget)) abort();

    char *snapshot = calloc(1, FETCH_RESPONSE_HEADERS_LIMIT);
    char accept_ch[1024] = "", critical_ch[1024] = "";
    size_t snapshot_length = 0;
    FetchResponseSecurityMetadata metadata;
    fetch_response_security_metadata_reset(&metadata);
    BrowserSession session;
    memset(&session, 0, sizeof(session));
    bool session_ok = browser_session_init(&session, &budget, 64 * 1024);
    char scratch[TILEFINCH_URL_SERIALIZED_LIMIT];

    size_t offset = 0;
    while (offset < size) {
        const uint8_t *newline = memchr(data + offset, '\n', size - offset);
        size_t end = newline == NULL ? size : (size_t) (newline - data);
        const char *line = (const char *) data + offset;
        size_t length = end - offset;
        offset = end + 1;
        /* libcurl rejects header lines carrying NUL bytes. */
        if (length == 0 || memchr(line, '\0', length) != NULL) continue;
        const char *colon = memchr(line, ':', length);
        if (colon == NULL) continue;
        size_t name_length = (size_t) (colon - line);
        const char *value = colon + 1;
        size_t value_length = length - name_length - 1;
        while (value_length != 0 && (*value == ' ' || *value == '\t')) {
            value++;
            value_length--;
        }
        while (value_length != 0
               && isspace((unsigned char) value[value_length - 1])) {
            value_length--;
        }
        bool forbidden = name_is(line, name_length, "set-cookie")
            || name_is(line, name_length, "set-cookie2");
        size_t required = name_length + 2 + value_length + 1;
        bool stored = false;
        if (!forbidden && name_length != 0
            && required < FETCH_RESPONSE_HEADERS_LIMIT - snapshot_length) {
            for (size_t i = 0; i < name_length; i++)
                snapshot[snapshot_length++] =
                    (char) tolower((unsigned char) line[i]);
            snapshot[snapshot_length++] = ':';
            snapshot[snapshot_length++] = ' ';
            memcpy(snapshot + snapshot_length, value, value_length);
            snapshot_length += value_length;
            snapshot[snapshot_length++] = '\n';
            snapshot[snapshot_length] = '\0';
            stored = true;
        }
        (void) fetch_response_security_metadata_collect(
            &metadata, line, name_length, value, value_length, stored);

        char *text = malloc(value_length + 1);
        if (text == NULL) continue;
        memcpy(text, value, value_length);
        text[value_length] = '\0';
        int64_t epoch = 0;
        (void) browser_cookie_parse_date(text, value_length, &epoch);
        if (name_is(line, name_length, "set-cookie") && session_ok
            && value_length < FETCH_SET_COOKIE_LIMIT) {
            (void) browser_session_cookie_set_http(&session, response_url,
                                                   text);
            (void) browser_session_cookie_set(&session,
                                              "http://site.test/", text);
        } else if (name_is(line, name_length, "content-range")) {
            uint64_t complete = 0;
            (void) media_http_parse_content_range(text, 0, 99, 100,
                                                  &complete);
            (void) media_http_parse_content_range(
                text, 1000, UINT64_MAX - 1, 4096, &complete);
        } else if (name_is(line, name_length, "accept-ch")) {
            size_t used = strlen(accept_ch);
            if (used + value_length + 3 < sizeof(accept_ch))
                snprintf(accept_ch + used, sizeof(accept_ch) - used, "%s%s",
                         used == 0 ? "" : ", ", text);
        } else if (name_is(line, name_length, "critical-ch")) {
            size_t used = strlen(critical_ch);
            if (used + value_length + 3 < sizeof(critical_ch))
                snprintf(critical_ch + used, sizeof(critical_ch) - used,
                         "%s%s", used == 0 ? "" : ", ", text);
        } else if (name_is(line, name_length, "integrity")) {
            (void) tilefinch_resource_integrity_verify(
                text, value_length, data, size);
            (void) tilefinch_resource_integrity_verify(
                text, value_length, (const uint8_t *) "", 0);
        } else if (name_is(line, name_length, "location")) {
            (void) tilefinch_url_resolve(response_url, text, scratch,
                                         sizeof(scratch));
        } else if (name_is(line, name_length, "request")) {
            FetchRequest request = {
                .method = "GET",
                .extra_headers = text,
                .content_type = text,
                .referer = text,
                .accept = text,
                .user_agent = text
            };
            FetchRequestValidationError error = FETCH_REQUEST_VALIDATION_OK;
            (void) fetch_request_validate(&request, &error);
        }
        free(text);
    }

    FetchResponseSecurityMetadata from_snapshot;
    fetch_response_security_metadata_reset(&from_snapshot);
    (void) fetch_response_security_metadata_from_snapshot(
        &from_snapshot, snapshot, snapshot_length, false);

    char hints[1024];
    (void) fetch_accepted_critical_client_hints(accept_ch, critical_ch, hints,
                                                sizeof(hints));
    (void) fetch_client_hint_tokens_cover(accept_ch, critical_ch);

    TilefinchContentSecurityPolicy *policy = malloc(sizeof(*policy));
    if (policy != NULL) {
        tilefinch_csp_init(policy);
        (void) tilefinch_csp_parse_response_headers(
            policy, response_url, snapshot, snapshot_length, false);
        static const char *const targets[] = {
            "https://www.site.test/a.js", "https://cdn.other.test/x/y.css",
            "http://www.site.test/img.png", "https://site.test:8443/",
            "https://a.b.site.test/w.js"
        };
        for (size_t i = 0; i < sizeof(targets) / sizeof(targets[0]); i++) {
            for (int destination = TILEFINCH_DESTINATION_DOCUMENT;
                 destination <= TILEFINCH_DESTINATION_WORKER; destination++)
                (void) tilefinch_csp_allows_request(
                    policy, (TilefinchRequestDestination) destination,
                    targets[i]);
            (void) tilefinch_csp_allows_base_uri(policy, targets[i]);
            (void) tilefinch_csp_allows_form_action(policy, targets[i]);
            (void) tilefinch_csp_allows_worker(policy, targets[i]);
            (void) tilefinch_csp_allows_ancestor(policy, targets[i]);
            (void) tilefinch_frame_embedding_allowed(
                policy, response_url, targets[i], snapshot, snapshot_length);
        }
        (void) tilefinch_csp_allows_script_attribute(policy);
        (void) tilefinch_csp_allows_style_attribute(policy);
        (void) tilefinch_csp_allows_dynamic_code(policy);
        (void) tilefinch_csp_has_frame_ancestors(policy);
        PocDocument document = {0};
        if (document_parse(&document, &budget, inline_page,
                           sizeof(inline_page) - 1u, 4096)) {
            lxb_dom_node_t *root = lxb_dom_interface_node(document.html);
            (void) tilefinch_csp_allows_inline_script(
                policy, first_element(root, "script"));
            (void) tilefinch_csp_allows_inline_style(
                policy, first_element(root, "style"));
            document_destroy(&document);
        }
        free(policy);
    }

    if (session_ok) {
        (void) browser_session_hsts_observe(&session, response_url, snapshot,
                                            snapshot_length);
        (void) browser_session_hsts_observe_metadata(&session, response_url,
                                                     &metadata);
        (void) browser_session_hsts_upgrade_url(
            &session, "http://www.site.test/x", scratch, sizeof(scratch));
        (void) browser_session_hsts_upgrade_url(
            &session, "http://a.www.site.test:80/", scratch, sizeof(scratch));
        char cookies[4096];
        (void) browser_session_cookie_header(&session, response_url, cookies,
                                             sizeof(cookies));
        (void) browser_session_cookie_get(&session, "https://site.test/dir/",
                                          cookies, sizeof(cookies));
        browser_session_destroy(&session);
    }

    /* WebSocket control/text payload validators see the raw bytes. */
    FetchWebSocketClosePayload close_payload;
    (void) fetch_websocket_close_payload_parse(
        data, size > 125 ? 125 : size, &close_payload);
    FetchWebSocketCloseAccumulator accumulator;
    memset(&accumulator, 0, sizeof(accumulator));
    size_t total = size > 125 ? 125 : size;
    for (size_t at = 0; at < total;) {
        size_t chunk = 1 + (data[at] % 7);
        if (chunk > total - at) chunk = total - at;
        FetchWebSocketClosePayloadStatus status =
            fetch_websocket_close_payload_accumulate(
                &accumulator, data + at, chunk, total - at - chunk,
                &close_payload);
        at += chunk;
        if (status != FETCH_WEBSOCKET_CLOSE_PAYLOAD_INCOMPLETE) break;
    }
    (void) fetch_websocket_text_payload_valid(data, size);

    free(snapshot);
    if (budget.current != 0) {
        fprintf(stderr, "fuzz_http: budget leak of %zu bytes\n",
                budget.current);
        budget_dump_active(&budget, stderr, 16);
        abort();
    }
    (void) budget_uninstall_lexbor(&budget);
    return 0;
}
