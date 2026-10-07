#include "tilefinch/content_security_policy.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>

#include "tilefinch/document.h"
#include "tilefinch/resource_integrity.h"
#include "tilefinch/sha256.h"
#include "tilefinch/url.h"
#include "diagnostic_trace.h"

static const char *const csp_directive_names[TILEFINCH_CSP_DIRECTIVE_COUNT] = {
    "default-src", "script-src", "style-src", "img-src", "font-src",
    "connect-src", "frame-src", "object-src", "base-uri", "form-action",
    "frame-ancestors", "worker-src", "media-src", "script-src-elem",
    "script-src-attr", "style-src-elem", "style-src-attr"
};

/* Site-census ledger (TILEFINCH_TRACE_CENSUS): one line per refusal. A
   check may run more than once for one resource; the census tool dedupes. */
static bool csp_census(const char *what, const char *url, bool allowed)
{
#ifndef TILEFINCH_NO_TRACE
    if (!allowed && tilefinch_trace_census()) {
        printf("census-csp-refusal what=%s url=\"%.200s\"\n", what,
               url == NULL ? "" : url);
    }
#else
    (void) what;
    (void) url;
#endif
    return allowed;
}

void tilefinch_csp_init(TilefinchContentSecurityPolicy *policy)
{
    if (policy == NULL) return;
    memset(policy, 0, sizeof(*policy));
    policy->valid = true;
}

static bool span_equal_ci(const char *value, size_t length,
                          const char *wanted)
{
    return strlen(wanted) == length
        && strncasecmp(value, wanted, length) == 0;
}

static int directive_id(const char *name, size_t length)
{
    for (size_t i = 0; i < TILEFINCH_CSP_DIRECTIVE_COUNT; i++) {
        if (span_equal_ci(name, length, csp_directive_names[i])) {
            return (int) i;
        }
    }
    return -1;
}

static bool parse_one_policy(TilefinchContentSecurityPolicy *csp,
                             const char *value, size_t length)
{
    if (csp->policy_count >= TILEFINCH_CSP_POLICY_LIMIT
        || length + 1u > sizeof(csp->storage) - csp->storage_used) {
        return false;
    }
    size_t base = csp->storage_used;
    memcpy(csp->storage + base, value, length);
    csp->storage[base + length] = '\0';
    csp->storage_used += length + 1u;
    TilefinchCspPolicy *policy = &csp->policies[csp->policy_count++];
    size_t at = 0;
    while (at < length) {
        while (at < length && (value[at] == ';'
               || isspace((unsigned char) value[at]))) at++;
        size_t end = at;
        while (end < length && value[end] != ';') end++;
        size_t name_start = at;
        while (at < end && !isspace((unsigned char) value[at])) at++;
        size_t name_end = at;
        while (at < end && isspace((unsigned char) value[at])) at++;
        size_t sources_end = end;
        while (sources_end > at
               && isspace((unsigned char) value[sources_end - 1])) {
            sources_end--;
        }
        int id = directive_id(value + name_start, name_end - name_start);
        if (id >= 0 && !policy->directives[id].present) {
            if (at > UINT16_MAX || sources_end - at > UINT16_MAX
                || base + at > UINT16_MAX) return false;
            policy->directives[id] = (TilefinchCspDirectiveValue) {
                .offset = (uint16_t) (base + at),
                .length = (uint16_t) (sources_end - at),
                .present = true
            };
        }
        at = end + (end < length ? 1u : 0u);
    }
    return true;
}

bool tilefinch_csp_parse_response_headers(
    TilefinchContentSecurityPolicy *csp, const char *document_url,
    const char *headers, size_t headers_length,
    bool security_headers_truncated)
{
    if (csp == NULL || document_url == NULL
        || (headers == NULL && headers_length != 0)) return false;
    tilefinch_csp_init(csp);
    if (!tilefinch_url_origin(document_url, csp->document_origin,
                              sizeof(csp->document_origin))
        || security_headers_truncated) {
        csp->valid = false;
        return false;
    }
    size_t offset = 0;
    while (offset < headers_length) {
        const char *line = headers + offset;
        const char *newline = memchr(line, '\n', headers_length - offset);
        size_t line_length = newline == NULL
            ? headers_length - offset : (size_t) (newline - line);
        const char *colon = memchr(line, ':', line_length);
        if (colon != NULL
            && span_equal_ci(line, (size_t) (colon - line),
                             "content-security-policy")) {
            csp->header_present = true;
            const char *value = colon + 1;
            const char *limit = line + line_length;
            while (value < limit && isspace((unsigned char) *value)) value++;
            while (limit > value
                   && isspace((unsigned char) limit[-1])) limit--;
            /* A combined field is equivalent to multiple policies. CSP
               source expressions cannot contain an unquoted comma. */
            const char *part = value;
            for (const char *cursor = value;; cursor++) {
                if (cursor != limit && *cursor != ',') continue;
                const char *part_end = cursor;
                while (part < part_end
                       && isspace((unsigned char) *part)) part++;
                while (part_end > part
                       && isspace((unsigned char) part_end[-1])) part_end--;
                if (!parse_one_policy(csp, part,
                                      (size_t) (part_end - part))) {
                    csp->valid = false;
                    return false;
                }
                if (cursor == limit) break;
                part = cursor + 1;
            }
        }
        offset += line_length + (newline == NULL ? 0u : 1u);
    }
    return true;
}

static const TilefinchCspDirectiveValue *directive_for(
    const TilefinchCspPolicy *policy, TilefinchCspDirective directive,
    bool fallback_default)
{
    if (policy->directives[directive].present) {
        return &policy->directives[directive];
    }
    if (directive >= TILEFINCH_CSP_SCRIPT_SRC_ELEM) {
        directive = directive <= TILEFINCH_CSP_SCRIPT_SRC_ATTR
            ? TILEFINCH_CSP_SCRIPT_SRC : TILEFINCH_CSP_STYLE_SRC;
        if (policy->directives[directive].present) {
            return &policy->directives[directive];
        }
    }
    return fallback_default && policy->directives[TILEFINCH_CSP_DEFAULT_SRC]
                                   .present
        ? &policy->directives[TILEFINCH_CSP_DEFAULT_SRC] : NULL;
}

typedef bool (*CspTokenVisitor)(const char *, size_t, void *);

static bool visit_tokens(const TilefinchContentSecurityPolicy *csp,
                         const TilefinchCspDirectiveValue *value,
                         CspTokenVisitor visitor, void *opaque,
                         size_t *token_count)
{
    if (token_count != NULL) *token_count = 0;
    if (value == NULL || !value->present) return false;
    size_t at = value->offset;
    size_t end = at + value->length;
    if (end > csp->storage_used) return false;
    while (at < end) {
        while (at < end && isspace((unsigned char) csp->storage[at])) at++;
        size_t start = at;
        while (at < end && !isspace((unsigned char) csp->storage[at])) at++;
        if (at == start) continue;
        if (token_count != NULL) (*token_count)++;
        if (visitor(csp->storage + start, at - start, opaque)) return true;
    }
    return false;
}

typedef struct {
    const TilefinchContentSecurityPolicy *csp;
    const char *url;
    /* Parsed once per policy evaluation, on first use, rather than once per
       source token: 0 unknown, 1 parsed, 2 failed. */
    uint8_t target_state;
    uint8_t document_state;
    TilefinchUrl target;
    TilefinchUrl document;
} UrlMatch;

static const TilefinchUrl *url_match_target(UrlMatch *match)
{
    if (match->target_state == 0) {
        match->target_state =
            tilefinch_url_parse(match->url, &match->target) ? 1 : 2;
    }
    return match->target_state == 1 ? &match->target : NULL;
}

static const TilefinchUrl *url_match_document(UrlMatch *match)
{
    if (match->document_state == 0) {
        match->document_state = tilefinch_url_parse(
            match->csp->document_origin, &match->document) ? 1 : 2;
    }
    return match->document_state == 1 ? &match->document : NULL;
}

static bool scheme_matches(TilefinchUrlScheme source,
                           TilefinchUrlScheme target)
{
    return source == target
        || (source == TILEFINCH_URL_SCHEME_HTTP
            && target == TILEFINCH_URL_SCHEME_HTTPS);
}

static bool host_source_matches(const char *token, size_t length,
                                UrlMatch *match)
{
    if (length == 0 || length >= 512) return false;
    const TilefinchUrl *parsed_target = url_match_target(match);
    if (parsed_target == NULL) return false;
    const TilefinchUrl target = *parsed_target;
    size_t at = 0;
    TilefinchUrlScheme source_scheme = TILEFINCH_URL_SCHEME_INVALID;
    const char *scheme_end = NULL;
    for (size_t i = 0; i + 2 < length; i++) {
        if (token[i] == ':' && token[i + 1] == '/'
            && token[i + 2] == '/') {
            scheme_end = token + i;
            at = i + 3;
            break;
        }
    }
    if (scheme_end != NULL) {
        size_t scheme_length = (size_t) (scheme_end - token);
        if (span_equal_ci(token, scheme_length, "https")) {
            source_scheme = TILEFINCH_URL_SCHEME_HTTPS;
        } else if (span_equal_ci(token, scheme_length, "http")) {
            source_scheme = TILEFINCH_URL_SCHEME_HTTP;
        } else return false;
    } else {
        const TilefinchUrl *document = url_match_document(match);
        if (document == NULL) return false;
        source_scheme = document->scheme;
    }
    if (!scheme_matches(source_scheme, target.scheme)) return false;
    size_t authority_end = at;
    while (authority_end < length && token[authority_end] != '/') {
        authority_end++;
    }
    size_t host_start = at;
    bool wildcard = authority_end - host_start > 2
        && token[host_start] == '*' && token[host_start + 1] == '.';
    if (wildcard) host_start += 2;
    size_t host_end = authority_end;
    size_t port_start = authority_end;
    for (size_t i = host_start; i < authority_end; i++) {
        if (token[i] == ':') {
            host_end = i;
            port_start = i + 1;
        }
    }
    size_t host_length = host_end - host_start;
    const char *target_host = target.value + target.host_offset;
    bool any_host = host_length == 1 && token[host_start] == '*';
    bool host_ok = any_host || (target.host_length == host_length
        && strncasecmp(target_host, token + host_start, host_length) == 0);
    if (wildcard) {
        host_ok = target.host_length > host_length
            && target_host[target.host_length - host_length - 1] == '.'
            && strncasecmp(target_host + target.host_length - host_length,
                           token + host_start, host_length) == 0;
    }
    if (!host_ok) return false;
    if (port_start < authority_end) {
        if (authority_end - port_start == 1 && token[port_start] == '*') {
            /* Any explicit or default port. */
        } else {
            unsigned port = 0;
            for (size_t i = port_start; i < authority_end; i++) {
                if (!isdigit((unsigned char) token[i])) return false;
                port = port * 10u + (unsigned) (token[i] - '0');
                if (port > 65535u) return false;
            }
            if (port == 0 || target.port != port) return false;
        }
    } else {
        unsigned default_port = target.scheme == TILEFINCH_URL_SCHEME_HTTPS
            ? 443u : 80u;
        if (target.port != default_port) return false;
    }
    if (authority_end < length) {
        size_t path_length = length - authority_end;
        bool prefix = token[length - 1] == '/';
        if ((prefix && target.path_length < path_length)
            || (!prefix && target.path_length != path_length)
            || memcmp(target.value + target.path_offset,
                      token + authority_end, path_length) != 0) return false;
    }
    return true;
}

static bool url_token_matches(const char *token, size_t length, void *opaque)
{
    UrlMatch *match = opaque;
    if (span_equal_ci(token, length, "'none'")) return false;
    if (span_equal_ci(token, length, "'self'")) {
        return tilefinch_url_same_origin(match->csp->document_origin,
                                         match->url);
    }
    if (length == 1 && token[0] == '*') {
        return url_match_target(match) != NULL;
    }
    if (length == 5 && strncasecmp(token, "data:", 5) == 0) {
        return strncasecmp(match->url, "data:", 5) == 0;
    }
    if (length == 5 && strncasecmp(token, "blob:", 5) == 0) {
        return strncasecmp(match->url, "blob:", 5) == 0;
    }
    if (length == 6 && strncasecmp(token, "https:", 6) == 0) {
        const TilefinchUrl *parsed = url_match_target(match);
        return parsed != NULL
            && parsed->scheme == TILEFINCH_URL_SCHEME_HTTPS;
    }
    if (length == 5 && strncasecmp(token, "http:", 5) == 0) {
        const TilefinchUrl *parsed = url_match_target(match);
        return parsed != NULL
            && (parsed->scheme == TILEFINCH_URL_SCHEME_HTTP
                || parsed->scheme == TILEFINCH_URL_SCHEME_HTTPS);
    }
    return token[0] != '\'' && host_source_matches(token, length, match);
}

static bool policy_allows_url(const TilefinchContentSecurityPolicy *csp,
                              const TilefinchCspPolicy *policy,
                              TilefinchCspDirective directive,
                              bool fallback_default, const char *url)
{
    const TilefinchCspDirectiveValue *value = directive_for(
        policy, directive, fallback_default);
    if (value == NULL) return true;
    UrlMatch match = {.csp = csp, .url = url};
    size_t count = 0;
    bool matched = visit_tokens(csp, value, url_token_matches, &match, &count);
    return count != 0 && matched;
}

static bool keyword_token(const char *token, size_t length, void *opaque)
{
    return span_equal_ci(token, length, opaque);
}

static bool directive_has_keyword(const TilefinchContentSecurityPolicy *csp,
                                  const TilefinchCspDirectiveValue *value,
                                  const char *keyword)
{
    return visit_tokens(csp, value, keyword_token, (void *) keyword, NULL);
}

/* One policy's verdict. For script-like requests a grant bit (nonce or
   integrity match) admits the URL, and 'strict-dynamic' replaces the URL
   list with the parser-inserted test. */
static bool policy_allows_request(const TilefinchContentSecurityPolicy *csp,
                                  size_t index,
                                  TilefinchCspDirective directive,
                                  bool fallback_default, const char *url,
                                  uint8_t grant, bool script_like)
{
    const TilefinchCspPolicy *policy = &csp->policies[index];
    const TilefinchCspDirectiveValue *value = directive_for(
        policy, directive, fallback_default);
    if (value == NULL || (grant & (1u << index)) != 0) return true;
    if (script_like
        && directive_has_keyword(csp, value, "'strict-dynamic'")) {
        return (grant & TILEFINCH_CSP_GRANT_PARSER_INSERTED) == 0;
    }
    return policy_allows_url(csp, policy, directive, fallback_default, url);
}

static bool csp_allows_url(const TilefinchContentSecurityPolicy *csp,
                           TilefinchCspDirective directive,
                           bool fallback_default, const char *url)
{
    if (csp == NULL || !csp->header_present) return true;
    if (!csp->valid || url == NULL) return false;
    for (size_t i = 0; i < csp->policy_count; i++) {
        if (!policy_allows_url(csp, &csp->policies[i], directive,
                               fallback_default, url)) return false;
    }
    return true;
}

static bool csp_request_directive(TilefinchRequestDestination destination,
                                  TilefinchCspDirective *directive)
{
    switch (destination) {
        case TILEFINCH_DESTINATION_SCRIPT:
            *directive = TILEFINCH_CSP_SCRIPT_SRC_ELEM; return true;
        case TILEFINCH_DESTINATION_STYLE:
            *directive = TILEFINCH_CSP_STYLE_SRC_ELEM; return true;
        case TILEFINCH_DESTINATION_FONT:
            *directive = TILEFINCH_CSP_FONT_SRC; return true;
        case TILEFINCH_DESTINATION_IMAGE:
            *directive = TILEFINCH_CSP_IMG_SRC; return true;
        case TILEFINCH_DESTINATION_FETCH:
            *directive = TILEFINCH_CSP_CONNECT_SRC; return true;
        case TILEFINCH_DESTINATION_FRAME:
            *directive = TILEFINCH_CSP_FRAME_SRC; return true;
        case TILEFINCH_DESTINATION_MEDIA:
            *directive = TILEFINCH_CSP_MEDIA_SRC; return true;
        case TILEFINCH_DESTINATION_OTHER:
            *directive = TILEFINCH_CSP_OBJECT_SRC; return true;
        default:
            return false;
    }
}

static bool csp_allows_request_granted(
    const TilefinchContentSecurityPolicy *csp,
    TilefinchRequestDestination destination, const char *target_url,
    uint8_t grant);

bool tilefinch_csp_allows_request_granted(
    const TilefinchContentSecurityPolicy *csp,
    TilefinchRequestDestination destination, const char *target_url,
    uint8_t grant)
{
#ifdef TILEFINCH_NO_TRACE
    return csp_allows_request_granted(csp, destination, target_url, grant);
#else
    /* Indexed by TilefinchRequestDestination. */
    static const char *const names[] = {
        "document", "frame", "script", "style", "image", "fetch", "other",
        "font", "media", "worker"
    };
    bool allowed = csp_allows_request_granted(
        csp, destination, target_url, grant);
    return csp_census((unsigned) destination < sizeof(names) / sizeof(names[0])
                          ? names[destination] : "request",
                      target_url, allowed);
#endif
}

static bool csp_allows_request_granted(
    const TilefinchContentSecurityPolicy *csp,
    TilefinchRequestDestination destination, const char *target_url,
    uint8_t grant)
{
    if (destination == TILEFINCH_DESTINATION_DOCUMENT) return true;
    if (destination == TILEFINCH_DESTINATION_WORKER) {
        return tilefinch_csp_allows_worker(csp, target_url);
    }
    TilefinchCspDirective directive;
    if (!csp_request_directive(destination, &directive)) return false;
    if (csp == NULL || !csp->header_present) return true;
    if (!csp->valid || target_url == NULL) return false;
    for (size_t i = 0; i < csp->policy_count; i++) {
        if (!policy_allows_request(
                csp, i, directive, true, target_url, grant,
                destination == TILEFINCH_DESTINATION_SCRIPT)) return false;
    }
    return true;
}

bool tilefinch_csp_allows_request(
    const TilefinchContentSecurityPolicy *csp,
    TilefinchRequestDestination destination, const char *target_url)
{
    return tilefinch_csp_allows_request_granted(
        csp, destination, target_url, 0);
}

bool tilefinch_csp_allows_worker(
    const TilefinchContentSecurityPolicy *csp, const char *target_url)
{
    if (csp == NULL || !csp->header_present) return true;
    if (!csp->valid || target_url == NULL) return false;
    for (size_t i = 0; i < csp->policy_count; i++) {
        const TilefinchCspPolicy *policy = &csp->policies[i];
        TilefinchCspDirective directive = TILEFINCH_CSP_WORKER_SRC;
        if (!policy->directives[directive].present) {
            directive = TILEFINCH_CSP_SCRIPT_SRC;
        }
        /* A worker is always script-initiated: 'strict-dynamic' admits it. */
        if (!policy_allows_request(csp, i, directive, true, target_url, 0,
                                   true)) return false;
    }
    return true;
}

typedef struct {
    const char *value;
    size_t length;
} CspSpan;

static bool nonce_token_matches(const char *token, size_t length,
                                void *opaque)
{
    const CspSpan *nonce = opaque;
    return nonce->length != 0 && length == nonce->length + 8u
        && strncasecmp(token, "'nonce-", 7) == 0
        && token[length - 1] == '\''
        && memcmp(token + 7, nonce->value, nonce->length) == 0;
}

/* hash-source: 'sha256-', 'sha384-' or 'sha512-' and a base64 value. */
static bool hash_source_token(const char *token, size_t length, void *opaque)
{
    (void) opaque;
    return length >= 10u && token[0] == '\'' && token[length - 1] == '\''
        && (strncasecmp(token + 1, "sha256-", 7) == 0
            || strncasecmp(token + 1, "sha384-", 7) == 0
            || strncasecmp(token + 1, "sha512-", 7) == 0);
}

typedef struct {
    const char *algorithm;
    const char *value;
    size_t value_length;
} IntegritySource;

static bool integrity_token_matches(const char *token, size_t length,
                                    void *opaque)
{
    const IntegritySource *source = opaque;
    return hash_source_token(token, length, NULL)
        && strncasecmp(token + 1, source->algorithm, 7) == 0
        && length == source->value_length + 9u
        && memcmp(token + 8, source->value, source->value_length) == 0;
}

/* CSP3 integrity bypass: every recognized SRI hash must be listed. SRI
   itself is enforced when the response arrives, so admitting the request
   on the listed digests admits only those bytes. */
static bool integrity_matches(const TilefinchContentSecurityPolicy *csp,
                              const TilefinchCspDirectiveValue *value,
                              const char *integrity, size_t length)
{
    if (integrity == NULL || length == 0
        || !visit_tokens(csp, value, hash_source_token, NULL, NULL))
        return false;
    size_t recognized = 0;
    size_t at = 0;
    TilefinchIntegrityToken token;
    while (tilefinch_integrity_next_token(integrity, length, &at, &token)) {
        if (token.value_length == 0) continue;
        IntegritySource source = {
            .algorithm = token.algorithm,
            .value = token.value,
            .value_length = token.value_length
        };
        recognized++;
        if (!visit_tokens(csp, value, integrity_token_matches, &source,
                          NULL)) return false;
    }
    return recognized != 0;
}

typedef struct {
    void (*visit)(void *, const char *, size_t);
    void *opaque;
} NonceVisit;

static bool nonce_source_visit(const char *token, size_t length,
                               void *opaque)
{
    const NonceVisit *visit = opaque;
    if (length > 8u && strncasecmp(token, "'nonce-", 7) == 0
        && token[length - 1] == '\'') {
        visit->visit(visit->opaque, token + 7, length - 8u);
    }
    return false;
}

void tilefinch_csp_visit_nonce_sources(
    const TilefinchContentSecurityPolicy *csp,
    void (*visit)(void *opaque, const char *value, size_t length),
    void *opaque)
{
    if (csp == NULL || !csp->header_present || !csp->valid || visit == NULL)
        return;
    NonceVisit state = {.visit = visit, .opaque = opaque};
    for (size_t i = 0; i < csp->policy_count; i++) {
        for (size_t d = 0; d < TILEFINCH_CSP_DIRECTIVE_COUNT; d++) {
            const TilefinchCspDirectiveValue *value =
                &csp->policies[i].directives[d];
            if (value->present)
                (void) visit_tokens(csp, value, nonce_source_visit, &state,
                                    NULL);
        }
    }
}

uint8_t tilefinch_csp_request_grant(
    const TilefinchContentSecurityPolicy *csp,
    TilefinchRequestDestination destination,
    const char *nonce, size_t nonce_length,
    const char *integrity, size_t integrity_length, bool parser_inserted)
{
    bool script = destination == TILEFINCH_DESTINATION_SCRIPT;
    uint8_t grant = script && parser_inserted
        ? TILEFINCH_CSP_GRANT_PARSER_INSERTED : 0u;
    TilefinchCspDirective directive;
    if (csp == NULL || !csp->header_present || !csp->valid
        || (!script && destination != TILEFINCH_DESTINATION_STYLE)
        || !csp_request_directive(destination, &directive)) return grant;
    CspSpan span = {.value = nonce, .length = nonce == NULL ? 0 : nonce_length};
    for (size_t i = 0; i < csp->policy_count; i++) {
        const TilefinchCspDirectiveValue *value = directive_for(
            &csp->policies[i], directive, true);
        if (value == NULL) continue;
        if (visit_tokens(csp, value, nonce_token_matches, &span, NULL)
            || (script && integrity_matches(
                    csp, value, integrity, integrity_length))) {
            grant |= (uint8_t) (1u << i);
        }
    }
    return grant;
}

bool tilefinch_csp_text_has_markup(const char *value, size_t length)
{
    for (size_t at = 0; value != NULL && at < length; at++) {
        if (value[at] != '<') continue;
        size_t left = length - at - 1u;
        if ((left >= 6u && strncasecmp(value + at + 1, "script", 6) == 0)
            || (left >= 5u && strncasecmp(value + at + 1, "style", 5) == 0))
            return true;
    }
    return false;
}

/* CSP3 "Is element nonceable?": a dangling-markup injection that swallows
   a legitimate nonce leaves "<script" or "<style" in an attribute. Like the
   major engines, the [[CryptographicNonce]] slot rather than the (hidden)
   content attribute decides whether there is a nonce at all. */
static const char *csp_element_nonce(struct lxb_dom_node *element,
                                     size_t *length)
{
    *length = 0;
    size_t nonce_length = 0;
    const char *nonce = document_element_nonce(element, &nonce_length);
    if (nonce == NULL || nonce_length == 0) return NULL;
    size_t visited = 0;
    for (lxb_dom_attr_t *attribute =
             lxb_dom_interface_element(element)->first_attr;
         attribute != NULL; attribute = attribute->next) {
        if (++visited > 256u) return NULL;
        size_t name_length = 0, value_length = 0;
        const char *name = (const char *) lxb_dom_attr_qualified_name(
            attribute, &name_length);
        const char *value = (const char *) lxb_dom_attr_value(
            attribute, &value_length);
        if (tilefinch_csp_text_has_markup(name, name_length)
            || tilefinch_csp_text_has_markup(value, value_length))
            return NULL;
    }
    *length = nonce_length;
    return nonce;
}

uint8_t tilefinch_csp_element_descendant_grant(
    const TilefinchContentSecurityPolicy *csp,
    struct lxb_dom_node *element, bool parser_inserted)
{
    size_t nonce_length = 0;
    const char *nonce = element == NULL || csp == NULL
        || !csp->header_present
        ? NULL : csp_element_nonce(element, &nonce_length);
    return tilefinch_csp_request_grant(
        csp, TILEFINCH_DESTINATION_SCRIPT, nonce, nonce_length, NULL, 0,
        parser_inserted);
}

uint8_t tilefinch_csp_element_grant(
    const TilefinchContentSecurityPolicy *csp,
    TilefinchRequestDestination destination,
    struct lxb_dom_node *element, bool parser_inserted)
{
    size_t nonce_length = 0, integrity_length = 0;
    const char *nonce = NULL, *integrity = NULL;
    if (element != NULL && csp != NULL && csp->header_present) {
        nonce = csp_element_nonce(element, &nonce_length);
        if (destination == TILEFINCH_DESTINATION_SCRIPT) {
            integrity = document_attribute(
                element, "integrity", &integrity_length);
        }
    }
    return tilefinch_csp_request_grant(
        csp, destination, nonce, nonce_length, integrity, integrity_length,
        parser_inserted);
}

static bool unsafe_eval_token(const char *token, size_t length, void *opaque)
{
    (void) opaque;
    return span_equal_ci(token, length, "'unsafe-eval'");
}

static bool csp_allows_dynamic_code(
    const TilefinchContentSecurityPolicy *csp);

bool tilefinch_csp_allows_dynamic_code(
    const TilefinchContentSecurityPolicy *csp)
{
    /* A policy query made once per realm, not an attempted eval: the
       engine's own refusal surfaces as an EvalError the ledger records. */
    return csp_census("policy-no-eval", "", csp_allows_dynamic_code(csp));
}

static bool csp_allows_dynamic_code(
    const TilefinchContentSecurityPolicy *csp)
{
    if (csp == NULL || !csp->header_present) return true;
    if (!csp->valid) return false;
    for (size_t i = 0; i < csp->policy_count; i++) {
        const TilefinchCspDirectiveValue *value = directive_for(
            &csp->policies[i], TILEFINCH_CSP_SCRIPT_SRC, true);
        if (value == NULL) continue;
        size_t count = 0;
        if (!visit_tokens(csp, value, unsafe_eval_token, NULL, &count))
            return false;
    }
    return true;
}

typedef struct {
    const char *nonce;
    size_t nonce_length;
    /* The element whose text a 'sha256-' source is compared with. Its
       digest is computed at the first such source: a policy without one
       (CNN's style-src is 'unsafe-inline' 'self') never hashes the
       megabytes of an inline stylesheet or script, which every check of
       that element did before. */
    struct lxb_dom_node *element;
    /* Base64 digests by algorithm (SHA-256, -384, -512), each computed at
       the first source of its algorithm. */
    char hash[3][92];
    bool hash_attempted[3];
    bool have_hash[3];
    bool has_nonce_or_hash_source;
    bool has_strict_dynamic;
    bool has_unsafe_inline;
} InlineMatch;

static bool inline_element_hash(struct lxb_dom_node *element,
                                size_t algorithm, char output[92]);

/* 0, 1 or 2 for a 'sha256-', 'sha384-' or 'sha512-' source; else 3. */
static size_t hash_source_algorithm(const char *token, size_t length)
{
    if (!hash_source_token(token, length, NULL)) return 3u;
    return token[4] == '2' ? 0u : token[4] == '3' ? 1u : 2u;
}

static bool inline_token_matches(const char *token, size_t length,
                                 void *opaque)
{
    InlineMatch *match = opaque;
    size_t algorithm = hash_source_algorithm(token, length);
    /* Any nonce or hash source (and, for scripts, 'strict-dynamic')
       disables 'unsafe-inline' (CSP3 "allow all inline"). */
    if ((length > 8 && strncasecmp(token, "'nonce-", 7) == 0)
        || algorithm < 3u) {
        match->has_nonce_or_hash_source = true;
    }
    if (span_equal_ci(token, length, "'strict-dynamic'")) {
        match->has_strict_dynamic = true;
    }
    if (span_equal_ci(token, length, "'unsafe-inline'")) {
        match->has_unsafe_inline = true;
    }
    if (match->nonce != NULL && length == match->nonce_length + 8u
        && strncasecmp(token, "'nonce-", 7) == 0
        && token[length - 1] == '\''
        && memcmp(token + 7, match->nonce, match->nonce_length) == 0) {
        return true;
    }
    if (algorithm == 3u || match->element == NULL) return false;
    if (!match->hash_attempted[algorithm]) {
        match->hash_attempted[algorithm] = true;
        match->have_hash[algorithm] = inline_element_hash(
            match->element, algorithm, match->hash[algorithm]);
    }
    size_t digest_length = strlen(match->hash[algorithm]);
    return match->have_hash[algorithm] && length == digest_length + 9u
        && memcmp(token + 8, match->hash[algorithm], digest_length) == 0;
}

static void digest_base64(const uint8_t *digest, size_t length, char *output)
{
    static const char alphabet[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t out = 0;
    for (size_t i = 0; i < length; i += 3) {
        size_t left = length - i;
        uint32_t word = (uint32_t) digest[i] << 16
            | (left > 1 ? (uint32_t) digest[i + 1] << 8 : 0u)
            | (left > 2 ? digest[i + 2] : 0u);
        output[out++] = alphabet[(word >> 18) & 63u];
        output[out++] = alphabet[(word >> 12) & 63u];
        output[out++] = left > 1 ? alphabet[(word >> 6) & 63u] : '=';
        output[out++] = left > 2 ? alphabet[word & 63u] : '=';
    }
    output[out] = '\0';
}

static size_t csp_inline_digests;

size_t tilefinch_csp_inline_digests(void)
{
    return csp_inline_digests;
}

static bool inline_element_hash(struct lxb_dom_node *element,
                                size_t algorithm, char output[92])
{
    if (element == NULL || algorithm > 2u) return false;
#if !defined(TILEFINCH_NO_TRACE) || defined(TILEFINCH_PSP_VALIDATION_LOG)
    csp_inline_digests++;
#endif
    TilefinchSha256 sha256;
    TilefinchSha512 sha512;
    bool ok = algorithm == 0u
        ? (tilefinch_sha256_init(&sha256), true)
        : tilefinch_sha512_begin(&sha512, algorithm == 1u);
    for (struct lxb_dom_node *child = element->first_child; ok
         && child != NULL; child = child->next) {
        size_t length = 0;
        const char *text = document_text_data(child, &length);
        if (text == NULL) continue;
        if (algorithm == 0u) {
            ok = tilefinch_sha256_update(
                &sha256, (const uint8_t *) text, length);
        } else {
            tilefinch_sha512_update(&sha512, (const uint8_t *) text, length);
        }
    }
    uint8_t digest[64];
    /* Empty inline elements have the ordinary digest of zero bytes. */
    if (algorithm == 0u) {
        ok = ok && tilefinch_sha256_final(&sha256, digest);
    } else {
        ok = tilefinch_sha512_finish(&sha512, digest) && ok;
    }
    if (!ok) return false;
    digest_base64(digest, algorithm == 0u ? 32u : algorithm == 1u ? 48u : 64u,
                  output);
    return true;
}

static bool policy_allows_inline(const TilefinchContentSecurityPolicy *csp,
                                 const TilefinchCspPolicy *policy,
                                 TilefinchCspDirective directive,
                                 struct lxb_dom_node *element)
{
    const TilefinchCspDirectiveValue *value = directive_for(
        policy, directive, true);
    if (value == NULL) return true;
    size_t nonce_length = 0;
    const char *nonce = csp_element_nonce(element, &nonce_length);
    InlineMatch match = {
        .nonce = nonce, .nonce_length = nonce_length, .element = element
    };
    size_t count = 0;
    if (visit_tokens(csp, value, inline_token_matches, &match, &count)) {
        return true;
    }
    if (count == 0 || match.has_nonce_or_hash_source
        || (match.has_strict_dynamic
            && directive == TILEFINCH_CSP_SCRIPT_SRC_ELEM)) return false;
    return match.has_unsafe_inline;
}

static bool csp_allows_inline(const TilefinchContentSecurityPolicy *csp,
                              TilefinchCspDirective directive,
                              struct lxb_dom_node *element)
{
    if (csp == NULL || !csp->header_present) return true;
    if (!csp->valid || element == NULL) return false;
    for (size_t i = 0; i < csp->policy_count; i++) {
        if (!policy_allows_inline(csp, &csp->policies[i], directive,
                                  element)) return false;
    }
    return true;
}

bool tilefinch_csp_allows_inline_script(
    const TilefinchContentSecurityPolicy *csp, struct lxb_dom_node *element)
{
    return csp_census("inline-script", "",
                      csp_allows_inline(csp, TILEFINCH_CSP_SCRIPT_SRC_ELEM,
                                        element));
}

bool tilefinch_csp_allows_inline_style(
    const TilefinchContentSecurityPolicy *csp, struct lxb_dom_node *element)
{
    return csp_census("inline-style", "",
                      csp_allows_inline(csp, TILEFINCH_CSP_STYLE_SRC_ELEM,
                                        element));
}

static bool csp_allows_inline_attribute(
    const TilefinchContentSecurityPolicy *csp,
    TilefinchCspDirective directive)
{
    if (csp == NULL || !csp->header_present) return true;
    if (!csp->valid) return false;
    for (size_t i = 0; i < csp->policy_count; i++) {
        const TilefinchCspDirectiveValue *value = directive_for(
            &csp->policies[i], directive, true);
        if (value == NULL) continue;
        InlineMatch match = {0};
        size_t count = 0;
        (void) visit_tokens(
            csp, value, inline_token_matches, &match, &count);
        if (count == 0 || match.has_nonce_or_hash_source
            || (match.has_strict_dynamic
                && directive == TILEFINCH_CSP_SCRIPT_SRC_ATTR)
            || !match.has_unsafe_inline) return false;
    }
    return true;
}

bool tilefinch_csp_allows_script_attribute(
    const TilefinchContentSecurityPolicy *csp)
{
    return csp_allows_inline_attribute(csp, TILEFINCH_CSP_SCRIPT_SRC_ATTR);
}

bool tilefinch_csp_allows_style_attribute(
    const TilefinchContentSecurityPolicy *csp)
{
    return csp_allows_inline_attribute(csp, TILEFINCH_CSP_STYLE_SRC_ATTR);
}

bool tilefinch_csp_allows_base_uri(
    const TilefinchContentSecurityPolicy *csp, const char *target_url)
{
    return csp_allows_url(csp, TILEFINCH_CSP_BASE_URI, false, target_url);
}

bool tilefinch_csp_allows_form_action(
    const TilefinchContentSecurityPolicy *csp, const char *target_url)
{
    return csp_allows_url(csp, TILEFINCH_CSP_FORM_ACTION, false, target_url);
}

bool tilefinch_csp_has_frame_ancestors(
    const TilefinchContentSecurityPolicy *csp)
{
    if (csp == NULL || !csp->header_present || !csp->valid) return false;
    for (size_t i = 0; i < csp->policy_count; i++) {
        if (csp->policies[i]
                .directives[TILEFINCH_CSP_FRAME_ANCESTORS].present) {
            return true;
        }
    }
    return false;
}

bool tilefinch_csp_allows_ancestor(
    const TilefinchContentSecurityPolicy *csp, const char *ancestor_url)
{
    return csp_allows_url(csp, TILEFINCH_CSP_FRAME_ANCESTORS, false,
                          ancestor_url);
}

bool tilefinch_frame_embedding_allowed(
    const TilefinchContentSecurityPolicy *csp,
    const char *response_url, const char *ancestor_url,
    const char *headers, size_t headers_length)
{
    if (csp == NULL || response_url == NULL || ancestor_url == NULL
        || (headers == NULL && headers_length != 0) || !csp->valid) {
        return false;
    }
    if (tilefinch_csp_has_frame_ancestors(csp)) {
        return tilefinch_csp_allows_ancestor(csp, ancestor_url);
    }
    bool saw_same_origin = false;
    size_t offset = 0;
    while (offset < headers_length) {
        const char *line = headers + offset;
        const char *newline = memchr(line, '\n', headers_length - offset);
        size_t line_length = newline == NULL
            ? headers_length - offset : (size_t) (newline - line);
        const char *colon = memchr(line, ':', line_length);
        if (colon != NULL
            && span_equal_ci(line, (size_t) (colon - line),
                             "x-frame-options")) {
            const char *at = colon + 1;
            const char *end = line + line_length;
            while (at < end) {
                while (at < end
                       && (isspace((unsigned char) *at) || *at == ',')) at++;
                const char *token_end = at;
                while (token_end < end && *token_end != ',') token_end++;
                const char *trimmed_end = token_end;
                while (trimmed_end > at
                       && isspace((unsigned char) trimmed_end[-1])) {
                    trimmed_end--;
                }
                size_t length = (size_t) (trimmed_end - at);
                if (span_equal_ci(at, length, "deny")) return false;
                if (span_equal_ci(at, length, "sameorigin")) {
                    saw_same_origin = true;
                }
                at = token_end < end ? token_end + 1 : end;
            }
        }
        offset += line_length + (newline == NULL ? 0u : 1u);
    }
    return !saw_same_origin
        || tilefinch_url_same_origin(response_url, ancestor_url);
}
