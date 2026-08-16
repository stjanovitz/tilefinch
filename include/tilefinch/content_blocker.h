#ifndef TILEFINCH_CONTENT_BLOCKER_H
#define TILEFINCH_CONTENT_BLOCKER_H

#include <stdbool.h>
#include <stddef.h>

#include "tilefinch/budget.h"

#define CONTENT_BLOCKER_RULE_LIMIT 4096u
#define CONTENT_BLOCKER_STRING_BYTES_LIMIT (128u * 1024u)
#define CONTENT_BLOCKER_FILE_BYTES_LIMIT (512u * 1024u)
#define CONTENT_BLOCKER_ALLOW_SITE_LIMIT 32u
#define CONTENT_BLOCKER_HOST_LIMIT 128u
#define CONTENT_BLOCKER_COSMETIC_CSS_LIMIT 1024u
#define CONTENT_BLOCKER_COOKIE_CSS_LIMIT 2048u

typedef enum {
    CONTENT_BLOCKER_OFF = 0,
    CONTENT_BLOCKER_BASIC,
    CONTENT_BLOCKER_CUSTOM
} ContentBlockerMode;

typedef struct ContentBlocker ContentBlocker;

typedef struct {
    ContentBlockerMode mode;
    size_t rule_count;
    size_t allow_rule_count;
    size_t ignored_rule_count;
    size_t allowed_site_count;
    size_t requests_considered;
    size_t requests_blocked;
    size_t retained_bytes;
    bool truncated;
} ContentBlockerMetrics;

ContentBlocker *content_blocker_create(Budget *budget);
void content_blocker_destroy(ContentBlocker *blocker);

/* Switching lists is transactional: a missing, oversized, truncated, or
   unreadable custom list leaves the active configuration untouched. Syntax
   outside the documented subset is counted and ignored. OFF and BASIC do
   not read custom_path. */
bool content_blocker_configure(ContentBlocker *blocker,
                               ContentBlockerMode mode,
                               const char *custom_path);
bool content_blocker_set_allowed_sites(
    ContentBlocker *blocker, const char *const *sites, size_t count);
bool content_blocker_metrics(const ContentBlocker *blocker,
                             ContentBlockerMetrics *metrics);

/* Extracts the bounded registrable-site host used by the per-site allowlist.
   Raw Unicode/invalid URLs fail closed rather than creating ambiguous keys. */
bool content_blocker_site_from_url(
    const char *url, char output[CONTENT_BLOCKER_HOST_LIMIT]);

/* Emits the deliberately conservative built-in cosmetic rules. Network and
   cosmetic policy share the same mode/site bypass, but composition with
   reader/font user CSS remains a frontend responsibility. */
bool content_blocker_cosmetic_css(
    char *output, size_t capacity, size_t *length);

/* Emits a bounded, privacy-preserving cookie-notice stylesheet. It hides
   well-known consent surfaces and releases their common scroll-lock classes;
   it never clicks an acceptance control or writes a consent cookie. */
bool content_blocker_cookie_banner_css(
    char *output, size_t capacity, size_t *length);

/* Top-level document navigations are never blocked. Other HTTP(S) requests
   are matched against request host, initiator site, party, and destination. */
bool content_blocker_should_block(
    ContentBlocker *blocker, const char *request_url,
    const char *initiator_url, const char *destination,
    const char *fetch_mode);

/* Same policy decision without telemetry. Resource loaders use it before
   accepting a cached response; the eventual transport decision remains the
   one counted request. */
bool content_blocker_would_block(
    const ContentBlocker *blocker, const char *request_url,
    const char *initiator_url, const char *destination,
    const char *fetch_mode);

#endif
