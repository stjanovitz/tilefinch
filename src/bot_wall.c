#include "tilefinch/bot_wall.h"

#include <stdio.h>
#include <string.h>
#include <strings.h>

#include "tilefinch/url.h"

bool tilefinch_bot_wall_status(long status)
{
    return status == 403 || status == 405
           || status == 429 || status == 503;
}

static bool bot_wall_contains(const char *text, size_t length,
                              const char *needle)
{
    size_t needle_length = strlen(needle);
    if (text == NULL || needle_length == 0 || length < needle_length) {
        return false;
    }
    for (size_t at = 0; at + needle_length <= length; at++) {
        if (strncasecmp(text + at, needle, needle_length) == 0) return true;
    }
    return false;
}

/* Whether a header whose name starts with `prefix` is present (and, when
   `value` is not NULL, whether its value contains it). */
static bool bot_wall_header(const char *headers, size_t length,
                            const char *prefix, const char *value)
{
    size_t prefix_length = strlen(prefix);
    size_t offset = 0;
    while (headers != NULL && offset < length) {
        const char *line = headers + offset;
        const char *newline = memchr(line, '\n', length - offset);
        size_t line_length = newline == NULL
            ? length - offset : (size_t) (newline - line);
        const char *colon = memchr(line, ':', line_length);
        if (colon != NULL && (size_t) (colon - line) >= prefix_length
            && strncasecmp(line, prefix, prefix_length) == 0
            && (value == NULL
                || bot_wall_contains(
                       colon + 1,
                       line_length - (size_t) (colon + 1 - line),
                       value))) {
            return true;
        }
        offset += line_length + (newline == NULL ? 0 : 1);
    }
    return false;
}

const char *tilefinch_bot_wall_header_vendor(
    long status, const char *server, const char *cf_mitigated,
    const char *headers, size_t headers_length)
{
    size_t server_length = server == NULL ? 0 : strlen(server);
    /* AWS WAF names its action on every challenge and CAPTCHA answer,
       including the 202 of a silent challenge. */
    if ((status == 202 || tilefinch_bot_wall_status(status))
        && (bot_wall_header(headers, headers_length,
                            "x-amzn-waf-action", "captcha")
            || bot_wall_header(headers, headers_length,
                               "x-amzn-waf-action", "challenge"))) {
        return "AWS WAF";
    }
    if (!tilefinch_bot_wall_status(status)) return NULL;
    if (bot_wall_contains(server, server_length, "datadome")
        || bot_wall_header(headers, headers_length, "x-datadome", NULL)
        || bot_wall_header(headers, headers_length, "x-dd-b", NULL)) {
        return "DataDome";
    }
    if ((cf_mitigated != NULL && strcasecmp(cf_mitigated, "challenge") == 0)
        || bot_wall_header(headers, headers_length,
                           "cf-mitigated", "challenge")) {
        return "Cloudflare";
    }
    if (bot_wall_header(headers, headers_length, "x-kpsdk-", NULL)) {
        return "Kasada";
    }
    if (bot_wall_header(headers, headers_length, "x-iinfo", NULL)
        || bot_wall_header(headers, headers_length, "x-cdn", "imperva")
        || bot_wall_header(headers, headers_length, "x-cdn", "incapsula")) {
        return "Imperva";
    }
    if (bot_wall_header(headers, headers_length, "x-px-", NULL)) {
        return "HUMAN";
    }
    return NULL;
}

const char *tilefinch_bot_wall_title_vendor(long status, const char *title)
{
    if (!tilefinch_bot_wall_status(status) || title == NULL) return NULL;
    static const struct {
        const char *title;
        const char *vendor;
    } titles[] = {
        {"Attention Required! | Cloudflare", "Cloudflare"},
        {"Just a moment...", "Cloudflare"},
        {"Access to this page has been denied", "HUMAN"},
        {"Pardon Our Interruption", "Imperva"},
        {"You have been blocked", "DataDome"},
        {"Access Denied", "Akamai"}
    };
    while (*title == ' ' || *title == '\t' || *title == '\n') title++;
    for (size_t i = 0; i < sizeof(titles) / sizeof(titles[0]); i++) {
        if (strncasecmp(title, titles[i].title,
                        strlen(titles[i].title)) == 0) {
            return titles[i].vendor;
        }
    }
    return NULL;
}

bool tilefinch_bot_wall_site(const char *url, char *site, size_t capacity)
{
    if (site == NULL || capacity == 0) return false;
    site[0] = '\0';
    TilefinchUrl parsed;
    if (url == NULL || !tilefinch_url_parse(url, &parsed)) return false;
    const char *host = url + parsed.host_offset;
    size_t length = parsed.host_length;
    if (length > 4 && strncasecmp(host, "www.", 4) == 0) {
        host += 4;
        length -= 4;
    }
    if (length == 0 || length >= capacity) return false;
    memcpy(site, host, length);
    site[length] = '\0';
    return true;
}

bool tilefinch_bot_wall_notice(const char *site, char *output,
                               size_t capacity)
{
    if (output == NULL || capacity == 0) return false;
    output[0] = '\0';
    const char *name = site == NULL || site[0] == '\0' ? "This site" : site;
    /* A host longer than 22 characters is shortened so the notice stays
       within the 80-byte chrome status and its headline on one line. */
    bool shorten = strlen(name) > 22;
    int written = snprintf(output, capacity,
                           "%.*s%s blocked Tilefinch\n"
                           "Bot protection: this site may not work",
                           shorten ? 19 : 22, name, shorten ? "..." : "");
    return written > 0 && (size_t) written < capacity;
}
