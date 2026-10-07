#include "tilefinch/site_identity.h"

#include "tilefinch/url.h"

#include <stdio.h>
#include <string.h>
#include <strings.h>

/* One browsing session per process: the opt-in lasts until the browser
   restarts. The PSP frontend is single-threaded for navigation, and every
   reader runs on the browser thread. */
static struct {
    bool google_opted_in;
    bool google_refusal_note;
    char google_fallback[TILEFINCH_URL_SERIALIZED_LIMIT];
} site_identity_state;

static bool site_identity_parse_google(const char *url, TilefinchUrl *parsed)
{
    if (url == NULL || !tilefinch_url_parse(url, parsed)
        || parsed->explicit_port || parsed->ipv6_literal) return false;
    const char *host = parsed->value + parsed->host_offset;
    size_t length = parsed->host_length;
    return (length == sizeof("google.com") - 1u
            && strncasecmp(host, "google.com", length) == 0)
        || (length == sizeof("www.google.com") - 1u
            && strncasecmp(host, "www.google.com", length) == 0);
}

bool site_identity_google_host(const char *url)
{
    TilefinchUrl parsed;
    return site_identity_parse_google(url, &parsed);
}

static bool site_identity_path_is(const TilefinchUrl *parsed,
                                  const char *path)
{
    size_t length = strlen(path);
    return parsed->path_length == length
        && memcmp(parsed->value + parsed->path_offset, path, length) == 0;
}

bool site_identity_google_search_url(const char *url)
{
    TilefinchUrl parsed;
    return site_identity_parse_google(url, &parsed)
        && site_identity_path_is(&parsed, "/search");
}

bool site_identity_google_opted_in(void)
{
    return site_identity_state.google_opted_in;
}

void site_identity_google_opt_in(void)
{
    site_identity_state.google_opted_in = true;
    site_identity_state.google_refusal_note = false;
}

static bool site_identity_marker_component(const char *at, size_t length)
{
    static const char name[] = SITE_IDENTITY_GOOGLE_OPT_IN_MARKER;
    size_t name_length = sizeof(name) - 1u;
    return length >= name_length && memcmp(at, name, name_length) == 0
        && (length == name_length || at[name_length] == '=');
}

bool site_identity_take_google_opt_in_marker(
    const char *url, char *output, size_t capacity)
{
    TilefinchUrl parsed;
    if (output == NULL || capacity == 0u
        || !site_identity_parse_google(url, &parsed)
        || !site_identity_path_is(&parsed, "/search")
        || !parsed.has_query) return false;
    const char *query = parsed.value + parsed.query_offset;
    size_t left = parsed.query_length;
    bool marked = false;
    for (const char *at = query; left != 0u;) {
        size_t span = 0u;
        while (span < left && at[span] != '&') span++;
        if (site_identity_marker_component(at, span)
            && span == sizeof(SITE_IDENTITY_GOOGLE_OPT_IN_MARKER "=1") - 1u
            && at[span - 1u] == '1') marked = true;
        if (span == left) break;
        at += span + 1u;
        left -= span + 1u;
    }
    if (!marked) return false;
    /* Rebuild prefix + surviving query components + fragment. */
    size_t used = parsed.query_offset - 1u;
    if (used >= capacity) return false;
    memcpy(output, parsed.value, used);
    bool first = true;
    left = parsed.query_length;
    for (const char *at = query; left != 0u;) {
        size_t span = 0u;
        while (span < left && at[span] != '&') span++;
        if (span != 0u && !site_identity_marker_component(at, span)) {
            if (used + 1u + span >= capacity) return false;
            output[used++] = first ? '?' : '&';
            memcpy(output + used, at, span);
            used += span;
            first = false;
        }
        if (span == left) break;
        at += span + 1u;
        left -= span + 1u;
    }
    if (parsed.has_fragment) {
        size_t fragment = parsed.fragment_length + 1u;
        if (used + fragment >= capacity) return false;
        memcpy(output + used, parsed.value + parsed.fragment_offset - 1u,
               fragment);
        used += fragment;
    }
    output[used] = '\0';
    site_identity_google_opt_in();
    return true;
}

const char *site_identity_user_agent_override(const char *document_url)
{
    return site_identity_state.google_opted_in
            && site_identity_google_host(document_url)
        ? TILEFINCH_GOOGLE_USER_AGENT : NULL;
}

static bool site_identity_google_sorry(const char *url)
{
    TilefinchUrl parsed;
    return site_identity_parse_google(url, &parsed)
        && parsed.path_length >= sizeof("/sorry/") - 1u
        && memcmp(parsed.value + parsed.path_offset, "/sorry/",
                  sizeof("/sorry/") - 1u) == 0;
}

bool site_identity_google_refusal(
    const char *request_url, const char *effective_url, long status,
    const char *content_type, bool failed)
{
    if (!site_identity_state.google_opted_in) return false;
    /* Google's scripts may also navigate to the interstitial directly. */
    if (site_identity_google_sorry(request_url)) return true;
    if (!site_identity_google_search_url(request_url)) return false;
    if (failed || status == 429) return true;
    if (effective_url != NULL && site_identity_google_sorry(effective_url))
        return true;
    /* Google answers a browser it does not support with an XHTML Mobile
       Profile page; its scripted search page is text/html. */
    return content_type != NULL
        && strncasecmp(content_type, "application/xhtml+xml",
                       sizeof("application/xhtml+xml") - 1u) == 0;
}

void site_identity_google_refused(const char *search_url)
{
    site_identity_state.google_opted_in = false;
    site_identity_state.google_refusal_note = true;
    site_identity_state.google_fallback[0] = '\0';
    if (site_identity_google_search_url(search_url)
        && strlen(search_url) < sizeof(site_identity_state.google_fallback))
        snprintf(site_identity_state.google_fallback,
                 sizeof(site_identity_state.google_fallback), "%s",
                 search_url);
}

bool site_identity_take_google_fallback(char *url, size_t capacity)
{
    if (site_identity_state.google_fallback[0] == '\0') return false;
    bool fits = url != NULL
        && strlen(site_identity_state.google_fallback) < capacity;
    if (fits)
        snprintf(url, capacity, "%s", site_identity_state.google_fallback);
    site_identity_state.google_fallback[0] = '\0';
    return fits;
}

void site_identity_clear_google_fallback(void)
{
    site_identity_state.google_fallback[0] = '\0';
}

bool site_identity_take_google_refusal_note(void)
{
    bool note = site_identity_state.google_refusal_note;
    site_identity_state.google_refusal_note = false;
    return note;
}

void site_identity_reset(void)
{
    memset(&site_identity_state, 0, sizeof(site_identity_state));
}
