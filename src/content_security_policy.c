#include "tilefinch/content_security_policy.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>

#include "tilefinch/document.h"
#include "tilefinch/sha256.h"
#include "tilefinch/url.h"

static const char *const csp_directive_names[TILEFINCH_CSP_DIRECTIVE_COUNT] = {
    "default-src", "script-src", "style-src", "img-src", "font-src",
    "connect-src", "frame-src", "object-src", "base-uri", "form-action",
    "frame-ancestors", "worker-src", "media-src"
};

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
} UrlMatch;

static bool scheme_matches(TilefinchUrlScheme source,
                           TilefinchUrlScheme target)
{
    return source == target
        || (source == TILEFINCH_URL_SCHEME_HTTP
            && target == TILEFINCH_URL_SCHEME_HTTPS);
}

static bool host_source_matches(const char *token, size_t length,
                                const UrlMatch *match)
{
    TilefinchUrl target;
    if (length == 0 || length >= 512
        || !tilefinch_url_parse(match->url, &target)) return false;
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
        TilefinchUrl document;
        if (!tilefinch_url_parse(match->csp->document_origin, &document)) {
            return false;
        }
        source_scheme = document.scheme;
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
        TilefinchUrl parsed;
        return tilefinch_url_parse(match->url, &parsed);
    }
    if (length == 5 && strncasecmp(token, "data:", 5) == 0) {
        return strncasecmp(match->url, "data:", 5) == 0;
    }
    if (length == 5 && strncasecmp(token, "blob:", 5) == 0) {
        return strncasecmp(match->url, "blob:", 5) == 0;
    }
    if (length == 6 && strncasecmp(token, "https:", 6) == 0) {
        TilefinchUrl parsed;
        return tilefinch_url_parse(match->url, &parsed)
            && parsed.scheme == TILEFINCH_URL_SCHEME_HTTPS;
    }
    if (length == 5 && strncasecmp(token, "http:", 5) == 0) {
        TilefinchUrl parsed;
        return tilefinch_url_parse(match->url, &parsed)
            && (parsed.scheme == TILEFINCH_URL_SCHEME_HTTP
                || parsed.scheme == TILEFINCH_URL_SCHEME_HTTPS);
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

bool tilefinch_csp_allows_request(
    const TilefinchContentSecurityPolicy *csp,
    TilefinchRequestDestination destination, const char *target_url)
{
    TilefinchCspDirective directive;
    switch (destination) {
        case TILEFINCH_DESTINATION_SCRIPT:
            directive = TILEFINCH_CSP_SCRIPT_SRC; break;
        case TILEFINCH_DESTINATION_STYLE:
            directive = TILEFINCH_CSP_STYLE_SRC; break;
        case TILEFINCH_DESTINATION_FONT:
            directive = TILEFINCH_CSP_FONT_SRC; break;
        case TILEFINCH_DESTINATION_IMAGE:
            directive = TILEFINCH_CSP_IMG_SRC; break;
        case TILEFINCH_DESTINATION_FETCH:
            directive = TILEFINCH_CSP_CONNECT_SRC; break;
        case TILEFINCH_DESTINATION_FRAME:
            directive = TILEFINCH_CSP_FRAME_SRC; break;
        case TILEFINCH_DESTINATION_MEDIA:
            directive = TILEFINCH_CSP_MEDIA_SRC; break;
        case TILEFINCH_DESTINATION_OTHER:
            directive = TILEFINCH_CSP_OBJECT_SRC; break;
        case TILEFINCH_DESTINATION_DOCUMENT:
            return true;
        default:
            return false;
    }
    return csp_allows_url(
        csp, directive, true, target_url);
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
        if (!policy_allows_url(csp, policy, directive, true, target_url))
            return false;
    }
    return true;
}

static bool unsafe_eval_token(const char *token, size_t length, void *opaque)
{
    (void) opaque;
    return span_equal_ci(token, length, "'unsafe-eval'");
}

bool tilefinch_csp_allows_dynamic_code(
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
    char hash[48];
    bool have_hash;
    bool has_nonce_or_hash_source;
    bool has_unsafe_inline;
} InlineMatch;

static bool inline_token_matches(const char *token, size_t length,
                                 void *opaque)
{
    InlineMatch *match = opaque;
    if ((length > 8 && strncasecmp(token, "'nonce-", 7) == 0)
        || (length > 10 && strncasecmp(token, "'sha256-", 8) == 0)) {
        match->has_nonce_or_hash_source = true;
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
    if (match->have_hash && length == strlen(match->hash) + 9u
        && strncasecmp(token, "'sha256-", 8) == 0
        && token[length - 1] == '\''
        && memcmp(token + 8, match->hash, strlen(match->hash)) == 0) {
        return true;
    }
    return false;
}

static void digest_base64(const uint8_t digest[32], char output[45])
{
    static const char alphabet[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t out = 0;
    for (size_t i = 0; i < 30; i += 3) {
        uint32_t word = ((uint32_t) digest[i] << 16)
            | ((uint32_t) digest[i + 1] << 8) | digest[i + 2];
        output[out++] = alphabet[(word >> 18) & 63u];
        output[out++] = alphabet[(word >> 12) & 63u];
        output[out++] = alphabet[(word >> 6) & 63u];
        output[out++] = alphabet[word & 63u];
    }
    uint32_t tail = (uint32_t) digest[30] << 16
        | (uint32_t) digest[31] << 8;
    output[out++] = alphabet[(tail >> 18) & 63u];
    output[out++] = alphabet[(tail >> 12) & 63u];
    output[out++] = alphabet[(tail >> 6) & 63u];
    output[out++] = '=';
    output[out] = '\0';
}

static bool inline_element_hash(struct lxb_dom_node *element,
                                char output[45])
{
    if (element == NULL) return false;
    TilefinchSha256 sha;
    tilefinch_sha256_init(&sha);
    for (struct lxb_dom_node *child = element->first_child; child != NULL;
         child = child->next) {
        size_t length = 0;
        const char *text = document_text_data(child, &length);
        if (text == NULL) continue;
        if (!tilefinch_sha256_update(
                &sha, (const uint8_t *) text, length)) return false;
    }
    uint8_t digest[32];
    /* Empty inline elements have the ordinary SHA-256 digest of zero bytes. */
    if (!tilefinch_sha256_final(&sha, digest)) return false;
    digest_base64(digest, output);
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
    const char *nonce = document_attribute(element, "nonce", &nonce_length);
    InlineMatch match = {.nonce = nonce, .nonce_length = nonce_length};
    match.have_hash = inline_element_hash(element, match.hash);
    size_t count = 0;
    if (visit_tokens(csp, value, inline_token_matches, &match, &count)) {
        return true;
    }
    if (count == 0 || match.has_nonce_or_hash_source) return false;
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
    return csp_allows_inline(csp, TILEFINCH_CSP_SCRIPT_SRC, element);
}

bool tilefinch_csp_allows_inline_style(
    const TilefinchContentSecurityPolicy *csp, struct lxb_dom_node *element)
{
    return csp_allows_inline(csp, TILEFINCH_CSP_STYLE_SRC, element);
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
            || !match.has_unsafe_inline) return false;
    }
    return true;
}

bool tilefinch_csp_allows_script_attribute(
    const TilefinchContentSecurityPolicy *csp)
{
    return csp_allows_inline_attribute(csp, TILEFINCH_CSP_SCRIPT_SRC);
}

bool tilefinch_csp_allows_style_attribute(
    const TilefinchContentSecurityPolicy *csp)
{
    return csp_allows_inline_attribute(csp, TILEFINCH_CSP_STYLE_SRC);
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
