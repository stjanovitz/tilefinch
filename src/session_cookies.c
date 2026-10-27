/* The cookie jar: RFC 6265bis parsing and storage, the Cookie header, the
   request-private overlay a redirect chain stages Set-Cookie into, seeds,
   and per-domain clearing. BrowserSession keeps the jar's storage; session.c
   reaches it through session_cookies.h. */
#include "session_cookies.h"

#include <ctype.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

#include "tilefinch/budget.h"
#include "tilefinch/public_suffix.h"
#include "tilefinch/url.h"
#include "diagnostic_trace.h"

#define budget_malloc(b, s) budget_malloc_category((b), BUDGET_CATEGORY_SESSION, (s))
#define budget_calloc(b, n, s) budget_calloc_category((b), BUDGET_CATEGORY_SESSION, (n), (s))
#define budget_realloc(b, p, s) budget_realloc_category((b), BUDGET_CATEGORY_SESSION, (p), (s))

static uint64_t session_hash_bytes(uint64_t hash, const void *data,
                                   size_t length)
{
    const unsigned char *bytes = data;
    for (size_t i = 0; i < length; i++)
        hash = (hash ^ bytes[i]) * UINT64_C(1099511628211);
    return hash;
}

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

int64_t session_cookie_now(void)
{
    return cookie_clock_override != 0 ? cookie_clock_override
                                      : (int64_t) time(NULL);
}

bool browser_cookie_parse_date(const char *value, size_t length,
                               int64_t *epoch_seconds)
{
    return parse_cookie_date(value, length, epoch_seconds);
}

const char *browser_cookie_entry_path(const BrowserCookieEntry *entry)
{
    if (entry == NULL) return "";
    return entry->long_path == NULL ? entry->path : entry->long_path;
}

/* A request-private copy of the jar for one redirect chain. It holds only
   the live cookies (and allowed sites) it copied, growing toward the
   session's BROWSER_COOKIE_ENTRIES slots only when a staged Set-Cookie
   needs a free slot, so the common request does not carry a full table. */
struct BrowserCookieOverlay {
    Budget *budget;
    bool site_data_allowed;
    BrowserCookieEntry *cookies;
    size_t cookie_capacity;
    size_t cookie_bytes;
    size_t cookie_long_path_bytes;
    size_t maximum_cookie_bytes;
    size_t maximum_cookie_long_path_bytes;
    size_t cookie_clock;
    char (*third_party_cookie_allowed_sites)[BROWSER_ORIGIN_LIMIT];
    size_t third_party_cookie_allowed_site_count;
    const char *credential_cookie_site;
    const char *const *credential_cookie_names;
    size_t credential_cookie_name_count;
#if !defined(__PSP__) || defined(TILEFINCH_PSP_VALIDATION_LOG)
    /* The source session's diagnostics: the transport sends the Cookie
       header this overlay builds, so header omissions are counted here. */
    BrowserCookieDiagnostics *header_diagnostics;
#endif
};

typedef struct {
    Budget *budget;
    BrowserCookieEntry *cookies;
    /* Slots in `cookies`; an overlay may hold fewer than
       BROWSER_COOKIE_ENTRIES and grow (never past it) through `overlay`. */
    size_t cookie_capacity;
    BrowserCookieOverlay *overlay;
    size_t *cookie_bytes;
    size_t *cookie_long_path_bytes;
    size_t maximum_cookie_bytes;
    size_t maximum_cookie_long_path_bytes;
    size_t *cookie_clock;
    const char (*third_party_cookie_allowed_sites)[BROWSER_ORIGIN_LIMIT];
    size_t third_party_cookie_allowed_site_count;
    const char *credential_cookie_site;
    const char *const *credential_cookie_names;
    size_t credential_cookie_name_count;
    BrowserCookieDiagnostics *diagnostics;
#if !defined(__PSP__) || defined(TILEFINCH_PSP_VALIDATION_LOG)
    /* Where cookie_string counts header omissions: set only for the
       request-private overlay, whose header is the one sent. */
    BrowserCookieDiagnostics *header_diagnostics;
#endif
} BrowserCookieStore;

static BrowserCookieStore browser_session_cookie_store(
    BrowserSession *session)
{
    return (BrowserCookieStore) {
        .budget = session == NULL ? NULL : session->budget,
        .cookies = session == NULL ? NULL : session->cookies,
        .cookie_capacity = session == NULL ? 0 : BROWSER_COOKIE_ENTRIES,
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
        , .credential_cookie_site = session == NULL
            ? NULL : session->credential_cookie_site
        , .credential_cookie_names = session == NULL
            ? NULL : session->credential_cookie_names
        , .credential_cookie_name_count = session == NULL
            ? 0 : session->credential_cookie_name_count
        , .diagnostics = session == NULL ? NULL : &session->cookie_diagnostics
    };
}

static BrowserCookieStore browser_overlay_cookie_store(
    BrowserCookieOverlay *overlay)
{
    return (BrowserCookieStore) {
        .budget = overlay == NULL ? NULL : overlay->budget,
        .cookies = overlay == NULL ? NULL : overlay->cookies,
        .cookie_capacity = overlay == NULL ? 0 : overlay->cookie_capacity,
        .overlay = overlay,
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
        , .credential_cookie_site = overlay == NULL
            ? NULL : overlay->credential_cookie_site
        , .credential_cookie_names = overlay == NULL
            ? NULL : overlay->credential_cookie_names
        , .credential_cookie_name_count = overlay == NULL
            ? 0 : overlay->credential_cookie_name_count
#if !defined(__PSP__) || defined(TILEFINCH_PSP_VALIDATION_LOG)
        , .header_diagnostics = overlay == NULL
            ? NULL : overlay->header_diagnostics
#endif
    };
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
    return session_security_site_list_contains(
        store->third_party_cookie_allowed_sites,
        store->third_party_cookie_allowed_site_count, top_level);
}

static BrowserCookieEntry *find_cookie(BrowserCookieStore *store,
                                       const char *domain, const char *path,
                                       size_t path_length,
                                       const char *name,
                                       const char *partition_key)
{
    for (size_t i = 0; i < store->cookie_capacity; i++) {
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
    for (size_t i = 0; i < store->cookie_capacity; i++) {
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
    return tilefinch_host_within(host, strlen(host), domain, strlen(domain));
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
    for (size_t i = 0; i < store->cookie_capacity; i++) {
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

/* The cookies a request may send, in RFC 6265 Cookie-header order. */
static bool cookie_candidates(const BrowserCookieStore *store, const char *url,
                              const TilefinchRequestContext *request,
                              const TilefinchRequestFacts *facts,
                              bool include_http_only, CookieURL *parsed,
                              size_t candidates[BROWSER_COOKIE_ENTRIES],
                              size_t *candidate_count)
{
    *candidate_count = 0;
    if (store == NULL
        || (store->cookies == NULL && store->overlay == NULL)) return false;
    if (!parse_cookie_url(url, parsed)) return false;
    int64_t now = session_cookie_now();
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
    size_t count = 0;
    for (size_t i = 0; i < store->cookie_capacity; i++) {
        const BrowserCookieEntry *entry = &store->cookies[i];
        if (entry->value == NULL
            || (entry->expires_at != 0 && entry->expires_at <= now)
            || (!include_http_only && entry->http_only)
            || (entry->secure && !parsed->secure)
            || (entry->host_only
                ? strcmp(parsed->host, entry->domain) != 0
                : !domain_matches(parsed->host, entry->domain))
            || !path_matches(
                   parsed->path, parsed->path_length,
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
        size_t position = count;
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
        count++;
    }
    *candidate_count = count;
    return true;
}

/* Which cookies survive when a header would exceed its bound: 0 for the
   site's registered credential cookies, 1 for cookies the server marked
   HttpOnly or __Host-/__Secure- (session cookies are), 2 for the rest. */
static unsigned cookie_header_rank(const BrowserCookieStore *store,
                                   const CookieURL *parsed,
                                   const BrowserCookieEntry *entry)
{
    if (store->credential_cookie_names != NULL
        && store->credential_cookie_site != NULL
        && domain_matches(parsed->host, store->credential_cookie_site)) {
        for (size_t i = 0; i < store->credential_cookie_name_count; i++) {
            if (strcmp(entry->name, store->credential_cookie_names[i]) == 0)
                return 0;
        }
    }
    if (entry->http_only || strncmp(entry->name, "__Host-", 7) == 0
        || strncmp(entry->name, "__Secure-", 9) == 0) return 1;
    return 2;
}

static size_t cookie_pair_length(const BrowserCookieEntry *entry)
{
    return strlen(entry->name) + 1 + entry->value_length;
}

static bool cookie_string(const BrowserCookieStore *store, const char *url,
                          const TilefinchRequestContext *request,
                          const TilefinchRequestFacts *facts,
                          bool include_http_only, char *output,
                          size_t output_capacity, uint64_t *fingerprint)
{
    bool writing = output != NULL && output_capacity != 0;
    if (writing) output[0] = '\0';
    if (!writing && fingerprint == NULL) return false;
    CookieURL parsed;
    size_t candidates[BROWSER_COOKIE_ENTRIES];
    size_t candidate_count = 0;
    if (!cookie_candidates(store, url, request, facts, include_http_only,
                           &parsed, candidates, &candidate_count))
        return false;
    /* A fingerprint describes what a bounded builder sends. */
    bool bounded = !writing
        || output_capacity >= BROWSER_COOKIE_HEADER_CAPACITY;
    size_t limit = bounded ? BROWSER_COOKIE_HEADER_CAPACITY - 1
                           : output_capacity - 1;
    size_t total = 0;
    bool fits = true;
    for (size_t candidate = 0; candidate < candidate_count && fits;
         candidate++) {
        size_t needed = (candidate == 0 ? 0 : 2) + cookie_pair_length(
            &store->cookies[candidates[candidate]]);
        if (needed > limit - total) fits = false;
        else total += needed;
    }
    bool keep[BROWSER_COOKIE_ENTRIES];
    for (size_t candidate = 0; candidate < candidate_count; candidate++)
        keep[candidate] = fits;
    if (!fits) {
        /* Callers may deliberately treat cookie construction failure as an
           empty Cookie header. Never leave a sendable partial prefix. */
        if (!bounded) return false;
        /* Same-named cookies form one group, led by its first (most
           specific) member, which is the value a server reads. The group
           takes its leader's rank, and once a member is left out every
           later member is too, so a shadowed value is never sent in place
           of the one it shadows. */
        _Static_assert(BROWSER_COOKIE_ENTRIES <= 256u,
                       "cookie group leaders are stored in bytes");
        uint8_t leader[BROWSER_COOKIE_ENTRIES];
        unsigned char rank_of[BROWSER_COOKIE_ENTRIES];
        bool omitted[BROWSER_COOKIE_ENTRIES];
        for (size_t candidate = 0; candidate < candidate_count;
             candidate++) {
            const BrowserCookieEntry *entry =
                &store->cookies[candidates[candidate]];
            size_t first = 0;
            while (first < candidate
                   && strcmp(store->cookies[candidates[first]].name,
                             entry->name) != 0) first++;
            leader[candidate] = (uint8_t) first;
            rank_of[candidate] = first == candidate
                ? (unsigned char) cookie_header_rank(store, &parsed, entry)
                : rank_of[first];
            omitted[candidate] = false;
        }
        size_t used = 0, kept = 0;
        for (unsigned rank = 0; rank < 3; rank++) {
            for (size_t candidate = 0; candidate < candidate_count;
                 candidate++) {
                if (rank_of[candidate] != rank) continue;
                const BrowserCookieEntry *entry =
                    &store->cookies[candidates[candidate]];
                size_t needed = (kept == 0 ? 0 : 2)
                    + cookie_pair_length(entry);
                if (omitted[leader[candidate]] || needed > limit - used) {
                    omitted[leader[candidate]] = true;
                    continue;
                }
                used += needed;
                kept++;
                keep[candidate] = true;
            }
        }
#if !defined(__PSP__) || defined(TILEFINCH_PSP_VALIDATION_LOG)
        BrowserCookieDiagnostics *d = store->header_diagnostics;
        if (writing && d != NULL && d->enabled
            && d->header_omissions <= SIZE_MAX - (candidate_count - kept))
            d->header_omissions += candidate_count - kept;
#endif
    }
    uint64_t hash = UINT64_C(1469598103934665603);
    size_t used = 0;
    for (size_t candidate = 0; candidate < candidate_count; candidate++) {
        if (!keep[candidate]) continue;
        const BrowserCookieEntry *entry =
            &store->cookies[candidates[candidate]];
        size_t name_length = strlen(entry->name);
        if (fingerprint != NULL) {
            static const char separator[] = "; ";
            if (used != 0)
                hash = session_hash_bytes(hash, separator, 2);
            hash = session_hash_bytes(hash, entry->name, name_length);
            hash = session_hash_bytes(hash, "=", 1);
            hash = session_hash_bytes(hash, entry->value, entry->value_length);
        }
        if (writing) {
            if (used != 0) { output[used++] = ';'; output[used++] = ' '; }
            memcpy(output + used, entry->name, name_length); used += name_length;
            output[used++] = '=';
            memcpy(output + used, entry->value, entry->value_length);
            used += entry->value_length;
            output[used] = '\0';
        } else {
            used = 1;
        }
    }
    if (fingerprint != NULL) *fingerprint = hash;
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
        &store, url, NULL, NULL, false, output, output_capacity, NULL);
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
                         output_capacity, NULL);
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
        output, output_capacity, NULL);
}

bool browser_session_cookie_header_fingerprint_context(
    const BrowserSession *session, const TilefinchRequestContext *context,
    uint64_t *fingerprint)
{
    if (fingerprint != NULL) *fingerprint = 0;
    TilefinchRequestFacts facts;
    if (session == NULL || context == NULL || fingerprint == NULL
        || !tilefinch_request_context_analyze(context, &facts)) return false;
    if (!session->site_data_allowed || !facts.sends_credentials) {
        *fingerprint = UINT64_C(1469598103934665603);
        return true;
    }
    BrowserCookieStore store = browser_session_cookie_store(
        (BrowserSession *) session);
    return cookie_string(
        &store, context->target_url, context, &facts, true,
        NULL, 0, fingerprint);
}

bool browser_session_cookie_value_context(
    const BrowserSession *session, const TilefinchRequestContext *context,
    const char *name, const char **value, size_t *length)
{
    if (value != NULL) *value = NULL;
    if (length != NULL) *length = 0;
    TilefinchRequestFacts facts;
    if (session == NULL || context == NULL || name == NULL || value == NULL
        || length == NULL || !session->site_data_allowed
        || !tilefinch_request_context_analyze(context, &facts)
        || !facts.sends_credentials) return false;
    BrowserCookieStore store = browser_session_cookie_store(
        (BrowserSession *) session);
    CookieURL parsed;
    size_t candidates[BROWSER_COOKIE_ENTRIES];
    size_t candidate_count = 0;
    if (!cookie_candidates(&store, context->target_url, context, &facts, true,
                           &parsed, candidates, &candidate_count))
        return false;
    for (size_t candidate = 0; candidate < candidate_count; candidate++) {
        const BrowserCookieEntry *entry =
            &session->cookies[candidates[candidate]];
        if (strcmp(entry->name, name) != 0) continue;
        *value = entry->value;
        *length = entry->value_length;
        return true;
    }
    return false;
}

void browser_session_set_credential_cookies(
    BrowserSession *session, const char *site,
    const char *const *names, size_t name_count)
{
    if (session == NULL) return;
    bool valid = site != NULL && names != NULL && name_count != 0;
    session->credential_cookie_site = valid ? site : NULL;
    session->credential_cookie_names = valid ? names : NULL;
    session->credential_cookie_name_count = valid ? name_count : 0;
}

/* Creation order is observable when paths have equal specificity. If a long
   session (or a replay seed near SIZE_MAX) exhausts the monotonic counter,
   compact the live order without allocating or changing serialization. */
static void cookie_clock_make_room(BrowserCookieStore *store)
{
    /* Both store views keep cookie_capacity <= BROWSER_COOKIE_ENTRIES. */
    if (store == NULL || store->cookies == NULL || store->cookie_clock == NULL
        || *store->cookie_clock < SIZE_MAX - 1) return;
    size_t ordered[BROWSER_COOKIE_ENTRIES];
    size_t count = 0;
    for (size_t i = 0; i < store->cookie_capacity; i++) {
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

typedef enum {
    COOKIE_DIAGNOSTIC_BYTES,
    COOKIE_DIAGNOSTIC_SLOT,
    COOKIE_DIAGNOSTIC_ALLOCATION,
    COOKIE_DIAGNOSTIC_POLICY_IGNORED,
    COOKIE_DIAGNOSTIC_EVICTION
} CookieDiagnosticKind;

static void cookie_diagnostic_add(BrowserCookieStore *store,
                                  CookieDiagnosticKind kind)
{
    BrowserCookieDiagnostics *d = store == NULL ? NULL : store->diagnostics;
    if (d == NULL || !d->enabled) return;
    size_t *counter = &d->evictions;
    switch (kind) {
    case COOKIE_DIAGNOSTIC_BYTES: counter = &d->byte_refusals; break;
    case COOKIE_DIAGNOSTIC_SLOT: counter = &d->slot_refusals; break;
    case COOKIE_DIAGNOSTIC_ALLOCATION: counter = &d->allocation_refusals; break;
    case COOKIE_DIAGNOSTIC_POLICY_IGNORED: counter = &d->policy_ignored; break;
    case COOKIE_DIAGNOSTIC_EVICTION: break;
    }
    if (*counter != SIZE_MAX) (*counter)++;
}

/* Adds free slots to an overlay's table (never past BROWSER_COOKIE_ENTRIES)
   and points `store` at the new table. */
static bool cookie_overlay_grow(BrowserCookieStore *store)
{
    BrowserCookieOverlay *overlay = store == NULL ? NULL : store->overlay;
    if (overlay == NULL || overlay->cookies != store->cookies
        || overlay->cookie_capacity != store->cookie_capacity
        || overlay->cookie_capacity >= BROWSER_COOKIE_ENTRIES) return false;
    size_t capacity = overlay->cookie_capacity;
    size_t grown = BROWSER_COOKIE_ENTRIES - capacity < 8u
        ? BROWSER_COOKIE_ENTRIES : capacity + 8u;
    BrowserCookieEntry *cookies = budget_calloc_category(
        overlay->budget, BUDGET_CATEGORY_RESOURCE, grown, sizeof(*cookies));
    /* Under budget pressure, one more slot still stores the cookie the
       jar would have stored. */
    if (cookies == NULL && grown != capacity + 1u) {
        grown = capacity + 1u;
        cookies = budget_calloc_category(
            overlay->budget, BUDGET_CATEGORY_RESOURCE, grown, sizeof(*cookies));
    }
    if (cookies == NULL) return false;
    if (capacity != 0) memcpy(cookies, overlay->cookies,
                              capacity * sizeof(*cookies));
    budget_free(overlay->budget, overlay->cookies);
    overlay->cookies = store->cookies = cookies;
    overlay->cookie_capacity = store->cookie_capacity = grown;
    return true;
}

static bool cookie_set_raw(BrowserCookieStore *store,
                       const TilefinchRequestContext *request,
                       const char *cookie, bool from_http)
{
    if (store == NULL || store->budget == NULL
        || (store->cookies == NULL && store->overlay == NULL)
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
    int64_t now = session_cookie_now();
    if (max_age_valid) {
        if (max_age <= 0) remove = true;
        else expires_at = max_age > INT64_MAX - now
                          ? INT64_MAX : now + max_age;
    } else if (expires_valid && expires_at <= now) {
        remove = true;
    }
    if (tilefinch_trace_cookie()) {
        fprintf(stderr, "cookie-store host=%s domain=%s path=%.*s name=%s remove=%d expires-valid=%d expires-delta=%lld max-age-valid=%d\n",
                parsed.host, domain, (int) path_length, path,
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
        && !cookie_store_third_party_allowed(store, request)) {
        cookie_diagnostic_add(store, COOKIE_DIAGNOSTIC_POLICY_IGNORED);
        return true;
    }
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
    bool evicting = false, grow = false;
    if (entry == NULL) {
        BrowserCookieEntry *oldest_domain = NULL;
        BrowserCookieEntry *oldest_global = NULL;
        size_t domain_count = 0;
        for (size_t i = 0; i < store->cookie_capacity; i++) {
            BrowserCookieEntry *candidate = &store->cookies[i];
            if (candidate->value == NULL) continue;
            /* Eviction must not become a back door around the protections
               enforced above on an exact-key overwrite.  Script (!from_http)
               may never retire an HttpOnly cookie, and an insecure origin may
               never retire a Secure one -- otherwise enough scripted writes
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
            for (size_t i = 0; i < store->cookie_capacity; i++) {
                if (store->cookies[i].value == NULL) {
                    entry = &store->cookies[i];
                    break;
                }
            }
            /* A smaller overlay has a free slot wherever the jar would. It
               grows only once the cookie has passed its checks and its
               copies are allocated, so a refused cookie leaves the table
               as it was. */
            if (entry == NULL
                && store->cookie_capacity < BROWSER_COOKIE_ENTRIES)
                grow = true;
            if (entry == NULL && !grow) {
                entry = oldest_global;
                evicting = entry != NULL;
            }
        }
    }
    if (entry == NULL && !grow) {
        cookie_diagnostic_add(store, COOKIE_DIAGNOSTIC_SLOT);
        return false;
    }
    bool newly_created = grow || entry->value == NULL;
    size_t old_length = newly_created ? 0 : entry->value_length;
    size_t old_long_path_length =
        grow || entry->long_path == NULL ? 0 : entry->path_length;
    if (*store->cookie_bytes - old_length + value_length
        > store->maximum_cookie_bytes) {
        cookie_diagnostic_add(store, COOKIE_DIAGNOSTIC_BYTES);
        return false;
    }
    size_t new_long_path_length =
        path_length >= sizeof(entry->path) ? path_length : 0;
    if (*store->cookie_long_path_bytes - old_long_path_length
            + new_long_path_length
        > store->maximum_cookie_long_path_bytes) {
        cookie_diagnostic_add(store, COOKIE_DIAGNOSTIC_BYTES);
        return false;
    }
    char *copy = budget_malloc(store->budget, value_length + 1);
    if (copy == NULL) {
        cookie_diagnostic_add(store, COOKIE_DIAGNOSTIC_ALLOCATION);
        return false;
    }
    memcpy(copy, value, value_length); copy[value_length] = '\0';
    char *long_path = NULL;
    if (new_long_path_length != 0) {
        long_path = budget_malloc(store->budget, path_length + 1);
        if (long_path == NULL) {
            cookie_diagnostic_add(store, COOKIE_DIAGNOSTIC_ALLOCATION);
            budget_free(store->budget, copy);
            return false;
        }
        memcpy(long_path, path, path_length);
        long_path[path_length] = '\0';
    }
    if (grow) {
        size_t first_new = store->cookie_capacity;
        if (!cookie_overlay_grow(store)) {
            cookie_diagnostic_add(store, COOKIE_DIAGNOSTIC_ALLOCATION);
            budget_free(store->budget, long_path);
            budget_free(store->budget, copy);
            return false;
        }
        entry = &store->cookies[first_new];
    }
    if (evicting) {
        cookie_diagnostic_add(store, COOKIE_DIAGNOSTIC_EVICTION);
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

static bool cookie_set(BrowserCookieStore *store,
                       const TilefinchRequestContext *request,
                       const char *cookie, bool from_http)
{
    bool okay = cookie_set_raw(store, request, cookie, from_http);
    BrowserCookieDiagnostics *d = store == NULL ? NULL : store->diagnostics;
    if (d != NULL && d->enabled) {
        if (!okay && d->refused != SIZE_MAX) d->refused++;
#ifndef __PSP__
        size_t *attempts = from_http ? &d->http_attempts : &d->script_attempts;
        if (*attempts != SIZE_MAX) (*attempts)++;
        if (store->cookie_bytes != NULL
            && *store->cookie_bytes > d->peak_value_bytes)
            d->peak_value_bytes = *store->cookie_bytes;
#endif
    }
    return okay;
}

void browser_session_cookie_diagnostics(BrowserSession *session, bool enable,
                                        BrowserCookieDiagnostics *diagnostics)
{
    if (session == NULL) {
        if (diagnostics != NULL) *diagnostics = (BrowserCookieDiagnostics) {0};
        return;
    }
    if (enable) session->cookie_diagnostics.enabled = true;
    if (diagnostics != NULL) *diagnostics = session->cookie_diagnostics;
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
    browser_session_site_adapter_document_cache_clear(session);
}

static bool site_cookie_matches(
    const BrowserCookieEntry *entry, const CookieURL *parsed)
{
    return entry != NULL && parsed != NULL && entry->value != NULL
        && (entry->host_only
            ? strcmp(parsed->host, entry->domain) == 0
            : domain_matches(parsed->host, entry->domain));
}

bool browser_session_cookie_clear_domain(BrowserSession *session,
                                         const char *url)
{
    CookieURL parsed;
    if (session == NULL || session->budget == NULL || url == NULL
        || !parse_cookie_url(url, &parsed)) return false;
    BrowserCookieStore store = browser_session_cookie_store(session);
    for (size_t i = 0; i < BROWSER_COOKIE_ENTRIES; i++) {
        BrowserCookieEntry *entry = &session->cookies[i];
        if (entry->value != NULL && domain_matches(entry->domain, parsed.host))
            clear_cookie_entry(&store, entry);
    }
    memset(&session->site_adapter_state, 0, sizeof(session->site_adapter_state));
    browser_session_site_adapter_document_cache_clear(session);
    return true;
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
    overlay->credential_cookie_site = source->credential_cookie_site;
    overlay->credential_cookie_names = source->credential_cookie_names;
    overlay->credential_cookie_name_count =
        source->credential_cookie_name_count;
#if !defined(__PSP__) || defined(TILEFINCH_PSP_VALIDATION_LOG)
    overlay->header_diagnostics =
        (BrowserCookieDiagnostics *) &source->cookie_diagnostics;
#endif
    size_t sites = source->third_party_cookie_allowed_site_count;
    if (sites > BROWSER_SECURITY_SITE_LIMIT) sites = BROWSER_SECURITY_SITE_LIMIT;
    if (sites != 0) {
        overlay->third_party_cookie_allowed_sites = budget_malloc_category(
            budget, BUDGET_CATEGORY_RESOURCE,
            sites * sizeof(overlay->third_party_cookie_allowed_sites[0]));
        if (overlay->third_party_cookie_allowed_sites == NULL) {
            browser_cookie_overlay_destroy(overlay);
            return NULL;
        }
        memcpy(overlay->third_party_cookie_allowed_sites,
               source->third_party_cookie_allowed_sites,
               sites * sizeof(overlay->third_party_cookie_allowed_sites[0]));
        overlay->third_party_cookie_allowed_site_count = sites;
    }
    /* A blocked session still gets a valid request-private jar, but it is
       intentionally empty. This closes the transport path that otherwise
       bypasses the public cookie-header gate. */
    if (!source->site_data_allowed) return overlay;
    size_t live = 0;
    for (size_t i = 0; i < BROWSER_COOKIE_ENTRIES; i++)
        if (source->cookies[i].value != NULL) live++;
    if (live != 0) {
        overlay->cookies = budget_calloc_category(
            budget, BUDGET_CATEGORY_RESOURCE, live, sizeof(*overlay->cookies));
        if (overlay->cookies == NULL) {
            browser_cookie_overlay_destroy(overlay);
            return NULL;
        }
        overlay->cookie_capacity = live;
    }
    size_t at = 0;
    for (size_t i = 0; i < BROWSER_COOKIE_ENTRIES && at < live; i++) {
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
        BrowserCookieEntry *copy = &overlay->cookies[at++];
        *copy = *entry;
        copy->value = value;
        copy->long_path = NULL;
        if (entry->long_path != NULL) {
            char *path = budget_malloc_category(
                budget, BUDGET_CATEGORY_RESOURCE,
                entry->path_length + 1);
            if (path == NULL) {
                browser_cookie_overlay_destroy(overlay);
                return NULL;
            }
            memcpy(path, entry->long_path, entry->path_length + 1);
            copy->long_path = path;
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
                         output, output_capacity, NULL);
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
        for (size_t i = 0; i < overlay->cookie_capacity; i++) {
            budget_free(budget, overlay->cookies[i].value);
            budget_free(budget, overlay->cookies[i].long_path);
        }
        budget_free(budget, overlay->cookies);
        budget_free(budget, overlay->third_party_cookie_allowed_sites);
        budget_free(budget, overlay);
    }
}

bool session_cookie_site_usage(const BrowserSession *session, const char *url,
                               BrowserSiteDataUsage *usage)
{
    CookieURL parsed;
    if (!parse_cookie_url(url, &parsed)) return false;
    int64_t now = session_cookie_now();
    for (size_t at = 0; at < BROWSER_COOKIE_ENTRIES; at++) {
        const BrowserCookieEntry *entry = &session->cookies[at];
        if (!site_cookie_matches(entry, &parsed)
            || (entry->expires_at != 0 && entry->expires_at <= now))
            continue;
        usage->cookie_count++;
        usage->cookie_bytes += entry->value_length + strlen(entry->name)
            + entry->path_length;
    }
    return true;
}

bool session_cookie_clear_site(BrowserSession *session, const char *url)
{
    CookieURL parsed;
    if (!parse_cookie_url(url, &parsed)) return false;
    BrowserCookieStore store = browser_session_cookie_store(session);
    for (size_t at = 0; at < BROWSER_COOKIE_ENTRIES; at++)
        if (site_cookie_matches(&session->cookies[at], &parsed))
            clear_cookie_entry(&store, &session->cookies[at]);
    return true;
}
