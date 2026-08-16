/* Page-visible networking: fetch/XHR bindings, the CORS request/response
   engine, response cookie/header recording, and the dynamic-script network
   machinery (task lifecycle, caches, admission, ready execution).  Split
   from js_runtime.c; shares the runtime internals through
   js_runtime_internal.h. */
#include "js_runtime_internal.h"

#include "tilefinch/content_blocker.h"
#include "tilefinch/platform.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>

static void copy_response_header_value(const FetchResult *fetched,
                                       const char *wanted, char *output,
                                       size_t capacity)
{
    if (fetched == NULL || wanted == NULL || output == NULL || capacity == 0) {
        return;
    }
    output[0] = '\0';
    size_t wanted_length = strlen(wanted);
    size_t offset = 0;
    while (offset < fetched->response_headers_length) {
        const char *line = fetched->response_headers + offset;
        const char *end = memchr(line, '\n',
                                 fetched->response_headers_length - offset);
        size_t length = end == NULL
            ? fetched->response_headers_length - offset
            : (size_t) (end - line);
        const char *colon = memchr(line, ':', length);
        if (colon != NULL && (size_t) (colon - line) == wanted_length
            && strncasecmp(line, wanted, wanted_length) == 0) {
            const char *value = colon + 1;
            const char *limit = line + length;
            while (value < limit && isspace((unsigned char) *value)) value++;
            while (limit > value
                   && isspace((unsigned char) limit[-1])) limit--;
            size_t value_length = (size_t) (limit - value);
            if (value_length >= capacity) value_length = capacity - 1;
            memcpy(output, value, value_length);
            output[value_length] = '\0';
            return;
        }
        offset += length + (end == NULL ? 0 : 1);
    }
}

void js_rt_record_network_response(ScriptResult *result,
                             const FetchResult *fetched)
{
    if (result == NULL || fetched == NULL) return;
    result->last_network_status = fetched->status_code;
    snprintf(result->last_network_url, sizeof(result->last_network_url), "%s",
             fetched->effective_url);
    result->last_network_response_bytes = fetched->length;
    unsigned long long hash = UINT64_C(14695981039346656037);
    for (size_t i = 0; i < fetched->length; i++) {
        hash ^= (unsigned char) fetched->data[i];
        hash *= UINT64_C(1099511628211);
    }
    result->last_network_body_hash = hash;
    snprintf(result->last_network_content_type,
             sizeof(result->last_network_content_type), "%s",
             fetched->content_type);
    copy_response_header_value(fetched, "content-encoding",
                               result->last_network_content_encoding,
                               sizeof(result->last_network_content_encoding));
    snprintf(result->last_network_server,
             sizeof(result->last_network_server), "%s", fetched->server);
    snprintf(result->last_network_cf_ray,
             sizeof(result->last_network_cf_ray), "%s", fetched->cf_ray);
    snprintf(result->last_network_cf_mitigated,
             sizeof(result->last_network_cf_mitigated), "%s",
             fetched->cf_mitigated);
    size_t shown = fetched->length < 16 ? fetched->length : 16;
    size_t at = 0;
    for (size_t i = 0;
         i < shown && at + 2 < sizeof(result->last_network_body_prefix); i++) {
        int written = snprintf(result->last_network_body_prefix + at,
                               sizeof(result->last_network_body_prefix) - at,
                               "%02x", (unsigned char) fetched->data[i]);
        if (written != 2) break;
        at += 2;
    }
    result->last_network_body_prefix[at] = '\0';
}

static bool response_content_is_textual(const char *content_type)
{
    if (content_type == NULL) return false;
    size_t length = strcspn(content_type, ";");
    while (length > 0
           && isspace((unsigned char) content_type[length - 1])) length--;
    if (length >= 5 && strncasecmp(content_type, "text/", 5) == 0) {
        return true;
    }
    static const char *const textual_types[] = {
        "application/json", "application/xml",
        "application/javascript", "application/ecmascript",
        "application/x-javascript", "application/x-www-form-urlencoded"
    };
    for (size_t i = 0;
         i < sizeof(textual_types) / sizeof(textual_types[0]); i++) {
        if (strlen(textual_types[i]) == length
            && strncasecmp(content_type, textual_types[i], length) == 0) {
            return true;
        }
    }
    return (length >= 5
            && strncasecmp(content_type + length - 5, "+json", 5) == 0)
        || (length >= 4
            && strncasecmp(content_type + length - 4, "+xml", 4) == 0);
}

/* Keep exactly one eager JavaScript representation of a native response.
   Textual payloads enter QuickJS as a string; all other payloads enter as an
   ArrayBuffer and are decoded only when Response.text/json or textual XHR
   semantics actually require it. */
void js_rt_script_set_response_body(JSContext *context, JSValue response,
                              const FetchResult *fetched)
{
    if (context == NULL || fetched == NULL || JS_IsException(response)) {
        return;
    }
    (void) JS_SetPropertyStr(context, response, "bodyLength",
                            JS_NewInt64(context, (int64_t) fetched->length));
    if (response_content_is_textual(fetched->content_type)) {
        (void) JS_SetPropertyStr(context, response, "body",
            JS_NewStringLen(context, fetched->data, fetched->length));
    } else {
        (void) JS_SetPropertyStr(context, response, "bodyBytes",
            JS_NewArrayBufferCopy(context,
                                  (const uint8_t *) fetched->data,
                                  fetched->length));
    }
}

/* Modern SPA API payloads such as custom emoji inventories and trending
   timelines run to several megabytes.  The per-response cap scales with the
   configured script file budget so constrained profiles keep their
   small reservations while SPA-sized budgets admit real payloads. */
#define JS_FETCH_MAXIMUM_BYTES (512u * 1024u)
static size_t js_fetch_response_limit(const DomBridge *bridge)
{
    size_t limit = JS_FETCH_MAXIMUM_BYTES;
    if (bridge != NULL && bridge->maximum_script_file_bytes > limit) {
        limit = bridge->maximum_script_file_bytes;
        if (limit > 8u * 1024u * 1024u) limit = 8u * 1024u * 1024u;
    }
    return limit;
}
#define JS_FETCH_MAXIMUM_BODY (64u * 1024u)
#define JS_FETCH_MAXIMUM_HEADERS (8u * 1024u)

typedef struct {
    TilefinchRequestContext context;
    const TilefinchContentSecurityPolicy *content_security_policy;
    const char *referrer_policy;
} ScriptRequestPolicy;

static bool script_request_policy_prepare(
    DomBridge *bridge, const char *target_url, const char *method,
    TilefinchRequestMode mode, TilefinchCredentialsMode credentials,
    TilefinchRequestDestination destination, bool allow_cross_origin,
    ScriptRequestPolicy *policy)
{
    if (bridge == NULL || target_url == NULL || policy == NULL) return false;
    memset(policy, 0, sizeof(*policy));
    policy->context = (TilefinchRequestContext) {
        .target_url = target_url,
        .initiator_url = bridge->document_url,
        .top_level_url = bridge->top_level_url,
        .method = method,
        .mode = mode,
        .credentials = credentials,
        .destination = destination,
        .initiator_opaque = bridge->opaque_origin
    };
    /* An opaque origin is never same-origin with its committed URL. Fetch's
       default SAME_ORIGIN credentials mode therefore becomes an actual OMIT
       request; explicit INCLUDE remains available subject to CORS/SameSite. */
    if (policy->context.initiator_opaque
        && policy->context.credentials
               == TILEFINCH_CREDENTIALS_SAME_ORIGIN) {
        policy->context.credentials = TILEFINCH_CREDENTIALS_OMIT;
    }
    policy->referrer_policy = bridge->referrer_policy;
    policy->content_security_policy =
        &bridge->document->content_security_policy;
    if (!tilefinch_request_context_valid(&policy->context)
        || (!allow_cross_origin
            && !tilefinch_request_same_origin(&policy->context))) {
        return false;
    }
    return true;
}

static const FetchRequest *script_request_policy_apply(
    const ScriptRequestPolicy *policy, const BrowserSession *session,
    const FetchRequest *transport, FetchPreparedPageRequest *prepared)
{
    if (policy == NULL || prepared == NULL
        || !fetch_prepare_page_request_context(
            &policy->context, policy->context.initiator_url,
            policy->referrer_policy, session,
            policy->content_security_policy, NULL, transport,
            prepared, NULL)) return NULL;
    return fetch_prepared_page_request(prepared);
}

void js_rt_script_store_response_cookies(
DomBridge *bridge, const FetchResult *fetched, const char *fallback_url,
TilefinchRequestMode mode, TilefinchCredentialsMode credentials,
TilefinchRequestDestination destination)
{
    if (bridge == NULL || bridge->session == NULL || fetched == NULL
        || credentials == TILEFINCH_CREDENTIALS_OMIT) return;
    for (size_t i = 0; i < fetched->set_cookie_count; i++) {
        const char *cookie_url = fetch_set_cookie_url(
            fetched, i, fallback_url);
        TilefinchRequestContext response = {
            .target_url = cookie_url,
            .initiator_url = bridge->document_url,
            .top_level_url = bridge->top_level_url,
            .method = "GET",
            .mode = mode,
            .credentials = credentials,
            .destination = destination,
            .initiator_opaque = bridge->opaque_origin
        };
        (void) browser_session_cookie_set_http_context(
            bridge->session, &response, fetched->set_cookies[i]);
    }
}

#define SCRIPT_CORS_HEADER_NAME_COUNT 32
#define SCRIPT_CORS_HEADER_NAME_LENGTH 64

typedef struct {
    bool required;
    bool method_requires_permission;
    char header_names[SCRIPT_CORS_HEADER_NAME_COUNT]
                     [SCRIPT_CORS_HEADER_NAME_LENGTH];
    size_t header_count;
    char serialized_header_names[1024];
} ScriptCorsPreflight;

static bool script_cors_method_safelisted(const char *method)
{
    return method != NULL
        && (strcasecmp(method, "GET") == 0
            || strcasecmp(method, "HEAD") == 0
            || strcasecmp(method, "POST") == 0);
}

static bool script_cors_unsafe_request_header_byte(unsigned char value)
{
    return (value < 0x20 && value != '\t') || value == 0x7f
        || strchr("\"():<>?@[\\]{}", value) != NULL;
}

static bool script_cors_language_value_safelisted(const char *value,
                                                  size_t length)
{
    if (length > 128) return false;
    for (size_t i = 0; i < length; i++) {
        unsigned char byte = (unsigned char) value[i];
        if (!isalnum(byte) && strchr(" *,-.;=", byte) == NULL) return false;
    }
    return true;
}

static bool script_cors_content_type_safelisted(const char *value)
{
    if (value == NULL || value[0] == '\0') return true;
    size_t length = strlen(value);
    if (length > 128) return false;
    for (size_t i = 0; i < length; i++) {
        if (script_cors_unsafe_request_header_byte(
                (unsigned char) value[i])) return false;
    }
    const char *start = value;
    const char *end = strchr(value, ';');
    if (end == NULL) end = value + length;
    while (start < end && isspace((unsigned char) *start)) start++;
    while (end > start && isspace((unsigned char) end[-1])) end--;
    static const char *const allowed[] = {
        "application/x-www-form-urlencoded", "multipart/form-data",
        "text/plain"
    };
    size_t essence_length = (size_t) (end - start);
    for (size_t i = 0; i < sizeof(allowed) / sizeof(allowed[0]); i++) {
        if (strlen(allowed[i]) == essence_length
            && strncasecmp(start, allowed[i], essence_length) == 0) {
            return true;
        }
    }
    return false;
}

static bool script_cors_range_value_safelisted(const char *value,
                                                size_t length)
{
    if (length > 128 || length < 8
        || strncasecmp(value, "bytes=", 6) != 0) return false;
    const char *at = value + 6;
    const char *end = value + length;
    bool before = false, after = false;
    while (at < end && isdigit((unsigned char) *at)) {
        before = true;
        at++;
    }
    if (at == end || *at++ != '-') return false;
    while (at < end && isdigit((unsigned char) *at)) {
        after = true;
        at++;
    }
    return at == end && (before || after);
}

static bool script_cors_add_header_name(
    ScriptCorsPreflight *preflight, const char *name, size_t length)
{
    if (preflight == NULL || name == NULL || length == 0
        || length >= SCRIPT_CORS_HEADER_NAME_LENGTH) return false;
    char normalized[SCRIPT_CORS_HEADER_NAME_LENGTH];
    for (size_t i = 0; i < length; i++) {
        normalized[i] = (char) tolower((unsigned char) name[i]);
    }
    normalized[length] = '\0';
    for (size_t i = 0; i < preflight->header_count; i++) {
        if (strcmp(preflight->header_names[i], normalized) == 0) return true;
    }
    if (preflight->header_count >= SCRIPT_CORS_HEADER_NAME_COUNT) return false;
    memcpy(preflight->header_names[preflight->header_count++], normalized,
           length + 1);
    return true;
}

static int script_cors_header_name_compare(const void *left,
                                           const void *right)
{
    return strcmp((const char *) left, (const char *) right);
}

static bool script_cors_analyze_request(
    const char *method, const char *content_type, const char *extra_headers,
    ScriptCorsPreflight *preflight)
{
    if (method == NULL || preflight == NULL) return false;
    memset(preflight, 0, sizeof(*preflight));
    if (!script_cors_method_safelisted(method)) {
        preflight->required = true;
        preflight->method_requires_permission = true;
    }
    if (!script_cors_content_type_safelisted(content_type)) {
        preflight->required = true;
        if (!script_cors_add_header_name(
                preflight, "content-type", strlen("content-type"))) {
            return false;
        }
    }
    for (const char *at = extra_headers; at != NULL && *at != '\0';) {
        const char *end = strchr(at, '\n');
        if (end == NULL) end = at + strlen(at);
        const char *colon = memchr(at, ':', (size_t) (end - at));
        if (colon == NULL || colon == at) return false;
        const char *value = colon + 1;
        const char *limit = end;
        while (value < limit && (*value == ' ' || *value == '\t')) value++;
        while (limit > value
               && (limit[-1] == ' ' || limit[-1] == '\t')) limit--;
        size_t name_length = (size_t) (colon - at);
        size_t value_length = (size_t) (limit - value);
        bool safelisted = false;
        if (name_length == strlen("accept-language")
            && strncasecmp(at, "accept-language", name_length) == 0) {
            safelisted = script_cors_language_value_safelisted(
                value, value_length);
        } else if (name_length == strlen("content-language")
                   && strncasecmp(
                          at, "content-language", name_length) == 0) {
            safelisted = script_cors_language_value_safelisted(
                value, value_length);
        } else if (name_length == strlen("range")
                   && strncasecmp(at, "range", name_length) == 0) {
            safelisted = script_cors_range_value_safelisted(
                value, value_length);
        }
        if (!safelisted) {
            preflight->required = true;
            if (!script_cors_add_header_name(
                    preflight, at, name_length)) return false;
        }
        at = *end == '\0' ? end : end + 1;
    }
    qsort(preflight->header_names, preflight->header_count,
          sizeof(preflight->header_names[0]),
          script_cors_header_name_compare);
    size_t used = 0;
    for (size_t i = 0; i < preflight->header_count; i++) {
        size_t length = strlen(preflight->header_names[i]);
        size_t separator = i == 0 ? 0 : 2;
        if (separator + length
            >= sizeof(preflight->serialized_header_names) - used) {
            return false;
        }
        if (separator != 0) {
            preflight->serialized_header_names[used++] = ',';
            preflight->serialized_header_names[used++] = ' ';
        }
        memcpy(preflight->serialized_header_names + used,
               preflight->header_names[i], length);
        used += length;
    }
    preflight->serialized_header_names[used] = '\0';
    return true;
}

static bool script_response_header_token_contains(
    const FetchResult *fetched, const char *header, const char *wanted,
    bool case_insensitive, bool *wildcard)
{
    if (wildcard != NULL) *wildcard = false;
    if (fetched == NULL || header == NULL || wanted == NULL) return false;
    size_t header_length = strlen(header), wanted_length = strlen(wanted);
    size_t offset = 0;
    while (offset < fetched->response_headers_length) {
        const char *line = fetched->response_headers + offset;
        const char *newline = memchr(
            line, '\n', fetched->response_headers_length - offset);
        size_t length = newline == NULL
            ? fetched->response_headers_length - offset
            : (size_t) (newline - line);
        const char *colon = memchr(line, ':', length);
        if (colon != NULL && (size_t) (colon - line) == header_length
            && strncasecmp(line, header, header_length) == 0) {
            const char *at = colon + 1;
            const char *end = line + length;
            while (at < end) {
                while (at < end
                       && (*at == ' ' || *at == '\t' || *at == ',')) at++;
                const char *start = at;
                while (at < end && *at != ',') at++;
                const char *limit = at;
                while (limit > start
                       && (limit[-1] == ' ' || limit[-1] == '\t'
                           || limit[-1] == '\r')) limit--;
                size_t token_length = (size_t) (limit - start);
                if (token_length == 1 && start[0] == '*'
                    && wildcard != NULL) *wildcard = true;
                if (token_length == wanted_length
                    && (case_insensitive
                          ? strncasecmp(start, wanted, wanted_length) == 0
                          : strncmp(start, wanted, wanted_length) == 0)) {
                    return true;
                }
            }
        }
        offset += length + (newline == NULL ? 0 : 1);
    }
    return false;
}

static bool script_cors_preflight_request(
    DomBridge *bridge, const char *url, const char *method,
    TilefinchCredentialsMode credentials,
    const ScriptCorsPreflight *preflight)
{
    if (bridge == NULL || url == NULL || method == NULL
        || preflight == NULL || !preflight->required) return true;
    ScriptRequestPolicy policy;
    if (!script_request_policy_prepare(
            bridge, url, "OPTIONS", TILEFINCH_REQUEST_MODE_CORS,
            TILEFINCH_CREDENTIALS_SAME_ORIGIN, TILEFINCH_DESTINATION_FETCH, true,
            &policy)) return false;
    char headers[1400];
    int written = snprintf(
        headers, sizeof(headers), "Access-Control-Request-Method: %s%s%s",
        method, preflight->header_count == 0 ? "" : "\nAccess-Control-Request-Headers: ",
        preflight->header_count == 0
          ? "" : preflight->serialized_header_names);
    if (written <= 0 || (size_t) written >= sizeof(headers)) return false;
    FetchRequest request = {
        .method = "OPTIONS", .extra_headers = headers,
        .cors_preflight = true, .allow_http_errors = true,
        .send_low_client_hints = true, .accept = "*/*",
        .redirect_same_origin_only = true
    };
    FetchPreparedPageRequest prepared;
    const FetchRequest *authorized = script_request_policy_apply(
        &policy, bridge->session, &request, &prepared);
    if (authorized == NULL) return false;
    request = *authorized;
    FetchRequestValidationError validation = FETCH_REQUEST_VALIDATION_OK;
    if (!fetch_request_validate(&request, &validation)) return false;
    FetchResult *fetched = fetch_result_create(bridge->document->budget);
    if (fetched == NULL) return false;
    if (bridge->result != NULL) bridge->result->network_requests++;
    bool ok = fetch_request_cancelable(
        bridge->document->budget, url, &request, 64u * 1024u,
        bridge->fetch_timeout_ms < 5000 ? 5000
                                       : (long) bridge->fetch_timeout_ms,
        NULL, NULL, fetched);
    bool credentials_included = credentials == TILEFINCH_CREDENTIALS_INCLUDE;
    bool allowed = ok && fetched->status_code >= 200
        && fetched->status_code < 300
        && fetched->effective_url[0] != '\0'
        && tilefinch_url_same_origin(url, fetched->effective_url)
        && fetch_cors_response_allows(
               fetched, request.origin, credentials_included);
    bool wildcard = false;
    if (allowed && preflight->method_requires_permission) {
        allowed = script_response_header_token_contains(
            fetched, "access-control-allow-methods", method, false,
            &wildcard)
            || (wildcard && !credentials_included);
    }
    for (size_t i = 0; allowed && i < preflight->header_count; i++) {
        wildcard = false;
        bool listed = script_response_header_token_contains(
            fetched, "access-control-allow-headers",
            preflight->header_names[i], true, &wildcard);
        allowed = listed || (wildcard && !credentials_included
                             && strcasecmp(
                                    preflight->header_names[i],
                                    "authorization") != 0);
    }
    if (!allowed && bridge->result != NULL) {
        bridge->result->network_failures++;
    }
    fetch_result_free(fetched);
    return allowed;
}

bool js_rt_script_response_origin_allowed(
const DomBridge *bridge, const FetchResult *fetched,
const char *request_url_or_origin,
TilefinchRequestDestination destination, TilefinchRequestMode mode,
TilefinchCredentialsMode credentials)
{
    if (destination != TILEFINCH_DESTINATION_FETCH
        && destination != TILEFINCH_DESTINATION_SCRIPT) return true;
    if (bridge == NULL || bridge->document_url == NULL || fetched == NULL) {
        return false;
    }
    const char *response_url = fetched->effective_url[0] == '\0'
        ? request_url_or_origin : fetched->effective_url;
    char target_origin[TILEFINCH_ORIGIN_SERIALIZED_LIMIT];
    char response_origin[TILEFINCH_ORIGIN_SERIALIZED_LIMIT];
    if (response_url == NULL || request_url_or_origin == NULL
        || !tilefinch_url_origin(request_url_or_origin, target_origin,
                              sizeof(target_origin))
        || !tilefinch_url_origin(response_url, response_origin,
                              sizeof(response_origin))
        || strcmp(target_origin, response_origin) != 0) return false;
    if (!bridge->opaque_origin
        && tilefinch_url_same_origin(bridge->document_url, response_url)) {
        return true;
    }
    if (mode != TILEFINCH_REQUEST_MODE_CORS) return false;
    char document_origin[TILEFINCH_ORIGIN_SERIALIZED_LIMIT];
    if (bridge->opaque_origin) {
        memcpy(document_origin, "null", sizeof("null"));
    } else if (!tilefinch_url_origin(bridge->document_url, document_origin,
                                    sizeof(document_origin))) {
        return false;
    }
    return fetch_cors_response_allows(
        fetched, document_origin,
        credentials == TILEFINCH_CREDENTIALS_INCLUDE);
}

static bool script_cors_response_header_safelisted(const char *name,
                                                    size_t length)
{
    static const char *const safelisted[] = {
        "cache-control", "content-language", "content-length",
        "content-type", "expires", "last-modified", "pragma"
    };
    for (size_t i = 0;
         i < sizeof(safelisted) / sizeof(safelisted[0]); i++) {
        if (strlen(safelisted[i]) == length
            && strncasecmp(name, safelisted[i], length) == 0) return true;
    }
    return false;
}

size_t js_rt_script_visible_response_headers(
const DomBridge *bridge, const FetchResult *fetched,
TilefinchCredentialsMode credentials, char *output, size_t capacity)
{
    if (output == NULL || capacity == 0 || fetched == NULL) return 0;
    output[0] = '\0';
    const char *response_url = fetched->effective_url;
    bool same_origin = bridge != NULL && !bridge->opaque_origin
        && bridge->document_url != NULL
        && response_url[0] != '\0'
        && tilefinch_url_same_origin(bridge->document_url, response_url);
    if (same_origin) {
        size_t length = fetched->response_headers_length;
        if (length >= capacity) length = capacity - 1;
        memcpy(output, fetched->response_headers, length);
        output[length] = '\0';
        return length;
    }
    size_t used = 0, offset = 0;
    while (offset < fetched->response_headers_length) {
        const char *line = fetched->response_headers + offset;
        const char *newline = memchr(
            line, '\n', fetched->response_headers_length - offset);
        size_t length = newline == NULL
            ? fetched->response_headers_length - offset
            : (size_t) (newline - line);
        const char *colon = memchr(line, ':', length);
        bool exposed = false;
        if (colon != NULL) {
            size_t name_length = (size_t) (colon - line);
            exposed = script_cors_response_header_safelisted(
                line, name_length);
            if (!exposed && name_length < 128) {
                char name[128];
                memcpy(name, line, name_length);
                name[name_length] = '\0';
                bool wildcard = false;
                exposed = script_response_header_token_contains(
                    fetched, "access-control-expose-headers", name, true,
                    &wildcard);
                if (!exposed && wildcard
                    && credentials != TILEFINCH_CREDENTIALS_INCLUDE) {
                    exposed = true;
                }
            }
        }
        if (exposed) {
            size_t copied = length + (newline == NULL ? 0 : 1);
            if (copied >= capacity - used) break;
            memcpy(output + used, line, length);
            used += length;
            if (newline != NULL) output[used++] = '\n';
        }
        offset += length + (newline == NULL ? 0 : 1);
    }
    output[used] = '\0';
    return used;
}

static bool script_parse_request_policy(
    JSContext *context, int argc, JSValueConst *argv, int mode_index,
    int credentials_index, TilefinchRequestMode *mode,
    TilefinchCredentialsMode *credentials)
{
    if (mode == NULL || credentials == NULL) return false;
    *mode = TILEFINCH_REQUEST_MODE_CORS;
    *credentials = TILEFINCH_CREDENTIALS_SAME_ORIGIN;
    const char *mode_name = NULL;
    const char *credentials_name = NULL;
    if (argc > mode_index && !JS_IsUndefined(argv[mode_index])) {
        mode_name = JS_ToCString(context, argv[mode_index]);
        if (mode_name == NULL) return false;
        if (strcmp(mode_name, "same-origin") == 0) {
            *mode = TILEFINCH_REQUEST_MODE_SAME_ORIGIN;
        } else if (strcmp(mode_name, "cors") == 0) {
            *mode = TILEFINCH_REQUEST_MODE_CORS;
        } else if (strcmp(mode_name, "no-cors") == 0) {
            *mode = TILEFINCH_REQUEST_MODE_NO_CORS;
        } else {
            JS_FreeCString(context, mode_name);
            return false;
        }
        JS_FreeCString(context, mode_name);
    }
    if (argc > credentials_index
        && !JS_IsUndefined(argv[credentials_index])) {
        credentials_name = JS_ToCString(context, argv[credentials_index]);
        if (credentials_name == NULL) return false;
        if (strcmp(credentials_name, "omit") == 0) {
            *credentials = TILEFINCH_CREDENTIALS_OMIT;
        } else if (strcmp(credentials_name, "same-origin") == 0) {
            *credentials = TILEFINCH_CREDENTIALS_SAME_ORIGIN;
        } else if (strcmp(credentials_name, "include") == 0) {
            *credentials = TILEFINCH_CREDENTIALS_INCLUDE;
        } else {
            JS_FreeCString(context, credentials_name);
            return false;
        }
        JS_FreeCString(context, credentials_name);
    }
    return true;
}

static JSValue script_throw_request_validation(
    JSContext *context, FetchRequestValidationError error)
{
    const char *message = "Invalid request";
    if (error == FETCH_REQUEST_VALIDATION_METHOD) {
        message = "Invalid HTTP method";
    } else if (error == FETCH_REQUEST_VALIDATION_HEADER_VALUE
               || error == FETCH_REQUEST_VALIDATION_EXTRA_HEADERS) {
        message = "Invalid HTTP header name or value";
    } else if (error == FETCH_REQUEST_VALIDATION_BODY) {
        message = "Invalid request body";
    } else if (error == FETCH_REQUEST_VALIDATION_CREDENTIALS
               || error == FETCH_REQUEST_VALIDATION_CONTEXT) {
        message = "Invalid request security context";
    }
    return JS_ThrowTypeError(context, "%s", message);
}

static JSValue js_schedule_dynamic_script(JSContext *context,
                                          JSValueConst this_value,
                                          int argc, JSValueConst *argv)
{
    (void) this_value;
    DomBridge *bridge = JS_GetContextOpaque(context);
    lxb_dom_node_t *node = argc > 0
        ? js_rt_bridge_node_arg(context, bridge, argv[0]) : NULL;
    if (bridge == NULL || node == NULL) {
        return JS_ThrowTypeError(context,
                                 "dynamic script element required");
    }
    return JS_NewInt32(context, js_rt_dynamic_prepare_subtree(context, node));
}

static JSValue js_fetch_sync(JSContext *context, JSValueConst this_value,
                             int argc, JSValueConst *argv)
{
    (void) this_value;
    DomBridge *bridge = JS_GetContextOpaque(context);
    if (bridge == NULL || argc < 2) {
        return JS_ThrowTypeError(context, "fetch requires method and URL");
    }
    size_t method_length = 0, reference_length = 0;
    const char *method = JS_ToCStringLen(
        context, &method_length, argv[0]);
    const char *reference = JS_ToCStringLen(
        context, &reference_length, argv[1]);
    const char *body = NULL;
    size_t body_length = 0;
    bool body_is_cstring = false;
    if (argc > 2 && !JS_IsUndefined(argv[2])) {
        if (JS_IsString(argv[2])) {
            body = JS_ToCStringLen(context, &body_length, argv[2]);
            body_is_cstring = true;
        } else {
            body = (const char *) JS_GetArrayBuffer(context, &body_length,
                                                    argv[2]);
        }
    }
    size_t content_type_length = 0, extra_headers_length = 0;
    const char *content_type = argc > 3 && !JS_IsUndefined(argv[3])
        ? JS_ToCStringLen(context, &content_type_length, argv[3]) : NULL;
    const char *extra_headers = argc > 4 && !JS_IsUndefined(argv[4])
        ? JS_ToCStringLen(context, &extra_headers_length, argv[4]) : NULL;
    if (method == NULL || reference == NULL
        || (argc > 2 && !JS_IsUndefined(argv[2]) && body == NULL)
        || (argc > 3 && !JS_IsUndefined(argv[3]) && content_type == NULL)
        || (argc > 4 && !JS_IsUndefined(argv[4]) && extra_headers == NULL)) {
        if (method != NULL) JS_FreeCString(context, method);
        if (reference != NULL) JS_FreeCString(context, reference);
        if (body_is_cstring && body != NULL) JS_FreeCString(context, body);
        if (content_type != NULL) JS_FreeCString(context, content_type);
        if (extra_headers != NULL) JS_FreeCString(context, extra_headers);
        return JS_EXCEPTION;
    }
    TilefinchRequestMode request_mode;
    TilefinchCredentialsMode credentials;
    bool valid_policy = script_parse_request_policy(
        context, argc, argv, 5, 6, &request_mode, &credentials);
    bool method_exact = strlen(method) == method_length;
    bool reference_exact = strlen(reference) == reference_length;
    bool content_type_exact = content_type == NULL
        || strlen(content_type) == content_type_length;
    bool extra_headers_exact = extra_headers == NULL
        || strlen(extra_headers) == extra_headers_length;
    FetchRequest request = {
        .method = method, .body = body,
        .body_length = body_length,
        .content_type = content_type, .extra_headers = extra_headers,
        .allow_http_errors = true, .send_low_client_hints = true,
        .accept = "*/*"
    };
    FetchRequestValidationError validation = FETCH_REQUEST_VALIDATION_OK;
    bool request_valid = method_exact && content_type_exact
        && extra_headers_exact;
    if (!method_exact) validation = FETCH_REQUEST_VALIDATION_METHOD;
    else if (!content_type_exact) {
        validation = FETCH_REQUEST_VALIDATION_HEADER_VALUE;
    } else if (!extra_headers_exact) {
        validation = FETCH_REQUEST_VALIDATION_EXTRA_HEADERS;
    } else {
        request_valid = fetch_request_validate(&request, &validation);
    }
    char url[TILEFINCH_URL_SERIALIZED_LIMIT];
    bool valid = valid_policy && request_valid && reference_exact
                 && body_length <= JS_FETCH_MAXIMUM_BODY
                 && extra_headers_length <= JS_FETCH_MAXIMUM_HEADERS
                 && tilefinch_url_resolve(bridge->document_url, reference, url,
                                       sizeof(url));
    ScriptRequestPolicy policy;
    valid = valid && script_request_policy_prepare(
        bridge, url, method, request_mode, credentials,
        TILEFINCH_DESTINATION_FETCH,
        request_mode == TILEFINCH_REQUEST_MODE_CORS, &policy);
    if (!valid) {
        JS_FreeCString(context, method); JS_FreeCString(context, reference);
        if (body_is_cstring && body != NULL) JS_FreeCString(context, body);
        if (content_type != NULL) JS_FreeCString(context, content_type);
        if (extra_headers != NULL) JS_FreeCString(context, extra_headers);
        if (!request_valid) {
            return script_throw_request_validation(context, validation);
        }
        if (!reference_exact) {
            return JS_ThrowTypeError(context, "Invalid request URL");
        }
        return JS_ThrowTypeError(
            context, "request rejected by origin, context, or quota policy");
    }
    FetchPreparedPageRequest prepared;
    const FetchRequest *authorized = script_request_policy_apply(
        &policy, bridge->session, &request, &prepared);
    if (authorized != NULL) request = *authorized;
    request.redirect_same_origin_only = true;
    if (authorized == NULL || !fetch_request_validate(&request, &validation)) {
        JS_FreeCString(context, method); JS_FreeCString(context, reference);
        if (body_is_cstring && body != NULL) JS_FreeCString(context, body);
        if (content_type != NULL) JS_FreeCString(context, content_type);
        if (extra_headers != NULL) JS_FreeCString(context, extra_headers);
        return script_throw_request_validation(context, validation);
    }
    bool cross_origin = !tilefinch_request_same_origin(&policy.context);
    if (cross_origin && request_mode == TILEFINCH_REQUEST_MODE_CORS) {
        ScriptCorsPreflight preflight;
        bool preflight_ok = script_cors_analyze_request(
                method, content_type, extra_headers, &preflight)
            && script_cors_preflight_request(
                bridge, url, method, credentials, &preflight);
        if (!preflight_ok) {
            JS_FreeCString(context, method);
            JS_FreeCString(context, reference);
            if (body_is_cstring && body != NULL) {
                JS_FreeCString(context, body);
            }
            if (content_type != NULL) JS_FreeCString(context, content_type);
            if (extra_headers != NULL) JS_FreeCString(context, extra_headers);
            return JS_ThrowTypeError(context, "CORS preflight failed");
        }
    }
    if (getenv("TILEFINCH_TRACE_REQUEST_BODY") != NULL && body != NULL
        && body_length <= 256 && memchr(body, '\0', body_length) == NULL) {
        fprintf(stderr, "page-request-body method=%s url=%s bytes=%zu value=\"%s\"\n",
                method, url, body_length, body);
    }
    FetchResult *fetched = fetch_result_create(bridge->document->budget);
    if (fetched == NULL) {
        JS_FreeCString(context, method); JS_FreeCString(context, reference);
        if (body_is_cstring && body != NULL) JS_FreeCString(context, body);
        if (content_type != NULL) JS_FreeCString(context, content_type);
        if (extra_headers != NULL) JS_FreeCString(context, extra_headers);
        return JS_ThrowInternalError(context, "network result allocation failed");
    }
    if (bridge->result != NULL) bridge->result->network_requests++;
    bool ok = fetch_request_cancelable(bridge->result == NULL ? NULL
                                       : bridge->document->budget,
                                       url, &request,
                                       js_fetch_response_limit(bridge),
                                       bridge->fetch_timeout_ms < 5000
                                           ? 5000
                                           : (long) bridge->fetch_timeout_ms,
                                       NULL, NULL, fetched);
    JS_FreeCString(context, method); JS_FreeCString(context, reference);
    if (body_is_cstring && body != NULL) JS_FreeCString(context, body);
    if (content_type != NULL) JS_FreeCString(context, content_type);
    if (extra_headers != NULL) JS_FreeCString(context, extra_headers);
    bool response_allowed = ok && js_rt_script_response_origin_allowed(
        bridge, fetched, url, policy.context.destination,
        policy.context.mode, policy.context.credentials);
    if (!response_allowed) {
        if (bridge->result != NULL) bridge->result->network_failures++;
        JSValue error = JS_ThrowInternalError(
            context, "network request failed: %s",
            ok ? "CORS or cross-origin redirect response blocked"
               : fetched->error);
        fetch_result_free(fetched);
        return error;
    }
    if (bridge->result != NULL) js_rt_record_network_response(bridge->result,
                                                       fetched);
    js_rt_script_store_response_cookies(
        bridge, fetched,
        fetched->effective_url[0] == '\0' ? url : fetched->effective_url,
        policy.context.mode, policy.context.credentials,
        policy.context.destination);
    char visible_headers[FETCH_RESPONSE_HEADERS_LIMIT];
    size_t visible_headers_length = js_rt_script_visible_response_headers(
        bridge, fetched, policy.context.credentials, visible_headers,
        sizeof(visible_headers));
    JSValue response = JS_NewObject(context);
    if (!JS_IsException(response)) {
        (void) JS_SetPropertyStr(context, response, "status",
                                JS_NewInt64(context, fetched->status_code));
        (void) JS_SetPropertyStr(context, response, "url",
                                JS_NewString(context,
                                  fetched->effective_url[0] == '\0'
                                  ? url : fetched->effective_url));
        (void) JS_SetPropertyStr(context, response, "contentType",
                                JS_NewString(context, fetched->content_type));
        (void) JS_SetPropertyStr(context, response, "headers",
                                JS_NewStringLen(
                                    context, visible_headers,
                                    visible_headers_length));
        js_rt_script_set_response_body(context, response, fetched);
    }
    fetch_result_free(fetched);
    return response;
}

static JSValue js_fetch_async(JSContext *context, JSValueConst this_value,
                              int argc, JSValueConst *argv)
{
    (void) this_value;
    DomBridge *bridge = JS_GetContextOpaque(context);
    if (bridge == NULL || argc < 2) {
        return JS_ThrowTypeError(context, "fetch requires method and URL");
    }
    size_t method_length = 0, reference_length = 0;
    const char *method = JS_ToCStringLen(
        context, &method_length, argv[0]);
    const char *reference = JS_ToCStringLen(
        context, &reference_length, argv[1]);
    const char *body = NULL;
    size_t body_length = 0;
    bool body_is_cstring = false;
    if (argc > 2 && !JS_IsUndefined(argv[2])) {
        if (JS_IsString(argv[2])) {
            body = JS_ToCStringLen(context, &body_length, argv[2]);
            body_is_cstring = true;
        } else {
            body = (const char *) JS_GetArrayBuffer(context, &body_length,
                                                    argv[2]);
        }
    }
    size_t content_type_length = 0, extra_headers_length = 0;
    const char *content_type = argc > 3 && !JS_IsUndefined(argv[3])
        ? JS_ToCStringLen(context, &content_type_length, argv[3]) : NULL;
    const char *extra_headers = argc > 4 && !JS_IsUndefined(argv[4])
        ? JS_ToCStringLen(context, &extra_headers_length, argv[4]) : NULL;
    bool legacy_timeout = argc == 6 && !JS_IsUndefined(argv[5])
        && JS_IsNumber(argv[5]);
    TilefinchRequestMode request_mode = TILEFINCH_REQUEST_MODE_CORS;
    TilefinchCredentialsMode credentials = TILEFINCH_CREDENTIALS_SAME_ORIGIN;
    bool valid_policy = legacy_timeout || script_parse_request_policy(
        context, argc, argv, 5, 6, &request_mode, &credentials);
    int timeout_index = legacy_timeout ? 5 : 7;
    int64_t requested_timeout_ms = 0;
    bool valid_timeout = argc <= timeout_index
        || JS_IsUndefined(argv[timeout_index])
        || JS_ToInt64(context, &requested_timeout_ms,
                      argv[timeout_index]) == 0;
    if (method == NULL || reference == NULL
        || (argc > 2 && !JS_IsUndefined(argv[2]) && body == NULL)
        || (argc > 3 && !JS_IsUndefined(argv[3]) && content_type == NULL)
        || (argc > 4 && !JS_IsUndefined(argv[4])
            && extra_headers == NULL) || !valid_policy || !valid_timeout) {
        if (method != NULL) JS_FreeCString(context, method);
        if (reference != NULL) JS_FreeCString(context, reference);
        if (body_is_cstring && body != NULL) JS_FreeCString(context, body);
        if (content_type != NULL) JS_FreeCString(context, content_type);
        if (extra_headers != NULL) JS_FreeCString(context, extra_headers);
        return JS_EXCEPTION;
    }
    bool method_exact = strlen(method) == method_length;
    bool reference_exact = strlen(reference) == reference_length;
    bool content_type_exact = content_type == NULL
        || strlen(content_type) == content_type_length;
    bool extra_headers_exact = extra_headers == NULL
        || strlen(extra_headers) == extra_headers_length;
    FetchRequest request = {
        .method = method, .body = body,
        .body_length = body_length,
        .content_type = content_type, .extra_headers = extra_headers,
        .allow_http_errors = true,
        .send_low_client_hints = true,
        .accept = "*/*"
    };
    FetchRequestValidationError validation = FETCH_REQUEST_VALIDATION_OK;
    bool request_valid = method_exact && content_type_exact
        && extra_headers_exact;
    if (!method_exact) validation = FETCH_REQUEST_VALIDATION_METHOD;
    else if (!content_type_exact) {
        validation = FETCH_REQUEST_VALIDATION_HEADER_VALUE;
    } else if (!extra_headers_exact) {
        validation = FETCH_REQUEST_VALIDATION_EXTRA_HEADERS;
    } else {
        request_valid = fetch_request_validate(&request, &validation);
    }
    char url[TILEFINCH_URL_SERIALIZED_LIMIT];
    bool valid = request_valid && reference_exact
                 && body_length <= JS_FETCH_MAXIMUM_BODY
                 && extra_headers_length <= JS_FETCH_MAXIMUM_HEADERS
                 && tilefinch_url_resolve(bridge->document_url, reference, url,
                                       sizeof(url))
                 && bridge->async_fetch_count < 8;
    ScriptRequestPolicy policy;
    valid = valid && script_request_policy_prepare(
        bridge, url, method, request_mode, credentials,
        TILEFINCH_DESTINATION_FETCH,
        request_mode == TILEFINCH_REQUEST_MODE_CORS, &policy);
    FetchPreparedPageRequest prepared;
    if (valid) {
        const FetchRequest *authorized = script_request_policy_apply(
            &policy, bridge->session, &request, &prepared);
        if (authorized != NULL) request = *authorized;
        request.redirect_same_origin_only = true;
        request_valid = authorized != NULL
            && fetch_request_validate(&request, &validation);
        valid = request_valid;
    }
    char target_origin[TILEFINCH_ORIGIN_SERIALIZED_LIMIT] = {0};
    bool target_origin_valid = valid
        && tilefinch_url_origin(url, target_origin, sizeof(target_origin));
    bool cross_origin = target_origin_valid
        && !tilefinch_request_same_origin(&policy.context);
    if (valid && !target_origin_valid) valid = false;
    if (valid && cross_origin
        && request_mode == TILEFINCH_REQUEST_MODE_CORS) {
        ScriptCorsPreflight preflight;
        bool preflight_ok = script_cors_analyze_request(
                method, content_type, extra_headers, &preflight)
            && script_cors_preflight_request(
                bridge, url, method, credentials, &preflight);
        if (!preflight_ok) {
            JS_FreeCString(context, method);
            JS_FreeCString(context, reference);
            if (body_is_cstring && body != NULL) {
                JS_FreeCString(context, body);
            }
            if (content_type != NULL) JS_FreeCString(context, content_type);
            if (extra_headers != NULL) JS_FreeCString(context, extra_headers);
            return JS_ThrowTypeError(context, "CORS preflight failed");
        }
    }
    if (valid && bridge->fetch_scheduler == NULL) {
        bridge->fetch_scheduler = script_runtime_fetch_scheduler(bridge->host);
    }
    long scheduler_timeout_ms = bridge->fetch_timeout_ms < 5000
        ? 5000 : (long) bridge->fetch_timeout_ms;
    if (requested_timeout_ms > 0
        && requested_timeout_ms < scheduler_timeout_ms) {
        scheduler_timeout_ms = (long) requested_timeout_ms;
    }
    uint64_t id = valid && bridge->fetch_scheduler != NULL
        ? fetch_scheduler_enqueue(
            bridge->fetch_scheduler, url, &request,
            js_fetch_response_limit(bridge), scheduler_timeout_ms)
        : 0;
    JS_FreeCString(context, method);
    JS_FreeCString(context, reference);
    if (body_is_cstring && body != NULL) JS_FreeCString(context, body);
    if (content_type != NULL) JS_FreeCString(context, content_type);
    if (extra_headers != NULL) JS_FreeCString(context, extra_headers);
    if (id == 0) {
        if (!request_valid) {
            return script_throw_request_validation(context, validation);
        }
        if (!reference_exact) {
            return JS_ThrowTypeError(context, "Invalid request URL");
        }
        if (bridge->result != NULL) {
            bridge->result->async_network_quota_rejected++;
        }
        return JS_ThrowRangeError(
            context, "async request rejected by origin, context, or quota");
    }
    ScriptAsyncFetch *async_fetch =
        &bridge->async_fetches[bridge->async_fetch_count++];
    *async_fetch = (ScriptAsyncFetch) {
        .id = id, .mode = request_mode, .credentials = credentials
    };
    snprintf(async_fetch->target_origin, sizeof(async_fetch->target_origin),
             "%s", target_origin);
    if (bridge->result != NULL) {
        bridge->result->network_requests++;
        bridge->result->async_network_queued++;
        if (bridge->async_fetch_count
            > bridge->result->async_network_peak_inflight) {
            bridge->result->async_network_peak_inflight =
                bridge->async_fetch_count;
        }
    }
    return JS_NewInt64(context, (int64_t) id);
}

static bool script_event_source_headers(void *opaque,
                                        const FetchResult *metadata)
{
    ScriptEventSource *source = opaque;
    if (source == NULL || !source->active || metadata == NULL) return false;
    source->headers_valid = metadata->status_code == 200
        && strncasecmp(metadata->content_type, "text/event-stream",
                       strlen("text/event-stream")) == 0
        && js_rt_script_response_origin_allowed(
            source->bridge, metadata, source->target_origin,
            TILEFINCH_DESTINATION_FETCH, TILEFINCH_REQUEST_MODE_CORS,
            source->credentials);
    source->headers_pending = source->headers_valid;
    if (metadata->effective_url[0] != '\0') {
        (void) tilefinch_url_origin(
            metadata->effective_url, source->response_origin,
            sizeof(source->response_origin));
    }
    if (!source->headers_valid) {
        snprintf(source->error, sizeof(source->error),
                 "EventSource requires HTTP 200 text/event-stream");
    }
    return source->headers_valid;
}

static bool script_event_source_body(void *opaque,
                                     const unsigned char *data,
                                     size_t length)
{
    ScriptEventSource *source = opaque;
    if (source == NULL || !source->active || !source->headers_valid
        || data == NULL || length == 0) {
        return length == 0;
    }
    if (length > SCRIPT_EVENT_SOURCE_PENDING_BYTES - source->pending_length) {
        snprintf(source->error, sizeof(source->error),
                 "EventSource pending-byte quota exceeded");
        return false;
    }
    memcpy(source->pending + source->pending_length, data, length);
    source->pending_length += length;
    return true;
}

static void script_event_source_clear(DomBridge *bridge,
                                      ScriptEventSource *source,
                                      bool cancel)
{
    if (bridge == NULL || source == NULL || !source->active) return;
    if (cancel && bridge->fetch_scheduler != NULL && source->id != 0) {
        (void) fetch_scheduler_cancel(
            bridge->fetch_scheduler, source->id, "EventSource closed");
        (void) fetch_scheduler_discard(bridge->fetch_scheduler, source->id);
    }
    budget_free(bridge->budget, source->pending);
    memset(source, 0, sizeof(*source));
    if (bridge->event_source_count != 0) bridge->event_source_count--;
}

static JSValue js_event_source_start(JSContext *context,
                                     JSValueConst this_value,
                                     int argc, JSValueConst *argv)
{
    (void) this_value;
    DomBridge *bridge = JS_GetContextOpaque(context);
    if (bridge == NULL || argc < 1) {
        return JS_ThrowTypeError(context, "EventSource requires a URL");
    }
    size_t reference_length = 0, last_id_length = 0;
    const char *reference = JS_ToCStringLen(
        context, &reference_length, argv[0]);
    bool with_credentials = argc > 1 && JS_ToBool(context, argv[1]) > 0;
    const char *last_id = argc > 2 && !JS_IsUndefined(argv[2])
        ? JS_ToCStringLen(context, &last_id_length, argv[2]) : NULL;
    if (reference == NULL
        || (last_id != NULL && (last_id_length > 1024
            || memchr(last_id, '\r', last_id_length) != NULL
            || memchr(last_id, '\n', last_id_length) != NULL))) {
        if (reference != NULL) JS_FreeCString(context, reference);
        if (last_id != NULL) JS_FreeCString(context, last_id);
        return JS_ThrowTypeError(context, "Invalid EventSource request");
    }
    char url[TILEFINCH_URL_SERIALIZED_LIMIT],
         target_origin[TILEFINCH_ORIGIN_SERIALIZED_LIMIT],
         extra_headers[1100] = {0};
    bool valid = strlen(reference) == reference_length
        && tilefinch_url_resolve(
            bridge->document_url, reference, url, sizeof(url))
        && tilefinch_url_origin(
            url, target_origin, sizeof(target_origin))
        && bridge->event_source_count < SCRIPT_EVENT_SOURCE_LIMIT;
    if (valid && last_id != NULL && last_id_length != 0) {
        int written = snprintf(extra_headers, sizeof(extra_headers),
                               "Last-Event-ID: %s", last_id);
        valid = written > 0 && (size_t) written < sizeof(extra_headers);
    }
    ScriptEventSource *source = NULL;
    for (size_t i = 0; valid && i < SCRIPT_EVENT_SOURCE_LIMIT; i++) {
        if (!bridge->event_sources[i].active) {
            source = &bridge->event_sources[i];
            break;
        }
    }
    TilefinchCredentialsMode credentials = with_credentials
        ? TILEFINCH_CREDENTIALS_INCLUDE
        : TILEFINCH_CREDENTIALS_SAME_ORIGIN;
    ScriptRequestPolicy policy;
    valid = valid && source != NULL
        && script_request_policy_prepare(
            bridge, url, "GET", TILEFINCH_REQUEST_MODE_CORS, credentials,
            TILEFINCH_DESTINATION_FETCH, true, &policy);
    FetchRequest request = {
        .method = "GET",
        .accept = "text/event-stream",
        .extra_headers = extra_headers[0] == '\0' ? NULL : extra_headers,
        .allow_http_errors = true,
        .send_low_client_hints = true,
        /* The streaming header callback performs the CORS decision before
           exposing the first byte; transport-level buffered CORS would only
           run after a long-lived response completed. */
        .enforce_cors = false,
        .cors_response_check_deferred = true,
        .redirect_same_origin_only = true
    };
    FetchPreparedPageRequest prepared;
    if (valid) {
        const FetchRequest *authorized = script_request_policy_apply(
            &policy, bridge->session, &request, &prepared);
        if (authorized != NULL) request = *authorized;
        valid = authorized != NULL && fetch_request_validate(&request, NULL);
    }
    if (valid && bridge->fetch_scheduler == NULL) {
        bridge->fetch_scheduler = script_runtime_fetch_scheduler(bridge->host);
    }
    unsigned char *pending = valid
        ? budget_malloc(bridge->budget, SCRIPT_EVENT_SOURCE_PENDING_BYTES)
        : NULL;
    valid = valid && pending != NULL && bridge->fetch_scheduler != NULL;
    if (valid) {
        memset(source, 0, sizeof(*source));
        source->active = true;
        source->bridge = bridge;
        source->credentials = credentials;
        source->pending = pending;
        snprintf(source->target_origin, sizeof(source->target_origin),
                 "%s", target_origin);
        source->stream = (FetchStreamOptions) {
            .on_headers = script_event_source_headers,
            .on_body = script_event_source_body,
            .opaque = source
        };
        source->id = fetch_scheduler_enqueue_stream(
            bridge->fetch_scheduler, url, &request, 256u * 1024u,
            bridge->fetch_timeout_ms < 30000
                ? 30000 : (long) bridge->fetch_timeout_ms,
            &source->stream);
        valid = source->id != 0;
        if (valid) bridge->event_source_count++;
    }
    if (!valid) {
        if (source != NULL && source->active) {
            script_event_source_clear(bridge, source, true);
        } else {
            budget_free(bridge->budget, pending);
        }
    }
    JS_FreeCString(context, reference);
    if (last_id != NULL) JS_FreeCString(context, last_id);
    if (!valid) {
        return JS_ThrowRangeError(
            context, "EventSource rejected by origin, memory, or quota");
    }
    if (bridge->result != NULL) {
        bridge->result->network_requests++;
        bridge->result->async_network_queued++;
    }
    return JS_NewInt64(context, (int64_t) source->id);
}

static JSValue js_event_source_close(JSContext *context,
                                     JSValueConst this_value,
                                     int argc, JSValueConst *argv)
{
    (void) this_value;
    DomBridge *bridge = JS_GetContextOpaque(context);
    int64_t id = 0;
    if (bridge == NULL || argc < 1
        || JS_ToInt64(context, &id, argv[0]) < 0) {
        return JS_EXCEPTION;
    }
    for (size_t i = 0; i < SCRIPT_EVENT_SOURCE_LIMIT; i++) {
        ScriptEventSource *source = &bridge->event_sources[i];
        if (source->active && source->id == (uint64_t) id) {
            script_event_source_clear(bridge, source, true);
            return JS_TRUE;
        }
    }
    return JS_FALSE;
}

void js_rt_event_sources_destroy(DomBridge *bridge)
{
    if (bridge == NULL) return;
    for (size_t i = 0; i < SCRIPT_EVENT_SOURCE_LIMIT; i++) {
        script_event_source_clear(bridge, &bridge->event_sources[i], true);
    }
}

static bool script_call_global(ScriptRuntime *runtime, const char *name,
                               int argc, JSValueConst *argv)
{
    JSContext *context = runtime->context;
    JSValue global = JS_GetGlobalObject(context);
    if (JS_IsException(global)) {
        js_rt_record_exception(context, &runtime->result);
        return false;
    }
    JSValue function = JS_GetPropertyStr(context, global, name);
    if (JS_IsException(function) || !JS_IsFunction(context, function)) {
        JS_FreeValue(context, function);
        JS_FreeValue(context, global);
        js_rt_record_exception(context, &runtime->result);
        return false;
    }
    JSValue result = JS_Call(context, function, global, argc, argv);
    JS_FreeValue(context, function);
    JS_FreeValue(context, global);
    if (JS_IsException(result)) {
        js_rt_record_exception(context, &runtime->result);
        JS_FreeValue(context, result);
        return false;
    }
    JS_FreeValue(context, result);
    return true;
}

bool js_rt_event_sources_deliver(ScriptRuntime *runtime,
                                 size_t completion_budget,
                                 size_t *author_tasks)
{
    if (runtime == NULL || completion_budget == 0) return true;
    DomBridge *bridge = &runtime->bridge;
    for (size_t i = 0; i < SCRIPT_EVENT_SOURCE_LIMIT; i++) {
        ScriptEventSource *source = &bridge->event_sources[i];
        if (!source->active) continue;
        const char *kind = NULL, *text = "";
        size_t text_length = 0;
        if (source->headers_pending) {
            source->headers_pending = false;
            kind = "open";
        } else if (source->pending_length != 0) {
            kind = "chunk";
            text = (const char *) source->pending;
            text_length = source->pending_length;
        }
        if (kind != NULL) {
            JSValue arguments[4] = {
                JS_NewInt64(runtime->context, (int64_t) source->id),
                JS_NewString(runtime->context, kind),
                strcmp(kind, "chunk") == 0
                    ? JS_NewArrayBufferCopy(
                        runtime->context, (const uint8_t *) text, text_length)
                    : JS_NewStringLen(runtime->context, text, text_length),
                JS_NewString(runtime->context, source->response_origin)
            };
            bool delivered = script_call_global(
                runtime, "__tilefinchDeliverEventSource", 4,
                arguments);
            for (size_t value = 0; value < 4; value++) {
                JS_FreeValue(runtime->context, arguments[value]);
            }
            if (!delivered) return false;
            if (strcmp(kind, "chunk") == 0) source->pending_length = 0;
            if (author_tasks != NULL) (*author_tasks)++;
            return true;
        }
        bool success = false;
        FetchStreamMetrics metrics;
        FetchResult *fetched = fetch_result_create(bridge->budget);
        if (fetched == NULL) return false;
        if (!fetch_scheduler_take_stream(
                bridge->fetch_scheduler, source->id, &success,
                &metrics, fetched)) {
            fetch_result_free(fetched);
            continue;
        }
        if (success && !js_rt_script_response_origin_allowed(
                bridge, fetched, source->target_origin,
                TILEFINCH_DESTINATION_FETCH, TILEFINCH_REQUEST_MODE_CORS,
                source->credentials)) {
            success = false;
            snprintf(fetched->error, sizeof(fetched->error), "%s",
                     "EventSource CORS response blocked");
        }
        const char *error = source->error[0] != '\0'
            ? source->error
            : fetched->error;
        uint64_t id = source->id;
        JSValue arguments[4] = {
            JS_NewInt64(runtime->context, (int64_t) id),
            JS_NewString(runtime->context, "finish"),
            JS_NewString(runtime->context, success ? "" : error),
            JS_NewString(runtime->context, source->response_origin)
        };
        bool delivered = script_call_global(
            runtime, "__tilefinchDeliverEventSource", 4, arguments);
        for (size_t value = 0; value < 4; value++) {
            JS_FreeValue(runtime->context, arguments[value]);
        }
        fetch_result_free(fetched);
        script_event_source_clear(bridge, source, false);
        if (bridge->result != NULL) {
            bridge->result->async_network_completed++;
            if (!success) bridge->result->network_failures++;
        }
        if (!delivered) return false;
        if (author_tasks != NULL) (*author_tasks)++;
        return true;
    }
    return true;
}

static JSValue js_fetch_cancel(JSContext *context, JSValueConst this_value,
                               int argc, JSValueConst *argv)
{
    (void) this_value;
    DomBridge *bridge = JS_GetContextOpaque(context);
    int64_t request_id = 0;
    const char *reason = argc > 1 && !JS_IsUndefined(argv[1])
                         ? JS_ToCString(context, argv[1]) : NULL;
    if (bridge == NULL || argc < 1
        || JS_ToInt64(context, &request_id, argv[0]) < 0) {
        if (reason != NULL) JS_FreeCString(context, reason);
        return JS_EXCEPTION;
    }
    size_t tracked_index = bridge->async_fetch_count;
    for (size_t i = 0; request_id > 0
         && i < bridge->async_fetch_count; i++) {
        if (bridge->async_fetches[i].id == (uint64_t) request_id) {
            tracked_index = i;
            break;
        }
    }
    bool tracked = tracked_index < bridge->async_fetch_count;
    bool marked_cancelled = tracked && bridge->fetch_scheduler != NULL
        && fetch_scheduler_cancel(bridge->fetch_scheduler,
                                  (uint64_t) request_id, reason);
    /* A transport can become complete between its last pump and an Abort.
       In that state cancel() correctly returns false, but the web request has
       not observed completion yet. Abort still wins: discard the result in
       place while ScriptAsyncFetch identifies it as outstanding. */
    bool discarded = tracked && bridge->fetch_scheduler != NULL
        && fetch_scheduler_discard(
            bridge->fetch_scheduler, (uint64_t) request_id);
    bool cancelled = marked_cancelled || discarded;
    if (discarded) {
        memmove(bridge->async_fetches + tracked_index,
                bridge->async_fetches + tracked_index + 1,
                (bridge->async_fetch_count - tracked_index - 1)
                    * sizeof(bridge->async_fetches[0]));
        bridge->async_fetch_count--;
    }
    if (cancelled) {
        /* Cancellation marks a scheduler item complete, but the bounded
           native slot is not reusable until the result is taken.  Drain it
           here so the logical FIFO may launch its next request immediately
           instead of waiting for an unrelated transport completion. */
        if (bridge->result != NULL) {
            bridge->result->async_network_cancelled++;
            if (reason != NULL && strstr(reason, "timed out") != NULL) {
                bridge->result->async_network_timed_out++;
            }
        }
    }
    if (reason != NULL) JS_FreeCString(context, reason);
    return JS_NewBool(context, cancelled);
}

static bool dynamic_source_body_usable(const BrowserSharedBody *body,
                                       size_t length)
{
    return body != NULL && body->data != NULL && body->length == length
        && budget_usable_size(body->data) > length
        && body->data[length] == 0;
}

char *js_rt_dynamic_copy_text(Budget *budget, const char *text)
{
    if (budget == NULL || text == NULL) return NULL;
    size_t length = strlen(text);
    if (length == SIZE_MAX) return NULL;
    char *copy = budget_malloc(budget, length + 1);
    if (copy != NULL) memcpy(copy, text, length + 1);
    return copy;
}

static void dynamic_module_policy_assign(
    ScriptDynamicTask *task, const char *response_policy)
{
    if (task == NULL) return;
    uint8_t response = js_rt_runtime_module_referrer_policy_code(response_policy);
    task->effective_referrer_policy = response_policy != NULL
        && response_policy[0] != '\0' && response != UINT8_MAX
            ? response : task->incoming_referrer_policy;
}

static void dynamic_cache_policy(const FetchResult *fetched,
                                 char cache_control[256], char vary[128])
{
    cache_control[0] = '\0';
    vary[0] = '\0';
    (void) fetch_response_header_value(
        fetched, "cache-control", cache_control, 256);
    (void) fetch_response_header_value(fetched, "vary", vary, 128);
}

static void dynamic_cache_store(BrowserSession *session, const char *url,
                                FetchResult *fetched,
                                const TilefinchRequestContext *context,
                                const TilefinchResourceGrant *grant)
{
    if (session == NULL || url == NULL || fetched == NULL
        || fetched->length == 0) return;
    char cache_control[256], vary[128];
    dynamic_cache_policy(fetched, cache_control, vary);
    if (fetch_result_share_body(fetched)) {
        (void) browser_session_cache_put_http_shared_resource(
            session, url, fetched->shared_body, fetched->etag,
            fetched->last_modified, fetched->content_type,
            cache_control, vary, js_rt_monotonic_time_ns(), context, grant);
    }
}

static void dynamic_module_cache_store(
    BrowserSession *session, const char *request_url,
    const char *effective_url, const char *initiator_origin,
    const char *top_level_url, bool initiator_opaque,
    TilefinchCredentialsMode credentials, FetchResult *fetched)
{
    if (session == NULL || request_url == NULL || effective_url == NULL
        || initiator_origin == NULL || fetched == NULL
        || fetched->length == 0) return;
    char cache_control[256], vary[128];
    char response_referrer_policy[FETCH_REFERRER_POLICY_LIMIT];
    bool referrer_policy_header_present = false;
    if (!fetch_response_referrer_policy(
            fetched, &referrer_policy_header_present,
            response_referrer_policy)) return;
    dynamic_cache_policy(fetched, cache_control, vary);
    BrowserModuleCacheProvenance provenance = {
        .effective_url = effective_url,
        .initiator_origin = initiator_origin,
        .top_level_url = top_level_url,
        .initiator_opaque = initiator_opaque,
        .response_referrer_policy = response_referrer_policy,
        .credentials = credentials,
        .cors_validated = true,
        .cors_redirect_origin_tainted =
            fetched->redirect_origin_tainted,
        .javascript_mime_validated = true,
        .referrer_policy_header_present =
            referrer_policy_header_present
    };
    if (fetch_result_share_body(fetched)) {
        (void) browser_session_cache_put_http_shared_module(
            session, request_url, fetched->shared_body, fetched->etag,
            fetched->last_modified, fetched->content_type,
            cache_control, vary, js_rt_monotonic_time_ns(), &provenance);
    } else {
        (void) browser_session_cache_put_http_module(
            session, request_url, (const unsigned char *) fetched->data,
            fetched->length, fetched->etag, fetched->last_modified,
            fetched->content_type, cache_control, vary,
            js_rt_monotonic_time_ns(), &provenance);
    }
}

static void dynamic_cache_revalidate(BrowserSession *session,
                                     const char *url,
                                     const FetchResult *fetched,
                                     const TilefinchRequestContext *context,
                                     const TilefinchResourceGrant *grant)
{
    if (session == NULL || url == NULL || fetched == NULL) return;
    char cache_control[256], vary[128];
    dynamic_cache_policy(fetched, cache_control, vary);
    (void) browser_session_cache_revalidate_resource(
        session, url, cache_control, vary, js_rt_monotonic_time_ns(),
        context, grant);
}

static TilefinchRequestContext dynamic_script_request_context(
    const DomBridge *bridge, const ScriptDynamicTask *task)
{
    return (TilefinchRequestContext) {
        .target_url = task == NULL ? NULL : task->request_url,
        .initiator_url = bridge == NULL ? NULL : bridge->document_url,
        .top_level_url = bridge == NULL ? NULL : bridge->top_level_url,
        .method = "GET",
        .mode = task == NULL ? TILEFINCH_REQUEST_MODE_NO_CORS : task->mode,
        .credentials = task == NULL ? TILEFINCH_CREDENTIALS_OMIT
                                    : task->credentials,
        .destination = TILEFINCH_DESTINATION_SCRIPT,
        .initiator_opaque = bridge != NULL && bridge->opaque_origin
    };
}

static bool dynamic_module_cache_revalidate(
    BrowserSession *session, const char *request_url,
    const char *effective_url, const char *initiator_origin,
    const char *top_level_url, bool initiator_opaque,
    TilefinchCredentialsMode credentials, const FetchResult *fetched)
{
    if (session == NULL || request_url == NULL || effective_url == NULL
        || initiator_origin == NULL || fetched == NULL) return false;
    char cache_control[256], vary[128];
    char response_referrer_policy[FETCH_REFERRER_POLICY_LIMIT];
    bool referrer_policy_header_present = false;
    if (!fetch_response_referrer_policy(
            fetched, &referrer_policy_header_present,
            response_referrer_policy)) return false;
    dynamic_cache_policy(fetched, cache_control, vary);
    BrowserModuleCacheProvenance provenance = {
        .effective_url = effective_url,
        .initiator_origin = initiator_origin,
        .top_level_url = top_level_url,
        .initiator_opaque = initiator_opaque,
        .response_referrer_policy = response_referrer_policy,
        .credentials = credentials,
        .cors_validated = true,
        .cors_redirect_origin_tainted =
            fetched->redirect_origin_tainted,
        .javascript_mime_validated = true,
        .referrer_policy_header_present =
            referrer_policy_header_present
    };
    return browser_session_cache_revalidate_module(
        session, request_url, cache_control, vary, js_rt_monotonic_time_ns(),
        &provenance);
}

static void dynamic_task_cancel_request(DomBridge *bridge,
                                        ScriptDynamicTask *task)
{
    if (bridge == NULL || task == NULL || task->request_id == 0
        || bridge->fetch_scheduler == NULL) return;
    (void) fetch_scheduler_cancel(
        bridge->fetch_scheduler, task->request_id,
        "dynamic script no longer active");
    (void) fetch_scheduler_discard(
        bridge->fetch_scheduler, task->request_id);
    task->request_id = 0;
}

void js_rt_dynamic_task_clear(DomBridge *bridge, ScriptDynamicTask *task,
                        bool cancelled, bool cancel_request)
{
    if (bridge == NULL || task == NULL || !task->active) return;
    if (cancel_request) dynamic_task_cancel_request(bridge, task);
    js_rt_bridge_script_quota_abort(bridge, &task->quota_reservation);
    if (task->source_length <= bridge->dynamic_script_bytes) {
        bridge->dynamic_script_bytes -= task->source_length;
    } else {
        bridge->dynamic_script_bytes = 0;
    }
    browser_shared_body_release(task->source_body);
    browser_shared_body_release(task->stale_body);
    script_resource_loader_plan_destroy(&task->resource_loader_plan);
    budget_free(bridge->budget, task->resource_loader_source);
    budget_free(bridge->budget, task->request_url);
    budget_free(bridge->budget, task->response_url);
    bridge_release_native_node_pin(bridge, task->node_handle);
    if (bridge->dynamic_script_count != 0) {
        bridge->dynamic_script_count--;
    }
    if (cancelled && bridge->result != NULL) {
        js_rt_saturating_add_size(&bridge->result->dynamic_scripts_cancelled, 1);
    }
    memset(task, 0, sizeof(*task));
}

static bool dynamic_task_take_source(DomBridge *bridge,
                                     ScriptDynamicTask *task,
                                     BrowserSharedBody *body,
                                     size_t length)
{
    if (bridge == NULL || task == NULL
        || (length != 0 && !dynamic_source_body_usable(body, length))) {
        if (bridge != NULL && task != NULL) {
            js_rt_bridge_script_quota_abort(bridge, &task->quota_reservation);
        }
        browser_shared_body_release(body);
        return false;
    }
    bool segmented = false;
    if (!task->module && length > bridge->maximum_script_file_bytes
        && dynamic_source_body_usable(body, length)) {
        segmented = script_resource_loader_plan_create(
            bridge->budget, (const char *) body->data, length,
            &task->resource_loader_plan)
            && task->resource_loader_plan.largest_statement_bytes
                   <= bridge->maximum_script_file_bytes
            && task->resource_loader_plan.largest_statement_bytes < SIZE_MAX;
        if (segmented) {
            task->resource_loader_source_capacity =
                task->resource_loader_plan.largest_statement_bytes + 1;
            task->resource_loader_source = budget_malloc_category(
                bridge->budget, BUDGET_CATEGORY_JAVASCRIPT,
                task->resource_loader_source_capacity);
            segmented = task->resource_loader_source != NULL;
        }
        if (!segmented) {
            script_resource_loader_plan_destroy(
                &task->resource_loader_plan);
            budget_free(bridge->budget, task->resource_loader_source);
            task->resource_loader_source = NULL;
            task->resource_loader_source_capacity = 0;
        }
    }
    if ((length > bridge->maximum_script_file_bytes && !segmented)
        || (length > task->quota_reservation.reserved_bytes
            && !js_rt_bridge_script_quota_expand(
                   bridge, &task->quota_reservation, length))
        || !js_rt_bridge_script_quota_commit(
               bridge, &task->quota_reservation, length)) {
        js_rt_bridge_script_quota_abort(bridge, &task->quota_reservation);
        if (bridge->result != NULL) {
            js_rt_saturating_add_size(
                &bridge->result->dynamic_scripts_quota_rejected, 1);
        }
        browser_shared_body_release(body);
        return false;
    }
    task->source_body = body;
    task->source_length = length;
    bridge->dynamic_script_bytes += length;
    if (bridge->result != NULL) {
        js_rt_saturating_add_size(&bridge->result->dynamic_script_bytes, length);
    }
    return true;
}

bool js_rt_dynamic_start_task(ScriptRuntime *runtime,
                        ScriptDynamicTask *task, bool *deferred)
{
    DomBridge *bridge = &runtime->bridge;
    if (deferred != NULL) *deferred = false;
    if (task == NULL || !task->active
        || task->state != SCRIPT_DYNAMIC_QUEUED) return true;

    /* Borrowed cache metadata is safe to inspect before reservation.  An
       exact fresh-body length avoids pinning a file-sized quota slice for a
       tiny cached script; ownership of the body is acquired only after the
       reservation succeeds. */
    const BrowserCacheEntry *cached = NULL;
    BrowserCacheStatus cache_status = BROWSER_CACHE_MISS;
    char initiator_origin[TILEFINCH_ORIGIN_SERIALIZED_LIMIT] = {0};
    bool has_initiator_origin = !task->module || tilefinch_url_origin(
        bridge->document_url, initiator_origin, sizeof(initiator_origin));
    bool cache_blocked = bridge->session != NULL
        && content_blocker_would_block(
               bridge->session->content_blocker, task->request_url,
               bridge->document_url, "script",
               task->module ? "cors" : "no-cors");
    TilefinchRequestContext resource_context =
        dynamic_script_request_context(bridge, task);
    if (!cache_blocked && bridge->session != NULL
        && has_initiator_origin && task->module && !bridge->opaque_origin) {
        cache_status = browser_session_cache_match_module(
            bridge->session, task->request_url, initiator_origin,
            bridge->top_level_url, bridge->opaque_origin,
            task->credentials, js_rt_monotonic_time_ns(), &cached);
        if (cached != NULL
            && !script_module_mime_type_allowed(cached->content_type)) {
            cached = NULL;
            cache_status = BROWSER_CACHE_MISS;
        }
    } else if (!cache_blocked && bridge->session != NULL && !task->module) {
        cache_status = browser_session_cache_match_classic_script(
            bridge->session, task->request_url, &resource_context,
            js_rt_monotonic_time_ns(), &cached);
    }
    size_t requested_bytes = bridge->maximum_script_file_bytes;
    if (cache_status == BROWSER_CACHE_FRESH && cached != NULL) {
        requested_bytes = cached->length;
    }
    ScriptQuotaReserveResult quota_status =
        cache_status == BROWSER_CACHE_FRESH && cached != NULL
        ? js_rt_bridge_script_quota_reserve_known(
              bridge, SCRIPT_QUOTA_PRECOUNTED_EXECUTABLE,
              requested_bytes, &task->quota_reservation)
        : js_rt_bridge_script_quota_reserve(
              bridge, SCRIPT_QUOTA_PRECOUNTED_EXECUTABLE,
              requested_bytes, &task->quota_reservation);
    if (quota_status == SCRIPT_QUOTA_RESERVE_DEFERRED) {
        if (deferred != NULL) *deferred = true;
        return true;
    }
    if (quota_status == SCRIPT_QUOTA_RESERVE_REJECTED) {
        if (bridge->result != NULL) {
            js_rt_saturating_add_size(
                &bridge->result->dynamic_scripts_quota_rejected, 1);
        }
        task->state = SCRIPT_DYNAMIC_READY;
        task->success = false;
        return true;
    }
    if (bridge->result != NULL) {
        js_rt_saturating_add_size(&bridge->result->dynamic_scripts_started, 1);
        js_rt_saturating_add_size(&bridge->result->network_requests, 1);
    }
    if (task->module && cached != NULL) {
        task->response_url = js_rt_dynamic_copy_text(
            bridge->budget, cached->module_effective_url);
        if (task->response_url == NULL) {
            js_rt_bridge_script_quota_abort(
                bridge, &task->quota_reservation);
            task->state = SCRIPT_DYNAMIC_READY;
            task->success = false;
            return true;
        }
        dynamic_module_policy_assign(
            task, cached->module_response_referrer_policy);
    }

    if (cache_status == BROWSER_CACHE_FRESH) {
        bool within_quota = cached != NULL
            && cached->length <= task->quota_reservation.reserved_bytes;
        BrowserSharedBody *body = !within_quota ? NULL
            : browser_shared_body_retain(cached->body);
        bool accepted = within_quota && dynamic_task_take_source(
            bridge, task, body, cached->length);
        if (!within_quota) {
            js_rt_bridge_script_quota_abort(
                bridge, &task->quota_reservation);
            if (bridge->result != NULL) {
                js_rt_saturating_add_size(
                    &bridge->result->dynamic_scripts_quota_rejected, 1);
            }
        }
        task->state = SCRIPT_DYNAMIC_READY;
        task->success = accepted;
        if (accepted && bridge->result != NULL) {
            js_rt_saturating_add_size(
                &bridge->result->dynamic_scripts_cache_hits, 1);
        }
        return true;
    }
    if (cache_status == BROWSER_CACHE_STALE && cached != NULL
        && cached->length <= bridge->maximum_script_file_bytes) {
        task->stale_body = browser_shared_body_retain(cached->body);
        if (!dynamic_source_body_usable(
                task->stale_body, cached->length)) {
            browser_shared_body_release(task->stale_body);
            task->stale_body = NULL;
        }
        task->stale_module_validated = task->module
            && task->stale_body != NULL;
        task->stale_resource_grant_valid = !task->module
            && task->stale_body != NULL && cached->resource_grant_valid;
        if (task->stale_resource_grant_valid) {
            task->stale_resource_grant = cached->resource_grant;
        }
    }

    if (bridge->fetch_scheduler == NULL) {
        bridge->fetch_scheduler = script_runtime_fetch_scheduler(runtime);
    }
    size_t response_limit = task->quota_reservation.reserved_bytes;
    if (!task->module
        && bridge->script_quota_bytes < bridge->maximum_script_bytes) {
        /*
         * A classic response may prove to be a safely segmentable loader
         * aggregate only after the complete bounded body is available.
         * Keep the ordinary logical reservation at the per-file ceiling so
         * unrelated async scripts remain concurrent; an accepted aggregate
         * atomically expands to its exact size before quota commit.
         */
        response_limit =
            bridge->maximum_script_bytes - bridge->script_quota_bytes;
    }
    ScriptRequestPolicy policy;
    FetchRequest request = {
        .method = "GET", .allow_http_errors = true,
        .send_low_client_hints = true, .accept = "*/*",
        .if_none_match = cached == NULL ? NULL : cached->etag,
        .if_modified_since = cached == NULL ? NULL : cached->last_modified,
        .cors_cached_response_validated = task->module && cached != NULL
            && !cached->module_cors_redirect_origin_tainted
            && (cached->etag[0] != '\0'
                || cached->last_modified[0] != '\0'),
        .redirect_same_origin_only = false
    };
    bool valid = response_limit != 0 && bridge->fetch_scheduler != NULL
        && script_request_policy_prepare(
               bridge, task->request_url, "GET", task->mode,
               task->credentials, TILEFINCH_DESTINATION_SCRIPT, true,
               &policy);
    if (valid) {
        policy.referrer_policy = js_rt_runtime_module_referrer_policy_text(
            task->incoming_referrer_policy);
        valid = policy.referrer_policy != NULL;
    }
    FetchPreparedPageRequest prepared;
    if (valid) {
        const FetchRequest *authorized = script_request_policy_apply(
            &policy, bridge->session, &request, &prepared);
        if (authorized != NULL) request = *authorized;
        FetchRequestValidationError validation =
            FETCH_REQUEST_VALIDATION_OK;
        valid = authorized != NULL
            && fetch_request_validate(&request, &validation);
    }
    long timeout_ms = bridge->fetch_timeout_ms < 5000
        ? 5000 : (long) bridge->fetch_timeout_ms;
    if (valid && fetch_scheduler_enqueue_would_block(
                     bridge->fetch_scheduler, response_limit)) {
        js_rt_bridge_script_quota_abort(
            bridge, &task->quota_reservation);
        /*
         * Admission has not started a logical request yet. Undo the existing
         * optimistic counters so repeated backpressure turns do not inflate
         * telemetry while the task remains queued.
         */
        if (bridge->result != NULL) {
            if (bridge->result->dynamic_scripts_started != 0) {
                bridge->result->dynamic_scripts_started--;
            }
            if (bridge->result->network_requests != 0) {
                bridge->result->network_requests--;
            }
        }
        if (deferred != NULL) *deferred = true;
        return true;
    }
    task->request_id = valid ? fetch_scheduler_enqueue(
        bridge->fetch_scheduler, task->request_url, &request,
        response_limit, timeout_ms) : 0;
    if (task->request_id == 0) {
        if (getenv("TILEFINCH_TRACE_SCRIPT_FAILURES") != NULL) {
            fprintf(stderr,
                    "dynamic-script-enqueue-failure url=\"%s\" "
                    "valid=%s response-limit=%zu scheduler=%s\n",
                    task->request_url == NULL ? "" : task->request_url,
                    valid ? "yes" : "no", response_limit,
                    bridge->fetch_scheduler == NULL ? "null" : "set");
        }
        js_rt_bridge_script_quota_abort(bridge, &task->quota_reservation);
        task->state = SCRIPT_DYNAMIC_READY;
        task->success = false;
        return true;
    }
    task->state = SCRIPT_DYNAMIC_FETCHING;
    return true;
}

bool js_rt_dynamic_take_completion(ScriptRuntime *runtime,
                             ScriptDynamicTask *task)
{
    DomBridge *bridge = &runtime->bridge;
    if (task == NULL || !task->active
        || task->state != SCRIPT_DYNAMIC_FETCHING
        || task->request_id == 0) return true;
    bool transport_success = false;
    FetchResult *fetched = fetch_result_create(bridge->budget);
    if (fetched == NULL) return false;
    if (!fetch_scheduler_take(
            bridge->fetch_scheduler, task->request_id,
            &transport_success, fetched)) {
        fetch_result_free(fetched);
        return true;
    }
    task->request_id = 0;
    char initiator_origin[TILEFINCH_ORIGIN_SERIALIZED_LIMIT] = {0};
    bool success = transport_success
        && (fetched->status_code == 304
            || (fetched->status_code >= 200 && fetched->status_code < 300));
    if (success && task->module) {
        success = tilefinch_url_origin(
            bridge->document_url, initiator_origin,
            sizeof(initiator_origin));
        if (success && fetched->status_code == 304) {
            success = task->stale_module_validated
                && (fetched->content_type[0] == '\0'
                    || script_module_mime_type_allowed(
                        fetched->content_type));
        } else if (success) {
            success = script_module_mime_type_allowed(
                fetched->content_type);
        }
    }
    char response_referrer_policy[FETCH_REFERRER_POLICY_LIMIT] = {0};
    bool referrer_policy_header_present = false;
    if (success && task->module) {
        success = fetch_response_referrer_policy(
            fetched, &referrer_policy_header_present,
            response_referrer_policy);
    }
    const char *response_url = fetched->effective_url[0] == '\0'
        ? (fetched->status_code == 304 && task->response_url != NULL
            ? task->response_url : task->request_url)
        : fetched->effective_url;
    TilefinchRequestContext resource_context =
        dynamic_script_request_context(bridge, task);
    TilefinchResourceGrant resource_grant = {0};
    bool resource_grant_valid = false;
    if (success && !task->module && fetched->status_code == 304) {
        if (fetched->effective_url[0] == '\0') {
            snprintf(fetched->effective_url, sizeof(fetched->effective_url),
                     "%s", response_url);
        }
        resource_grant_valid = task->stale_resource_grant_valid
            && fetch_resource_grant_revalidate_304(
                fetched, &resource_context, &task->stale_resource_grant,
                false, fetched->content_type[0] == '\0'
                    ? task->stale_resource_grant.mime_validated
                    : script_module_mime_type_allowed(fetched->content_type),
                false, &resource_grant, NULL);
        success = resource_grant_valid;
    } else if (success) {
        /* The scheduler can omit effective_url for a synthetic response.
           Complete that truthful field in place instead of copying the
           entire bounded response envelope (currently almost 16 KiB) just
           to alter one string for the resource-authority check.  The same
           effective URL is used by the cache and cookie paths below. */
        if (fetched->effective_url[0] == '\0') {
            snprintf(fetched->effective_url,
                     sizeof(fetched->effective_url), "%s", response_url);
        }
        resource_grant_valid = fetch_resource_grant_create(
            fetched, &resource_context, task->module,
            script_module_mime_type_allowed(fetched->content_type),
            task->module, &resource_grant, NULL);
        success = resource_grant_valid;
    }
    char *replacement_response_url = NULL;
    bool retain_response_url = task->module
        || strcmp(response_url, task->request_url) != 0;
    if (success && retain_response_url) {
        replacement_response_url = js_rt_dynamic_copy_text(
            bridge->budget, response_url);
        if (replacement_response_url == NULL) success = false;
    }
    BrowserSharedBody *body = NULL;
    size_t length = 0;
    if (success && fetched->status_code == 304) {
        body = task->stale_body;
        task->stale_body = NULL;
        length = body == NULL ? 0 : body->length;
        success = body != NULL;
        if (success) {
            success = task->module
                ? dynamic_module_cache_revalidate(
                    bridge->session, task->request_url, response_url,
                    initiator_origin, bridge->top_level_url,
                    bridge->opaque_origin, task->credentials, fetched)
                : (dynamic_cache_revalidate(
                       bridge->session, task->request_url, fetched,
                       &resource_context, &resource_grant), true);
        }
        if (success && task->module
            && referrer_policy_header_present) {
            dynamic_module_policy_assign(
                task, response_referrer_policy);
        }
        if (success) {
            if (bridge->result != NULL) {
                js_rt_saturating_add_size(
                    &bridge->result->dynamic_scripts_cache_hits, 1);
            }
        }
    }
    if (success) {
        budget_free(bridge->budget, task->response_url);
        task->response_url = replacement_response_url;
        replacement_response_url = NULL;
        if (bridge->result != NULL) {
            js_rt_record_network_response(bridge->result, fetched);
        }
        js_rt_script_store_response_cookies(
            bridge, fetched, task->request_url, task->mode,
            task->credentials, TILEFINCH_DESTINATION_SCRIPT);
    }
    if (success && fetched->status_code != 304) {
        if (task->module) {
            dynamic_module_policy_assign(
                task, response_referrer_policy);
        }
        length = fetched->length;
        if (length != 0) {
            if (fetched->shared_body == NULL && fetched->data != NULL
                && fetched->capacity > length + 1) {
                char *trimmed = budget_realloc_category(
                    fetched->budget, BUDGET_CATEGORY_RESOURCE,
                    fetched->data, length + 1);
                if (trimmed != NULL) {
                    fetched->data = trimmed;
                    fetched->capacity = length + 1;
                    fetched->data[length] = '\0';
                }
            }
            success = fetch_result_share_body(fetched);
            if (success) {
                if (task->module) {
                    dynamic_module_cache_store(
                        bridge->session, task->request_url,
                        task->response_url == NULL
                            ? task->request_url : task->response_url,
                        initiator_origin, bridge->top_level_url,
                        bridge->opaque_origin, task->credentials, fetched);
                } else {
                    dynamic_cache_store(
                        bridge->session, task->request_url, fetched,
                        &resource_context, &resource_grant);
                }
                body = browser_shared_body_retain(fetched->shared_body);
                success = body != NULL;
            }
        }
    }
    budget_free(bridge->budget, replacement_response_url);
    if (success) {
        success = dynamic_task_take_source(bridge, task, body, length);
        body = NULL;
    }
    if (!success) {
        js_rt_bridge_script_quota_abort(bridge, &task->quota_reservation);
    }
    browser_shared_body_release(body);
    if (!success && bridge->result != NULL) {
        js_rt_saturating_add_size(&bridge->result->network_failures, 1);
        if (js_rt_network_error_is_timeout(fetched->error)) {
            js_rt_saturating_add_size(&bridge->result->async_network_timed_out, 1);
        }
    }
    task->state = SCRIPT_DYNAMIC_READY;
    task->success = success;
    fetch_result_free(fetched);
    return true;
}

ScriptQuotaProgressResult script_runtime_script_quota_progress(
    ScriptRuntime *runtime, unsigned maximum_wait_ms)
{
    if (runtime == NULL) return SCRIPT_QUOTA_PROGRESS_FAILED;
    DomBridge *bridge = &runtime->bridge;
    bool fetching = false;
    for (size_t i = 0; i < SCRIPT_DYNAMIC_TASK_LIMIT; i++) {
        const ScriptDynamicTask *task = &bridge->dynamic_scripts[i];
        if (task->active && task->state == SCRIPT_DYNAMIC_FETCHING) {
            fetching = true;
            break;
        }
    }
    if (!fetching || bridge->fetch_scheduler == NULL) {
        return SCRIPT_QUOTA_PROGRESS_EXHAUSTED;
    }

    (void) fetch_scheduler_pump(
        bridge->fetch_scheduler, 1,
        maximum_wait_ms > 4 ? 4 : maximum_wait_ms);
    bool settled = false;
    bool pending = false;
    for (size_t i = 0; i < SCRIPT_DYNAMIC_TASK_LIMIT; i++) {
        ScriptDynamicTask *task = &bridge->dynamic_scripts[i];
        if (!task->active || task->state != SCRIPT_DYNAMIC_FETCHING) continue;
        if (!js_rt_dynamic_take_completion(runtime, task)) {
            return SCRIPT_QUOTA_PROGRESS_FAILED;
        }
        if (task->state == SCRIPT_DYNAMIC_FETCHING) pending = true;
        else settled = true;
    }
    return settled ? SCRIPT_QUOTA_PROGRESS_SETTLED
        : (pending ? SCRIPT_QUOTA_PROGRESS_PENDING
                   : SCRIPT_QUOTA_PROGRESS_EXHAUSTED);
}

static bool dynamic_admit_compile(ScriptRuntime *runtime,
                                  size_t working_bytes)
{
    size_t reserve = working_bytes > SCRIPT_DYNAMIC_EXECUTION_RESERVE_BYTES
        ? working_bytes : SCRIPT_DYNAMIC_EXECUTION_RESERVE_BYTES;
    bool budget_pressure = budget_pressure_required(
        runtime->budget, working_bytes, reserve);
    size_t heap = script_runtime_heap_remaining(runtime);
    bool heap_pressure = working_bytes > heap
        || reserve > heap - (working_bytes > heap ? heap : working_bytes);
    if (!budget_pressure && !heap_pressure) return true;
    (void) script_runtime_collect_and_trim(runtime);
    if (runtime->session != NULL
        && runtime->session->budget == runtime->budget) {
        (void) browser_session_cache_reclaim(runtime->session,
                                             working_bytes + reserve);
    }
    budget_pressure = budget_pressure_required(
        runtime->budget, working_bytes, reserve);
    heap = script_runtime_heap_remaining(runtime);
    heap_pressure = working_bytes > heap
        || reserve > heap - (working_bytes > heap ? heap : working_bytes);
    return !budget_pressure && !heap_pressure;
}

static void dynamic_source_lease_release(void *opaque)
{
    browser_shared_body_release((BrowserSharedBody *) opaque);
}

static bool dynamic_runtime_failure_is_fatal(const ScriptRuntime *runtime,
                                             size_t failures_before)
{
    if (runtime == NULL) return true;
    return runtime->watchdog.interrupted
        || runtime->budget->failure_count != failures_before;
}

static bool dynamic_ordered_blocked(const DomBridge *bridge,
                                    const ScriptDynamicTask *candidate)
{
    if (bridge == NULL || candidate == NULL || !candidate->ordered) {
        return false;
    }
    for (size_t i = 0; i < SCRIPT_DYNAMIC_TASK_LIMIT; i++) {
        const ScriptDynamicTask *task = &bridge->dynamic_scripts[i];
        if (task->active && task->ordered
            && task->sequence < candidate->sequence) return true;
    }
    return false;
}

bool js_rt_dynamic_classic_continuation_pending(
    const ScriptRuntime *runtime)
{
    if (runtime == NULL) return false;
    const DomBridge *bridge = &runtime->bridge;
    for (size_t i = 0; i < SCRIPT_DYNAMIC_TASK_LIMIT; i++) {
        const ScriptDynamicTask *task = &bridge->dynamic_scripts[i];
        if (task->active && task->success
            && task->state == SCRIPT_DYNAMIC_READY
            && task->resource_loader_plan.statement_count != 0
            && task->resource_loader_preflight_statement > 0
            && task->resource_loader_statement
                   < task->resource_loader_plan.statement_count) {
            return true;
        }
    }
    return false;
}

bool js_rt_dynamic_execute_ready(ScriptRuntime *runtime,
                           size_t completion_budget,
                           size_t *completed_out)
{
    if (completed_out != NULL) *completed_out = 0;
    DomBridge *bridge = &runtime->bridge;
    size_t maximum = completion_budget;
    size_t completed = 0;
    while (completed < maximum) {
        ScriptDynamicTask *selected = NULL;
        for (size_t i = 0; i < SCRIPT_DYNAMIC_TASK_LIMIT; i++) {
            ScriptDynamicTask *task = &bridge->dynamic_scripts[i];
            if (!task->active || task->state != SCRIPT_DYNAMIC_READY) {
                continue;
            }
            if (dynamic_ordered_blocked(bridge, task)) {
                if (bridge->result != NULL) {
                    js_rt_saturating_add_size(
                        &bridge->result->dynamic_scripts_ordered_waits, 1);
                }
                continue;
            }
            if (selected == NULL || task->sequence < selected->sequence) {
                selected = task;
            }
        }
        if (selected == NULL) break;
        size_t node_slot = 0;
        lxb_dom_node_t *selected_node =
            js_rt_bridge_node_slot_for_handle(
                bridge, selected->node_handle, &node_slot)
                ? bridge->nodes[node_slot] : NULL;
        if (selected_node == NULL) {
            /* Destructive DOM replacement can free a script element while
               its request is pending.  The generation mismatch is the only
               safe fact left: do not re-register or dereference the retained
               raw address, execute its body, or synthesize an event for it.
               A detached element retained by author JavaScript remains valid
               and, per HTML's preparation semantics, still executes. */
            if (bridge->result != NULL) {
                js_rt_saturating_add_size(
                    &bridge->result->dynamic_scripts_completed, 1);
            }
            js_rt_dynamic_task_clear(bridge, selected, false, false);
            completed++;
            continue;
        }
        bool fatal = false;
        if (!selected->success) {
            if (getenv("TILEFINCH_TRACE_SCRIPT_FAILURES") != NULL) {
                fprintf(stderr,
                        "dynamic-script-ready-failure url=\"%s\" "
                        "sequence=%llu module=%s state=%d\n",
                        selected->request_url == NULL
                            ? "" : selected->request_url,
                        (unsigned long long) selected->sequence,
                        selected->module ? "yes" : "no",
                        (int) selected->state);
            }
            const char *previous_source =
                runtime->promise_rejection_state.active_source;
            runtime->promise_rejection_state.active_source =
                selected->request_url == NULL
                    ? "<dynamic-script-admission>" : selected->request_url;
            size_t failures_before = runtime->budget->failure_count;
            bool dispatched = script_runtime_dispatch_node(
                runtime, selected_node, "error", NULL);
            runtime->promise_rejection_state.active_source = previous_source;
            fatal = !dispatched && dynamic_runtime_failure_is_fatal(
                runtime, failures_before);
            if (bridge->result != NULL) {
                js_rt_saturating_add_size(
                    &bridge->result->dynamic_scripts_failed, 1);
            }
        } else {
            const char *source = selected->source_length == 0
                ? "" : (const char *) selected->source_body->data;
            const char *source_url = selected->response_url == NULL
                ? selected->request_url : selected->response_url;
            ScriptLazyWebpackPlan lazy_plan;
            bool has_lazy_plan = !selected->module
                && selected->source_body != NULL
                && selected->source_length
                       >= SCRIPT_DYNAMIC_LAZY_MINIMUM_BYTES
                && getenv("TILEFINCH_DISABLE_LAZY_WEBPACK") == NULL
                && script_lazy_webpack_plan_create(
                       runtime->budget, source, selected->source_length,
                       &lazy_plan);
            if (has_lazy_plan
                && lazy_plan.factory_source_bytes
                       < selected->source_length / 2) {
                script_lazy_webpack_plan_destroy(&lazy_plan);
                has_lazy_plan = false;
            }
            bool resource_loader_preflight =
                selected->resource_loader_plan.statement_count != 0
                && selected->resource_loader_preflight_statement
                       < selected->resource_loader_plan.statement_count;
            size_t resource_loader_index = resource_loader_preflight
                ? selected->resource_loader_preflight_statement
                : selected->resource_loader_statement;
            const ScriptResourceLoaderStatement *resource_loader_statement =
                resource_loader_index
                      < selected->resource_loader_plan.statement_count
                    ? &selected->resource_loader_plan
                           .statements[resource_loader_index]
                    : NULL;
            size_t working_bytes =
                resource_loader_statement != NULL
                    ? resource_loader_statement->source_length
                    : (has_lazy_plan
                        ? lazy_plan.largest_factory_bytes
                        : selected->source_length);
            if (!dynamic_admit_compile(runtime, working_bytes)) {
                if (has_lazy_plan) {
                    script_lazy_webpack_plan_destroy(&lazy_plan);
                }
                const char *previous_source =
                    runtime->promise_rejection_state.active_source;
                runtime->promise_rejection_state.active_source =
                    source_url == NULL
                        ? "<dynamic-script-compile-admission>" : source_url;
                size_t failures_before = runtime->budget->failure_count;
                bool dispatched = script_runtime_dispatch_node(
                    runtime, selected_node, "error", NULL);
                runtime->promise_rejection_state.active_source =
                    previous_source;
                fatal = !dispatched
                    && dynamic_runtime_failure_is_fatal(
                           runtime, failures_before);
                if (bridge->result != NULL) {
                    js_rt_saturating_add_size(
                        &bridge->result->dynamic_scripts_failed, 1);
                }
            } else {
                size_t failures_before = runtime->budget->failure_count;
                bool handled = false;
                if (selected->resource_loader_plan.statement_count != 0) {
                    size_t index = resource_loader_index;
                    const ScriptResourceLoaderStatement *statement =
                        resource_loader_statement;
                    bool final_segment = !resource_loader_preflight
                        && statement != NULL
                        && index + 1
                             == selected->resource_loader_plan.statement_count;
                    bool evaluated = statement != NULL
                        && statement->source_offset
                               <= selected->source_length
                        && statement->source_length
                               <= selected->source_length
                                    - statement->source_offset
                        && statement->source_length
                               < selected->resource_loader_source_capacity;
                    if (evaluated) {
                        memcpy(
                            selected->resource_loader_source,
                            source + statement->source_offset,
                            statement->source_length);
                        selected->resource_loader_source[
                            statement->source_length] = '\0';
                    }
                    evaluated = evaluated
                        && (resource_loader_preflight
                            ? js_rt_preflight_external_classic_segment(
                                  runtime, selected_node,
                                  selected->resource_loader_source,
                                  statement->source_length, source_url)
                            : js_rt_evaluate_external_classic_segment(
                               runtime, selected_node,
                               selected->resource_loader_source,
                               statement->source_length, source_url,
                               final_segment));
                    if (evaluated && final_segment
                        && selected->resource_loader_plan
                               .statement_source_bytes
                               <= selected->source_length) {
                        /*
                         * Segment evaluation accounts for the bytes actually
                         * compiled on every successful turn. On final
                         * success, include the comments and whitespace
                         * between registrations so whole-script telemetry
                         * retains its established meaning.
                         */
                        js_rt_saturating_add_size(
                            &runtime->result.external_script_bytes,
                            selected->source_length
                                - selected->resource_loader_plan
                                      .statement_source_bytes);
                    }
                    fatal = !evaluated
                        && dynamic_runtime_failure_is_fatal(
                               runtime, failures_before);
                    if (!evaluated && bridge->result != NULL) {
                        js_rt_saturating_add_size(
                            &bridge->result->dynamic_scripts_failed, 1);
                    }
                    if (evaluated && resource_loader_preflight) {
                        selected->resource_loader_preflight_statement++;
                        completed++;
                        continue;
                    }
                    if (evaluated && !final_segment) {
                        selected->resource_loader_statement++;
                        completed++;
                        continue;
                    }
                    handled = true;
                } else if (has_lazy_plan) {
                    BrowserSharedBody *lease = browser_shared_body_retain(
                        selected->source_body);
                    if (lease != NULL) {
                        ScriptLazyEvaluation lazy =
                            script_runtime_evaluate_external_lazy_webpack(
                                runtime, selected_node, source,
                                selected->source_length, source_url,
                                &lazy_plan, lease,
                                dynamic_source_lease_release, NULL);
                        handled = lazy != SCRIPT_LAZY_EVALUATION_FALLBACK;
                        if (!handled) browser_shared_body_release(lease);
                    }
                    script_lazy_webpack_plan_destroy(&lazy_plan);
                }
                if (!handled) {
                    bool evaluated = selected->module
                        ? script_runtime_evaluate_external_module_context(
                              runtime, selected_node, source,
                              selected->source_length, selected->request_url,
                              source_url,
                              js_rt_runtime_module_referrer_policy_text(
                                  selected->effective_referrer_policy),
                              selected->credentials, NULL)
                        : script_runtime_evaluate_external_typed(
                              runtime, selected_node, source,
                              selected->source_length, source_url, false,
                              NULL);
                    fatal = !evaluated
                        && dynamic_runtime_failure_is_fatal(
                               runtime, failures_before);
                } else {
                    fatal = dynamic_runtime_failure_is_fatal(
                        runtime, failures_before);
                }
                if (!fatal) {
                    (void) script_runtime_record_resource_timing(
                        runtime, source_url, "script");
                }
            }
        }
        if (bridge->result != NULL) {
            js_rt_saturating_add_size(
                &bridge->result->dynamic_scripts_completed, 1);
        }
        js_rt_dynamic_task_clear(bridge, selected, false, false);
        completed++;
        if (fatal) {
            if (completed_out != NULL) *completed_out = completed;
            return false;
        }
    }
    if (completed_out != NULL) *completed_out = completed;
    return true;
}

bool js_fetch_cors_install(JSContext *context, JSValue global)
{
    return js_rt_install_function(context, global, "__tilefinchFetchSync",
                                  js_fetch_sync, 7)
        && js_rt_install_function(context, global, "__tilefinchFetchAsync",
                                  js_fetch_async, 8)
        && js_rt_install_function(context, global, "__tilefinchCancelNetwork",
                                  js_fetch_cancel, 2)
        && js_rt_install_function(context, global,
                                  "__tilefinchEventSourceStart",
                                  js_event_source_start, 3)
        && js_rt_install_function(context, global,
                                  "__tilefinchEventSourceClose",
                                  js_event_source_close, 1)
        && js_rt_install_function(context, global,
                                  "__tilefinchScheduleDynamicScript",
                                  js_schedule_dynamic_script, 1);
}
