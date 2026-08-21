#ifndef TILEFINCH_SESSION_H
#define TILEFINCH_SESSION_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "tilefinch/budget.h"
#include "tilefinch/request_context.h"

#define BROWSER_STORAGE_ENTRIES 64
#define BROWSER_COOKIE_ENTRIES 32
#define BROWSER_COOKIE_PER_DOMAIN_LIMIT 8
#define BROWSER_COOKIE_LONG_PATH_LIMIT 2048
#define BROWSER_COOKIE_LONG_PATH_BYTES (8u * 1024u)
/* A large article page can cycle ~30 tiny mask/icon responses plus content
   images and stylesheets through this LRU across script-driven re-renders;
   8 slots thrashed before any re-discovered node could reuse its body.
   Bytes stay bounded by maximum_cache_bytes regardless of slot count. */
#define BROWSER_CACHE_ENTRIES 64
#define BROWSER_ORIGIN_LIMIT 320
#define BROWSER_KEY_LIMIT 96
#define BROWSER_REFERRER_POLICY_LIMIT 40
#define BROWSER_MODULE_REFERRER_POLICY_LIMIT BROWSER_REFERRER_POLICY_LIMIT
/* Site adapters may retain one small, validated provider configuration
   between navigations.  Fixed inline storage keeps this state visible to the
   session budget and prevents an adapter from growing an unbounded cache. */
#define BROWSER_SITE_ADAPTER_STATE_KEY_LIMIT 64
#define BROWSER_SITE_ADAPTER_STATE_DATA_LIMIT 1408

struct ContentBlocker;
struct FetchResponseSecurityMetadata;

typedef enum {
    BROWSER_COOKIE_SAME_SITE_DEFAULT = 0,
    BROWSER_COOKIE_SAME_SITE_LAX,
    BROWSER_COOKIE_SAME_SITE_STRICT,
    BROWSER_COOKIE_SAME_SITE_NONE
} BrowserCookieSameSite;

typedef struct {
    char origin[BROWSER_ORIGIN_LIMIT];
    char key[BROWSER_KEY_LIMIT];
    char *value;
    size_t value_length;
    bool local;
} BrowserStorageEntry;

typedef struct {
    char domain[BROWSER_ORIGIN_LIMIT];
    char path[BROWSER_ORIGIN_LIMIT];
    /* Ordinary paths stay inline. Exceptional long paths are allocated only
       on demand and share a separate bounded session quota. */
    char *long_path;
    size_t path_length;
    char name[BROWSER_KEY_LIMIT];
    char *value;
    size_t value_length;
    int64_t expires_at;
    bool host_only;
    bool secure;
    bool http_only;
    BrowserCookieSameSite same_site;
    bool partitioned;
    char partition_key[BROWSER_ORIGIN_LIMIT];
    size_t creation_sequence;
} BrowserCookieEntry;

/* Deterministic HTTP traces may begin after a challenge or other preparatory
   navigation has populated the cookie jar.  A replay seed retains only the
   policy-relevant cookie shape; values are represented solely by their byte
   lengths and are materialized as inert placeholders. */
typedef struct {
    char domain[BROWSER_ORIGIN_LIMIT];
    char path[BROWSER_ORIGIN_LIMIT];
    char name[BROWSER_KEY_LIMIT];
    size_t value_length;
    bool host_only;
    bool secure;
    bool http_only;
    BrowserCookieSameSite same_site;
    bool partitioned;
    char partition_key[BROWSER_ORIGIN_LIMIT];
    size_t creation_sequence;
} BrowserCookieSeedEntry;

/* Main-thread-only, immutable response payload ownership.  Retain/release are
   deliberately non-atomic: curl worker callbacks must use their concurrent
   transfer storage and hand completed payloads back to the browser thread
   before creating or touching a BrowserSharedBody.  After take(), consumers
   may read data but must not mutate or resize it. */
typedef struct BrowserSharedBody {
    Budget *budget;
    unsigned char *data;
    size_t length;
    size_t references;
} BrowserSharedBody;

/* Security provenance for an ECMAScript module representation.  The HTTP
   cache key remains the request URL (module-map identity), while the final
   response URL is retained separately because it is the base URL used to
   resolve that module's imports and expose import.meta.url. */
typedef struct {
    const char *effective_url;
    const char *initiator_origin;
    /* Immutable top-level document URL used to derive the network partition.
       Opaque initiators are deliberately not admitted to the shared module
       cache because their serialized "null" origin is not an identity. */
    const char *top_level_url;
    bool initiator_opaque;
    /* Normalized final-response Referrer-Policy override. An empty value
       means that no recognized response token overrides the incoming graph
       policy. On 304, header_present distinguishes omission (retain the
       stored override) from a present invalid-only field (clear it). */
    const char *response_referrer_policy;
    TilefinchCredentialsMode credentials;
    bool cors_validated;
    /* Sticky redirect taint changes the CORS wire Origin to `null`.  A
       tainted validation cannot authorize a later direct headerless 304. */
    bool cors_redirect_origin_tainted;
    bool javascript_mime_validated;
    bool referrer_policy_header_present;
} BrowserModuleCacheProvenance;

typedef struct {
    char url[2048];
    unsigned char *data;
    BrowserSharedBody *body;
    /* Optional QuickJS classic-script bytecode for this exact response
       body. It shares the HTTP cache's byte ceiling and LRU lifetime, so a
       compiled artifact can never outlive content replacement or turn into
       a second unbounded cache. */
    BrowserSharedBody *classic_script_bytecode;
    uint64_t classic_script_source_hash;
    size_t classic_script_source_length;
    /* Immutable selector-program fragment derived from this exact CSS
       response. Like script bytecode it is RAM-only, charged to the HTTP
       cache ceiling, and discarded with the response or on replacement. */
    BrowserSharedBody *stylesheet_compiled_fragment;
    /* Pointer-free structural parse IR for this exact CSS response and
       viewport. It is RAM-only and shares the response cache's byte/LRU
       ceiling; declaration values are still parsed into each destination
       stylesheet's own intern tables. */
    BrowserSharedBody *stylesheet_parsed_ir;
    /* Optional immutable RGBA target decoded from this exact authorized
       image response. It shares both the response entry's partition and its
       byte/LRU ceiling, so repeat navigations can lease pixels without a
       second decode or a separate unbounded image cache. */
    BrowserSharedBody *decoded_image_pixels;
    int decoded_image_source_width;
    int decoded_image_source_height;
    int decoded_image_width;
    int decoded_image_height;
    uint64_t response_body_hash;
    /* Optional final response URL when it differs from the request/cache key.
       Stylesheets use this as the base for imports and relative resources. */
    char *response_url;
    size_t length;
    size_t stamp;
    char etag[192];
    char last_modified[128];
    char content_type[128];
    char vary[128];
    uint64_t stored_at_ns;
    uint64_t fresh_until_ns;
    bool no_cache;
    bool must_revalidate;
    bool immutable;
    /* Classic no-CORS scripts send no Origin header. Responses which Vary on
       Origin are reusable only by that request class, never generic fetch. */
    bool classic_script_origin_variant;
    /* True only when response_url provenance was explicitly retained.  A
       request-key fallback is useful for CSS bases but is not CORS evidence. */
    bool response_url_known;
    /* Normalized final-response Referrer-Policy.  The empty string is a
       known value (no recognized response override); the separate bit keeps
       that distinct from a generic cache entry with no retained provenance. */
    char response_referrer_policy[BROWSER_REFERRER_POLICY_LIMIT];
    bool response_referrer_policy_known;
    /* Page-resource responses carry a typed grant plus the exact network
       partition/principal which earned it. These strings are allocated only
       for authorized subresources, keeping generic cache entries compact
       while preventing a restrictive CORP response from authorizing another
       site. */
    char *resource_partition_key;
    char *resource_initiator_origin;
    char *resource_initiator_site;
    TilefinchResourceGrant resource_grant;
    bool resource_grant_valid;
    /* Fragments are excluded from the HTTP cache key but remain part of the
       module-map request identity. */
    char *module_request_fragment;
    char *module_effective_url;
    char *module_initiator_origin;
    char *module_partition_key;
    char module_response_referrer_policy[
        BROWSER_MODULE_REFERRER_POLICY_LIMIT];
    TilefinchCredentialsMode module_credentials;
    bool module_cors_validated;
    bool module_cors_redirect_origin_tainted;
    bool module_javascript_mime_validated;
} BrowserCacheEntry;

typedef enum {
    BROWSER_CACHE_MISS = 0,
    BROWSER_CACHE_FRESH,
    BROWSER_CACHE_STALE
} BrowserCacheStatus;

typedef struct {
    char key[BROWSER_SITE_ADAPTER_STATE_KEY_LIMIT];
    unsigned char data[BROWSER_SITE_ADAPTER_STATE_DATA_LIMIT];
    size_t length;
    uint64_t stored_at_ns;
    uint64_t cookie_fingerprint;
    bool valid;
} BrowserSiteAdapterState;

typedef struct BrowserSession {
    Budget *budget;
    /* Non-owning engine-lifetime request policy. */
    struct ContentBlocker *content_blocker;
    BrowserStorageEntry storage[BROWSER_STORAGE_ENTRIES];
    BrowserCookieEntry cookies[BROWSER_COOKIE_ENTRIES];
    BrowserCacheEntry cache[BROWSER_CACHE_ENTRIES];
    BrowserSiteAdapterState site_adapter_state;
    size_t storage_bytes;
    size_t cookie_bytes;
    size_t cookie_long_path_bytes;
    size_t cache_bytes;
    size_t maximum_storage_bytes;
    size_t maximum_cookie_bytes;
    size_t maximum_cookie_long_path_bytes;
    size_t maximum_cache_bytes;
    size_t clock;
    size_t cookie_clock;
    size_t cache_hits;
    size_t cache_fresh_hits;
    size_t cache_stale_hits;
    size_t cache_misses;
    size_t cache_evictions;
    /* Global page policy. HTTP cache remains independently configurable. */
    bool site_data_allowed;
    /* Security compatibility grants are bounded. Mixed-content grants are
       deliberately session-only; third-party-cookie grants are copied from
       the profile at startup. Request and cookie hot paths never touch the
       Memory Stick. */
#define BROWSER_SECURITY_SITE_LIMIT 16u
    char mixed_content_allowed_sites[BROWSER_SECURITY_SITE_LIMIT]
                                    [BROWSER_ORIGIN_LIMIT];
    char third_party_cookie_allowed_sites[BROWSER_SECURITY_SITE_LIMIT]
                                        [BROWSER_ORIGIN_LIMIT];
    size_t mixed_content_allowed_site_count;
    size_t third_party_cookie_allowed_site_count;
#define BROWSER_HSTS_ENTRY_LIMIT 16u
    struct {
        char host[BROWSER_ORIGIN_LIMIT];
        int64_t expires_at;
        size_t stamp;
        bool include_subdomains;
    } hsts[BROWSER_HSTS_ENTRY_LIMIT];
    size_t hsts_clock;
    BudgetReservation accounting_reservation;
    size_t accounting_bytes;
} BrowserSession;

/* Request-private cookie state used while following redirects. The concrete
   layout is intentionally hidden so callers cannot accidentally pay for a
   full BrowserSession (storage and cache included) per active transfer. */
typedef struct BrowserCookieOverlay BrowserCookieOverlay;

/* Pin cookie expiry decisions to a replayed capture's timeline. Monotone:
   keeps the maximum epoch seen; zero (initial) means the machine clock. */
void browser_session_advance_cookie_clock(int64_t epoch_seconds);

/* RFC 6265 cookie-date parser shared with the replay layer. */
bool browser_cookie_parse_date(const char *value, size_t length,
                               int64_t *epoch_seconds);

bool browser_session_init(BrowserSession *session, Budget *budget,
                          size_t maximum_cache_bytes);
void browser_session_set_site_data_allowed(
    BrowserSession *session, bool allowed);
bool browser_session_site_data_allowed(const BrowserSession *session);
bool browser_session_set_mixed_content_site_allowed(
    BrowserSession *session, const char *url, bool allowed);
bool browser_session_mixed_content_site_allowed(
    const BrowserSession *session, const char *url);
bool browser_session_set_third_party_cookie_site_allowed(
    BrowserSession *session, const char *url, bool allowed);
bool browser_session_third_party_cookie_site_allowed(
    const BrowserSession *session, const char *url);
/* HSTS is intentionally memory-only. It strengthens a running session
   without adding boot or navigation-path storage I/O. */
bool browser_session_hsts_observe(
    BrowserSession *session, const char *response_url,
    const char *headers, size_t headers_length);
bool browser_session_hsts_observe_metadata(
    BrowserSession *session, const char *response_url,
    const struct FetchResponseSecurityMetadata *metadata);
bool browser_session_hsts_upgrade_url(
    BrowserSession *session, const char *url,
    char *output, size_t output_capacity);
bool browser_session_site_adapter_state_put(
    BrowserSession *session, const char *key, const void *data,
    size_t data_length, uint64_t now_ns);
bool browser_session_site_adapter_state_get(
    BrowserSession *session, const char *key, void *data,
    size_t data_capacity, size_t *data_length, uint64_t now_ns,
    uint64_t maximum_age_ns);
void browser_session_site_adapter_state_remove(
    BrowserSession *session, const char *key);
bool browser_session_storage_get(const BrowserSession *session,
                                 const char *url, bool local,
                                 const char *key, const char **value,
                                 size_t *value_length);
size_t browser_session_storage_length(
    const BrowserSession *session, const char *url, bool local);
bool browser_session_storage_key(
    const BrowserSession *session, const char *url, bool local,
    size_t index, const char **key);
bool browser_session_storage_set(BrowserSession *session, const char *url,
                                 bool local, const char *key,
                                 const char *value, size_t value_length);
void browser_session_storage_remove(BrowserSession *session, const char *url,
                                    bool local, const char *key);
void browser_session_storage_clear(BrowserSession *session, const char *url,
                                   bool local);
/* Clears every entry in one storage namespace. Passing false clears only
   sessionStorage; persistent storage code deliberately never calls that
   form. */
void browser_session_storage_clear_all(BrowserSession *session, bool local);
bool browser_session_cookie_get(const BrowserSession *session,
                                const char *url, char *output,
                                size_t output_capacity);
bool browser_session_cookie_header(const BrowserSession *session,
                                   const char *url, char *output,
                                   size_t output_capacity);
bool browser_session_cookie_header_context(
    const BrowserSession *session, const TilefinchRequestContext *context,
    char *output, size_t output_capacity);
/* Builder fast path: facts must have been analyzed from this exact immutable
   context. Public callers normally use browser_session_cookie_header_context. */
bool browser_session_cookie_header_request_facts(
    const BrowserSession *session, const TilefinchRequestContext *context,
    const TilefinchRequestFacts *facts,
    char *output, size_t output_capacity);
bool browser_session_cookie_set(BrowserSession *session, const char *url,
                                const char *cookie);
bool browser_session_cookie_set_http(BrowserSession *session,
                                     const char *url, const char *cookie);
bool browser_session_cookie_set_context(
    BrowserSession *session, const TilefinchRequestContext *context,
    const char *cookie);
bool browser_session_cookie_set_http_context(
    BrowserSession *session, const TilefinchRequestContext *context,
    const char *cookie);
const char *browser_cookie_entry_path(const BrowserCookieEntry *entry);
/* Imports a complete redacted cookie seed into an empty jar transactionally.
   Exact cookie values are intentionally neither accepted nor recoverable. */
bool browser_session_cookie_import_redacted_seed(
    BrowserSession *session, const BrowserCookieSeedEntry *entries,
    size_t count);
void browser_session_cookie_clear(BrowserSession *session);
BrowserCookieOverlay *browser_session_cookie_overlay_create(
    Budget *budget, const BrowserSession *source);
bool browser_cookie_overlay_header_context(
    const BrowserCookieOverlay *overlay,
    const TilefinchRequestContext *context,
    char *output, size_t output_capacity);
bool browser_cookie_overlay_set_http_context(
    BrowserCookieOverlay *overlay,
    const TilefinchRequestContext *context,
    const char *cookie);
void browser_cookie_overlay_destroy(BrowserCookieOverlay *overlay);
bool browser_session_cache_get(BrowserSession *session, const char *url,
                               const unsigned char **data, size_t *length);
const BrowserCacheEntry *browser_session_cache_lookup(
    BrowserSession *session, const char *url);
const char *browser_cache_entry_response_url(const BrowserCacheEntry *entry);
bool browser_session_cache_set_response_url(BrowserSession *session,
                                            const char *request_url,
                                            const char *response_url);
/* Atomically retains the final response URL and normalized response policy.
   Empty policy is valid and known.  Once the request entry has been found,
   invalid provenance or allocation failure clears both provenance fields
   while leaving its response body available to generic cache consumers. */
bool browser_session_cache_set_response_provenance(
    BrowserSession *session, const char *request_url, const char *final_url,
    const char *normalized_referrer_policy);
/* Resource-authorized variants are not visible to the generic URL lookup.
   Update final-response provenance through the same partition/principal key
   that earned the cached grant. */
bool browser_session_cache_set_resource_response_provenance(
    BrowserSession *session, const char *request_url,
    const TilefinchRequestContext *context, const char *final_url,
    const char *normalized_referrer_policy);
bool browser_session_cache_clear_response_provenance(
    BrowserSession *session, const char *request_url);
BrowserCacheStatus browser_session_cache_match_http(
    BrowserSession *session, const char *url, uint64_t now_ns,
    const BrowserCacheEntry **entry);
BrowserCacheStatus browser_session_cache_match_classic_script(
    BrowserSession *session, const char *url,
    const TilefinchRequestContext *context, uint64_t now_ns,
    const BrowserCacheEntry **entry);
BrowserCacheStatus browser_session_cache_match_resource(
    BrowserSession *session, const char *url,
    const TilefinchRequestContext *context, uint64_t now_ns,
    const BrowserCacheEntry **entry);
BrowserCacheStatus browser_session_cache_match_module(
    BrowserSession *session, const char *request_url,
    const char *initiator_origin, const char *top_level_url,
    bool initiator_opaque, TilefinchCredentialsMode credentials,
    uint64_t now_ns, const BrowserCacheEntry **entry);
bool browser_session_cache_put(BrowserSession *session, const char *url,
                               const unsigned char *data, size_t length);
bool browser_session_cache_put_response(BrowserSession *session,
                                        const char *url,
                                        const unsigned char *data,
                                        size_t length, const char *etag,
                                        const char *last_modified,
                                        const char *content_type);
bool browser_session_cache_put_http(BrowserSession *session,
                                    const char *url,
                                    const unsigned char *data,
                                    size_t length, const char *etag,
                                    const char *last_modified,
                                    const char *content_type,
                                    const char *cache_control,
                                    const char *vary, uint64_t now_ns);
BrowserSharedBody *browser_shared_body_take(Budget *budget,
                                            unsigned char *data,
                                            size_t length);
BrowserSharedBody *browser_shared_body_retain(BrowserSharedBody *body);
void browser_shared_body_release(BrowserSharedBody *body);
BrowserSharedBody *browser_session_classic_script_bytecode_acquire(
    BrowserSession *session, const char *request_url,
    const unsigned char *source, size_t source_length);
bool browser_session_classic_script_bytecode_may_fit(
    BrowserSession *session, const char *request_url,
    const unsigned char *source, size_t source_length,
    size_t minimum_bytecode_length);
bool browser_session_classic_script_bytecode_put(
    BrowserSession *session, const char *request_url,
    const unsigned char *source, size_t source_length,
    const unsigned char *bytecode, size_t bytecode_length);
void browser_session_classic_script_bytecode_invalidate(
    BrowserSession *session, const char *request_url,
    const unsigned char *source, size_t source_length);
/* Resolves the exact authorized response once and retains either requested
   RAM-only compiler artifact. A NULL output skips that artifact. */
void browser_session_stylesheet_artifacts_acquire(
    BrowserSession *session, const char *request_url,
    const TilefinchRequestContext *request_context,
    const unsigned char *source, size_t source_length,
    BrowserSharedBody **compiled_fragment,
    BrowserSharedBody **parsed_ir);
/* On success, takes ownership of fragment. On failure, the caller retains
   it. This avoids holding a second artifact-sized copy at first load. */
bool browser_session_stylesheet_fragment_put_take(
    BrowserSession *session, const char *request_url,
    const TilefinchRequestContext *request_context,
    const unsigned char *source, size_t source_length,
    unsigned char *fragment, size_t fragment_length);
/* On success, takes ownership of the IR buffer. */
bool browser_session_stylesheet_ir_put_take(
    BrowserSession *session, const char *request_url,
    const TilefinchRequestContext *request_context,
    const unsigned char *source, size_t source_length,
    unsigned char *ir, size_t ir_length);
typedef struct {
    BrowserSharedBody *pixels;
    int source_width;
    int source_height;
    int width;
    int height;
} BrowserDecodedImage;
/* Acquires a decoded target only from the exact partition-authorized image
   response supplied by source/source_length. The returned lease must be
   released with browser_shared_body_release(). */
bool browser_session_decoded_image_acquire(
    BrowserSession *session, const char *request_url,
    const TilefinchRequestContext *request_context,
    const unsigned char *source, size_t source_length,
    BrowserDecodedImage *decoded);
/* Retains one immutable RGBA surface under the response cache's existing
   byte and LRU bounds. The caller keeps its lease on every return path. */
bool browser_session_decoded_image_put(
    BrowserSession *session, const char *request_url,
    const TilefinchRequestContext *request_context,
    const unsigned char *source, size_t source_length,
    BrowserSharedBody *pixels, int source_width, int source_height,
    int width, int height);
bool browser_session_cache_put_http_shared(
    BrowserSession *session, const char *url, BrowserSharedBody *body,
    const char *etag, const char *last_modified, const char *content_type,
    const char *cache_control, const char *vary, uint64_t now_ns);
bool browser_session_cache_put_http_shared_classic_script(
    BrowserSession *session, const char *url, BrowserSharedBody *body,
    const char *etag, const char *last_modified, const char *content_type,
    const char *cache_control, const char *vary, uint64_t now_ns,
    const TilefinchRequestContext *context,
    const TilefinchResourceGrant *grant);
bool browser_session_cache_put_http_shared_resource(
    BrowserSession *session, const char *url, BrowserSharedBody *body,
    const char *etag, const char *last_modified, const char *content_type,
    const char *cache_control, const char *vary, uint64_t now_ns,
    const TilefinchRequestContext *context,
    const TilefinchResourceGrant *grant);
bool browser_session_cache_put_http_module(
    BrowserSession *session, const char *request_url,
    const unsigned char *data, size_t length, const char *etag,
    const char *last_modified, const char *content_type,
    const char *cache_control, const char *vary, uint64_t now_ns,
    const BrowserModuleCacheProvenance *provenance);
bool browser_session_cache_put_http_shared_module(
    BrowserSession *session, const char *request_url, BrowserSharedBody *body,
    const char *etag, const char *last_modified, const char *content_type,
    const char *cache_control, const char *vary, uint64_t now_ns,
    const BrowserModuleCacheProvenance *provenance);
bool browser_session_cache_revalidate(BrowserSession *session,
                                      const char *url,
                                      const char *cache_control,
                                      const char *vary, uint64_t now_ns);
bool browser_session_cache_revalidate_classic_script(
    BrowserSession *session, const char *url, const char *cache_control,
    const char *vary, uint64_t now_ns,
    const TilefinchRequestContext *context,
    const TilefinchResourceGrant *grant);
bool browser_session_cache_revalidate_resource(
    BrowserSession *session, const char *url, const char *cache_control,
    const char *vary, uint64_t now_ns,
    const TilefinchRequestContext *context,
    const TilefinchResourceGrant *grant);
uint64_t browser_session_stylesheet_cache_signature(
    const BrowserSession *session, uint64_t now_ns);
bool browser_session_cache_revalidate_module(
    BrowserSession *session, const char *request_url,
    const char *cache_control, const char *vary, uint64_t now_ns,
    const BrowserModuleCacheProvenance *provenance);
/* Evicts least-recently-used response entries until at least target_bytes of
   the shared page Budget has actually become available, or the optional cache
   is empty. Shared bodies still leased by another subsystem may therefore be
   evicted without contributing to the returned physical-byte count. */
size_t browser_session_cache_reclaim(BrowserSession *session,
                                     size_t target_bytes);
/* Changes the live cache ceiling. A smaller ceiling evicts least-recently
   used entries before returning; zero and values beyond the shared Budget
   ceiling are rejected without changing the session. */
bool browser_session_cache_set_maximum_bytes(BrowserSession *session,
                                             size_t maximum_bytes);
void browser_session_cache_clear(BrowserSession *session);
void browser_session_destroy(BrowserSession *session);

#endif
