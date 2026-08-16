#include "tilefinch/content_blocker.h"

#include "tilefinch/url.h"

#include <ctype.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#define BLOCKER_RULE_ALLOW UINT8_C(1)
#define BLOCKER_RULE_THIRD_PARTY UINT8_C(2)
#define BLOCKER_RULE_FIRST_PARTY UINT8_C(4)

#define BLOCKER_TYPE_SCRIPT UINT8_C(1)
#define BLOCKER_TYPE_IMAGE UINT8_C(2)
#define BLOCKER_TYPE_STYLE UINT8_C(4)
#define BLOCKER_TYPE_FONT UINT8_C(8)
#define BLOCKER_TYPE_MEDIA UINT8_C(16)
#define BLOCKER_TYPE_XHR UINT8_C(32)
#define BLOCKER_TYPE_FRAME UINT8_C(64)
#define BLOCKER_TYPE_OTHER UINT8_C(128)
#define BLOCKER_TYPE_ALL UINT8_C(255)

typedef struct {
    uint32_t host_offset;
    uint16_t host_length;
    uint8_t types;
    uint8_t flags;
} ContentBlockerRule;

struct ContentBlocker {
    Budget *budget;
    ContentBlockerRule *rules;
    char *strings;
    uint16_t *table;
    size_t rule_count;
    size_t string_bytes;
    size_t table_capacity;
    char *allowed_sites;
    uint16_t allowed_offsets[CONTENT_BLOCKER_ALLOW_SITE_LIMIT];
    uint8_t allowed_lengths[CONTENT_BLOCKER_ALLOW_SITE_LIMIT];
    size_t allowed_site_count;
    ContentBlockerMode mode;
    size_t allow_rule_count;
    size_t ignored_rule_count;
    size_t requests_considered;
    size_t requests_blocked;
    bool truncated;
};

typedef struct {
    ContentBlockerRule *rules;
    char *strings;
    uint16_t *table;
    size_t rule_count;
    size_t string_bytes;
    size_t table_capacity;
    size_t allow_rule_count;
    size_t ignored_rule_count;
    bool truncated;
} ContentBlockerCandidate;

/* Deliberately conservative: these are dedicated advertising/auction hosts,
   not general analytics/CDN hosts. BASIC applies them only to third-party
   subresources to minimize compatibility surprises. */
static const char *const basic_hosts[] = {
    "33across.com", "adform.net", "adnxs.com", "adsrvr.org",
    "amazon-adsystem.com", "bidswitch.net", "casalemedia.com",
    "contextweb.com", "criteo.com", "criteo.net", "demdex.net",
    "doubleclick.net", "googleadservices.com", "googlesyndication.com",
    "indexww.com", "lijit.com", "mathtag.com", "media.net",
    "moatads.com", "openx.net", "outbrain.com", "pubmatic.com",
    "quantserve.com", "rlcdn.com", "rubiconproject.com",
    "serving-sys.com", "sharethrough.com", "smartadserver.com",
    "smaato.net", "taboola.com", "teads.tv", "triplelift.com",
    "yieldmo.com", "zedo.com"
};

static const char cosmetic_css[] =
    "ins.adsbygoogle,.advertisement,.advertising,.ad-banner,.ad-container,"
    ".ad-slot,.sponsored,.promoted,.promotedlink,[data-ad],[data-ad-slot],"
    "[data-ad-client],[aria-label=\"advertisement\"],"
    "[aria-label=\"sponsored\"]{display:none!important}";

/* Deliberately limited to established consent-manager roots and explicit
   cookie-notice names. Generic modal/dialog selectors would hide sign-in,
   payment, and accessibility UI. Sourcepoint uses generated container IDs,
   hence the one bounded prefix match. */
static const char cookie_banner_css[] =
    "#onetrust-banner-sdk,#onetrust-consent-sdk,#onetrust-pc-sdk,"
    "#CybotCookiebotDialog,#CybotCookiebotDialogBodyUnderlay,#didomi-host,"
    "#truste-consent-track,#truste-consent-content,.truste_overlay,"
    ".truste_box_overlay,.qc-cmp2-container,.qc-cmp2-ui,"
    "#cookie-law-info-bar,#sp-cc,#msccBanner,#wcpConsentBannerCtrl,"
    ".cli-modal-backdrop,.cc-window,.cc-revoke,"
    ".cmplz-cookiebanner,.cmplz-cookiebanner-container,.cookie-banner,"
    ".cookie-consent,.cookie-notice,.cookies-eu-banner,.js-cookie-consent,"
    ".js-cookie-consent-banner,.cookie-policy-banner,.gdpr-banner,"
    ".fc-consent-root,[id^=\"sp_message_container_\"],"
    ":is([data-testid=\"cookie-policy-banner\"],"
    "[aria-label=\"Cookie banner\"],[aria-label=\"Cookie consent\"])"
    "{display:none!important}"
    "html.sp-message-open,body.sp-message-open,body.didomi-popup-open,"
    "body.cookie-modal-open,body.cookie-consent-open,"
    "body.cmplz-soft-cookiewall{overflow:auto!important;"
    "position:static!important}";

bool content_blocker_cosmetic_css(
    char *output, size_t capacity, size_t *length)
{
    size_t required = sizeof(cosmetic_css) - 1u;
    if (length != NULL) *length = required;
    if (output == NULL || capacity <= required) return false;
    memcpy(output, cosmetic_css, required + 1u);
    return true;
}

bool content_blocker_cookie_banner_css(
    char *output, size_t capacity, size_t *length)
{
    size_t required = sizeof(cookie_banner_css) - 1u;
    if (length != NULL) *length = required;
    if (output == NULL || capacity <= required) return false;
    memcpy(output, cookie_banner_css, required + 1u);
    return true;
}

static uint32_t blocker_hash(const char *value, size_t length)
{
    uint32_t hash = UINT32_C(2166136261);
    for (size_t i = 0; i < length; i++) {
        hash ^= (unsigned char) value[i];
        hash *= UINT32_C(16777619);
    }
    return hash;
}

static bool blocker_host_character(unsigned char value)
{
    return (value >= 'a' && value <= 'z')
        || (value >= '0' && value <= '9') || value == '-' || value == '.';
}

static bool blocker_normalize_host(const char *value, size_t length,
                                   char output[CONTENT_BLOCKER_HOST_LIMIT])
{
    if (value == NULL || length == 0 || length >= CONTENT_BLOCKER_HOST_LIMIT)
        return false;
    bool dot = false, label_character = false;
    for (size_t i = 0; i < length; i++) {
        unsigned char byte = (unsigned char) value[i];
        if (byte >= 'A' && byte <= 'Z') byte += (unsigned char) ('a' - 'A');
        if (!blocker_host_character(byte)) return false;
        if (byte == '.') {
            if (!label_character || i + 1u == length) return false;
            dot = true;
            label_character = false;
        } else {
            label_character = true;
        }
        output[i] = (char) byte;
    }
    output[length] = '\0';
    return dot && label_character;
}

static bool blocker_url_host(const char *url,
                             char output[CONTENT_BLOCKER_HOST_LIMIT])
{
    TilefinchUrl parsed;
    return tilefinch_url_parse(url, &parsed)
        && !parsed.ipv6_literal
        && blocker_normalize_host(
               url + parsed.host_offset, parsed.host_length, output);
}

bool content_blocker_site_from_url(
    const char *url, char output[CONTENT_BLOCKER_HOST_LIMIT])
{
    if (output == NULL) return false;
    output[0] = '\0';
    char key[TILEFINCH_ORIGIN_SERIALIZED_LIMIT];
    if (!tilefinch_url_site_key(url, key, sizeof(key))) return false;
    TilefinchUrl parsed;
    return tilefinch_url_parse(key, &parsed)
        && blocker_normalize_host(
               key + parsed.host_offset, parsed.host_length, output);
}

static bool blocker_host_suffix(const char *host, const char *suffix)
{
    size_t host_length = strlen(host), suffix_length = strlen(suffix);
    if (suffix_length > host_length) return false;
    const char *tail = host + host_length - suffix_length;
    return strcmp(tail, suffix) == 0
        && (tail == host || tail[-1] == '.');
}

static uint8_t blocker_destination_type(const char *destination)
{
    if (destination == NULL || destination[0] == '\0'
        || strcasecmp(destination, "empty") == 0)
        return BLOCKER_TYPE_XHR;
    if (strcasecmp(destination, "script") == 0)
        return BLOCKER_TYPE_SCRIPT;
    if (strcasecmp(destination, "image") == 0)
        return BLOCKER_TYPE_IMAGE;
    if (strcasecmp(destination, "style") == 0)
        return BLOCKER_TYPE_STYLE;
    if (strcasecmp(destination, "font") == 0)
        return BLOCKER_TYPE_FONT;
    if (strcasecmp(destination, "video") == 0
        || strcasecmp(destination, "audio") == 0)
        return BLOCKER_TYPE_MEDIA;
    if (strcasecmp(destination, "iframe") == 0
        || strcasecmp(destination, "frame") == 0)
        return BLOCKER_TYPE_FRAME;
    return BLOCKER_TYPE_OTHER;
}

static uint8_t blocker_option_type(const char *option, bool *known)
{
    *known = true;
    if (strcasecmp(option, "script") == 0) return BLOCKER_TYPE_SCRIPT;
    if (strcasecmp(option, "image") == 0) return BLOCKER_TYPE_IMAGE;
    if (strcasecmp(option, "stylesheet") == 0
        || strcasecmp(option, "css") == 0) return BLOCKER_TYPE_STYLE;
    if (strcasecmp(option, "font") == 0) return BLOCKER_TYPE_FONT;
    if (strcasecmp(option, "xmlhttprequest") == 0
        || strcasecmp(option, "xhr") == 0) return BLOCKER_TYPE_XHR;
    if (strcasecmp(option, "subdocument") == 0
        || strcasecmp(option, "frame") == 0) return BLOCKER_TYPE_FRAME;
    /* Media playback and ping/websocket transports do not currently enter
       this policy seam. Retaining those modifiers would advertise rules
       that can never fire, so count the complete rule as ignored. */
    if (strcasecmp(option, "media") == 0
        || strcasecmp(option, "other") == 0
        || strcasecmp(option, "ping") == 0
        || strcasecmp(option, "websocket") == 0) {
        *known = false;
        return 0;
    }
    *known = false;
    return 0;
}

static bool blocker_parse_options(char *options, uint8_t *flags,
                                  uint8_t *types)
{
    uint8_t included = 0, excluded = 0;
    bool saw_included = false;
    for (char *token = strtok(options, ","); token != NULL;
         token = strtok(NULL, ",")) {
        while (*token == ' ' || *token == '\t') token++;
        bool negate = *token == '~';
        if (negate) token++;
        if (strcasecmp(token, "third-party") == 0
            || strcasecmp(token, "3p") == 0) {
            *flags |= negate ? BLOCKER_RULE_FIRST_PARTY
                             : BLOCKER_RULE_THIRD_PARTY;
            continue;
        }
        if (strcasecmp(token, "first-party") == 0
            || strcasecmp(token, "1p") == 0) {
            *flags |= negate ? BLOCKER_RULE_THIRD_PARTY
                             : BLOCKER_RULE_FIRST_PARTY;
            continue;
        }
        if (strcasecmp(token, "important") == 0
            || strcasecmp(token, "match-case") == 0) continue;
        bool known = false;
        uint8_t type = blocker_option_type(token, &known);
        if (known) {
            if (negate) excluded |= type;
            else {
                included |= type;
                saw_included = true;
            }
            continue;
        }
        /* Scope, redirect, scriptlet, CSP, header, and procedural modifiers
           cannot be approximated safely by a hostname-only matcher. */
        return false;
    }
    if ((*flags & (BLOCKER_RULE_FIRST_PARTY | BLOCKER_RULE_THIRD_PARTY))
            == (BLOCKER_RULE_FIRST_PARTY | BLOCKER_RULE_THIRD_PARTY))
        return false;
    *types = (uint8_t) ((saw_included ? included : BLOCKER_TYPE_ALL)
                       & (uint8_t) ~excluded);
    return *types != 0;
}

static bool blocker_candidate_add(ContentBlockerCandidate *candidate,
                                  const char *host, size_t host_length,
                                  uint8_t flags, uint8_t types)
{
    if (candidate->rule_count >= CONTENT_BLOCKER_RULE_LIMIT
        || host_length + 1u > CONTENT_BLOCKER_STRING_BYTES_LIMIT
                                  - candidate->string_bytes) {
        candidate->truncated = true;
        return false;
    }
    size_t index = candidate->rule_count++;
    ContentBlockerRule *rule = &candidate->rules[index];
    rule->host_offset = (uint32_t) candidate->string_bytes;
    rule->host_length = (uint16_t) host_length;
    rule->types = types;
    rule->flags = flags;
    memcpy(candidate->strings + candidate->string_bytes, host, host_length);
    candidate->strings[candidate->string_bytes + host_length] = '\0';
    candidate->string_bytes += host_length + 1u;
    if ((flags & BLOCKER_RULE_ALLOW) != 0) candidate->allow_rule_count++;
    return true;
}

static void blocker_candidate_destroy(Budget *budget,
                                      ContentBlockerCandidate *candidate)
{
    budget_free(budget, candidate->rules);
    budget_free(budget, candidate->strings);
    budget_free(budget, candidate->table);
    memset(candidate, 0, sizeof(*candidate));
}

static bool blocker_parse_line(ContentBlockerCandidate *candidate, char *line)
{
    char *start = line;
    while (*start == ' ' || *start == '\t') start++;
    char *end = start + strlen(start);
    while (end > start && isspace((unsigned char) end[-1])) *--end = '\0';
    if (*start == '\0' || *start == '!' || *start == '[') return true;
    if (strstr(start, "##") != NULL || strstr(start, "#@#") != NULL
        || strstr(start, "#$#") != NULL || strstr(start, "#%#") != NULL) {
        candidate->ignored_rule_count++;
        return true;
    }

    uint8_t flags = 0, types = BLOCKER_TYPE_ALL;
    if (strncmp(start, "@@", 2) == 0) {
        flags |= BLOCKER_RULE_ALLOW;
        start += 2;
    }
    char host[CONTENT_BLOCKER_HOST_LIMIT];
    size_t host_length = 0;
    if (strncmp(start, "||", 2) == 0) {
        start += 2;
        char *separator = strchr(start, '^');
        if (separator == NULL || separator == start) {
            candidate->ignored_rule_count++;
            return true;
        }
        host_length = (size_t) (separator - start);
        char *tail = separator + 1;
        if (*tail == '$') {
            if (!blocker_parse_options(tail + 1, &flags, &types)) {
                candidate->ignored_rule_count++;
                return true;
            }
        } else if (*tail != '\0') {
            candidate->ignored_rule_count++;
            return true;
        }
    } else {
        char *space = strpbrk(start, " \t");
        if (space != NULL) {
            *space++ = '\0';
            while (*space == ' ' || *space == '\t') space++;
            if ((strcmp(start, "0.0.0.0") != 0
                 && strcmp(start, "127.0.0.1") != 0)
                || *space == '\0' || strpbrk(space, " \t") != NULL) {
                candidate->ignored_rule_count++;
                return true;
            }
            start = space;
        }
        host_length = strlen(start);
    }
    if (!blocker_normalize_host(start, host_length, host)) {
        candidate->ignored_rule_count++;
        return true;
    }
    (void) blocker_candidate_add(
        candidate, host, strlen(host), flags, types);
    return true;
}

static bool blocker_candidate_index(Budget *budget,
                                    ContentBlockerCandidate *candidate)
{
    if (candidate->rule_count == 0) return true;
    size_t capacity = 16;
    while (capacity < candidate->rule_count * 2u) capacity <<= 1u;
    candidate->table = budget_calloc_category(
        budget, BUDGET_CATEGORY_SESSION, capacity, sizeof(uint16_t));
    if (candidate->table == NULL) return false;
    candidate->table_capacity = capacity;
    for (size_t index = 0; index < candidate->rule_count; index++) {
        const ContentBlockerRule *rule = &candidate->rules[index];
        const char *host = candidate->strings + rule->host_offset;
        size_t slot = blocker_hash(host, rule->host_length) & (capacity - 1u);
        while (candidate->table[slot] != 0)
            slot = (slot + 1u) & (capacity - 1u);
        candidate->table[slot] = (uint16_t) (index + 1u);
    }
    return true;
}

static bool blocker_load_custom(ContentBlocker *blocker, const char *path,
                                ContentBlockerCandidate *candidate)
{
    if (path == NULL || path[0] == '\0') return false;
    FILE *file = fopen(path, "rb");
    if (file == NULL) return false;
    candidate->rules = budget_calloc_category(
        blocker->budget, BUDGET_CATEGORY_SESSION,
        CONTENT_BLOCKER_RULE_LIMIT, sizeof(*candidate->rules));
    candidate->strings = budget_malloc_category(
        blocker->budget, BUDGET_CATEGORY_SESSION,
        CONTENT_BLOCKER_STRING_BYTES_LIMIT);
    if (candidate->rules == NULL || candidate->strings == NULL) {
        fclose(file);
        return false;
    }
    char line[2048];
    size_t file_bytes = 0;
    bool ok = true;
    while (fgets(line, sizeof(line), file) != NULL) {
        size_t length = strlen(line);
        if (length == sizeof(line) - 1u && line[length - 1u] != '\n') {
            ok = false;
            break;
        }
        if (length > CONTENT_BLOCKER_FILE_BYTES_LIMIT - file_bytes) {
            ok = false;
            break;
        }
        file_bytes += length;
        line[strcspn(line, "\r\n")] = '\0';
        if (!blocker_parse_line(candidate, line)) {
            ok = false;
            break;
        }
    }
    if (ferror(file)) ok = false;
    if (fclose(file) != 0) ok = false;
    if (!ok || candidate->truncated) return false;
    if (candidate->rule_count == 0) {
        budget_free(blocker->budget, candidate->rules);
        budget_free(blocker->budget, candidate->strings);
        candidate->rules = NULL;
        candidate->strings = NULL;
    } else {
        ContentBlockerRule *rules = budget_realloc_category(
            blocker->budget, BUDGET_CATEGORY_SESSION, candidate->rules,
            candidate->rule_count * sizeof(*candidate->rules));
        if (rules == NULL) return false;
        candidate->rules = rules;
        char *strings = budget_realloc_category(
            blocker->budget, BUDGET_CATEGORY_SESSION, candidate->strings,
            candidate->string_bytes);
        if (strings == NULL) return false;
        candidate->strings = strings;
    }
    return blocker_candidate_index(blocker->budget, candidate);
}

ContentBlocker *content_blocker_create(Budget *budget)
{
    if (budget == NULL) return NULL;
    ContentBlocker *blocker = budget_calloc_category(
        budget, BUDGET_CATEGORY_SESSION, 1, sizeof(*blocker));
    if (blocker != NULL) blocker->budget = budget;
    return blocker;
}

void content_blocker_destroy(ContentBlocker *blocker)
{
    if (blocker == NULL) return;
    Budget *budget = blocker->budget;
    budget_free(budget, blocker->rules);
    budget_free(budget, blocker->strings);
    budget_free(budget, blocker->table);
    budget_free(budget, blocker->allowed_sites);
    memset(blocker, 0, sizeof(*blocker));
    budget_free(budget, blocker);
}

bool content_blocker_configure(ContentBlocker *blocker,
                               ContentBlockerMode mode,
                               const char *custom_path)
{
    if (blocker == NULL || mode < CONTENT_BLOCKER_OFF
        || mode > CONTENT_BLOCKER_CUSTOM) return false;
    ContentBlockerCandidate candidate = {0};
    if (mode == CONTENT_BLOCKER_CUSTOM
        && !blocker_load_custom(blocker, custom_path, &candidate)) {
        blocker_candidate_destroy(blocker->budget, &candidate);
        return false;
    }
    budget_free(blocker->budget, blocker->rules);
    budget_free(blocker->budget, blocker->strings);
    budget_free(blocker->budget, blocker->table);
    blocker->rules = candidate.rules;
    blocker->strings = candidate.strings;
    blocker->table = candidate.table;
    blocker->rule_count = candidate.rule_count;
    blocker->string_bytes = candidate.string_bytes;
    blocker->table_capacity = candidate.table_capacity;
    blocker->allow_rule_count = candidate.allow_rule_count;
    blocker->ignored_rule_count = candidate.ignored_rule_count;
    blocker->truncated = candidate.truncated;
    blocker->mode = mode;
    blocker->requests_considered = 0;
    blocker->requests_blocked = 0;
    return true;
}

bool content_blocker_set_allowed_sites(
    ContentBlocker *blocker, const char *const *sites, size_t count)
{
    if (blocker == NULL || count > CONTENT_BLOCKER_ALLOW_SITE_LIMIT)
        return false;
    size_t bytes = 0;
    char normalized[CONTENT_BLOCKER_ALLOW_SITE_LIMIT]
                   [CONTENT_BLOCKER_HOST_LIMIT];
    for (size_t i = 0; i < count; i++) {
        size_t length = sites == NULL || sites[i] == NULL
            ? 0 : strlen(sites[i]);
        if (!blocker_normalize_host(sites[i], length, normalized[i]))
            return false;
        bytes += length + 1u;
    }
    char *pool = bytes == 0 ? NULL : budget_malloc_category(
        blocker->budget, BUDGET_CATEGORY_SESSION, bytes);
    if (bytes != 0 && pool == NULL) return false;
    uint16_t offsets[CONTENT_BLOCKER_ALLOW_SITE_LIMIT] = {0};
    uint8_t lengths[CONTENT_BLOCKER_ALLOW_SITE_LIMIT] = {0};
    size_t used = 0;
    for (size_t i = 0; i < count; i++) {
        size_t length = strlen(normalized[i]);
        offsets[i] = (uint16_t) used;
        lengths[i] = (uint8_t) length;
        memcpy(pool + used, normalized[i], length + 1u);
        used += length + 1u;
    }
    budget_free(blocker->budget, blocker->allowed_sites);
    blocker->allowed_sites = pool;
    memcpy(blocker->allowed_offsets, offsets, sizeof(offsets));
    memcpy(blocker->allowed_lengths, lengths, sizeof(lengths));
    blocker->allowed_site_count = count;
    return true;
}

static bool blocker_site_allowed(const ContentBlocker *blocker,
                                 const char *initiator_url)
{
    char site[CONTENT_BLOCKER_HOST_LIMIT];
    if (!content_blocker_site_from_url(initiator_url, site)) return false;
    for (size_t i = 0; i < blocker->allowed_site_count; i++) {
        const char *allowed = blocker->allowed_sites
            + blocker->allowed_offsets[i];
        if (strcmp(site, allowed) == 0) return true;
    }
    return false;
}

static bool blocker_custom_host_decision(
    const ContentBlocker *blocker, const char *host, uint8_t type,
    bool third_party, bool *allowed, bool *blocked)
{
    if (blocker->table == NULL || blocker->table_capacity == 0) return true;
    for (const char *suffix = host;;) {
        size_t length = strlen(suffix);
        size_t slot = blocker_hash(suffix, length)
                      & (blocker->table_capacity - 1u);
        while (blocker->table[slot] != 0) {
            const ContentBlockerRule *rule = &blocker->rules[
                blocker->table[slot] - 1u];
            const char *rule_host = blocker->strings + rule->host_offset;
            if (rule->host_length == length
                && memcmp(rule_host, suffix, length) == 0
                && (rule->types & type) != 0
                && ((rule->flags & BLOCKER_RULE_THIRD_PARTY) == 0
                    || third_party)
                && ((rule->flags & BLOCKER_RULE_FIRST_PARTY) == 0
                    || !third_party)) {
                if ((rule->flags & BLOCKER_RULE_ALLOW) != 0) *allowed = true;
                else *blocked = true;
            }
            slot = (slot + 1u) & (blocker->table_capacity - 1u);
        }
        const char *dot = strchr(suffix, '.');
        if (dot == NULL) break;
        suffix = dot + 1;
    }
    return true;
}

bool content_blocker_would_block(
    const ContentBlocker *blocker, const char *request_url,
    const char *initiator_url, const char *destination,
    const char *fetch_mode)
{
    if (blocker == NULL || blocker->mode == CONTENT_BLOCKER_OFF
        || request_url == NULL || initiator_url == NULL) return false;
    if (destination != NULL && fetch_mode != NULL
        && strcasecmp(destination, "document") == 0
        && strcasecmp(fetch_mode, "navigate") == 0) return false;
    char host[CONTENT_BLOCKER_HOST_LIMIT];
    char request_site[CONTENT_BLOCKER_HOST_LIMIT];
    char initiator_site[CONTENT_BLOCKER_HOST_LIMIT];
    if (!blocker_url_host(request_url, host)
        || !content_blocker_site_from_url(request_url, request_site)
        || !content_blocker_site_from_url(initiator_url, initiator_site))
        return false;
    if (blocker_site_allowed(blocker, initiator_url)) return false;
    bool third_party = strcmp(request_site, initiator_site) != 0;
    bool blocked = false, allowed = false;
    if (blocker->mode == CONTENT_BLOCKER_BASIC) {
        if (third_party) {
            for (size_t i = 0;
                 i < sizeof(basic_hosts) / sizeof(basic_hosts[0]); i++) {
                if (blocker_host_suffix(host, basic_hosts[i])) {
                    blocked = true;
                    break;
                }
            }
        }
    } else {
        (void) blocker_custom_host_decision(
            blocker, host, blocker_destination_type(destination),
            third_party, &allowed, &blocked);
    }
    return blocked && !allowed;
}

bool content_blocker_should_block(
    ContentBlocker *blocker, const char *request_url,
    const char *initiator_url, const char *destination,
    const char *fetch_mode)
{
    if (blocker == NULL || blocker->mode == CONTENT_BLOCKER_OFF)
        return false;
    blocker->requests_considered++;
    bool decision = content_blocker_would_block(
        blocker, request_url, initiator_url, destination, fetch_mode);
    if (decision) blocker->requests_blocked++;
    return decision;
}

bool content_blocker_metrics(const ContentBlocker *blocker,
                             ContentBlockerMetrics *metrics)
{
    if (blocker == NULL || metrics == NULL) return false;
    *metrics = (ContentBlockerMetrics) {
        .mode = blocker->mode,
        .rule_count = blocker->mode == CONTENT_BLOCKER_BASIC
            ? sizeof(basic_hosts) / sizeof(basic_hosts[0])
            : blocker->rule_count,
        .allow_rule_count = blocker->allow_rule_count,
        .ignored_rule_count = blocker->ignored_rule_count,
        .allowed_site_count = blocker->allowed_site_count,
        .requests_considered = blocker->requests_considered,
        .requests_blocked = blocker->requests_blocked,
        .retained_bytes = budget_usable_size(blocker->rules)
            + budget_usable_size(blocker->strings)
            + budget_usable_size(blocker->table)
            + budget_usable_size(blocker->allowed_sites),
        .truncated = blocker->truncated
    };
    return true;
}
