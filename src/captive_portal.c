#include "tilefinch/captive_portal.h"

#include <ctype.h>
#include <string.h>
#include <strings.h>

static bool content_type_html(const char *value)
{
    if (value == NULL) return false;
    while (isspace((unsigned char) *value)) value++;
    static const char html[] = "text/html";
    static const char xhtml[] = "application/xhtml+xml";
    return strncasecmp(value, html, sizeof(html) - 1u) == 0
        || strncasecmp(value, xhtml, sizeof(xhtml) - 1u) == 0;
}

static bool body_looks_html(const unsigned char *body, size_t length)
{
    if (body == NULL || length == 0) return false;
    size_t inspected = length < 256u ? length : 256u;
    size_t at = 0;
    if (inspected >= 3u && body[0] == 0xefu && body[1] == 0xbbu
        && body[2] == 0xbfu) at = 3u;
    while (at < inspected && isspace(body[at])) at++;
    if (at >= inspected || body[at] != '<') return false;
    at++;
    while (at < inspected && isspace(body[at])) at++;
    static const char *const starts[] = {
        "!doctype", "html", "head", "body", "form"
    };
    for (size_t i = 0; i < sizeof(starts) / sizeof(starts[0]); i++) {
        size_t wanted = strlen(starts[i]);
        if (wanted <= inspected - at
            && strncasecmp((const char *) body + at, starts[i], wanted) == 0) {
            return true;
        }
    }
    return false;
}

TilefinchCaptiveProbeResult tilefinch_captive_probe_classify(
    const TilefinchCaptiveProbeResponse *response)
{
    if (response == NULL) return TILEFINCH_CAPTIVE_PROBE_FAILED;
    if (!response->transport_succeeded) {
        return response->timed_out ? TILEFINCH_CAPTIVE_PROBE_RETRYABLE
                                   : TILEFINCH_CAPTIVE_PROBE_FAILED;
    }
    if (response->status_code == 204 && response->body_length == 0)
        return TILEFINCH_CAPTIVE_PROBE_INTERNET;
    if (response->status_code >= 300 && response->status_code < 400
        && response->location != NULL && response->location[0] != '\0') {
        return TILEFINCH_CAPTIVE_PROBE_PORTAL;
    }
    if (response->status_code == 511
        || content_type_html(response->content_type)
        || body_looks_html(response->body, response->body_length)
        || (response->status_code >= 200 && response->status_code < 300
            && response->body_length != 0)) {
        return TILEFINCH_CAPTIVE_PROBE_PORTAL;
    }
    return response->status_code >= 500
        ? TILEFINCH_CAPTIVE_PROBE_RETRYABLE
        : TILEFINCH_CAPTIVE_PROBE_FAILED;
}

bool tilefinch_captive_probe_portal_url(
    const char *probe_url, const char *location,
    char output[TILEFINCH_URL_SERIALIZED_LIMIT])
{
    if (probe_url == NULL || output == NULL) return false;
    const char *candidate = probe_url;
    char resolved[TILEFINCH_URL_SERIALIZED_LIMIT];
    if (location != NULL && location[0] != '\0') {
        if (!tilefinch_url_resolve(
                probe_url, location, resolved, sizeof(resolved))) return false;
        candidate = resolved;
    }
    TilefinchUrl parsed;
    if (!tilefinch_url_parse(candidate, &parsed)
        || (parsed.scheme != TILEFINCH_URL_SCHEME_HTTP
            && parsed.scheme != TILEFINCH_URL_SCHEME_HTTPS)) return false;
    return tilefinch_url_normalize(
        candidate, output, TILEFINCH_URL_SERIALIZED_LIMIT);
}
