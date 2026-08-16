#include "tilefinch/session.h"

#include "tilefinch/public_suffix.h"
#include "tilefinch/fetch.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

#include "tilefinch/url.h"

#define budget_malloc(b, s) budget_malloc_category((b), BUDGET_CATEGORY_SESSION, (s))
#define budget_calloc(b, n, s) budget_calloc_category((b), BUDGET_CATEGORY_SESSION, (n), (s))
#define budget_realloc(b, p, s) budget_realloc_category((b), BUDGET_CATEGORY_SESSION, (p), (s))

#define DEFAULT_STORAGE_BYTES (16u * 1024u)
#define DEFAULT_COOKIE_BYTES (4u * 1024u)

static BrowserCacheEntry *cache_find_key(BrowserSession *session,
                                         const char *key);
static BrowserCacheEntry *cache_find_resource_key(
    BrowserSession *session, const char *key,
    const TilefinchRequestContext *context);

static int month_number(const char *value)
{
    static const char months[] = "JanFebMarAprMayJunJulAugSepOctNovDec";
    for (int month = 0; month < 12; month++) {
        if (strncasecmp(value, months + month * 3, 3) == 0) return month + 1;
    }
    return 0;
}

static int64_t days_from_civil(int year, unsigned month, unsigned day)
{
    year -= month <= 2;
    int era = (year >= 0 ? year : year - 399) / 400;
    unsigned yoe = (unsigned) (year - era * 400);
    unsigned adjusted_month = month > 2 ? month - 3 : month + 9;
    unsigned doy = (153u * adjusted_month + 2) / 5
                   + day - 1;
    unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return (int64_t) era * 146097 + (int64_t) doe - 719468;
}

static bool parse_cookie_date(const char *value, size_t length,
                              int64_t *epoch)
{
    char copy[96];
    if (length == 0 || length >= sizeof(copy)) return false;
    memcpy(copy, value, length); copy[length] = '\0';
    char weekday[4] = {0}, month_name[4] = {0}, zone[4] = {0};
    int day, year, hour, minute, second;
    if (sscanf(copy, "%3[^,], %d %3s %d %d:%d:%d %3s", weekday,
               &day, month_name, &year, &hour, &minute, &second, zone) != 8
        || strcasecmp(zone, "GMT") != 0) return false;
    int month = month_number(month_name);
    if (month == 0 || day < 1 || day > 31 || year < 1601
        || hour < 0 || hour > 23 || minute < 0 || minute > 59
        || second < 0 || second > 60) return false;
    *epoch = days_from_civil(year, (unsigned) month, (unsigned) day) * 86400
             + hour * 3600 + minute * 60 + second;
    return true;
}

/* Deterministic replay serves responses captured at an earlier wall-clock
   time. Cookie storage and serialization decisions must replay against that
   capture timeline, not the machine clock, or a short-lived cookie stored at
   capture time silently expires once the real clock passes its deadline and
   every later request in the trace stops matching. The replay layer advances
   this clock from served records' Date headers; zero means real time. */
static int64_t cookie_clock_override;

void browser_session_advance_cookie_clock(int64_t epoch_seconds)
{
    if (epoch_seconds > cookie_clock_override) {
        cookie_clock_override = epoch_seconds;
    }
}

static int64_t cookie_now(void)
{
    return cookie_clock_override != 0 ? cookie_clock_override
                                      : (int64_t) time(NULL);
}

bool browser_cookie_parse_date(const char *value, size_t length,
                               int64_t *epoch_seconds)
{
    return parse_cookie_date(value, length, epoch_seconds);
}

static bool copy_origin(const char *url,
                        char output[BROWSER_ORIGIN_LIMIT])
{
    return tilefinch_url_origin(url, output, BROWSER_ORIGIN_LIMIT);
}

static uint64_t session_hash_bytes(uint64_t hash, const void *data,
                                   size_t length)
{
    const unsigned char *bytes = data;
    for (size_t i = 0; i < length; i++)
        hash = (hash ^ bytes[i]) * UINT64_C(1099511628211);
    return hash;
}

const char *browser_cookie_entry_path(const BrowserCookieEntry *entry)
{
    if (entry == NULL) return "";
    return entry->long_path == NULL ? entry->path : entry->long_path;
}

static uint64_t session_cookie_fingerprint(const BrowserSession *session)
{
    uint64_t hash = UINT64_C(1469598103934665603);
    for (size_t i = 0; i < BROWSER_COOKIE_ENTRIES; i++) {
        const BrowserCookieEntry *cookie = &session->cookies[i];
        hash = session_hash_bytes(hash, &i, sizeof(i));
        hash = session_hash_bytes(
            hash, cookie->domain, strlen(cookie->domain) + 1u);
        hash = session_hash_bytes(
            hash, browser_cookie_entry_path(cookie),
            cookie->path_length + 1u);
        hash = session_hash_bytes(
            hash, cookie->name, strlen(cookie->name) + 1u);
        hash = session_hash_bytes(
            hash, &cookie->value_length, sizeof(cookie->value_length));
        if (cookie->value != NULL) {
            hash = session_hash_bytes(
                hash, cookie->value, cookie->value_length);
        }
        hash = session_hash_bytes(
            hash, &cookie->expires_at, sizeof(cookie->expires_at));
        hash = session_hash_bytes(
            hash, &cookie->host_only, sizeof(cookie->host_only));
        hash = session_hash_bytes(
            hash, &cookie->secure, sizeof(cookie->secure));
        hash = session_hash_bytes(
            hash, &cookie->http_only, sizeof(cookie->http_only));
        hash = session_hash_bytes(
            hash, &cookie->same_site, sizeof(cookie->same_site));
        hash = session_hash_bytes(
            hash, &cookie->partitioned, sizeof(cookie->partitioned));
        hash = session_hash_bytes(
            hash, cookie->partition_key,
            strlen(cookie->partition_key) + 1u);
        hash = session_hash_bytes(
            hash, &cookie->creation_sequence,
            sizeof(cookie->creation_sequence));
    }
    return hash;
}

bool browser_session_init(BrowserSession *session, Budget *budget,
                          size_t maximum_cache_bytes)
{
    if (session == NULL || budget == NULL || maximum_cache_bytes == 0) {
        return false;
    }
    memset(session, 0, sizeof(*session));
    session->budget = budget;
    session->maximum_storage_bytes = DEFAULT_STORAGE_BYTES;
    session->maximum_cookie_bytes = DEFAULT_COOKIE_BYTES;
    session->maximum_cookie_long_path_bytes =
        BROWSER_COOKIE_LONG_PATH_BYTES;
    session->maximum_cache_bytes = maximum_cache_bytes;
    session->site_data_allowed = true;
    session->accounting_bytes = sizeof(session->storage)
                                + sizeof(session->cookies)
                                + sizeof(session->cache)
                                + sizeof(session->site_adapter_state);
    if (!budget_reservation_acquire(
            &session->accounting_reservation, budget,
            BUDGET_CATEGORY_SESSION, session->accounting_bytes)) {
        memset(session, 0, sizeof(*session));
        return false;
    }
    return true;
}

void browser_session_set_site_data_allowed(
    BrowserSession *session, bool allowed)
{
    if (session != NULL) session->site_data_allowed = allowed;
}

bool browser_session_site_data_allowed(const BrowserSession *session)
{
    return session == NULL || session->site_data_allowed;
}

bool browser_session_site_adapter_state_put(
    BrowserSession *session, const char *key, const void *data,
    size_t data_length, uint64_t now_ns)
{
    if (session == NULL || session->budget == NULL || key == NULL
        || data == NULL || data_length == 0
        || data_length > BROWSER_SITE_ADAPTER_STATE_DATA_LIMIT
        || strlen(key) >= BROWSER_SITE_ADAPTER_STATE_KEY_LIMIT) {
        return false;
    }
    BrowserSiteAdapterState replacement = {0};
    snprintf(replacement.key, sizeof(replacement.key), "%s", key);
    memcpy(replacement.data, data, data_length);
    replacement.length = data_length;
    replacement.stored_at_ns = now_ns;
    replacement.cookie_fingerprint =
        session_cookie_fingerprint(session);
    replacement.valid = true;
    session->site_adapter_state = replacement;
    return true;
}

bool browser_session_site_adapter_state_get(
    BrowserSession *session, const char *key, void *data,
    size_t data_capacity, size_t *data_length, uint64_t now_ns,
    uint64_t maximum_age_ns)
{
    if (data_length != NULL) *data_length = 0;
    if (session == NULL || session->budget == NULL || key == NULL
        || data == NULL) return false;
    BrowserSiteAdapterState *state = &session->site_adapter_state;
    bool expired = maximum_age_ns != 0
        && (now_ns < state->stored_at_ns
            || now_ns - state->stored_at_ns > maximum_age_ns);
    if (!state->valid || strcmp(state->key, key) != 0
        || state->length > data_capacity || expired
        || state->cookie_fingerprint
               != session_cookie_fingerprint(session)) {
        return false;
    }
    memcpy(data, state->data, state->length);
    if (data_length != NULL) *data_length = state->length;
    return true;
}

void browser_session_site_adapter_state_remove(
    BrowserSession *session, const char *key)
{
    if (session == NULL || key == NULL) return;
    BrowserSiteAdapterState *state = &session->site_adapter_state;
    if (state->valid && strcmp(state->key, key) == 0)
        memset(state, 0, sizeof(*state));
}

static BrowserStorageEntry *find_storage(BrowserSession *session,
                                         const char *origin, bool local,
                                         const char *key)
{
    for (size_t i = 0; i < BROWSER_STORAGE_ENTRIES; i++) {
        BrowserStorageEntry *entry = &session->storage[i];
        if (entry->value != NULL && entry->local == local
            && strcmp(entry->origin, origin) == 0
            && strcmp(entry->key, key) == 0) return entry;
    }
    return NULL;
}

bool browser_session_storage_get(const BrowserSession *session,
                                 const char *url, bool local,
                                 const char *key, const char **value,
                                 size_t *value_length)
{
    if (session == NULL || !session->site_data_allowed || key == NULL)
        return false;
    char origin[BROWSER_ORIGIN_LIMIT];
    if (!copy_origin(url, origin)) return false;
    BrowserStorageEntry *entry = find_storage((BrowserSession *) session,
                                              origin, local, key);
    if (entry == NULL) return false;
    if (value != NULL) *value = entry->value;
    if (value_length != NULL) *value_length = entry->value_length;
    return true;
}

size_t browser_session_storage_length(
    const BrowserSession *session, const char *url, bool local)
{
    if (session == NULL || !session->site_data_allowed) return 0;
    char origin[BROWSER_ORIGIN_LIMIT];
    if (!copy_origin(url, origin)) return 0;
    size_t count = 0;
    for (size_t i = 0; i < BROWSER_STORAGE_ENTRIES; i++) {
        const BrowserStorageEntry *entry = &session->storage[i];
        if (entry->value != NULL && entry->local == local
            && strcmp(entry->origin, origin) == 0) count++;
    }
    return count;
}

bool browser_session_storage_key(
    const BrowserSession *session, const char *url, bool local,
    size_t index, const char **key)
{
    if (session == NULL || !session->site_data_allowed) return false;
    char origin[BROWSER_ORIGIN_LIMIT];
    if (!copy_origin(url, origin)) return false;
    size_t seen = 0;
    for (size_t i = 0; i < BROWSER_STORAGE_ENTRIES; i++) {
        const BrowserStorageEntry *entry = &session->storage[i];
        if (entry->value == NULL || entry->local != local
            || strcmp(entry->origin, origin) != 0) continue;
        if (seen++ != index) continue;
        if (key != NULL) *key = entry->key;
        return true;
    }
    return false;
}

bool browser_session_storage_set(BrowserSession *session, const char *url,
                                 bool local, const char *key,
                                 const char *value, size_t value_length)
{
    if (session == NULL || !session->site_data_allowed
        || key == NULL || value == NULL
        || strlen(key) >= BROWSER_KEY_LIMIT) return false;
    char origin[BROWSER_ORIGIN_LIMIT];
    if (!copy_origin(url, origin)) return false;
    BrowserStorageEntry *entry = find_storage(session, origin, local, key);
    if (entry == NULL) {
        for (size_t i = 0; i < BROWSER_STORAGE_ENTRIES; i++) {
            if (session->storage[i].value == NULL) {
                entry = &session->storage[i];
                break;
            }
        }
    }
    if (entry == NULL) return false;
    size_t old_length = entry->value == NULL ? 0 : entry->value_length;
    size_t retained = session->storage_bytes >= old_length
        ? session->storage_bytes - old_length : 0;
    if (retained > session->maximum_storage_bytes
        || value_length > session->maximum_storage_bytes - retained)
        return false;
    char *copy = budget_malloc(session->budget, value_length + 1);
    if (copy == NULL) return false;
    memcpy(copy, value, value_length);
    copy[value_length] = '\0';
    budget_free(session->budget, entry->value);
    snprintf(entry->origin, sizeof(entry->origin), "%s", origin);
    snprintf(entry->key, sizeof(entry->key), "%s", key);
    entry->value = copy;
    entry->value_length = value_length;
    entry->local = local;
    session->storage_bytes = session->storage_bytes - old_length + value_length;
    return true;
}

void browser_session_storage_remove(BrowserSession *session, const char *url,
                                    bool local, const char *key)
{
    if (session == NULL || !session->site_data_allowed || key == NULL) return;
    char origin[BROWSER_ORIGIN_LIMIT];
    if (!copy_origin(url, origin)) return;
    BrowserStorageEntry *entry = find_storage(session, origin, local, key);
    if (entry == NULL) return;
    session->storage_bytes -= entry->value_length;
    budget_free(session->budget, entry->value);
    memset(entry, 0, sizeof(*entry));
}

void browser_session_storage_clear(BrowserSession *session, const char *url,
                                   bool local)
{
    if (session == NULL || !session->site_data_allowed) return;
    char origin[BROWSER_ORIGIN_LIMIT];
    if (!copy_origin(url, origin)) return;
    for (size_t i = 0; i < BROWSER_STORAGE_ENTRIES; i++) {
        BrowserStorageEntry *entry = &session->storage[i];
        if (entry->value != NULL && entry->local == local
            && strcmp(entry->origin, origin) == 0) {
            session->storage_bytes -= entry->value_length;
            budget_free(session->budget, entry->value);
            memset(entry, 0, sizeof(*entry));
        }
    }
}

void browser_session_storage_clear_all(BrowserSession *session, bool local)
{
    if (session == NULL || session->budget == NULL) return;
    for (size_t i = 0; i < BROWSER_STORAGE_ENTRIES; i++) {
        BrowserStorageEntry *entry = &session->storage[i];
        if (entry->value == NULL || entry->local != local) continue;
        if (entry->value_length <= session->storage_bytes) {
            session->storage_bytes -= entry->value_length;
        } else {
            session->storage_bytes = 0;
        }
        budget_free(session->budget, entry->value);
        memset(entry, 0, sizeof(*entry));
    }
}

struct BrowserCookieOverlay {
    Budget *budget;
    bool site_data_allowed;
    BrowserCookieEntry cookies[BROWSER_COOKIE_ENTRIES];
    size_t cookie_bytes;
    size_t cookie_long_path_bytes;
    size_t maximum_cookie_bytes;
    size_t maximum_cookie_long_path_bytes;
    size_t cookie_clock;
    char third_party_cookie_allowed_sites[BROWSER_SECURITY_SITE_LIMIT]
                                         [BROWSER_ORIGIN_LIMIT];
    size_t third_party_cookie_allowed_site_count;
};

typedef struct {
    Budget *budget;
    BrowserCookieEntry *cookies;
    size_t *cookie_bytes;
    size_t *cookie_long_path_bytes;
    size_t maximum_cookie_bytes;
    size_t maximum_cookie_long_path_bytes;
    size_t *cookie_clock;
    const char (*third_party_cookie_allowed_sites)[BROWSER_ORIGIN_LIMIT];
    size_t third_party_cookie_allowed_site_count;
} BrowserCookieStore;

static BrowserCookieStore browser_session_cookie_store(
    BrowserSession *session)
{
    return (BrowserCookieStore) {
        .budget = session == NULL ? NULL : session->budget,
        .cookies = session == NULL ? NULL : session->cookies,
        .cookie_bytes = session == NULL ? NULL : &session->cookie_bytes,
        .cookie_long_path_bytes = session == NULL
            ? NULL : &session->cookie_long_path_bytes,
        .maximum_cookie_bytes = session == NULL
            ? 0 : session->maximum_cookie_bytes,
        .maximum_cookie_long_path_bytes = session == NULL
            ? 0 : session->maximum_cookie_long_path_bytes,
        .cookie_clock = session == NULL ? NULL : &session->cookie_clock
        , .third_party_cookie_allowed_sites = session == NULL ? NULL
            : session->third_party_cookie_allowed_sites
        , .third_party_cookie_allowed_site_count = session == NULL ? 0
            : session->third_party_cookie_allowed_site_count
    };
}

static BrowserCookieStore browser_overlay_cookie_store(
    BrowserCookieOverlay *overlay)
{
    return (BrowserCookieStore) {
        .budget = overlay == NULL ? NULL : overlay->budget,
        .cookies = overlay == NULL ? NULL : overlay->cookies,
        .cookie_bytes = overlay == NULL ? NULL : &overlay->cookie_bytes,
        .cookie_long_path_bytes = overlay == NULL
            ? NULL : &overlay->cookie_long_path_bytes,
        .maximum_cookie_bytes = overlay == NULL
            ? 0 : overlay->maximum_cookie_bytes,
        .maximum_cookie_long_path_bytes = overlay == NULL
            ? 0 : overlay->maximum_cookie_long_path_bytes,
        .cookie_clock = overlay == NULL ? NULL : &overlay->cookie_clock
        , .third_party_cookie_allowed_sites = overlay == NULL ? NULL
            : overlay->third_party_cookie_allowed_sites
        , .third_party_cookie_allowed_site_count = overlay == NULL ? 0
            : overlay->third_party_cookie_allowed_site_count
    };
}

static bool security_site_key(const char *url,
                              char output[BROWSER_ORIGIN_LIMIT])
{
    return url != NULL
        && tilefinch_url_site_key(url, output, BROWSER_ORIGIN_LIMIT);
}

static bool security_site_list_contains(
    const char (*sites)[BROWSER_ORIGIN_LIMIT], size_t count,
    const char *url)
{
    char site[BROWSER_ORIGIN_LIMIT];
    if (sites == NULL || !security_site_key(url, site)) return false;
    for (size_t i = 0; i < count; i++)
        if (strcmp(sites[i], site) == 0) return true;
    return false;
}

static bool security_site_list_set(
    char (*sites)[BROWSER_ORIGIN_LIMIT], size_t *count,
    const char *url, bool allowed)
{
    char site[BROWSER_ORIGIN_LIMIT];
    if (sites == NULL || count == NULL || !security_site_key(url, site))
        return false;
    size_t found = *count;
    for (size_t i = 0; i < *count; i++)
        if (strcmp(sites[i], site) == 0) { found = i; break; }
    if (allowed) {
        if (found < *count) return true;
        if (*count >= BROWSER_SECURITY_SITE_LIMIT) return false;
        snprintf(sites[(*count)++], BROWSER_ORIGIN_LIMIT, "%s", site);
        return true;
    }
    if (found == *count) return true;
    if (found + 1u < *count)
        memmove(sites[found], sites[found + 1u],
                (*count - found - 1u) * sizeof(sites[0]));
    memset(sites[--*count], 0, sizeof(sites[0]));
    return true;
}

bool browser_session_set_mixed_content_site_allowed(
    BrowserSession *session, const char *url, bool allowed)
{
    return session != NULL && security_site_list_set(
        session->mixed_content_allowed_sites,
        &session->mixed_content_allowed_site_count, url, allowed);
}

bool browser_session_mixed_content_site_allowed(
    const BrowserSession *session, const char *url)
{
    return session != NULL && security_site_list_contains(
        session->mixed_content_allowed_sites,
        session->mixed_content_allowed_site_count, url);
}

bool browser_session_set_third_party_cookie_site_allowed(
    BrowserSession *session, const char *url, bool allowed)
{
    return session != NULL && security_site_list_set(
        session->third_party_cookie_allowed_sites,
        &session->third_party_cookie_allowed_site_count, url, allowed);
}

bool browser_session_third_party_cookie_site_allowed(
    const BrowserSession *session, const char *url)
{
    return session != NULL && security_site_list_contains(
        session->third_party_cookie_allowed_sites,
        session->third_party_cookie_allowed_site_count, url);
}

static bool hsts_host_from_url(const char *url, char *host, size_t capacity)
{
    TilefinchUrl parsed;
    if (url == NULL || host == NULL || capacity == 0
        || !tilefinch_url_parse(url, &parsed)
        || parsed.host_length == 0 || parsed.host_length >= capacity
        || parsed.ipv6_literal) return false;
    bool only_address = true;
    for (size_t i = 0; i < parsed.host_length; i++) {
        unsigned char c = (unsigned char) url[parsed.host_offset + i];
        if (!(isdigit(c) || c == '.')) only_address = false;
        host[i] = (char) tolower(c);
    }
    host[parsed.host_length] = '\0';
    return !only_address;
}

static bool hsts_host_matches(const char *host, const char *stored,
                              bool include_subdomains)
{
    if (strcmp(host, stored) == 0) return true;
    size_t host_length = strlen(host), stored_length = strlen(stored);
    return include_subdomains && host_length > stored_length
        && host[host_length - stored_length - 1u] == '.'
        && strcmp(host + host_length - stored_length, stored) == 0;
}

bool browser_session_hsts_upgrade_url(
    BrowserSession *session, const char *url,
    char *output, size_t output_capacity)
{
    if (url == NULL || output == NULL || output_capacity == 0) return false;
    TilefinchUrl parsed;
    if (!tilefinch_url_parse(url, &parsed)) return false;
    if (parsed.scheme != TILEFINCH_URL_SCHEME_HTTP || session == NULL) {
        return tilefinch_url_normalize(url, output, output_capacity);
    }
    char host[BROWSER_ORIGIN_LIMIT];
    if (!hsts_host_from_url(url, host, sizeof(host)))
        return tilefinch_url_normalize(url, output, output_capacity);
    int64_t now = cookie_now();
    for (size_t i = 0; i < BROWSER_HSTS_ENTRY_LIMIT; i++) {
        if (session->hsts[i].host[0] == '\0'
            || session->hsts[i].expires_at <= now
            || !hsts_host_matches(host, session->hsts[i].host,
                                  session->hsts[i].include_subdomains)) {
            continue;
        }
        session->hsts[i].stamp = ++session->hsts_clock;
        return tilefinch_url_upgrade_to_https(url, output, output_capacity);
    }
    return tilefinch_url_normalize(url, output, output_capacity);
}

static bool browser_session_hsts_apply(
    BrowserSession *session, const char *host, uint64_t max_age,
    bool include_subdomains);

bool browser_session_hsts_observe(
    BrowserSession *session, const char *response_url,
    const char *headers, size_t headers_length)
{
    if (session == NULL || response_url == NULL
        || (headers == NULL && headers_length != 0)) return false;
    FetchResponseSecurityMetadata metadata;
    fetch_response_security_metadata_reset(&metadata);
    for (size_t offset = 0; offset < headers_length;) {
        const char *line = headers + offset;
        const char *newline = memchr(line, '\n', headers_length - offset);
        size_t length = newline == NULL ? headers_length - offset
                                        : (size_t) (newline - line);
        const char *colon = memchr(line, ':', length);
        if (colon != NULL) {
            const char *value = colon + 1;
            const char *end = line + length;
            while (value < end && (*value == ' ' || *value == '\t')) value++;
            while (end > value && (end[-1] == '\r' || end[-1] == ' '
                                   || end[-1] == '\t')) end--;
            (void) fetch_response_security_metadata_collect(
                &metadata, line, (size_t) (colon - line), value,
                (size_t) (end - value), true);
        }
        offset += length + (newline == NULL ? 0u : 1u);
    }
    return browser_session_hsts_observe_metadata(
        session, response_url, &metadata);
}

bool browser_session_hsts_observe_metadata(
    BrowserSession *session, const char *response_url,
    const FetchResponseSecurityMetadata *metadata)
{
    TilefinchUrl parsed;
    char host[BROWSER_ORIGIN_LIMIT];
    return session != NULL && response_url != NULL && metadata != NULL
        && metadata->version == FETCH_RESPONSE_SECURITY_METADATA_VERSION
        && metadata->hsts_state == FETCH_SECURITY_FIELD_VALID
        && tilefinch_url_parse(response_url, &parsed)
        && parsed.scheme == TILEFINCH_URL_SCHEME_HTTPS
        && hsts_host_from_url(response_url, host, sizeof(host))
        && browser_session_hsts_apply(
            session, host, metadata->hsts_max_age,
            metadata->hsts_include_subdomains);
}

static bool browser_session_hsts_apply(
    BrowserSession *session, const char *host, uint64_t max_age,
    bool include_subdomains)
{
    if (session == NULL || host == NULL || host[0] == '\0') return false;
    size_t existing = BROWSER_HSTS_ENTRY_LIMIT;
    size_t empty = BROWSER_HSTS_ENTRY_LIMIT;
    size_t oldest = 0;
    for (size_t i = 0; i < BROWSER_HSTS_ENTRY_LIMIT; i++) {
        if (strcmp(session->hsts[i].host, host) == 0) {
            existing = i;
            break;
        }
        if (session->hsts[i].host[0] == '\0'
            && empty == BROWSER_HSTS_ENTRY_LIMIT) empty = i;
        if (session->hsts[i].stamp < session->hsts[oldest].stamp) oldest = i;
    }
    if (max_age == 0) {
        if (existing != BROWSER_HSTS_ENTRY_LIMIT) {
            memset(&session->hsts[existing], 0,
                   sizeof(session->hsts[existing]));
        }
        return true;
    }
    size_t slot = existing != BROWSER_HSTS_ENTRY_LIMIT ? existing
        : (empty != BROWSER_HSTS_ENTRY_LIMIT ? empty : oldest);
    int64_t now = cookie_now();
    uint64_t room = now < INT64_MAX ? (uint64_t) (INT64_MAX - now) : 0;
    session->hsts[slot].expires_at = max_age > room
        ? INT64_MAX : now + (int64_t) max_age;
    session->hsts[slot].include_subdomains = include_subdomains;
    session->hsts[slot].stamp = ++session->hsts_clock;
    snprintf(session->hsts[slot].host, sizeof(session->hsts[slot].host),
             "%s", host);
    return true;
}

static bool cookie_store_third_party_allowed(
    const BrowserCookieStore *store, const TilefinchRequestContext *request)
{
    if (store == NULL || request == NULL) return false;
    /* A cross-site top-level navigation is changing the first-party site,
       not embedding that site as a third party. Its ordinary Lax/Strict
       rules still apply independently below. */
    if (request->top_level_navigation) return true;
    const char *top_level = request->top_level_url != NULL
        ? request->top_level_url : request->initiator_url;
    return security_site_list_contains(
        store->third_party_cookie_allowed_sites,
        store->third_party_cookie_allowed_site_count, top_level);
}

static BrowserCookieEntry *find_cookie(BrowserCookieStore *store,
                                       const char *domain, const char *path,
                                       size_t path_length,
                                       const char *name,
                                       const char *partition_key)
{
    for (size_t i = 0; i < BROWSER_COOKIE_ENTRIES; i++) {
        BrowserCookieEntry *entry = &store->cookies[i];
        if (entry->value != NULL && strcmp(entry->domain, domain) == 0
            && entry->path_length == path_length
            && memcmp(
                   browser_cookie_entry_path(entry), path,
                   path_length) == 0
            && strcmp(entry->name, name) == 0
            && strcmp(entry->partition_key,
                      partition_key == NULL ? "" : partition_key) == 0) {
            return entry;
        }
    }
    return NULL;
}

static void clear_cookie_entry(BrowserCookieStore *store,
                               BrowserCookieEntry *entry)
{
    if (store == NULL || entry == NULL || entry->value == NULL) return;
    *store->cookie_bytes -= entry->value_length;
    if (entry->long_path != NULL) {
        *store->cookie_long_path_bytes -= entry->path_length;
        budget_free(store->budget, entry->long_path);
    }
    budget_free(store->budget, entry->value);
    memset(entry, 0, sizeof(*entry));
}

static void reclaim_expired_cookies(BrowserCookieStore *store, int64_t now)
{
    for (size_t i = 0; i < BROWSER_COOKIE_ENTRIES; i++) {
        BrowserCookieEntry *entry = &store->cookies[i];
        if (entry->value != NULL && entry->expires_at != 0
            && entry->expires_at <= now) {
            clear_cookie_entry(store, entry);
        }
    }
}

typedef struct {
    char host[BROWSER_ORIGIN_LIMIT];
    const char *path;
    size_t path_length;
    bool secure;
} CookieURL;

static bool parse_cookie_url(const char *url, CookieURL *parsed)
{
    TilefinchUrl view;
    if (url == NULL || parsed == NULL || !tilefinch_url_parse(url, &view))
        return false;
    parsed->secure = tilefinch_url_is_secure(&view);
    size_t host_length = view.host_length;
    if (host_length == 0 || host_length >= sizeof(parsed->host)) return false;
    for (size_t i = 0; i < host_length; i++) {
        parsed->host[i] = (char) tolower(
            (unsigned char) url[view.host_offset + i]);
    }
    parsed->host[host_length] = '\0';
    parsed->path = view.path_length == 0 ? "/" : url + view.path_offset;
    parsed->path_length = view.path_length == 0 ? 1 : view.path_length;
    return true;
}

static bool domain_matches(const char *host, const char *domain)
{
    if (strcmp(host, domain) == 0) return true;
    size_t host_length = strlen(host), domain_length = strlen(domain);
    return host_length > domain_length
           && host[host_length - domain_length - 1] == '.'
           && strcmp(host + host_length - domain_length, domain) == 0;
}

static bool path_matches(const char *request_path, size_t request_length,
                         const char *cookie_path, size_t cookie_length)
{
    if (request_path == NULL || cookie_path == NULL || cookie_length == 0
        || request_length < cookie_length
        || memcmp(request_path, cookie_path, cookie_length) != 0) return false;
    return request_length == cookie_length
        || cookie_path[cookie_length - 1] == '/'
        || request_path[cookie_length] == '/';
}

static bool insecure_cookie_overlays_secure(
    const BrowserCookieStore *store, const char *name, const char *domain,
    const char *path, size_t path_length)
{
    if (store == NULL || name == NULL || domain == NULL || path == NULL)
        return false;
    for (size_t i = 0; i < BROWSER_COOKIE_ENTRIES; i++) {
        const BrowserCookieEntry *existing = &store->cookies[i];
        if (existing->value == NULL || !existing->secure
            || strcmp(existing->name, name) != 0) {
            continue;
        }
        bool domains_overlap = domain_matches(domain, existing->domain)
            || domain_matches(existing->domain, domain);
        /* RFC 6265bis deliberately makes this comparison asymmetric: a new
           deeper path can shadow an existing Secure cookie, while a new
           broader or disjoint path cannot. */
        if (domains_overlap
            && path_matches(
                path, path_length, browser_cookie_entry_path(existing),
                existing->path_length)) return true;
    }
    return false;
}

static bool cookie_string(const BrowserCookieStore *store, const char *url,
                          const TilefinchRequestContext *request,
                          const TilefinchRequestFacts *facts,
                          bool include_http_only, char *output,
                          size_t output_capacity)
{
    if (store == NULL || store->cookies == NULL || output == NULL
        || output_capacity == 0) return false;
    CookieURL parsed;
    if (!parse_cookie_url(url, &parsed)) return false;
    size_t used = 0;
    int64_t now = cookie_now();
    char partition_key[BROWSER_ORIGIN_LIMIT] = {0};
    const char *top_level = request == NULL ? url
        : (request->top_level_url != NULL ? request->top_level_url
                                         : request->initiator_url);
    if (top_level != NULL) {
        (void) tilefinch_url_site_key(top_level, partition_key,
                                  sizeof(partition_key));
    }
    bool same_site = request == NULL
        || (facts != NULL ? facts->same_site
                          : tilefinch_request_same_site(request));
    bool third_party_allowed = same_site
        || cookie_store_third_party_allowed(store, request);
    bool lax_allowed = request == NULL
        || (facts != NULL ? facts->allows_lax_cookie
                          : tilefinch_request_allows_lax_cookie(request));
    output[0] = '\0';
    size_t candidates[BROWSER_COOKIE_ENTRIES];
    size_t candidate_count = 0;
    for (size_t i = 0; i < BROWSER_COOKIE_ENTRIES; i++) {
        const BrowserCookieEntry *entry = &store->cookies[i];
        if (entry->value == NULL
            || (entry->expires_at != 0 && entry->expires_at <= now)
            || (!include_http_only && entry->http_only)
            || (entry->secure && !parsed.secure)
            || (entry->host_only
                ? strcmp(parsed.host, entry->domain) != 0
                : !domain_matches(parsed.host, entry->domain))
            || !path_matches(
                   parsed.path, parsed.path_length,
                   browser_cookie_entry_path(entry), entry->path_length)
            || (entry->same_site == BROWSER_COOKIE_SAME_SITE_STRICT
                && !same_site)
            || ((entry->same_site == BROWSER_COOKIE_SAME_SITE_LAX
                 || entry->same_site == BROWSER_COOKIE_SAME_SITE_DEFAULT)
                && !lax_allowed)
            || (entry->partitioned
                && (partition_key[0] == '\0'
                    || strcmp(entry->partition_key, partition_key) != 0))) {
            continue;
        }
        if (!entry->partitioned && !third_party_allowed) continue;
        /* RFC 6265 serialization prefers more specific paths, then older
           creation times. This is observable for same-named cookies. */
        size_t entry_path_length = entry->path_length;
        size_t position = candidate_count;
        while (position != 0) {
            const BrowserCookieEntry *previous =
                &store->cookies[candidates[position - 1]];
            size_t previous_path_length = previous->path_length;
            if (previous_path_length > entry_path_length
                || (previous_path_length == entry_path_length
                    && previous->creation_sequence
                           <= entry->creation_sequence)) {
                break;
            }
            candidates[position] = candidates[position - 1];
            position--;
        }
        candidates[position] = i;
        candidate_count++;
    }
    for (size_t candidate = 0; candidate < candidate_count; candidate++) {
        const BrowserCookieEntry *entry =
            &store->cookies[candidates[candidate]];
        size_t name_length = strlen(entry->name);
        size_t needed = (used == 0 ? 0 : 2) + name_length + 1
                        + entry->value_length;
        if (needed >= output_capacity - used) {
            /* Callers may deliberately treat cookie construction failure as
               an empty Cookie header. Never leave a sendable partial prefix. */
            output[0] = '\0';
            return false;
        }
        if (used != 0) { output[used++] = ';'; output[used++] = ' '; }
        memcpy(output + used, entry->name, name_length); used += name_length;
        output[used++] = '=';
        memcpy(output + used, entry->value, entry->value_length);
        used += entry->value_length;
        output[used] = '\0';
    }
    return true;
}

bool browser_session_cookie_get(const BrowserSession *session,
                                const char *url, char *output,
                                size_t output_capacity)
{
    if (session != NULL && !session->site_data_allowed) {
        if (output == NULL || output_capacity == 0) return false;
        output[0] = '\0';
        return true;
    }
    BrowserCookieStore store = browser_session_cookie_store(
        (BrowserSession *) session);
    return cookie_string(
        &store, url, NULL, NULL, false, output, output_capacity);
}

bool browser_session_cookie_header(const BrowserSession *session,
                                   const char *url, char *output,
                                   size_t output_capacity)
{
    if (session != NULL && !session->site_data_allowed) {
        if (output == NULL || output_capacity == 0) return false;
        output[0] = '\0';
        return true;
    }
    TilefinchRequestContext context = {
        .target_url = url, .initiator_url = url, .top_level_url = url,
        .method = "GET", .mode = TILEFINCH_REQUEST_MODE_SAME_ORIGIN,
        .credentials = TILEFINCH_CREDENTIALS_INCLUDE,
        .destination = TILEFINCH_DESTINATION_OTHER
    };
    BrowserCookieStore store = browser_session_cookie_store(
        (BrowserSession *) session);
    return cookie_string(&store, url, &context, NULL, true, output,
                         output_capacity);
}

bool browser_session_cookie_header_context(
    const BrowserSession *session, const TilefinchRequestContext *context,
    char *output, size_t output_capacity)
{
    TilefinchRequestFacts facts;
    if (!tilefinch_request_context_analyze(context, &facts)) {
        if (output != NULL && output_capacity != 0) output[0] = '\0';
        return false;
    }
    return browser_session_cookie_header_request_facts(
        session, context, &facts, output, output_capacity);
}

bool browser_session_cookie_header_request_facts(
    const BrowserSession *session, const TilefinchRequestContext *context,
    const TilefinchRequestFacts *facts,
    char *output, size_t output_capacity)
{
    if (session == NULL || context == NULL || facts == NULL
        || facts->source != context || !facts->valid
        || output == NULL || output_capacity == 0) {
        if (output != NULL && output_capacity != 0) output[0] = '\0';
        return false;
    }
    if (!session->site_data_allowed) {
        output[0] = '\0';
        return true;
    }
    if (!facts->sends_credentials) {
        output[0] = '\0';
        return true;
    }
    BrowserCookieStore store = browser_session_cookie_store(
        (BrowserSession *) session);
    return cookie_string(
        &store, context->target_url, context, facts, true,
        output, output_capacity);
}

/* Creation order is observable when paths have equal specificity. If a long
   session (or a replay seed near SIZE_MAX) exhausts the monotonic counter,
   compact the live order without allocating or changing serialization. */
static void cookie_clock_make_room(BrowserCookieStore *store)
{
    if (store == NULL || store->cookies == NULL || store->cookie_clock == NULL
        || *store->cookie_clock < SIZE_MAX - 1) return;
    size_t ordered[BROWSER_COOKIE_ENTRIES];
    size_t count = 0;
    for (size_t i = 0; i < BROWSER_COOKIE_ENTRIES; i++) {
        if (store->cookies[i].value == NULL) continue;
        size_t position = count;
        while (position != 0) {
            size_t prior = ordered[position - 1];
            size_t prior_sequence = store->cookies[prior].creation_sequence;
            size_t current_sequence = store->cookies[i].creation_sequence;
            if (prior_sequence < current_sequence
                || (prior_sequence == current_sequence && prior < i)) break;
            ordered[position] = prior;
            position--;
        }
        ordered[position] = i;
        count++;
    }
    for (size_t i = 0; i < count; i++) {
        store->cookies[ordered[i]].creation_sequence = i + 1;
    }
    *store->cookie_clock = count;
}

static bool cookie_set(BrowserCookieStore *store,
                       const TilefinchRequestContext *request,
                       const char *cookie, bool from_http)
{
    if (store == NULL || store->budget == NULL || store->cookies == NULL
        || store->cookie_bytes == NULL
        || store->cookie_long_path_bytes == NULL
        || store->cookie_clock == NULL
        || cookie == NULL || request == NULL
        || !tilefinch_request_context_valid(request)
        || !tilefinch_request_sends_credentials(request)) return false;
    for (const unsigned char *at = (const unsigned char *) cookie;
         *at != '\0'; at++) {
        if (*at < 0x20 || *at >= 0x7f) return false;
    }
    const char *url = request == NULL ? NULL : request->target_url;
    CookieURL parsed;
    if (!parse_cookie_url(url, &parsed)) return false;
    const char *equals = strchr(cookie, '=');
    if (equals == NULL) return false;
    size_t name_length = (size_t) (equals - cookie);
    while (name_length > 0 && isspace((unsigned char) cookie[name_length - 1])) {
        name_length--;
    }
    const char *value = equals + 1;
    while (isspace((unsigned char) *value)) value++;
    const char *end = strchr(value, ';');
    size_t value_length = end == NULL ? strlen(value) : (size_t) (end - value);
    while (value_length > 0
           && isspace((unsigned char) value[value_length - 1])) value_length--;
    if (name_length == 0 || name_length >= BROWSER_KEY_LIMIT) return false;
    static const char separators[] = "()<>@,;:\\\"/[]?={} \t";
    for (size_t i = 0; i < name_length; i++) {
        unsigned char byte = (unsigned char) cookie[i];
        if (byte <= 0x20 || byte >= 0x7f
            || strchr(separators, (int) byte) != NULL) return false;
    }
    char name[BROWSER_KEY_LIMIT];
    memcpy(name, cookie, name_length); name[name_length] = '\0';
    char domain[BROWSER_ORIGIN_LIMIT];
    snprintf(domain, sizeof(domain), "%s", parsed.host);
    const char *path = "/";
    size_t path_length = 1;
    for (size_t i = parsed.path_length; i > 1; i--) {
        if (parsed.path[i - 1] == '/') {
            path = parsed.path;
            path_length = i - 1;
            break;
        }
    }
    bool host_only = true, domain_attribute_present = false;
    bool secure = false, http_only = false;
    BrowserCookieSameSite same_site = BROWSER_COOKIE_SAME_SITE_DEFAULT;
    bool partitioned = false, remove = false;
    bool max_age_valid = false, expires_valid = false;
    int64_t max_age = 0, expires_at = 0;
    const char *attribute = end == NULL ? NULL : end + 1;
    while (attribute != NULL && *attribute != '\0') {
        while (*attribute == ';' || isspace((unsigned char) *attribute)) {
            attribute++;
        }
        const char *attribute_end = strchr(attribute, ';');
        size_t attribute_length = attribute_end == NULL
                                  ? strlen(attribute)
                                  : (size_t) (attribute_end - attribute);
        while (attribute_length > 0
               && isspace((unsigned char) attribute[attribute_length - 1])) {
            attribute_length--;
        }
        const char *attribute_equals = memchr(attribute, '=', attribute_length);
        size_t key_length = attribute_equals == NULL ? attribute_length
                            : (size_t) (attribute_equals - attribute);
        while (key_length > 0
               && isspace((unsigned char) attribute[key_length - 1])) {
            key_length--;
        }
        const char *attribute_value = attribute_equals == NULL ? ""
                                      : attribute_equals + 1;
        size_t attribute_value_length = attribute_equals == NULL ? 0
            : attribute_length - (size_t) (attribute_value - attribute);
        while (attribute_value_length > 0
               && isspace((unsigned char) *attribute_value)) {
            attribute_value++; attribute_value_length--;
        }
        if (key_length == 6 && strncasecmp(attribute, "Domain", 6) == 0
            && attribute_value_length != 0) {
            domain_attribute_present = true;
            if (*attribute_value == '.') {
                attribute_value++; attribute_value_length--;
            }
            if (attribute_value_length == 0
                || attribute_value_length >= sizeof(domain)) return false;
            for (size_t i = 0; i < attribute_value_length; i++) {
                domain[i] = (char) tolower((unsigned char) attribute_value[i]);
            }
            domain[attribute_value_length] = '\0';
            if (!domain_matches(parsed.host, domain)) return false;
            bool exact_host = strcmp(parsed.host, domain) == 0;
            bool public_suffix = false;
            if (!tilefinch_public_suffix_classify(domain, &public_suffix)) {
                /* Exact invalid/non-DNS hosts remain safely host-only. A
                   parent scope is never inferred without PSL classification. */
                if (!exact_host) return false;
                host_only = true;
            } else if (public_suffix) {
                /* RFC 6265's public-suffix exception admits an exact request
                   host but deliberately turns it back into a host-only cookie. */
                if (!exact_host) return false;
                host_only = true;
            } else {
                host_only = false;
            }
        } else if (key_length == 4
                   && strncasecmp(attribute, "Path", 4) == 0
                   && attribute_value_length != 0
                   && attribute_value[0] == '/') {
            path = attribute_value;
            path_length = attribute_value_length;
        } else if (key_length == 6
                   && strncasecmp(attribute, "Secure", 6) == 0) {
            secure = true;
        } else if (key_length == 8
                   && strncasecmp(attribute, "HttpOnly", 8) == 0) {
            http_only = from_http;
        } else if (key_length == 8
                   && strncasecmp(attribute, "SameSite", 8) == 0) {
            if (attribute_value_length == 4
                && strncasecmp(attribute_value, "None", 4) == 0) {
                same_site = BROWSER_COOKIE_SAME_SITE_NONE;
            } else if (attribute_value_length == 3
                       && strncasecmp(attribute_value, "Lax", 3) == 0) {
                same_site = BROWSER_COOKIE_SAME_SITE_LAX;
            } else if (attribute_value_length == 6
                       && strncasecmp(attribute_value, "Strict", 6) == 0) {
                same_site = BROWSER_COOKIE_SAME_SITE_STRICT;
            }
        } else if (key_length == 11
                   && strncasecmp(attribute, "Partitioned", 11) == 0) {
            partitioned = true;
        } else if (key_length == 7
                   && strncasecmp(attribute, "Max-Age", 7) == 0
                   && attribute_value_length != 0
                   && attribute_value_length < 32) {
            char number[32];
            memcpy(number, attribute_value, attribute_value_length);
            number[attribute_value_length] = '\0';
            char *after = NULL;
            long long parsed_age = strtoll(number, &after, 10);
            if (after != number && *after == '\0') {
                max_age_valid = true;
                max_age = (int64_t) parsed_age;
            }
        } else if (key_length == 7
                   && strncasecmp(attribute, "Expires", 7) == 0) {
            expires_valid = parse_cookie_date(attribute_value,
                                              attribute_value_length,
                                              &expires_at);
        }
        attribute = attribute_end == NULL ? NULL : attribute_end + 1;
    }
    int64_t now = cookie_now();
    if (max_age_valid) {
        if (max_age <= 0) remove = true;
        else expires_at = max_age > INT64_MAX - now
                          ? INT64_MAX : now + max_age;
    } else if (expires_valid && expires_at <= now) {
        remove = true;
    }
    if (getenv("TILEFINCH_TRACE_COOKIE") != NULL) {
        fprintf(stderr, "cookie-store name=%s remove=%d expires-valid=%d expires-delta=%lld max-age-valid=%d\n",
                name, remove, expires_valid,
                (long long) (expires_valid ? expires_at - now : 0),
                max_age_valid);
    }
    if (secure && !parsed.secure) return false;
    if (same_site == BROWSER_COOKIE_SAME_SITE_NONE && !secure) return false;
    if (partitioned && !secure) return false;
    /* Partitioned cookies remain available cross-site. Ordinary cookies are
       ignored (rather than turning a valid response into a network error)
       unless the top-level site has an explicit bounded grant. */
    if (!partitioned && !tilefinch_request_same_site(request)
        && !cookie_store_third_party_allowed(store, request)) return true;
    if (path_length == 0 || path_length >= BROWSER_COOKIE_LONG_PATH_LIMIT) {
        return false;
    }
    if (strncmp(name, "__Secure-", 9) == 0 && (!secure || !parsed.secure))
        return false;
    if (strncmp(name, "__Host-", 7) == 0
        && (!secure || !parsed.secure || domain_attribute_present
            || path_length != 1 || path[0] != '/')) return false;
    char partition_key[BROWSER_ORIGIN_LIMIT] = {0};
    if (partitioned) {
        const char *top_level = request->top_level_url != NULL
            ? request->top_level_url : request->initiator_url;
        if (top_level == NULL
            || !tilefinch_url_site_key(top_level, partition_key,
                                    sizeof(partition_key))) return false;
    }
    /* Expired cookies are absent from the logical jar, so they must not keep
       occupying its fixed slots or byte quota. Reclaim only after the new
       cookie has passed syntax and security validation, but before matching
       or selecting an insertion slot. */
    reclaim_expired_cookies(store, now);
    /* An insecure origin must not create a same-named, more-specific cookie
       which serializes ahead of an applicable Secure cookie. This comparison
       spans compatible domain/path scopes and partitioned/unpartitioned jars,
       not only the exact storage key. */
    if (!secure && !parsed.secure
        && insecure_cookie_overlays_secure(
               store, name, domain, path, path_length)) {
        return false;
    }
    BrowserCookieEntry *entry = find_cookie(
        store, domain, path, path_length, name, partition_key);
    /* Preserve the separate exact-key non-HTTP protection for HttpOnly. */
    if (entry != NULL && !from_http && entry->http_only) return false;
    if (remove) {
        clear_cookie_entry(store, entry);
        return true;
    }
    bool evicting = false;
    if (entry == NULL) {
        BrowserCookieEntry *oldest_domain = NULL;
        BrowserCookieEntry *oldest_global = NULL;
        size_t domain_count = 0;
        for (size_t i = 0; i < BROWSER_COOKIE_ENTRIES; i++) {
            BrowserCookieEntry *candidate = &store->cookies[i];
            if (candidate->value == NULL) continue;
            /* Eviction must not become a back door around the protections
               enforced above on an exact-key overwrite.  Script (!from_http)
               may never retire an HttpOnly cookie, and an insecure origin may
               never retire a Secure one -- otherwise eight scripted writes
               evict the session cookie and the next write forges it. */
            bool evictable = (from_http || !candidate->http_only)
                && (secure || parsed.secure || !candidate->secure);
            if (strcmp(candidate->domain, domain) == 0) domain_count++;
            if (!evictable) continue;
            if (oldest_global == NULL
                || candidate->creation_sequence
                       < oldest_global->creation_sequence) {
                oldest_global = candidate;
            }
            if (strcmp(candidate->domain, domain) == 0) {
                if (oldest_domain == NULL
                    || candidate->creation_sequence
                           < oldest_domain->creation_sequence) {
                    oldest_domain = candidate;
                }
            }
        }
        if (domain_count >= BROWSER_COOKIE_PER_DOMAIN_LIMIT) {
            entry = oldest_domain;
            evicting = entry != NULL;
        } else {
            for (size_t i = 0; i < BROWSER_COOKIE_ENTRIES; i++) {
                if (store->cookies[i].value == NULL) {
                    entry = &store->cookies[i];
                    break;
                }
            }
            if (entry == NULL) {
                entry = oldest_global;
                evicting = entry != NULL;
            }
        }
    }
    if (entry == NULL) return false;
    bool newly_created = entry->value == NULL;
    size_t old_length = entry->value == NULL ? 0 : entry->value_length;
    size_t old_long_path_length =
        entry->long_path == NULL ? 0 : entry->path_length;
    if (*store->cookie_bytes - old_length + value_length
        > store->maximum_cookie_bytes) return false;
    size_t new_long_path_length =
        path_length >= sizeof(entry->path) ? path_length : 0;
    if (*store->cookie_long_path_bytes - old_long_path_length
            + new_long_path_length
        > store->maximum_cookie_long_path_bytes) return false;
    char *copy = budget_malloc(store->budget, value_length + 1);
    if (copy == NULL) return false;
    memcpy(copy, value, value_length); copy[value_length] = '\0';
    char *long_path = NULL;
    if (new_long_path_length != 0) {
        long_path = budget_malloc(store->budget, path_length + 1);
        if (long_path == NULL) {
            budget_free(store->budget, copy);
            return false;
        }
        memcpy(long_path, path, path_length);
        long_path[path_length] = '\0';
    }
    if (evicting) {
        clear_cookie_entry(store, entry);
        newly_created = true;
        old_length = 0;
        old_long_path_length = 0;
    }
    size_t creation_sequence = entry->creation_sequence;
    if (newly_created) {
        cookie_clock_make_room(store);
        creation_sequence = ++*store->cookie_clock;
    }
    if (!newly_created && entry->long_path != NULL) {
        budget_free(store->budget, entry->long_path);
        entry->long_path = NULL;
    }
    budget_free(store->budget, entry->value);
    snprintf(entry->domain, sizeof(entry->domain), "%s", domain);
    memset(entry->path, 0, sizeof(entry->path));
    if (long_path == NULL) {
        memcpy(entry->path, path, path_length);
        entry->path[path_length] = '\0';
    }
    entry->long_path = long_path;
    entry->path_length = path_length;
    snprintf(entry->name, sizeof(entry->name), "%s", name);
    entry->value = copy; entry->value_length = value_length;
    entry->expires_at = expires_at;
    entry->host_only = host_only;
    entry->secure = secure;
    entry->http_only = http_only;
    entry->same_site = same_site;
    entry->partitioned = partitioned;
    snprintf(entry->partition_key, sizeof(entry->partition_key), "%s",
             partition_key);
    entry->creation_sequence = creation_sequence;
    *store->cookie_bytes = *store->cookie_bytes - old_length + value_length;
    *store->cookie_long_path_bytes =
        *store->cookie_long_path_bytes - old_long_path_length
        + new_long_path_length;
    return true;
}

bool browser_session_cookie_set(BrowserSession *session, const char *url,
                                const char *cookie)
{
    if (session == NULL || !session->site_data_allowed) return false;
    TilefinchRequestContext context = {
        .target_url = url, .initiator_url = url, .top_level_url = url,
        .method = "GET", .mode = TILEFINCH_REQUEST_MODE_SAME_ORIGIN,
        .credentials = TILEFINCH_CREDENTIALS_INCLUDE,
        .destination = TILEFINCH_DESTINATION_OTHER
    };
    BrowserCookieStore store = browser_session_cookie_store(session);
    return cookie_set(&store, &context, cookie, false);
}

bool browser_session_cookie_set_http(BrowserSession *session,
                                     const char *url, const char *cookie)
{
    if (session == NULL || !session->site_data_allowed) return false;
    TilefinchRequestContext context = {
        .target_url = url, .initiator_url = url, .top_level_url = url,
        .method = "GET", .mode = TILEFINCH_REQUEST_MODE_SAME_ORIGIN,
        .credentials = TILEFINCH_CREDENTIALS_INCLUDE,
        .destination = TILEFINCH_DESTINATION_OTHER
    };
    BrowserCookieStore store = browser_session_cookie_store(session);
    return cookie_set(&store, &context, cookie, true);
}

bool browser_session_cookie_set_context(
    BrowserSession *session, const TilefinchRequestContext *context,
    const char *cookie)
{
    if (session == NULL || !session->site_data_allowed) return false;
    BrowserCookieStore store = browser_session_cookie_store(session);
    return context != NULL && cookie_set(&store, context, cookie, false);
}

bool browser_session_cookie_set_http_context(
    BrowserSession *session, const TilefinchRequestContext *context,
    const char *cookie)
{
    if (session == NULL || !session->site_data_allowed) return false;
    BrowserCookieStore store = browser_session_cookie_store(session);
    return context != NULL && cookie_set(&store, context, cookie, true);
}

static bool cookie_seed_text_valid(const char *value, size_t capacity,
                                   bool allow_empty)
{
    if (value == NULL || capacity == 0) return false;
    const char *end = memchr(value, '\0', capacity);
    if (end == NULL) return false;
    size_t length = (size_t) (end - value);
    if ((!allow_empty && length == 0) || length >= capacity) return false;
    for (size_t i = 0; i < length; i++) {
        unsigned char byte = (unsigned char) value[i];
        if (byte < 0x20 || byte >= 0x7f) return false;
    }
    return true;
}

static bool cookie_seed_entry_valid(const BrowserCookieSeedEntry *entry)
{
    if (entry == NULL
        || !cookie_seed_text_valid(entry->domain,
                                   sizeof(entry->domain), false)
        || !cookie_seed_text_valid(entry->path,
                                   sizeof(entry->path), false)
        || entry->path[0] != '/'
        || !cookie_seed_text_valid(entry->name,
                                   sizeof(entry->name), false)
        || !cookie_seed_text_valid(entry->partition_key,
                                   sizeof(entry->partition_key), true)
        || entry->same_site > BROWSER_COOKIE_SAME_SITE_NONE
        /* Keep SIZE_MAX outside the imported identity space. Ordinary
           insertion compacts a near-exhausted clock before incrementing. */
        || entry->creation_sequence == 0
        || entry->creation_sequence == SIZE_MAX
        || (entry->same_site == BROWSER_COOKIE_SAME_SITE_NONE
            && !entry->secure)
        || (entry->partitioned
            && (!entry->secure || entry->partition_key[0] == '\0'))
        || (!entry->partitioned && entry->partition_key[0] != '\0')
        || (strncmp(entry->name, "__Secure-", 9) == 0
            && !entry->secure)
        || (strncmp(entry->name, "__Host-", 7) == 0
            && (!entry->secure || !entry->host_only
                || strcmp(entry->path, "/") != 0))) return false;
    static const char separators[] = "()<>@,;:\\\"/[]?={} \t";
    for (const unsigned char *at = (const unsigned char *) entry->name;
         *at != '\0'; at++) {
        if (*at <= 0x20 || *at >= 0x7f
            || strchr(separators, (int) *at) != NULL) return false;
    }
    return true;
}

bool browser_session_cookie_import_redacted_seed(
    BrowserSession *session, const BrowserCookieSeedEntry *entries,
    size_t count)
{
    if (session == NULL || session->budget == NULL
        || !session->site_data_allowed
        || count > BROWSER_COOKIE_ENTRIES
        || (count != 0 && entries == NULL)
        || session->cookie_bytes != 0
        || session->cookie_long_path_bytes != 0
        || session->cookie_clock != 0) {
        return false;
    }
    for (size_t i = 0; i < BROWSER_COOKIE_ENTRIES; i++) {
        if (session->cookies[i].value != NULL) return false;
    }
    size_t total = 0;
    size_t maximum_sequence = 0;
    for (size_t i = 0; i < count; i++) {
        const BrowserCookieSeedEntry *entry = &entries[i];
        if (!cookie_seed_entry_valid(entry)
            || entry->value_length > session->maximum_cookie_bytes - total) {
            return false;
        }
        total += entry->value_length;
        if (entry->creation_sequence > maximum_sequence) {
            maximum_sequence = entry->creation_sequence;
        }
        for (size_t prior = 0; prior < i; prior++) {
            const BrowserCookieSeedEntry *other = &entries[prior];
            if (entry->creation_sequence == other->creation_sequence
                || (strcmp(entry->domain, other->domain) == 0
                    && strcmp(entry->path, other->path) == 0
                    && strcmp(entry->name, other->name) == 0
                    && strcmp(entry->partition_key,
                              other->partition_key) == 0)) return false;
        }
    }
    for (size_t i = 0; i < count; i++) {
        const BrowserCookieSeedEntry *seed = &entries[i];
        BrowserCookieEntry *entry = &session->cookies[i];
        char *placeholder = budget_malloc(
            session->budget, seed->value_length + 1);
        if (placeholder == NULL) {
            for (size_t rollback = 0; rollback < i; rollback++) {
                budget_free(session->budget,
                            session->cookies[rollback].value);
                memset(&session->cookies[rollback], 0,
                       sizeof(session->cookies[rollback]));
            }
            session->cookie_bytes = 0;
            session->cookie_clock = 0;
            return false;
        }
        memset(placeholder, 'x', seed->value_length);
        placeholder[seed->value_length] = '\0';
        snprintf(entry->domain, sizeof(entry->domain), "%s", seed->domain);
        snprintf(entry->path, sizeof(entry->path), "%s", seed->path);
        entry->path_length = strlen(entry->path);
        snprintf(entry->name, sizeof(entry->name), "%s", seed->name);
        entry->value = placeholder;
        entry->value_length = seed->value_length;
        /* A trace seed describes the logical jar at its capture boundary.
           Treat it as session-lifetime so wall-clock drift cannot expire it
           before deterministic replay consumes the captured responses. */
        entry->expires_at = 0;
        entry->host_only = seed->host_only;
        entry->secure = seed->secure;
        entry->http_only = seed->http_only;
        entry->same_site = seed->same_site;
        entry->partitioned = seed->partitioned;
        snprintf(entry->partition_key, sizeof(entry->partition_key), "%s",
                 seed->partition_key);
        entry->creation_sequence = seed->creation_sequence;
    }
    session->cookie_bytes = total;
    session->cookie_clock = maximum_sequence;
    return true;
}

void browser_session_cookie_clear(BrowserSession *session)
{
    if (session == NULL || session->budget == NULL) return;
    BrowserCookieStore store = browser_session_cookie_store(session);
    for (size_t i = 0; i < BROWSER_COOKIE_ENTRIES; i++) {
        clear_cookie_entry(&store, &session->cookies[i]);
    }
    session->cookie_clock = 0;
    memset(&session->site_adapter_state, 0,
           sizeof(session->site_adapter_state));
}

BrowserCookieOverlay *browser_session_cookie_overlay_create(
    Budget *budget, const BrowserSession *source)
{
    if (budget == NULL || source == NULL || source->budget == NULL) return NULL;
    BrowserCookieOverlay *overlay = budget_calloc_category(
        budget, BUDGET_CATEGORY_RESOURCE, 1, sizeof(*overlay));
    if (overlay == NULL) return NULL;
    overlay->budget = budget;
    overlay->site_data_allowed = source->site_data_allowed;
    overlay->maximum_cookie_bytes = source->maximum_cookie_bytes;
    overlay->maximum_cookie_long_path_bytes =
        source->maximum_cookie_long_path_bytes;
    overlay->cookie_clock = source->cookie_clock;
    overlay->third_party_cookie_allowed_site_count =
        source->third_party_cookie_allowed_site_count;
    memcpy(overlay->third_party_cookie_allowed_sites,
           source->third_party_cookie_allowed_sites,
           sizeof(overlay->third_party_cookie_allowed_sites));
    /* A blocked session still gets a valid request-private jar, but it is
       intentionally empty. This closes the transport path that otherwise
       bypasses the public cookie-header gate. */
    if (!source->site_data_allowed) return overlay;
    for (size_t i = 0; i < BROWSER_COOKIE_ENTRIES; i++) {
        const BrowserCookieEntry *entry = &source->cookies[i];
        if (entry->value == NULL) continue;
        char *value = budget_malloc_category(
            budget, BUDGET_CATEGORY_RESOURCE, entry->value_length + 1);
        if (value == NULL) {
            browser_cookie_overlay_destroy(overlay);
            return NULL;
        }
        memcpy(value, entry->value, entry->value_length);
        value[entry->value_length] = '\0';
        overlay->cookies[i] = *entry;
        overlay->cookies[i].value = value;
        overlay->cookies[i].long_path = NULL;
        if (entry->long_path != NULL) {
            char *path = budget_malloc_category(
                budget, BUDGET_CATEGORY_RESOURCE,
                entry->path_length + 1);
            if (path == NULL) {
                browser_cookie_overlay_destroy(overlay);
                return NULL;
            }
            memcpy(path, entry->long_path, entry->path_length + 1);
            overlay->cookies[i].long_path = path;
            overlay->cookie_long_path_bytes += entry->path_length;
        }
        overlay->cookie_bytes += entry->value_length;
    }
    return overlay;
}

bool browser_cookie_overlay_header_context(
    const BrowserCookieOverlay *overlay,
    const TilefinchRequestContext *context,
    char *output, size_t output_capacity)
{
    if (overlay == NULL || context == NULL
        || !tilefinch_request_context_valid(context)
        || output == NULL || output_capacity == 0) {
        if (output != NULL && output_capacity != 0) output[0] = '\0';
        return false;
    }
    if (!overlay->site_data_allowed) {
        output[0] = '\0';
        return true;
    }
    if (!tilefinch_request_sends_credentials(context)) {
        output[0] = '\0';
        return true;
    }
    BrowserCookieStore store = browser_overlay_cookie_store(
        (BrowserCookieOverlay *) overlay);
    return cookie_string(&store, context->target_url, context, NULL, true,
                         output, output_capacity);
}

bool browser_cookie_overlay_set_http_context(
    BrowserCookieOverlay *overlay,
    const TilefinchRequestContext *context,
    const char *cookie)
{
    if (overlay == NULL || !overlay->site_data_allowed) return false;
    BrowserCookieStore store = browser_overlay_cookie_store(overlay);
    return context != NULL && cookie_set(&store, context, cookie, true);
}

void browser_cookie_overlay_destroy(BrowserCookieOverlay *overlay)
{
    if (overlay == NULL) return;
    Budget *budget = overlay->budget;
    if (budget != NULL) {
        for (size_t i = 0; i < BROWSER_COOKIE_ENTRIES; i++) {
            budget_free(budget, overlay->cookies[i].value);
            budget_free(budget, overlay->cookies[i].long_path);
        }
        budget_free(budget, overlay);
    }
}

bool browser_session_cache_get(BrowserSession *session, const char *url,
                               const unsigned char **data, size_t *length)
{
    const BrowserCacheEntry *entry = browser_session_cache_lookup(session,
                                                                  url);
    if (entry == NULL) return false;
    if (data != NULL) *data = entry->data;
    if (length != NULL) *length = entry->length;
    return true;
}

const BrowserCacheEntry *browser_session_cache_lookup(
    BrowserSession *session, const char *url)
{
    if (session == NULL || url == NULL) return NULL;
    char key[TILEFINCH_URL_SERIALIZED_LIMIT];
    if (!tilefinch_url_request_key(url, key, sizeof(key))) {
        session->cache_misses++;
        return NULL;
    }
    for (size_t i = 0; i < BROWSER_CACHE_ENTRIES; i++) {
        BrowserCacheEntry *entry = &session->cache[i];
        if (entry->data != NULL
            && !entry->classic_script_origin_variant
            && !entry->resource_grant_valid
            && entry->module_request_fragment == NULL
            && strcmp(entry->url, key) == 0) {
            entry->stamp = ++session->clock;
            session->cache_hits++;
            return entry;
        }
    }
    session->cache_misses++;
    return NULL;
}

BrowserCacheStatus browser_session_cache_match_http(
    BrowserSession *session, const char *url, uint64_t now_ns,
    const BrowserCacheEntry **matched)
{
    if (matched != NULL) *matched = NULL;
    const BrowserCacheEntry *entry = browser_session_cache_lookup(session,
                                                                  url);
    if (entry == NULL) return BROWSER_CACHE_MISS;
    if (matched != NULL) *matched = entry;
    bool fresh = !entry->no_cache
                 && (entry->immutable || now_ns < entry->fresh_until_ns);
    if (fresh) {
        session->cache_fresh_hits++;
        return BROWSER_CACHE_FRESH;
    }
    session->cache_stale_hits++;
    return BROWSER_CACHE_STALE;
}

BrowserCacheStatus browser_session_cache_match_classic_script(
    BrowserSession *session, const char *url,
    const TilefinchRequestContext *context, uint64_t now_ns,
    const BrowserCacheEntry **matched)
{
    BrowserCacheStatus status = browser_session_cache_match_resource(
        session, url, context, now_ns, matched);
    if (status == BROWSER_CACHE_MISS) return status;
    if (matched == NULL || *matched == NULL
        || (*matched)->resource_grant.destination
               != TILEFINCH_DESTINATION_SCRIPT) {
        if (matched != NULL) *matched = NULL;
        return BROWSER_CACHE_MISS;
    }
    return status;
}

BrowserCacheStatus browser_session_cache_match_resource(
    BrowserSession *session, const char *url,
    const TilefinchRequestContext *context, uint64_t now_ns,
    const BrowserCacheEntry **matched)
{
    if (matched != NULL) *matched = NULL;
    if (session == NULL || url == NULL) return BROWSER_CACHE_MISS;
    char key[TILEFINCH_URL_SERIALIZED_LIMIT];
    if (!tilefinch_url_request_key(url, key, sizeof(key))) {
        session->cache_misses++;
        return BROWSER_CACHE_MISS;
    }
    BrowserCacheEntry *entry = cache_find_resource_key(
        session, key, context);
    if (entry == NULL) {
        session->cache_misses++;
        return BROWSER_CACHE_MISS;
    }
    entry->stamp = ++session->clock;
    session->cache_hits++;
    if (matched != NULL) *matched = entry;
    bool fresh = !entry->no_cache
        && (entry->immutable || now_ns < entry->fresh_until_ns);
    if (fresh) {
        session->cache_fresh_hits++;
        return BROWSER_CACHE_FRESH;
    }
    session->cache_stale_hits++;
    return BROWSER_CACHE_STALE;
}

static bool cache_module_provenance_matches(
    const BrowserCacheEntry *entry, const char *request_url,
    const char *initiator_origin, const char *top_level_url,
    bool initiator_opaque,
    TilefinchCredentialsMode credentials)
{
    char partition[TILEFINCH_ORIGIN_SERIALIZED_LIMIT];
    const char *fragment = request_url == NULL ? NULL
        : strchr(request_url, '#');
    if (fragment == NULL && request_url != NULL) fragment = "";
    return !initiator_opaque && top_level_url != NULL
        && tilefinch_url_site_key(top_level_url, partition,
                                  sizeof(partition))
        && entry != NULL && fragment != NULL && initiator_origin != NULL
        && entry->module_request_fragment != NULL
        && strcmp(entry->module_request_fragment, fragment) == 0
        && entry->module_effective_url != NULL
        && entry->module_effective_url[0] != '\0'
        && entry->module_initiator_origin != NULL
        && strcmp(entry->module_initiator_origin, initiator_origin) == 0
        && entry->module_partition_key != NULL
        && strcmp(entry->module_partition_key, partition) == 0
        && entry->module_credentials == credentials
        && entry->module_cors_validated
        && entry->module_javascript_mime_validated;
}

BrowserCacheStatus browser_session_cache_match_module(
    BrowserSession *session, const char *request_url,
    const char *initiator_origin, const char *top_level_url,
    bool initiator_opaque, TilefinchCredentialsMode credentials,
    uint64_t now_ns, const BrowserCacheEntry **matched)
{
    if (matched != NULL) *matched = NULL;
    if (session == NULL || request_url == NULL || initiator_origin == NULL
        || top_level_url == NULL || initiator_opaque
        || (credentials != TILEFINCH_CREDENTIALS_SAME_ORIGIN
            && credentials != TILEFINCH_CREDENTIALS_INCLUDE)) {
        if (session != NULL) session->cache_misses++;
        return BROWSER_CACHE_MISS;
    }
    char key[TILEFINCH_URL_SERIALIZED_LIMIT];
    if (!tilefinch_url_request_key(request_url, key, sizeof(key))) {
        session->cache_misses++;
        return BROWSER_CACHE_MISS;
    }
    BrowserCacheEntry *entry = NULL;
    for (size_t i = 0; i < BROWSER_CACHE_ENTRIES; i++) {
        if (session->cache[i].data != NULL
            && strcmp(session->cache[i].url, key) == 0
            && cache_module_provenance_matches(
                   &session->cache[i], request_url, initiator_origin,
                   top_level_url, initiator_opaque,
                   credentials)) {
            entry = &session->cache[i];
            break;
        }
    }
    if (entry == NULL) {
        session->cache_misses++;
        return BROWSER_CACHE_MISS;
    }
    entry->stamp = ++session->clock;
    session->cache_hits++;
    if (matched != NULL) *matched = entry;
    bool fresh = !entry->no_cache
        && (entry->immutable || now_ns < entry->fresh_until_ns);
    if (fresh) {
        session->cache_fresh_hits++;
        return BROWSER_CACHE_FRESH;
    }
    session->cache_stale_hits++;
    return BROWSER_CACHE_STALE;
}

bool browser_session_cache_put(BrowserSession *session, const char *url,
                               const unsigned char *data, size_t length)
{
    return browser_session_cache_put_response(session, url, data, length,
                                              NULL, NULL, NULL);
}

bool browser_session_cache_put_response(BrowserSession *session,
                                        const char *url,
                                        const unsigned char *data,
                                        size_t length, const char *etag,
                                        const char *last_modified,
                                        const char *content_type)
{
    return browser_session_cache_put_http(
        session, url, data, length, etag, last_modified, content_type,
        "immutable", NULL, 0);
}

static bool cache_directive(const char *value, const char *wanted,
                            uint64_t *number)
{
    if (value == NULL) return false;
    size_t wanted_length = strlen(wanted);
    for (const char *at = value; *at != '\0';) {
        while (*at == ' ' || *at == '\t' || *at == ',') at++;
        const char *end = strchr(at, ',');
        if (end == NULL) end = at + strlen(at);
        while (end > at && isspace((unsigned char) end[-1])) end--;
        const char *equals = memchr(at, '=', (size_t) (end - at));
        size_t name_length = (size_t) ((equals == NULL ? end : equals) - at);
        while (name_length != 0
               && isspace((unsigned char) at[name_length - 1])) name_length--;
        if (name_length == wanted_length
            && strncasecmp(at, wanted, wanted_length) == 0) {
            if (number != NULL) {
                if (equals == NULL) return false;
                const char *digits = equals + 1;
                while (digits < end && isspace((unsigned char) *digits)) digits++;
                if (digits == end
                    || !isdigit((unsigned char) *digits)) return false;
                char *parsed_end = NULL;
                unsigned long long parsed = strtoull(digits, &parsed_end, 10);
                while (parsed_end < end
                       && isspace((unsigned char) *parsed_end)) parsed_end++;
                if (parsed_end == digits || parsed_end != end) return false;
                *number = (uint64_t) parsed;
            }
            return true;
        }
        at = *end == '\0' ? end : end + 1;
    }
    return false;
}

static bool vary_star(const char *vary)
{
    if (vary == NULL) return false;
    for (const char *at = vary; *at != '\0'; at++) {
        if (*at == '*') return true;
    }
    return false;
}

static bool vary_supported(const char *vary)
{
    if (vary == NULL) return true;
    /* libcurl's decoded representation uses one stable Accept-Encoding
       behavior for every request, and the engine sends one fixed
       User-Agent for the whole session, so neither key can actually vary
       within this cache's lifetime. Other request-header variants are not
       yet represented in the bounded cache key, so conservatively do not
       store. */
    for (const char *at = vary; *at != '\0';) {
        while (*at == ' ' || *at == '\t' || *at == ',') at++;
        if (*at == '\0') break;
        const char *end = strchr(at, ',');
        if (end == NULL) end = at + strlen(at);
        while (end > at && isspace((unsigned char) end[-1])) end--;
        size_t length = (size_t) (end - at);
        static const char accept_encoding[] = "accept-encoding";
        static const char user_agent[] = "user-agent";
        bool supported =
            (length == sizeof(accept_encoding) - 1
             && strncasecmp(at, accept_encoding, length) == 0)
            || (length == sizeof(user_agent) - 1
                && strncasecmp(at, user_agent, length) == 0);
        if (!supported) return false;
        at = *end == '\0' ? end : end + 1;
    }
    return true;
}

static bool vary_supported_classic_script(const char *vary,
                                          bool *origin_variant)
{
    if (origin_variant != NULL) *origin_variant = false;
    if (vary == NULL) return true;
    for (const char *at = vary; *at != '\0';) {
        while (*at == ' ' || *at == '\t' || *at == ',') at++;
        if (*at == '\0') break;
        const char *end = strchr(at, ',');
        if (end == NULL) end = at + strlen(at);
        while (end > at && isspace((unsigned char) end[-1])) end--;
        size_t length = (size_t) (end - at);
        static const char accept_encoding[] = "accept-encoding";
        static const char user_agent[] = "user-agent";
        static const char origin[] = "origin";
        bool is_origin = length == sizeof(origin) - 1
            && strncasecmp(at, origin, length) == 0;
        bool supported = is_origin
            || (length == sizeof(accept_encoding) - 1
                && strncasecmp(at, accept_encoding, length) == 0)
            || (length == sizeof(user_agent) - 1
                && strncasecmp(at, user_agent, length) == 0);
        if (!supported) return false;
        if (is_origin && origin_variant != NULL) *origin_variant = true;
        at = *end == '\0' ? end : end + 1;
    }
    return true;
}

static bool vary_supported_module(const char *vary)
{
    if (vary == NULL) return true;
    for (const char *at = vary; *at != '\0';) {
        while (*at == ' ' || *at == '\t' || *at == ',') at++;
        if (*at == '\0') break;
        const char *end = strchr(at, ',');
        if (end == NULL) end = at + strlen(at);
        while (end > at && isspace((unsigned char) end[-1])) end--;
        size_t length = (size_t) (end - at);
        static const char accept_encoding[] = "accept-encoding";
        static const char origin[] = "origin";
        bool supported =
            (length == sizeof(accept_encoding) - 1
             && strncasecmp(at, accept_encoding, length) == 0)
            || (length == sizeof(origin) - 1
                && strncasecmp(at, origin, length) == 0);
        if (!supported) return false;
        at = *end == '\0' ? end : end + 1;
    }
    return true;
}

static bool cache_referrer_policy_normalized(const char *policy)
{
    if (policy == NULL) return false;
    if (policy[0] == '\0') return true;
    static const char *known[] = {
        "no-referrer", "no-referrer-when-downgrade", "origin",
        "origin-when-cross-origin", "same-origin", "strict-origin",
        "strict-origin-when-cross-origin", "unsafe-url"
    };
    for (size_t i = 0; i < sizeof(known) / sizeof(known[0]); i++) {
        if (strcmp(policy, known[i]) == 0) return true;
    }
    return false;
}

static bool cache_module_referrer_policy_valid(const char *policy)
{
    return policy == NULL || cache_referrer_policy_normalized(policy);
}

static bool cache_module_referrer_provenance_valid(
    const BrowserModuleCacheProvenance *provenance)
{
    if (provenance == NULL) return true;
    const char *policy = provenance->response_referrer_policy;
    return (policy == NULL
            || strlen(policy)
                   < sizeof(((BrowserCacheEntry *) 0)
                                ->module_response_referrer_policy))
        && cache_module_referrer_policy_valid(policy);
}

static BrowserCacheEntry *cache_find_key(BrowserSession *session,
                                         const char *key)
{
    for (size_t i = 0; i < BROWSER_CACHE_ENTRIES; i++) {
        if (session->cache[i].data != NULL
            && !session->cache[i].resource_grant_valid
            && strcmp(session->cache[i].url, key) == 0) {
            return &session->cache[i];
        }
    }
    return NULL;
}

static bool cache_resource_context_identity(
    const TilefinchRequestContext *context,
    char partition[TILEFINCH_ORIGIN_SERIALIZED_LIMIT],
    char initiator_origin[TILEFINCH_ORIGIN_SERIALIZED_LIMIT],
    char initiator_site[TILEFINCH_ORIGIN_SERIALIZED_LIMIT])
{
    if (!tilefinch_request_context_valid(context)
        || context->initiator_url == NULL) return false;
    const char *top = context->top_level_url != NULL
        ? context->top_level_url : context->initiator_url;
    if (!tilefinch_url_site_key(top, partition,
                                TILEFINCH_ORIGIN_SERIALIZED_LIMIT)) {
        return false;
    }
    if (context->initiator_opaque) {
        /* "null" is a serialization, not an identity: two sandboxed frames
           under the same top-level site still have distinct opaque origins.
           RequestContext intentionally carries no forgeable opaque nonce, so
           these responses remain memory-local to their owning load rather
           than being collapsed into one shared cache principal. */
        return false;
    }
    return tilefinch_url_origin(
               context->initiator_url, initiator_origin,
               TILEFINCH_ORIGIN_SERIALIZED_LIMIT)
        && tilefinch_url_site_key(
               context->initiator_url, initiator_site,
               TILEFINCH_ORIGIN_SERIALIZED_LIMIT);
}

static bool cache_resource_provenance_matches(
    const BrowserCacheEntry *entry, const TilefinchRequestContext *context)
{
    char partition[TILEFINCH_ORIGIN_SERIALIZED_LIMIT];
    char initiator_origin[TILEFINCH_ORIGIN_SERIALIZED_LIMIT];
    char initiator_site[TILEFINCH_ORIGIN_SERIALIZED_LIMIT];
    if (entry == NULL || !entry->resource_grant_valid
        || entry->resource_partition_key == NULL
        || entry->resource_initiator_origin == NULL
        || entry->resource_initiator_site == NULL
        || !cache_resource_context_identity(
               context, partition, initiator_origin, initiator_site)) {
        return false;
    }
    const TilefinchResourceGrant *grant = &entry->resource_grant;
    return strcmp(entry->resource_partition_key, partition) == 0
        && strcmp(entry->resource_initiator_origin, initiator_origin) == 0
        && strcmp(entry->resource_initiator_site, initiator_site) == 0
        && grant->destination == context->destination
        && grant->mode == context->mode
        && grant->credentials == context->credentials
        && grant->initiator_opaque == context->initiator_opaque;
}

static BrowserCacheEntry *cache_find_resource_key(
    BrowserSession *session, const char *key,
    const TilefinchRequestContext *context)
{
    if (session == NULL || key == NULL || context == NULL) return NULL;
    for (size_t i = 0; i < BROWSER_CACHE_ENTRIES; i++) {
        BrowserCacheEntry *entry = &session->cache[i];
        if (entry->data != NULL && strcmp(entry->url, key) == 0
            && cache_resource_provenance_matches(entry, context)) {
            return entry;
        }
    }
    return NULL;
}

BrowserSharedBody *browser_shared_body_take(Budget *budget,
                                            unsigned char *data,
                                            size_t length)
{
    if (budget == NULL || data == NULL || length == 0
        || !budget_owns(budget, data)
        || budget_usable_size(data) < length) return NULL;
    BrowserSharedBody *body = budget_malloc(budget, sizeof(*body));
    if (body == NULL) return NULL;
    *body = (BrowserSharedBody) {
        .budget = budget,
        .data = data,
        .length = length,
        .references = 1
    };
    return body;
}

BrowserSharedBody *browser_shared_body_retain(BrowserSharedBody *body)
{
    if (body == NULL || body->references == 0
        || body->references == SIZE_MAX) return NULL;
    body->references++;
    return body;
}

void browser_shared_body_release(BrowserSharedBody *body)
{
    if (body == NULL || body->references == 0) return;
    body->references--;
    if (body->references != 0) return;
    Budget *budget = body->budget;
    budget_free(budget, body->data);
    body->data = NULL;
    budget_free(budget, body);
}

static char *cache_copy_text(Budget *budget, const char *text);
static BrowserCacheEntry *cache_response_entry(
    BrowserSession *session, const char *request_url);

static void cache_remove(BrowserSession *session, BrowserCacheEntry *entry)
{
    if (entry == NULL) return;
    if (entry->classic_script_bytecode != NULL) {
        size_t bytecode_length = entry->classic_script_bytecode->length;
        if (bytecode_length <= session->cache_bytes) {
            session->cache_bytes -= bytecode_length;
        } else {
            session->cache_bytes = 0;
        }
        browser_shared_body_release(entry->classic_script_bytecode);
    }
    if (entry->stylesheet_compiled_fragment != NULL) {
        size_t fragment_length =
            entry->stylesheet_compiled_fragment->length;
        if (fragment_length <= session->cache_bytes) {
            session->cache_bytes -= fragment_length;
        } else {
            session->cache_bytes = 0;
        }
        browser_shared_body_release(entry->stylesheet_compiled_fragment);
    }
    if (entry->stylesheet_parsed_ir != NULL) {
        size_t ir_length = entry->stylesheet_parsed_ir->length;
        if (ir_length <= session->cache_bytes) {
            session->cache_bytes -= ir_length;
        } else {
            session->cache_bytes = 0;
        }
        browser_shared_body_release(entry->stylesheet_parsed_ir);
    }
    if (entry->data != NULL) {
        if (entry->length <= session->cache_bytes) {
            session->cache_bytes -= entry->length;
        } else {
            session->cache_bytes = 0;
        }
        if (entry->body != NULL) browser_shared_body_release(entry->body);
        else budget_free(session->budget, entry->data);
    }
    budget_free(session->budget, entry->response_url);
    budget_free(session->budget, entry->module_effective_url);
    budget_free(session->budget, entry->module_initiator_origin);
    budget_free(session->budget, entry->module_partition_key);
    budget_free(session->budget, entry->module_request_fragment);
    budget_free(session->budget, entry->resource_partition_key);
    budget_free(session->budget, entry->resource_initiator_origin);
    budget_free(session->budget, entry->resource_initiator_site);
    memset(entry, 0, sizeof(*entry));
}

static uint64_t cache_script_source_hash(
    const unsigned char *source, size_t source_length)
{
    return session_hash_bytes(
        UINT64_C(1469598103934665603), source, source_length);
}

/* Resource authorization can retain multiple representations of one request
   URL under different top-level partitions. Compiled bytecode is tied to the
   exact immutable response bytes, so locate that body rather than falling
   back to the old one-entry-per-URL cache lookup. */
static BrowserCacheEntry *cache_script_response_entry(
    BrowserSession *session, const char *request_url,
    const unsigned char *source, size_t source_length)
{
    if (session == NULL || request_url == NULL || source == NULL
        || source_length == 0) return NULL;
    char key[TILEFINCH_URL_SERIALIZED_LIMIT];
    if (!tilefinch_url_request_key(request_url, key, sizeof(key))) {
        return NULL;
    }
    for (size_t i = 0; i < BROWSER_CACHE_ENTRIES; i++) {
        BrowserCacheEntry *entry = &session->cache[i];
        if (entry->data != NULL && entry->length == source_length
            && strcmp(entry->url, key) == 0
            && (entry->data == source
                || memcmp(entry->data, source, source_length) == 0)) {
            return entry;
        }
    }
    return NULL;
}

static BrowserCacheEntry *cache_stylesheet_response_entry(
    BrowserSession *session, const char *request_url,
    const TilefinchRequestContext *request_context,
    const unsigned char *source, size_t source_length)
{
    if (session == NULL || request_url == NULL || request_context == NULL
        || !tilefinch_request_context_valid(request_context)
        || request_context->destination != TILEFINCH_DESTINATION_STYLE
        || source == NULL
        || source_length == 0) return NULL;
    char key[TILEFINCH_URL_SERIALIZED_LIMIT];
    if (!tilefinch_url_request_key(request_url, key, sizeof(key))) {
        return NULL;
    }
    BrowserCacheEntry *entry = cache_find_resource_key(
        session, key, request_context);
    if (entry == NULL || entry->data == NULL
        || entry->length != source_length
        || (entry->data != source
            && memcmp(entry->data, source, source_length) != 0)) {
        /* A speculative preload can retain the authorized body in the
           document ledger while its HTTP representation remains generic.
           The fragment conveys no request authority: callers already hold
           the exact, independently admitted CSS bytes, and the byte-for-byte
           check below makes this only a compiler accelerator. */
        entry = cache_script_response_entry(
            session, request_url, source, source_length);
    }
    return entry;
}

BrowserSharedBody *browser_session_classic_script_bytecode_acquire(
    BrowserSession *session, const char *request_url,
    const unsigned char *source, size_t source_length)
{
    if (session == NULL || request_url == NULL || source == NULL
        || source_length == 0) return NULL;
    BrowserCacheEntry *entry = cache_script_response_entry(
        session, request_url, source, source_length);
    if (entry == NULL || entry->classic_script_bytecode == NULL
        || entry->length != source_length
        || entry->classic_script_source_length != source_length
        || entry->classic_script_source_hash
               != cache_script_source_hash(source, source_length)
        || memcmp(entry->data, source, source_length) != 0) {
        return NULL;
    }
    entry->stamp = ++session->clock;
    return browser_shared_body_retain(entry->classic_script_bytecode);
}

bool browser_session_classic_script_bytecode_may_fit(
    BrowserSession *session, const char *request_url,
    const unsigned char *source, size_t source_length,
    size_t minimum_bytecode_length)
{
    BrowserCacheEntry *entry = cache_script_response_entry(
        session, request_url, source, source_length);
    return entry != NULL && entry->data != NULL
        && entry->length <= session->maximum_cache_bytes
        && minimum_bytecode_length
               <= session->maximum_cache_bytes - entry->length;
}

static void cache_script_bytecode_invalidate_entry(
    BrowserSession *session, BrowserCacheEntry *entry)
{
    if (entry == NULL || entry->classic_script_bytecode == NULL) return;
    size_t length = entry->classic_script_bytecode->length;
    if (length <= session->cache_bytes) session->cache_bytes -= length;
    else session->cache_bytes = 0;
    browser_shared_body_release(entry->classic_script_bytecode);
    entry->classic_script_bytecode = NULL;
    entry->classic_script_source_hash = 0;
    entry->classic_script_source_length = 0;
}

void browser_session_classic_script_bytecode_invalidate(
    BrowserSession *session, const char *request_url,
    const unsigned char *source, size_t source_length)
{
    cache_script_bytecode_invalidate_entry(
        session, cache_script_response_entry(
            session, request_url, source, source_length));
}

bool browser_session_classic_script_bytecode_put(
    BrowserSession *session, const char *request_url,
    const unsigned char *source, size_t source_length,
    const unsigned char *bytecode, size_t bytecode_length)
{
    if (session == NULL || request_url == NULL || source == NULL
        || source_length == 0 || bytecode == NULL || bytecode_length == 0
        || bytecode_length > session->maximum_cache_bytes) return false;
    BrowserCacheEntry *entry = cache_script_response_entry(
        session, request_url, source, source_length);
    uint64_t source_hash = cache_script_source_hash(source, source_length);
    if (entry == NULL || entry->data == NULL
        || entry->length != source_length
        || cache_script_source_hash(entry->data, entry->length)
               != source_hash
        || memcmp(entry->data, source, source_length) != 0) return false;

    cache_script_bytecode_invalidate_entry(session, entry);
    while (session->cache_bytes
           > session->maximum_cache_bytes - bytecode_length) {
        BrowserCacheEntry *victim = NULL;
        for (size_t i = 0; i < BROWSER_CACHE_ENTRIES; i++) {
            BrowserCacheEntry *candidate = &session->cache[i];
            if (candidate->data != NULL && candidate != entry
                && (victim == NULL || candidate->stamp < victim->stamp)) {
                victim = candidate;
            }
        }
        if (victim == NULL) return false;
        cache_remove(session, victim);
        session->cache_evictions++;
    }
    unsigned char *copy = budget_malloc(
        session->budget, bytecode_length);
    if (copy == NULL) return false;
    memcpy(copy, bytecode, bytecode_length);
    BrowserSharedBody *body = browser_shared_body_take(
        session->budget, copy, bytecode_length);
    if (body == NULL) {
        budget_free(session->budget, copy);
        return false;
    }
    entry->classic_script_bytecode = body;
    entry->classic_script_source_hash = source_hash;
    entry->classic_script_source_length = source_length;
    entry->stamp = ++session->clock;
    session->cache_bytes += bytecode_length;
    return true;
}

void browser_session_stylesheet_artifacts_acquire(
    BrowserSession *session, const char *request_url,
    const TilefinchRequestContext *request_context,
    const unsigned char *source, size_t source_length,
    BrowserSharedBody **compiled_fragment, BrowserSharedBody **parsed_ir)
{
    if (compiled_fragment != NULL) *compiled_fragment = NULL;
    if (parsed_ir != NULL) *parsed_ir = NULL;
    if (session == NULL || request_url == NULL || source == NULL
        || source_length == 0
        || (compiled_fragment == NULL && parsed_ir == NULL)) return;
    BrowserCacheEntry *entry = cache_stylesheet_response_entry(
        session, request_url, request_context, source, source_length);
    if (entry == NULL) return;
    bool retained = false;
    if (compiled_fragment != NULL
        && entry->stylesheet_compiled_fragment != NULL) {
        *compiled_fragment = browser_shared_body_retain(
            entry->stylesheet_compiled_fragment);
        retained = *compiled_fragment != NULL;
    }
    if (parsed_ir != NULL && entry->stylesheet_parsed_ir != NULL) {
        *parsed_ir = browser_shared_body_retain(
            entry->stylesheet_parsed_ir);
        retained = retained || *parsed_ir != NULL;
    }
    if (retained) entry->stamp = ++session->clock;
}

static void cache_stylesheet_fragment_invalidate_entry(
    BrowserSession *session, BrowserCacheEntry *entry)
{
    if (entry == NULL || entry->stylesheet_compiled_fragment == NULL) return;
    size_t length = entry->stylesheet_compiled_fragment->length;
    if (length <= session->cache_bytes) session->cache_bytes -= length;
    else session->cache_bytes = 0;
    browser_shared_body_release(entry->stylesheet_compiled_fragment);
    entry->stylesheet_compiled_fragment = NULL;
}

bool browser_session_stylesheet_fragment_put_take(
    BrowserSession *session, const char *request_url,
    const TilefinchRequestContext *request_context,
    const unsigned char *source, size_t source_length,
    unsigned char *fragment, size_t fragment_length)
{
    if (session == NULL || request_url == NULL || source == NULL
        || source_length == 0 || fragment == NULL || fragment_length == 0
        || fragment_length > session->maximum_cache_bytes) return false;
    BrowserCacheEntry *entry = cache_stylesheet_response_entry(
        session, request_url, request_context, source, source_length);
    if (entry == NULL) return false;
    cache_stylesheet_fragment_invalidate_entry(session, entry);
    while (session->cache_bytes
           > session->maximum_cache_bytes - fragment_length) {
        BrowserCacheEntry *victim = NULL;
        for (size_t i = 0; i < BROWSER_CACHE_ENTRIES; i++) {
            BrowserCacheEntry *candidate = &session->cache[i];
            if (candidate->data != NULL && candidate != entry
                && (victim == NULL || candidate->stamp < victim->stamp)) {
                victim = candidate;
            }
        }
        if (victim == NULL) return false;
        cache_remove(session, victim);
        session->cache_evictions++;
    }
    BrowserSharedBody *body = browser_shared_body_take(
        session->budget, fragment, fragment_length);
    if (body == NULL) return false;
    entry->stylesheet_compiled_fragment = body;
    entry->stamp = ++session->clock;
    session->cache_bytes += fragment_length;
    return true;
}

static void cache_stylesheet_ir_invalidate_entry(
    BrowserSession *session, BrowserCacheEntry *entry)
{
    if (entry == NULL || entry->stylesheet_parsed_ir == NULL) return;
    size_t length = entry->stylesheet_parsed_ir->length;
    if (length <= session->cache_bytes) session->cache_bytes -= length;
    else session->cache_bytes = 0;
    browser_shared_body_release(entry->stylesheet_parsed_ir);
    entry->stylesheet_parsed_ir = NULL;
}

bool browser_session_stylesheet_ir_put_take(
    BrowserSession *session, const char *request_url,
    const TilefinchRequestContext *request_context,
    const unsigned char *source, size_t source_length,
    unsigned char *ir, size_t ir_length)
{
    if (session == NULL || request_url == NULL || source == NULL
        || source_length == 0 || ir == NULL || ir_length == 0
        || ir_length > session->maximum_cache_bytes) return false;
    BrowserCacheEntry *entry = cache_stylesheet_response_entry(
        session, request_url, request_context, source, source_length);
    if (entry == NULL) return false;
    cache_stylesheet_ir_invalidate_entry(session, entry);
    while (session->cache_bytes
           > session->maximum_cache_bytes - ir_length) {
        BrowserCacheEntry *victim = NULL;
        for (size_t i = 0; i < BROWSER_CACHE_ENTRIES; i++) {
            BrowserCacheEntry *candidate = &session->cache[i];
            if (candidate->data != NULL && candidate != entry
                && (victim == NULL || candidate->stamp < victim->stamp)) {
                victim = candidate;
            }
        }
        if (victim == NULL) return false;
        cache_remove(session, victim);
        session->cache_evictions++;
    }
    BrowserSharedBody *body = browser_shared_body_take(
        session->budget, ir, ir_length);
    if (body == NULL) return false;
    entry->stylesheet_parsed_ir = body;
    entry->stamp = ++session->clock;
    session->cache_bytes += ir_length;
    return true;
}

static BrowserCacheEntry *cache_response_entry(BrowserSession *session,
                                               const char *request_url)
{
    if (session == NULL || request_url == NULL) return NULL;
    char key[TILEFINCH_URL_SERIALIZED_LIMIT];
    if (!tilefinch_url_request_key(request_url, key, sizeof(key))) return NULL;
    return cache_find_key(session, key);
}

static void cache_clear_response_provenance_entry(
    BrowserSession *session, BrowserCacheEntry *entry)
{
    if (session == NULL || entry == NULL) return;
    budget_free(session->budget, entry->response_url);
    entry->response_url = NULL;
    entry->response_url_known = false;
    entry->response_referrer_policy[0] = '\0';
    entry->response_referrer_policy_known = false;
}

bool browser_session_cache_clear_response_provenance(
    BrowserSession *session, const char *request_url)
{
    BrowserCacheEntry *entry = cache_response_entry(session, request_url);
    if (entry == NULL) return false;
    cache_clear_response_provenance_entry(session, entry);
    return true;
}

static bool cache_replace_response_url(BrowserSession *session,
                                       BrowserCacheEntry *entry,
                                       const char *response_url)
{
    if (session == NULL || entry == NULL || response_url == NULL
        || response_url[0] == '\0'
        || strlen(response_url) >= TILEFINCH_URL_SERIALIZED_LIMIT) return false;
    TilefinchUrl parsed;
    if (!tilefinch_url_parse(response_url, &parsed)) return false;
    if (strcmp(entry->url, response_url) == 0) {
        budget_free(session->budget, entry->response_url);
        entry->response_url = NULL;
        return true;
    }
    if (entry->response_url != NULL
        && strcmp(entry->response_url, response_url) == 0) {
        return true;
    }
    char *replacement = cache_copy_text(session->budget, response_url);
    if (replacement == NULL) return false;
    budget_free(session->budget, entry->response_url);
    entry->response_url = replacement;
    return true;
}

const char *browser_cache_entry_response_url(const BrowserCacheEntry *entry)
{
    if (entry == NULL) return NULL;
    if (entry->response_url != NULL && entry->response_url[0] != '\0') {
        return entry->response_url;
    }
    return entry->module_effective_url != NULL
           && entry->module_effective_url[0] != '\0'
        ? entry->module_effective_url : entry->url;
}

bool browser_session_cache_set_response_url(BrowserSession *session,
                                            const char *request_url,
                                            const char *response_url)
{
    BrowserCacheEntry *entry = cache_response_entry(session, request_url);
    if (entry == NULL) return false;
    if (!cache_replace_response_url(session, entry, response_url)) {
        cache_clear_response_provenance_entry(session, entry);
        return false;
    }
    entry->response_url_known = true;
    entry->response_referrer_policy[0] = '\0';
    entry->response_referrer_policy_known = false;
    return true;
}

bool browser_session_cache_set_response_provenance(
    BrowserSession *session, const char *request_url, const char *final_url,
    const char *normalized_referrer_policy)
{
    BrowserCacheEntry *entry = cache_response_entry(session, request_url);
    if (entry == NULL) return false;
    if (normalized_referrer_policy == NULL
        || strlen(normalized_referrer_policy)
               >= sizeof(entry->response_referrer_policy)
        || !cache_referrer_policy_normalized(normalized_referrer_policy)
        || !cache_replace_response_url(session, entry, final_url)) {
        cache_clear_response_provenance_entry(session, entry);
        return false;
    }
    size_t policy_length = strlen(normalized_referrer_policy);
    memcpy(entry->response_referrer_policy, normalized_referrer_policy,
           policy_length + 1u);
    entry->response_url_known = true;
    entry->response_referrer_policy_known = true;
    return true;
}

bool browser_session_cache_set_resource_response_provenance(
    BrowserSession *session, const char *request_url,
    const TilefinchRequestContext *context, const char *final_url,
    const char *normalized_referrer_policy)
{
    if (session == NULL || request_url == NULL || context == NULL) {
        return false;
    }
    char key[TILEFINCH_URL_SERIALIZED_LIMIT];
    if (!tilefinch_url_request_key(request_url, key, sizeof(key))) {
        return false;
    }
    BrowserCacheEntry *entry = cache_find_resource_key(
        session, key, context);
    if (entry == NULL || normalized_referrer_policy == NULL
        || strlen(normalized_referrer_policy)
               >= sizeof(entry->response_referrer_policy)
        || !cache_referrer_policy_normalized(normalized_referrer_policy)
        || !cache_replace_response_url(session, entry, final_url)) {
        if (entry != NULL) {
            cache_clear_response_provenance_entry(session, entry);
        }
        return false;
    }
    size_t policy_length = strlen(normalized_referrer_policy);
    memcpy(entry->response_referrer_policy, normalized_referrer_policy,
           policy_length + 1u);
    entry->response_url_known = true;
    entry->response_referrer_policy_known = true;
    return true;
}

static char *cache_copy_text(Budget *budget, const char *text)
{
    if (budget == NULL || text == NULL) return NULL;
    size_t length = strlen(text);
    if (length == SIZE_MAX) return NULL;
    char *copy = budget_malloc(budget, length + 1);
    if (copy != NULL) memcpy(copy, text, length + 1);
    return copy;
}

static void cache_set_policy(BrowserCacheEntry *entry,
                             const char *cache_control, const char *vary,
                             uint64_t now_ns)
{
    uint64_t seconds = 0;
    bool has_max_age = cache_directive(cache_control, "max-age", &seconds);
    entry->stored_at_ns = now_ns;
    entry->fresh_until_ns = now_ns;
    if (has_max_age) {
        uint64_t duration = seconds > UINT64_MAX / UINT64_C(1000000000)
                            ? UINT64_MAX
                            : seconds * UINT64_C(1000000000);
        entry->fresh_until_ns = duration > UINT64_MAX - now_ns
                                ? UINT64_MAX : now_ns + duration;
    }
    entry->no_cache = cache_directive(cache_control, "no-cache", NULL);
    entry->must_revalidate = cache_directive(cache_control,
                                             "must-revalidate", NULL);
    entry->immutable = cache_directive(cache_control, "immutable", NULL);
    snprintf(entry->vary, sizeof(entry->vary), "%s", vary == NULL ? "" : vary);
}

bool browser_session_cache_put_http(BrowserSession *session,
                                    const char *url,
                                    const unsigned char *data,
                                    size_t length, const char *etag,
                                    const char *last_modified,
                                    const char *content_type,
                                    const char *cache_control,
                                    const char *vary, uint64_t now_ns)
{
    return browser_session_cache_put_http_module(
        session, url, data, length, etag, last_modified, content_type,
        cache_control, vary, now_ns, NULL);
}

bool browser_session_cache_put_http_module(
    BrowserSession *session, const char *url,
    const unsigned char *data, size_t length, const char *etag,
    const char *last_modified, const char *content_type,
    const char *cache_control, const char *vary, uint64_t now_ns,
    const BrowserModuleCacheProvenance *provenance)
{
    if (session == NULL || data == NULL || length == 0
        || length == SIZE_MAX) return false;
    /* QuickJS' lexer performs a bounded one-byte sentinel read at source end.
       Cache payload length remains the HTTP byte length, while the backing
       allocation carries an internal terminator for text consumers. */
    unsigned char *copy = budget_malloc(session->budget, length + 1);
    if (copy == NULL) return false;
    memcpy(copy, data, length);
    copy[length] = 0;
    BrowserSharedBody *body = browser_shared_body_take(
        session->budget, copy, length);
    if (body == NULL) {
        budget_free(session->budget, copy);
        return false;
    }
    bool stored = browser_session_cache_put_http_shared_module(
        session, url, body, etag, last_modified, content_type,
        cache_control, vary, now_ns, provenance);
    browser_shared_body_release(body);
    return stored;
}

bool browser_session_cache_put_http_shared(
    BrowserSession *session, const char *url, BrowserSharedBody *body,
    const char *etag, const char *last_modified, const char *content_type,
    const char *cache_control, const char *vary, uint64_t now_ns)
{
    return browser_session_cache_put_http_shared_module(
        session, url, body, etag, last_modified, content_type,
        cache_control, vary, now_ns, NULL);
}

static bool cache_put_http_shared_variant(
    BrowserSession *session, const char *url, BrowserSharedBody *body,
    const char *etag, const char *last_modified, const char *content_type,
    const char *cache_control, const char *vary, uint64_t now_ns,
    const BrowserModuleCacheProvenance *provenance,
    bool classic_script_request,
    const TilefinchRequestContext *resource_context,
    const TilefinchResourceGrant *resource_grant)
{
    if (session == NULL || url == NULL || body == NULL
        || body->data == NULL || body->length == 0
        || body->references == 0 || body->budget != session->budget
        || !budget_owns(body->budget, body->data)
        || budget_usable_size(body->data) < body->length) return false;
    if ((resource_context == NULL) != (resource_grant == NULL)) return false;
    if (provenance != NULL && resource_context != NULL) return false;
    if (resource_context != NULL
        && (!tilefinch_request_context_valid(resource_context)
            || resource_grant->destination != resource_context->destination
            || resource_grant->mode != resource_context->mode
            || resource_grant->credentials != resource_context->credentials
            || resource_grant->initiator_opaque
                   != resource_context->initiator_opaque)) return false;
    if (provenance != NULL
        && (provenance->effective_url == NULL
            || provenance->effective_url[0] == '\0'
            || provenance->initiator_origin == NULL
            || provenance->initiator_origin[0] == '\0'
            || provenance->top_level_url == NULL
            || provenance->top_level_url[0] == '\0'
            || provenance->initiator_opaque
            || !cache_module_referrer_provenance_valid(provenance)
            || (provenance->credentials != TILEFINCH_CREDENTIALS_SAME_ORIGIN
                && provenance->credentials != TILEFINCH_CREDENTIALS_INCLUDE))) {
        return false;
    }
    size_t length = body->length;
    char key[TILEFINCH_URL_SERIALIZED_LIMIT];
    if (!tilefinch_url_request_key(url, key, sizeof(key))
        || strlen(key) >= sizeof(session->cache[0].url)) return false;
    BrowserCacheEntry *existing = resource_context == NULL
        ? cache_find_key(session, key)
        : cache_find_resource_key(session, key, resource_context);
    bool origin_variant = false;
    bool vary_ok = provenance != NULL
        ? vary_supported_module(vary)
        : classic_script_request
            ? vary_supported_classic_script(vary, &origin_variant)
            : vary_supported(vary);
    if (cache_directive(cache_control, "no-store", NULL) || vary_star(vary)
        || !vary_ok) {
        cache_remove(session, existing);
        return false;
    }
    if (length > session->maximum_cache_bytes) return false;
    char partition[TILEFINCH_ORIGIN_SERIALIZED_LIMIT] = {0};
    char initiator_origin[TILEFINCH_ORIGIN_SERIALIZED_LIMIT] = {0};
    char initiator_site[TILEFINCH_ORIGIN_SERIALIZED_LIMIT] = {0};
    if (resource_context != NULL
        && !cache_resource_context_identity(
               resource_context, partition, initiator_origin,
               initiator_site)) return false;
    char *resource_partition_key = resource_context == NULL ? NULL
        : cache_copy_text(session->budget, partition);
    char *resource_initiator_origin = resource_context == NULL ? NULL
        : cache_copy_text(session->budget, initiator_origin);
    char *resource_initiator_site = resource_context == NULL ? NULL
        : cache_copy_text(session->budget, initiator_site);
    if (resource_context != NULL
        && (resource_partition_key == NULL
            || resource_initiator_origin == NULL
            || resource_initiator_site == NULL)) {
        budget_free(session->budget, resource_partition_key);
        budget_free(session->budget, resource_initiator_origin);
        budget_free(session->budget, resource_initiator_site);
        return false;
    }
    char *module_effective_url = provenance == NULL ? NULL
        : cache_copy_text(session->budget, provenance->effective_url);
    char *module_initiator_origin = provenance == NULL ? NULL
        : cache_copy_text(session->budget, provenance->initiator_origin);
    char module_partition[TILEFINCH_ORIGIN_SERIALIZED_LIMIT] = {0};
    char *module_partition_key = provenance == NULL ? NULL
        : (tilefinch_url_site_key(provenance->top_level_url,
                                  module_partition,
                                  sizeof(module_partition))
            ? cache_copy_text(session->budget, module_partition) : NULL);
    const char *request_fragment = strchr(url, '#');
    if (request_fragment == NULL) request_fragment = "";
    char *module_request_fragment = provenance == NULL ? NULL
        : cache_copy_text(session->budget, request_fragment);
    if (provenance != NULL
        && (module_effective_url == NULL
            || module_initiator_origin == NULL
            || module_partition_key == NULL
            || module_request_fragment == NULL)) {
        budget_free(session->budget, module_effective_url);
        budget_free(session->budget, module_initiator_origin);
        budget_free(session->budget, module_partition_key);
        budget_free(session->budget, module_request_fragment);
        budget_free(session->budget, resource_partition_key);
        budget_free(session->budget, resource_initiator_origin);
        budget_free(session->budget, resource_initiator_site);
        return false;
    }
    /* Retain first: replacing an entry that already references this body is
       safe, and all failure paths leave the caller's ownership unchanged. */
    BrowserSharedBody *retained = browser_shared_body_retain(body);
    if (retained == NULL) {
        budget_free(session->budget, module_effective_url);
        budget_free(session->budget, module_initiator_origin);
        budget_free(session->budget, module_partition_key);
        budget_free(session->budget, module_request_fragment);
        budget_free(session->budget, resource_partition_key);
        budget_free(session->budget, resource_initiator_origin);
        budget_free(session->budget, resource_initiator_site);
        return false;
    }
    BrowserCacheEntry *entry = NULL;
    for (size_t i = 0; i < BROWSER_CACHE_ENTRIES; i++) {
        if (&session->cache[i] == existing) {
            entry = &session->cache[i]; break;
        }
        if (entry == NULL || session->cache[i].data == NULL
            || session->cache[i].stamp < entry->stamp) entry = &session->cache[i];
    }
    size_t replaced_bytes = entry->data == NULL ? 0 : entry->length;
    if (entry->classic_script_bytecode != NULL) {
        size_t bytecode_length = entry->classic_script_bytecode->length;
        if (bytecode_length > SIZE_MAX - replaced_bytes) {
            browser_shared_body_release(retained);
            budget_free(session->budget, module_effective_url);
            budget_free(session->budget, module_initiator_origin);
            budget_free(session->budget, module_partition_key);
            budget_free(session->budget, module_request_fragment);
            budget_free(session->budget, resource_partition_key);
            budget_free(session->budget, resource_initiator_origin);
            budget_free(session->budget, resource_initiator_site);
            return false;
        }
        replaced_bytes += bytecode_length;
    }
    if (entry->stylesheet_compiled_fragment != NULL) {
        size_t fragment_length =
            entry->stylesheet_compiled_fragment->length;
        if (fragment_length > SIZE_MAX - replaced_bytes) {
            browser_shared_body_release(retained);
            budget_free(session->budget, module_effective_url);
            budget_free(session->budget, module_initiator_origin);
            budget_free(session->budget, module_partition_key);
            budget_free(session->budget, module_request_fragment);
            budget_free(session->budget, resource_partition_key);
            budget_free(session->budget, resource_initiator_origin);
            budget_free(session->budget, resource_initiator_site);
            return false;
        }
        replaced_bytes += fragment_length;
    }
    if (entry->stylesheet_parsed_ir != NULL) {
        size_t ir_length = entry->stylesheet_parsed_ir->length;
        if (ir_length > SIZE_MAX - replaced_bytes) {
            browser_shared_body_release(retained);
            budget_free(session->budget, module_effective_url);
            budget_free(session->budget, module_initiator_origin);
            budget_free(session->budget, module_partition_key);
            budget_free(session->budget, module_request_fragment);
            budget_free(session->budget, resource_partition_key);
            budget_free(session->budget, resource_initiator_origin);
            budget_free(session->budget, resource_initiator_site);
            return false;
        }
        replaced_bytes += ir_length;
    }
    if (replaced_bytes > session->cache_bytes) {
        browser_shared_body_release(retained);
        budget_free(session->budget, module_effective_url);
        budget_free(session->budget, module_initiator_origin);
        budget_free(session->budget, module_partition_key);
        budget_free(session->budget, module_request_fragment);
        budget_free(session->budget, resource_partition_key);
        budget_free(session->budget, resource_initiator_origin);
        budget_free(session->budget, resource_initiator_site);
        return false;
    }
    size_t bytes_without_entry = session->cache_bytes - replaced_bytes;
    /* Compare by subtraction so a corrupt/hostile near-SIZE_MAX ledger cannot
       wrap the prospective total into an apparently admissible value. */
    while (bytes_without_entry > session->maximum_cache_bytes - length) {
        BrowserCacheEntry *victim = NULL;
        for (size_t i = 0; i < BROWSER_CACHE_ENTRIES; i++) {
            BrowserCacheEntry *candidate = &session->cache[i];
            if (candidate->data != NULL && candidate != entry
                && (victim == NULL || candidate->stamp < victim->stamp)) {
                victim = candidate;
            }
        }
        if (victim == NULL) {
            browser_shared_body_release(retained);
            budget_free(session->budget, module_effective_url);
            budget_free(session->budget, module_initiator_origin);
            budget_free(session->budget, module_partition_key);
            budget_free(session->budget, module_request_fragment);
            budget_free(session->budget, resource_partition_key);
            budget_free(session->budget, resource_initiator_origin);
            budget_free(session->budget, resource_initiator_site);
            return false;
        }
        cache_remove(session, victim);
        if (replaced_bytes > session->cache_bytes) {
            browser_shared_body_release(retained);
            budget_free(session->budget, module_effective_url);
            budget_free(session->budget, module_initiator_origin);
            budget_free(session->budget, module_partition_key);
            budget_free(session->budget, module_request_fragment);
            budget_free(session->budget, resource_partition_key);
            budget_free(session->budget, resource_initiator_origin);
            budget_free(session->budget, resource_initiator_site);
            return false;
        }
        bytes_without_entry = session->cache_bytes - replaced_bytes;
        session->cache_evictions++;
    }
    if (entry->data != NULL) {
        cache_remove(session, entry);
    }
    snprintf(entry->url, sizeof(entry->url), "%s", key);
    entry->body = retained;
    entry->data = retained->data;
    entry->length = length;
    entry->response_body_hash = session_hash_bytes(
        UINT64_C(1469598103934665603), retained->data, length);
    entry->stamp = ++session->clock;
    snprintf(entry->etag, sizeof(entry->etag), "%s", etag == NULL ? "" : etag);
    snprintf(entry->last_modified, sizeof(entry->last_modified), "%s",
             last_modified == NULL ? "" : last_modified);
    snprintf(entry->content_type, sizeof(entry->content_type), "%s",
             content_type == NULL ? "" : content_type);
    cache_set_policy(entry, cache_control, vary, now_ns);
    entry->classic_script_origin_variant = origin_variant;
    entry->module_effective_url = module_effective_url;
    entry->module_initiator_origin = module_initiator_origin;
    entry->module_partition_key = module_partition_key;
    entry->module_request_fragment = module_request_fragment;
    entry->resource_partition_key = resource_partition_key;
    entry->resource_initiator_origin = resource_initiator_origin;
    entry->resource_initiator_site = resource_initiator_site;
    if (resource_grant != NULL) {
        entry->resource_grant = *resource_grant;
        entry->resource_grant_valid = true;
    }
    if (provenance != NULL) {
        snprintf(entry->module_response_referrer_policy,
                 sizeof(entry->module_response_referrer_policy), "%s",
                 provenance->response_referrer_policy == NULL
                   ? "" : provenance->response_referrer_policy);
        entry->module_credentials = provenance->credentials;
        entry->module_cors_validated = provenance->cors_validated;
        entry->module_cors_redirect_origin_tainted =
            provenance->cors_redirect_origin_tainted;
        entry->module_javascript_mime_validated =
            provenance->javascript_mime_validated;
    }
    session->cache_bytes = bytes_without_entry + length;
    return true;
}

bool browser_session_cache_put_http_shared_module(
    BrowserSession *session, const char *url, BrowserSharedBody *body,
    const char *etag, const char *last_modified, const char *content_type,
    const char *cache_control, const char *vary, uint64_t now_ns,
    const BrowserModuleCacheProvenance *provenance)
{
    return cache_put_http_shared_variant(
        session, url, body, etag, last_modified, content_type,
        cache_control, vary, now_ns, provenance, false, NULL, NULL);
}

bool browser_session_cache_put_http_shared_classic_script(
    BrowserSession *session, const char *url, BrowserSharedBody *body,
    const char *etag, const char *last_modified, const char *content_type,
    const char *cache_control, const char *vary, uint64_t now_ns,
    const TilefinchRequestContext *context,
    const TilefinchResourceGrant *grant)
{
    return context != NULL
            && context->destination == TILEFINCH_DESTINATION_SCRIPT
        && browser_session_cache_put_http_shared_resource(
            session, url, body, etag, last_modified, content_type,
            cache_control, vary, now_ns, context, grant);
}

bool browser_session_cache_put_http_shared_resource(
    BrowserSession *session, const char *url, BrowserSharedBody *body,
    const char *etag, const char *last_modified, const char *content_type,
    const char *cache_control, const char *vary, uint64_t now_ns,
    const TilefinchRequestContext *context,
    const TilefinchResourceGrant *grant)
{
    return cache_put_http_shared_variant(
        session, url, body, etag, last_modified, content_type,
        cache_control, vary, now_ns, NULL, true, context, grant);
}

bool browser_session_cache_revalidate(BrowserSession *session,
                                      const char *url,
                                      const char *cache_control,
                                      const char *vary, uint64_t now_ns)
{
    return browser_session_cache_revalidate_module(
        session, url, cache_control, vary, now_ns, NULL);
}

static bool cache_revalidate_variant(
    BrowserSession *session, const char *url,
    const char *cache_control, const char *vary, uint64_t now_ns,
    const BrowserModuleCacheProvenance *provenance,
    bool classic_script_request,
    const TilefinchRequestContext *resource_context,
    const TilefinchResourceGrant *resource_grant)
{
    if (session == NULL || url == NULL) return false;
    if ((resource_context == NULL) != (resource_grant == NULL)
        || (provenance != NULL && resource_context != NULL)) return false;
    char key[TILEFINCH_URL_SERIALIZED_LIMIT];
    if (!tilefinch_url_request_key(url, key, sizeof(key))) return false;
    BrowserCacheEntry *entry = resource_context == NULL
        ? cache_find_key(session, key)
        : cache_find_resource_key(session, key, resource_context);
    if (entry == NULL) return false;
    if (resource_grant != NULL
        && (resource_grant->destination != resource_context->destination
            || resource_grant->mode != resource_context->mode
            || resource_grant->credentials != resource_context->credentials
            || resource_grant->initiator_opaque
                   != resource_context->initiator_opaque)) return false;
    char *replacement_effective_url = NULL;
    if (provenance != NULL) {
        if (!provenance->cors_validated
            || !provenance->javascript_mime_validated
            || !cache_module_referrer_provenance_valid(provenance)
            || !cache_module_provenance_matches(
                entry, url, provenance->initiator_origin,
                provenance->top_level_url, provenance->initiator_opaque,
                provenance->credentials)
            || provenance->effective_url == NULL
            || provenance->effective_url[0] == '\0') {
            return false;
        }
    }
    bool origin_variant = entry->classic_script_origin_variant;
    bool vary_ok = false;
    if (provenance != NULL) {
        vary_ok = vary_supported_module(vary);
    } else if (classic_script_request) {
        vary_ok = vary == NULL || vary[0] == '\0'
            || vary_supported_classic_script(vary, &origin_variant);
    } else if (resource_context == NULL) {
        vary_ok = vary_supported(vary);
    }
    if (cache_directive(cache_control, "no-store", NULL) || vary_star(vary)
        || !vary_ok) {
        cache_remove(session, entry);
        return false;
    }
    if (provenance != NULL
        && strcmp(entry->module_effective_url,
                  provenance->effective_url) != 0) {
        replacement_effective_url = cache_copy_text(
            session->budget, provenance->effective_url);
        if (replacement_effective_url == NULL) return false;
    }
    if (cache_control == NULL || cache_control[0] == '\0') {
        uint64_t duration = entry->fresh_until_ns >= entry->stored_at_ns
                            ? entry->fresh_until_ns - entry->stored_at_ns : 0;
        entry->stored_at_ns = now_ns;
        entry->fresh_until_ns = duration > UINT64_MAX - now_ns
                                ? UINT64_MAX : now_ns + duration;
        if (vary != NULL && vary[0] != '\0') {
            snprintf(entry->vary, sizeof(entry->vary), "%s", vary);
        }
    } else {
        char retained_vary[sizeof(entry->vary)];
        snprintf(retained_vary, sizeof(retained_vary), "%s", entry->vary);
        cache_set_policy(entry, cache_control,
                         vary == NULL || vary[0] == '\0'
                         ? retained_vary : vary, now_ns);
    }
    entry->stamp = ++session->clock;
    entry->classic_script_origin_variant = origin_variant;
    if (provenance != NULL && replacement_effective_url != NULL) {
        budget_free(session->budget, entry->module_effective_url);
        entry->module_effective_url = replacement_effective_url;
    }
    if (provenance != NULL) {
        entry->module_cors_redirect_origin_tainted =
            provenance->cors_redirect_origin_tainted;
        if (provenance->referrer_policy_header_present) {
            snprintf(entry->module_response_referrer_policy,
                     sizeof(entry->module_response_referrer_policy), "%s",
                     provenance->response_referrer_policy == NULL
                       ? "" : provenance->response_referrer_policy);
        }
    } else {
        budget_free(session->budget, entry->module_effective_url);
        budget_free(session->budget, entry->module_initiator_origin);
        budget_free(session->budget, entry->module_partition_key);
        budget_free(session->budget, entry->module_request_fragment);
        entry->module_effective_url = NULL;
        entry->module_initiator_origin = NULL;
        entry->module_partition_key = NULL;
        entry->module_request_fragment = NULL;
        entry->module_response_referrer_policy[0] = '\0';
        entry->module_credentials = TILEFINCH_CREDENTIALS_OMIT;
        entry->module_cors_validated = false;
        entry->module_cors_redirect_origin_tainted = false;
        entry->module_javascript_mime_validated = false;
    }
    if (resource_grant != NULL) {
        entry->resource_grant = *resource_grant;
        entry->resource_grant_valid = true;
    }
    return true;
}

uint64_t browser_session_stylesheet_cache_signature(
    const BrowserSession *session, uint64_t now_ns)
{
    if (session == NULL) return 0;
    uint64_t combined = UINT64_C(1469598103934665603);
    size_t count = 0;
    for (size_t i = 0; i < BROWSER_CACHE_ENTRIES; i++) {
        const BrowserCacheEntry *entry = &session->cache[i];
        if (entry->data == NULL || !entry->resource_grant_valid
            || entry->resource_grant.destination
                   != TILEFINCH_DESTINATION_STYLE) {
            continue;
        }
        /* A compiled sheet may bypass the ordered loader. Any stale style
           grant therefore makes reuse ineligible until that loader has
           revalidated/replaced it; omitting stale entries would let two
           empty signatures compare equal. */
        if (entry->no_cache
            || (!entry->immutable && now_ns >= entry->fresh_until_ns)) {
            return 0;
        }
        uint64_t item = session_hash_bytes(
            UINT64_C(1469598103934665603), entry->url,
            strlen(entry->url));
        item = session_hash_bytes(
            item, &entry->response_body_hash,
            sizeof(entry->response_body_hash));
        item = session_hash_bytes(
            item, entry->resource_partition_key,
            entry->resource_partition_key == NULL
                ? 0 : strlen(entry->resource_partition_key));
        item = session_hash_bytes(
            item, entry->resource_initiator_origin,
            entry->resource_initiator_origin == NULL
                ? 0 : strlen(entry->resource_initiator_origin));
        item = session_hash_bytes(
            item, &entry->resource_grant.mode,
            sizeof(entry->resource_grant.mode));
        item = session_hash_bytes(
            item, &entry->resource_grant.credentials,
            sizeof(entry->resource_grant.credentials));
        item = session_hash_bytes(
            item, &entry->response_url_known,
            sizeof(entry->response_url_known));
        item = session_hash_bytes(
            item, entry->response_url,
            entry->response_url == NULL ? 0 : strlen(entry->response_url));
        item = session_hash_bytes(
            item, &entry->response_referrer_policy_known,
            sizeof(entry->response_referrer_policy_known));
        item = session_hash_bytes(
            item, entry->response_referrer_policy,
            entry->response_referrer_policy_known
                ? strlen(entry->response_referrer_policy) : 0);
        /* Commutative combination makes slot replacement and LRU movement
           irrelevant while any content/authority change invalidates reuse. */
        combined ^= item + UINT64_C(0x9e3779b97f4a7c15)
                    + (item << 6) + (item >> 2);
        count++;
    }
    return session_hash_bytes(combined, &count, sizeof(count));
}

bool browser_session_cache_revalidate_module(
    BrowserSession *session, const char *url,
    const char *cache_control, const char *vary, uint64_t now_ns,
    const BrowserModuleCacheProvenance *provenance)
{
    return cache_revalidate_variant(
        session, url, cache_control, vary, now_ns, provenance, false,
        NULL, NULL);
}

bool browser_session_cache_revalidate_classic_script(
    BrowserSession *session, const char *url, const char *cache_control,
    const char *vary, uint64_t now_ns,
    const TilefinchRequestContext *context,
    const TilefinchResourceGrant *grant)
{
    return context != NULL
            && context->destination == TILEFINCH_DESTINATION_SCRIPT
        && browser_session_cache_revalidate_resource(
            session, url, cache_control, vary, now_ns, context, grant);
}

bool browser_session_cache_revalidate_resource(
    BrowserSession *session, const char *url, const char *cache_control,
    const char *vary, uint64_t now_ns,
    const TilefinchRequestContext *context,
    const TilefinchResourceGrant *grant)
{
    return cache_revalidate_variant(
        session, url, cache_control, vary, now_ns, NULL, true,
        context, grant);
}

size_t browser_session_cache_reclaim(BrowserSession *session,
                                     size_t target_bytes)
{
    if (session == NULL || session->budget == NULL || target_bytes == 0) {
        return 0;
    }
    size_t before = budget_remaining(session->budget);
    size_t reclaimed = 0;
    while (reclaimed < target_bytes) {
        BrowserCacheEntry *victim = NULL;
        for (size_t i = 0; i < BROWSER_CACHE_ENTRIES; i++) {
            BrowserCacheEntry *candidate = &session->cache[i];
            if (candidate->data != NULL
                && (victim == NULL || candidate->stamp < victim->stamp)) {
                victim = candidate;
            }
        }
        if (victim == NULL) break;
        cache_remove(session, victim);
        session->cache_evictions++;
        size_t after = budget_remaining(session->budget);
        reclaimed = after > before ? after - before : 0;
    }
    return reclaimed;
}

bool browser_session_cache_set_maximum_bytes(BrowserSession *session,
                                             size_t maximum_bytes)
{
    if (session == NULL || session->budget == NULL || maximum_bytes == 0
        || maximum_bytes > session->budget->limit) {
        return false;
    }
    while (session->cache_bytes > maximum_bytes) {
        BrowserCacheEntry *victim = NULL;
        for (size_t i = 0; i < BROWSER_CACHE_ENTRIES; i++) {
            BrowserCacheEntry *candidate = &session->cache[i];
            if (candidate->data != NULL
                && (victim == NULL || candidate->stamp < victim->stamp)) {
                victim = candidate;
            }
        }
        if (victim == NULL) return false;
        cache_remove(session, victim);
        session->cache_evictions++;
    }
    session->maximum_cache_bytes = maximum_bytes;
    return true;
}

void browser_session_cache_clear(BrowserSession *session)
{
    if (session == NULL || session->budget == NULL) return;
    for (size_t i = 0; i < BROWSER_CACHE_ENTRIES; i++) {
        cache_remove(session, &session->cache[i]);
    }
    session->clock = 0;
}

void browser_session_destroy(BrowserSession *session)
{
    if (session == NULL || session->budget == NULL) return;
    for (size_t i = 0; i < BROWSER_STORAGE_ENTRIES; i++) {
        budget_free(session->budget, session->storage[i].value);
    }
    for (size_t i = 0; i < BROWSER_COOKIE_ENTRIES; i++) {
        budget_free(session->budget, session->cookies[i].value);
        budget_free(session->budget, session->cookies[i].long_path);
    }
    for (size_t i = 0; i < BROWSER_CACHE_ENTRIES; i++) {
        cache_remove(session, &session->cache[i]);
    }
    budget_reservation_release(&session->accounting_reservation);
    memset(session, 0, sizeof(*session));
}
