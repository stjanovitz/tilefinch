#include "tilefinch/url.h"

#include "tilefinch/public_suffix.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

static bool ascii_equal_ci(const char *left, const char *right, size_t length)
{
    for (size_t i = 0; i < length; i++) {
        if (tolower((unsigned char) left[i])
            != tolower((unsigned char) right[i])) return false;
    }
    return true;
}

static bool scheme_character(unsigned char value, bool first)
{
    return first ? isalpha(value) != 0
        : isalnum(value) != 0 || value == '+' || value == '-' || value == '.';
}

static bool host_character(unsigned char value)
{
    return isalnum(value) != 0 || value == '-' || value == '.'
        || value == '_' || value >= 0x80;
}

static uint16_t default_port(TilefinchUrlScheme scheme)
{
    return scheme == TILEFINCH_URL_SCHEME_HTTPS ? 443 : 80;
}

static bool ipv4_tail_valid(const char *value, size_t start, size_t end)
{
    size_t part = 0;
    size_t at = start;
    while (at < end) {
        if (part == 4) return false;
        size_t digits = 0;
        unsigned value_part = 0;
        while (at < end && value[at] != '.') {
            unsigned char byte = (unsigned char) value[at++];
            if (!isdigit(byte) || digits == 3) return false;
            value_part = value_part * 10u + (unsigned) (byte - '0');
            digits++;
        }
        if (digits == 0 || value_part > 255u) return false;
        part++;
        if (at < end) at++;
    }
    return part == 4;
}

static bool ipv6_literal_valid(const char *value, size_t start, size_t end)
{
    size_t groups = 0;
    size_t at = start;
    bool compressed = false;
    if (at < end && value[at] == ':') {
        if (at + 1 >= end || value[at + 1] != ':') return false;
        compressed = true;
        at += 2;
        if (at == end) return true;
    }
    while (at < end) {
        size_t token_start = at;
        while (at < end && value[at] != ':') at++;
        size_t token_end = at;
        if (token_end == token_start) return false;
        bool ipv4_tail = false;
        for (size_t i = token_start; i < token_end; i++) {
            if (value[i] == '.') ipv4_tail = true;
        }
        if (ipv4_tail) {
            if (at != end || groups > 6
                || !ipv4_tail_valid(value, token_start, token_end)) {
                return false;
            }
            groups += 2;
            break;
        }
        if (token_end - token_start > 4) return false;
        for (size_t i = token_start; i < token_end; i++) {
            if (!isxdigit((unsigned char) value[i])) return false;
        }
        if (++groups > 8) return false;
        if (at == end) break;
        if (at + 1 < end && value[at + 1] == ':') {
            if (compressed) return false;
            compressed = true;
            at += 2;
            if (at == end) break;
        } else {
            at++;
            if (at == end) return false;
        }
    }
    return compressed ? groups < 8 : groups == 8;
}

bool tilefinch_url_parse(const char *value, TilefinchUrl *url)
{
    if (value == NULL || url == NULL) return false;
    memset(url, 0, sizeof(*url));
    size_t length = strlen(value);
    if (length == 0 || length >= TILEFINCH_URL_SERIALIZED_LIMIT) return false;
    for (size_t i = 0; i < length; i++) {
        unsigned char byte = (unsigned char) value[i];
        /* Encoding/IDNA is not silently guessed by this bounded layer.  Page
           URLs must already be ASCII serialized; reject whitespace, control
           bytes, and raw Unicode hosts instead of deriving ambiguous origins. */
        if (byte <= 0x20 || byte == 0x7f || byte >= 0x80) return false;
    }
    size_t colon = 0;
    while (colon < length && value[colon] != ':') {
        if (!scheme_character((unsigned char) value[colon], colon == 0))
            return false;
        colon++;
    }
    if (colon == 0 || colon + 2 >= length || value[colon] != ':'
        || value[colon + 1] != '/' || value[colon + 2] != '/') return false;
    TilefinchUrlScheme scheme = TILEFINCH_URL_SCHEME_INVALID;
    if (colon == 4 && ascii_equal_ci(value, "http", 4))
        scheme = TILEFINCH_URL_SCHEME_HTTP;
    else if (colon == 5 && ascii_equal_ci(value, "https", 5))
        scheme = TILEFINCH_URL_SCHEME_HTTPS;
    else return false;

    size_t authority = colon + 3;
    size_t authority_end = authority;
    while (authority_end < length && value[authority_end] != '/'
           && value[authority_end] != '?' && value[authority_end] != '#') {
        authority_end++;
    }
    if (authority_end == authority) return false;
    size_t host_start = authority;
    for (size_t i = authority; i < authority_end; i++) {
        if (value[i] == '@') host_start = i + 1;
    }
    /* Userinfo is never a page-visible identity and must not be silently
       stripped into diagnostics/history or leaked as an HTTP credential. */
    if (host_start != authority) return false;
    if (host_start == authority_end) return false;
    size_t host_end = authority_end;
    size_t port_start = authority_end;
    bool ipv6 = value[host_start] == '[';
    if (ipv6) {
        size_t close = host_start + 1;
        while (close < authority_end && value[close] != ']') close++;
        if (close == authority_end || close == host_start + 1
            || !ipv6_literal_valid(value, host_start + 1, close)) return false;
        host_end = close + 1;
        if (host_end < authority_end) {
            if (value[host_end] != ':') return false;
            port_start = host_end + 1;
        }
    } else {
        size_t last_colon = authority_end;
        for (size_t i = host_start; i < authority_end; i++) {
            if (value[i] == ':') last_colon = i;
        }
        if (last_colon < authority_end) {
            host_end = last_colon;
            port_start = last_colon + 1;
        }
        if (host_end == host_start || host_end - host_start > 253) return false;
        if (value[host_start] == '.' || value[host_end - 1] == '.') return false;
        bool previous_dot = false;
        for (size_t i = host_start; i < host_end; i++) {
            unsigned char byte = (unsigned char) value[i];
            if (byte >= 0x80 || !host_character(byte)) return false;
            if (byte == '.' && previous_dot) return false;
            previous_dot = byte == '.';
        }
    }
    bool explicit_port = port_start < authority_end;
    unsigned long port = default_port(scheme);
    if (explicit_port) {
        if (port_start == authority_end) return false;
        port = 0;
        for (size_t i = port_start; i < authority_end; i++) {
            unsigned char byte = (unsigned char) value[i];
            if (!isdigit(byte)) return false;
            unsigned long digit = (unsigned long) (byte - '0');
            if (port > (65535ul - digit) / 10ul) return false;
            port = port * 10ul + digit;
        }
        if (port == 0) return false;
    }

    size_t fragment = length;
    for (size_t i = authority_end; i < length; i++) {
        if (value[i] == '#') { fragment = i; break; }
    }
    size_t query = fragment;
    for (size_t i = authority_end; i < fragment; i++) {
        if (value[i] == '?') { query = i; break; }
    }
    size_t path_end = query < fragment ? query : fragment;
    url->value = value;
    url->length = length;
    url->scheme = scheme;
    url->scheme_length = colon;
    url->authority_offset = authority;
    url->authority_length = authority_end - authority;
    url->host_offset = host_start;
    url->host_length = host_end - host_start;
    url->path_offset = authority_end;
    url->path_length = path_end - authority_end;
    url->query_offset = query < fragment ? query + 1 : length;
    url->query_length = query < fragment ? fragment - query - 1 : 0;
    url->fragment_offset = fragment < length ? fragment + 1 : length;
    url->fragment_length = fragment < length ? length - fragment - 1 : 0;
    url->port = (uint16_t) port;
    url->explicit_port = explicit_port;
    url->ipv6_literal = ipv6;
    url->has_query = query < fragment;
    url->has_fragment = fragment < length;
    return true;
}

bool tilefinch_url_is_secure(const TilefinchUrl *url)
{
    return url != NULL && url->scheme == TILEFINCH_URL_SCHEME_HTTPS;
}

bool tilefinch_url_potentially_trustworthy(const char *value)
{
    TilefinchUrl url;
    if (!tilefinch_url_parse(value, &url)) return false;
    if (url.scheme == TILEFINCH_URL_SCHEME_HTTPS) return true;
    const char *host = value + url.host_offset;
    size_t length = url.host_length;
    if (url.ipv6_literal) {
        return length == 5 && ascii_equal_ci(host, "[::1]", 5);
    }
    if ((length == 9 && ascii_equal_ci(host, "localhost", 9))
        || (length > 10 && host[length - 10] == '.'
            && ascii_equal_ci(host + length - 9, "localhost", 9))) {
        return true;
    }
    /* Restrict the development exception to a syntactically valid IANA
       loopback /8 address, not private networks or names beginning "127.". */
    return length >= 7 && host[0] == '1' && host[1] == '2'
        && host[2] == '7' && host[3] == '.'
        && ipv4_tail_valid(value, url.host_offset,
                           url.host_offset + url.host_length);
}

static bool same_origin_view(const TilefinchUrl *left, const TilefinchUrl *right)
{
    return left->scheme == right->scheme && left->port == right->port
        && left->host_length == right->host_length
        && ascii_equal_ci(left->value + left->host_offset,
                          right->value + right->host_offset,
                          left->host_length);
}

bool tilefinch_url_same_origin(const char *left, const char *right)
{
    TilefinchUrl left_url, right_url;
    return tilefinch_url_parse(left, &left_url)
        && tilefinch_url_parse(right, &right_url)
        && same_origin_view(&left_url, &right_url);
}

bool tilefinch_url_upgrade_to_https(const char *value, char *output,
                                    size_t output_size)
{
    TilefinchUrl url;
    if (value == NULL || output == NULL || output_size == 0
        || !tilefinch_url_parse(value, &url)
        || url.scheme != TILEFINCH_URL_SCHEME_HTTP) return false;
    const char *host = value + url.host_offset;
    size_t host_length = url.host_length;
    size_t suffix_offset = url.path_offset;
    if (suffix_offset > url.length) return false;
    char candidate[TILEFINCH_URL_SERIALIZED_LIMIT];
    int written;
    if (!url.explicit_port || url.port == 80u) {
        written = snprintf(candidate, sizeof(candidate), "https://%.*s%s",
                           (int) host_length, host,
                           value + suffix_offset);
    } else {
        written = snprintf(candidate, sizeof(candidate), "https://%.*s:%u%s",
                           (int) host_length, host, (unsigned) url.port,
                           value + suffix_offset);
    }
    return written > 0 && (size_t) written < sizeof(candidate)
        && tilefinch_url_normalize(candidate, output, output_size);
}

bool tilefinch_url_is_downgrade(const char *source, const char *target)
{
    TilefinchUrl source_url, target_url;
    return tilefinch_url_parse(source, &source_url)
        && tilefinch_url_parse(target, &target_url)
        && source_url.scheme == TILEFINCH_URL_SCHEME_HTTPS
        && target_url.scheme == TILEFINCH_URL_SCHEME_HTTP;
}

static bool append_bytes(char *output, size_t output_size, size_t *used,
                         const char *value, size_t length, bool lower)
{
    if (length >= output_size - *used) return false;
    for (size_t i = 0; i < length; i++) {
        unsigned char byte = (unsigned char) value[i];
        output[(*used)++] = lower ? (char) tolower(byte) : (char) byte;
    }
    output[*used] = '\0';
    return true;
}

bool tilefinch_url_origin(const char *value, char *output, size_t output_size)
{
    TilefinchUrl url;
    if (output == NULL || output_size == 0) return false;
    output[0] = '\0';
    if (!tilefinch_url_parse(value, &url)) return false;
    size_t used = 0;
    const char *scheme = url.scheme == TILEFINCH_URL_SCHEME_HTTPS
                         ? "https://" : "http://";
    if (!append_bytes(output, output_size, &used, scheme, strlen(scheme), false)
        || !append_bytes(output, output_size, &used,
                         value + url.host_offset, url.host_length, true)) {
        return false;
    }
    if (url.port != default_port(url.scheme)) {
        int written = snprintf(output + used, output_size - used, ":%u",
                               (unsigned) url.port);
        if (written < 0 || (size_t) written >= output_size - used) return false;
    }
    return true;
}

typedef struct {
    const char *value;
    size_t length;
} PathSegment;

static bool percent_dot(const char *value, size_t length)
{
    return length == 3 && value[0] == '%' && value[1] == '2'
        && (value[2] == 'e' || value[2] == 'E');
}

static bool single_dot_segment(const char *value, size_t length)
{
    return (length == 1 && value[0] == '.') || percent_dot(value, length);
}

static bool double_dot_segment(const char *value, size_t length)
{
    return (length == 2 && value[0] == '.' && value[1] == '.')
        || (length == 4
            && ((value[0] == '.' && percent_dot(value + 1, 3))
                || (percent_dot(value, 3) && value[3] == '.')))
        || (length == 6 && percent_dot(value, 3)
            && percent_dot(value + 3, 3));
}

static bool append_normalized_path(char *output, size_t output_size,
                                   size_t *used, const char *path,
                                   size_t length)
{
    /* HTTP(S) URLs always have an absolute path. Keep empty path segments:
       servers can distinguish /a//b from /a/b, so collapsing them would merge
       cache and request identities. Only special-URL dot segments fold. */
    PathSegment segments[256];
    size_t segment_count = 0;
    if (length == 0) {
        return append_bytes(output, output_size, used, "/", 1, false);
    }
    if (path[0] != '/' && path[0] != '\\') return false;
    size_t cursor = 1;
    while (cursor <= length) {
        size_t start = cursor;
        while (cursor < length && path[cursor] != '/' && path[cursor] != '\\')
            cursor++;
        size_t part_length = cursor - start;
        bool last = cursor == length;
        if (single_dot_segment(path + start, part_length)) {
            if (last) {
                if (segment_count >= sizeof(segments) / sizeof(segments[0]))
                    return false;
                segments[segment_count++] = (PathSegment) {path + cursor, 0};
            }
        } else if (double_dot_segment(path + start, part_length)) {
            if (segment_count != 0) segment_count--;
            if (last) {
                if (segment_count >= sizeof(segments) / sizeof(segments[0]))
                    return false;
                segments[segment_count++] = (PathSegment) {path + cursor, 0};
            }
        } else {
            if (segment_count >= sizeof(segments) / sizeof(segments[0]))
                return false;
            segments[segment_count++] = (PathSegment) {
                path + start, part_length
            };
        }
        if (last) break;
        cursor++;
    }
    if (!append_bytes(output, output_size, used, "/", 1, false)) return false;
    for (size_t i = 0; i < segment_count; i++) {
        if (i != 0
            && !append_bytes(output, output_size, used, "/", 1, false)) {
            return false;
        }
        if (!append_bytes(output, output_size, used, segments[i].value,
                          segments[i].length, false)) return false;
    }
    return true;
}

bool tilefinch_url_normalize(const char *value, char *output,
                          size_t output_size)
{
    TilefinchUrl url;
    char origin[TILEFINCH_ORIGIN_SERIALIZED_LIMIT];
    if (output == NULL || output_size == 0 || !tilefinch_url_parse(value, &url)
        || !tilefinch_url_origin(value, origin, sizeof(origin))) return false;
    size_t used = 0;
    output[0] = '\0';
    if (!append_bytes(output, output_size, &used, origin, strlen(origin), false)
        || !append_normalized_path(output, output_size, &used,
                                   value + url.path_offset,
                                   url.path_length)) return false;
    if (url.has_query) {
        if (!append_bytes(output, output_size, &used, "?", 1, false)
            || !append_bytes(output, output_size, &used,
                             value + url.query_offset, url.query_length,
                             false)) return false;
    }
    if (url.has_fragment) {
        if (!append_bytes(output, output_size, &used, "#", 1, false)
            || !append_bytes(output, output_size, &used,
                             value + url.fragment_offset,
                             url.fragment_length, false)) return false;
    }
    return true;
}

bool tilefinch_url_request_key(const char *value, char *output,
                            size_t output_size)
{
    if (!tilefinch_url_normalize(value, output, output_size)) return false;
    char *fragment = strchr(output, '#');
    if (fragment != NULL) *fragment = '\0';
    return true;
}

static bool looks_absolute(const char *reference)
{
    if (reference == NULL || !isalpha((unsigned char) reference[0]))
        return false;
    for (size_t i = 1; reference[i] != '\0'; i++) {
        unsigned char byte = (unsigned char) reference[i];
        if (byte == ':') return true;
        if (!scheme_character(byte, false)) return false;
    }
    return false;
}

bool tilefinch_url_resolve(const char *base, const char *reference,
                        char *output, size_t output_size)
{
    TilefinchUrl base_url;
    if (base == NULL || reference == NULL || output == NULL || output_size == 0
        || !tilefinch_url_parse(base, &base_url)) return false;
    if (reference[0] == '\0') {
        /* A relative URL with an empty input inherits path and query, but not
           the base fragment. Keeping the fragment here would make href=""
           resolve differently from the URL Standard and could preserve a
           stale in-page target across a real navigation. */
        size_t base_without_fragment = base_url.has_fragment
            ? base_url.fragment_offset - 1 : base_url.length;
        char inherited[TILEFINCH_URL_SERIALIZED_LIMIT];
        if (base_without_fragment >= sizeof(inherited)) return false;
        memcpy(inherited, base, base_without_fragment);
        inherited[base_without_fragment] = '\0';
        return tilefinch_url_normalize(inherited, output, output_size);
    }
    if (looks_absolute(reference))
        return tilefinch_url_normalize(reference, output, output_size);
    char assembled[TILEFINCH_URL_SERIALIZED_LIMIT];
    size_t used = 0;
    assembled[0] = '\0';
    if (reference[0] == '/' && reference[1] == '/') {
        const char *scheme = base_url.scheme == TILEFINCH_URL_SCHEME_HTTPS
                             ? "https:" : "http:";
        if (!append_bytes(assembled, sizeof(assembled), &used,
                          scheme, strlen(scheme), false)
            || !append_bytes(assembled, sizeof(assembled), &used,
                             reference, strlen(reference), false)) return false;
        return tilefinch_url_normalize(assembled, output, output_size);
    }
    size_t base_without_fragment = base_url.has_fragment
        ? base_url.fragment_offset - 1 : base_url.length;
    if (reference[0] == '#') {
        if (!append_bytes(assembled, sizeof(assembled), &used, base,
                          base_without_fragment, false)
            || !append_bytes(assembled, sizeof(assembled), &used, reference,
                             strlen(reference), false)) return false;
        return tilefinch_url_normalize(assembled, output, output_size);
    }
    size_t base_without_query = base_url.has_query
        ? base_url.query_offset - 1 : base_without_fragment;
    if (reference[0] == '?') {
        if (!append_bytes(assembled, sizeof(assembled), &used, base,
                          base_without_query, false)
            || !append_bytes(assembled, sizeof(assembled), &used, reference,
                             strlen(reference), false)) return false;
        return tilefinch_url_normalize(assembled, output, output_size);
    }
    char origin[TILEFINCH_ORIGIN_SERIALIZED_LIMIT];
    if (!tilefinch_url_origin(base, origin, sizeof(origin))
        || !append_bytes(assembled, sizeof(assembled), &used, origin,
                         strlen(origin), false)) return false;
    if (reference[0] == '/' || reference[0] == '\\') {
        if (!append_bytes(assembled, sizeof(assembled), &used, reference,
                          strlen(reference), false)) return false;
    } else {
        size_t directory_length = base_url.path_length;
        while (directory_length != 0
               && base[base_url.path_offset + directory_length - 1] != '/'
               && base[base_url.path_offset + directory_length - 1] != '\\') {
            directory_length--;
        }
        if (directory_length == 0
            && !append_bytes(assembled, sizeof(assembled), &used,
                             "/", 1, false)) return false;
        if (directory_length != 0
            && !append_bytes(assembled, sizeof(assembled), &used,
                             base + base_url.path_offset,
                             directory_length, false)) return false;
        if (!append_bytes(assembled, sizeof(assembled), &used, reference,
                          strlen(reference), false)) return false;
    }
    return tilefinch_url_normalize(assembled, output, output_size);
}

bool tilefinch_url_site_key(const char *value, char *output, size_t output_size)
{
    if (value == NULL || output == NULL || output_size == 0) return false;
    TilefinchUrl url;
    if (!tilefinch_url_parse(value, &url) || url.host_length > 253) return false;
    char host[254];
    for (size_t i = 0; i < url.host_length; i++) {
        host[i] = (char) tolower(
            (unsigned char) value[url.host_offset + i]);
    }
    host[url.host_length] = '\0';
    char registrable[254];
    const char *site_host = tilefinch_registrable_domain(
        host, registrable, sizeof(registrable)) ? registrable : host;
    const char *scheme = url.scheme == TILEFINCH_URL_SCHEME_HTTPS
                         ? "https" : "http";
    int written = snprintf(output, output_size, "%s://%s", scheme,
                           site_host);
    return written > 0 && (size_t) written < output_size;
}
