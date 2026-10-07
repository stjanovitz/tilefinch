#ifndef TILEFINCH_SESSION_H
#define TILEFINCH_SESSION_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "tilefinch/budget.h"
#include "tilefinch/captive_portal.h"
#include "tilefinch/request_context.h"
#include "tilefinch/site_storage.h"

/* Site storage (localStorage, sessionStorage, OPFS) is kept per origin.
   Every site starts in RAM with a small allowance that grows in the
   background while the device has memory to spare. A site that outgrows
   RAM can be moved to the Memory Stick when the user agrees, for this
   session or always. Byte counts are key plus value (or path plus data). */
#define BROWSER_SITE_STORAGE_SITES 48u
#define BROWSER_SITE_STORAGE_ITEMS 1024u
#define BROWSER_SITE_STORAGE_SITE_ITEMS 256u
#define BROWSER_SITE_STORAGE_INITIAL_BYTES (32u * 1024u)
#define BROWSER_SITE_STORAGE_MEMORY_SITE_BYTES (512u * 1024u)
#define BROWSER_SITE_STORAGE_MEMORY_TOTAL_BYTES (1024u * 1024u)
/* RAM growth is refused unless this much budget stays free afterwards. */
#define BROWSER_SITE_STORAGE_GROWTH_RESERVE_BYTES (3u * 1024u * 1024u)
#define BROWSER_SITE_STORAGE_STICK_SITE_BYTES (4u * 1024u * 1024u)
#define BROWSER_SITE_STORAGE_STICK_VALUE_BYTES (1024u * 1024u)
#define BROWSER_SITE_STORAGE_DIRECTORY_LIMIT 192u
#define BROWSER_OPFS_PATH_LIMIT 256
#define BROWSER_OPFS_FILE_BYTE_LIMIT (64u * 1024u)
#define BROWSER_OPFS_STICK_FILE_BYTE_LIMIT (512u * 1024u)
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
/* Generated adapter documents are useful for immediate Back/Forward and a
   repeated search, but raw provider responses are far too large to retain on
   PSP. Keep two short-lived, exact-keyed scriptless documents instead. */
#define BROWSER_SITE_ADAPTER_DOCUMENT_CACHE_ENTRIES 2
#define BROWSER_SITE_ADAPTER_DOCUMENT_CACHE_ADAPTER_LIMIT 32
#define BROWSER_SITE_ADAPTER_DOCUMENT_CACHE_KEY_LIMIT 2048
#define BROWSER_SITE_ADAPTER_DOCUMENT_CACHE_ENTRY_LIMIT (96u * 1024u)

struct ContentBlocker;
struct FetchResponseSecurityMetadata;
struct BrowserCaptivePortalStash;
struct BrowserSiteStore;

typedef enum {
    BROWSER_COOKIE_SAME_SITE_DEFAULT = 0,
    BROWSER_COOKIE_SAME_SITE_LAX,
    BROWSER_COOKIE_SAME_SITE_STRICT,
    BROWSER_COOKIE_SAME_SITE_NONE
} BrowserCookieSameSite;

/* A site ran out of RAM for storage and may be offered the Memory Stick.
   Sizes are what the prompt shows before anything is written. */
typedef struct {
    char origin[BROWSER_ORIGIN_LIMIT];
    size_t current_bytes;
    size_t needed_bytes;
    size_t stick_limit_bytes;
} BrowserSiteStorageRequest;

typedef struct {
    char origin[BROWSER_ORIGIN_LIMIT];
    BrowserSiteStorageTier tier;
    BrowserSiteStoragePolicy policy;
    size_t bytes;             /* live data this session (0 until loaded) */
    size_t limit_bytes;       /* RAM allowance, or the Memory Stick limit */
    size_t item_count;
    size_t file_bytes;        /* on the Memory Stick, including dead records */
    bool loaded;
} BrowserSiteStorageInfo;

typedef enum {
    BROWSER_OPFS_NONE = 0,
    BROWSER_OPFS_FILE = 1,
    BROWSER_OPFS_DIRECTORY = 2
} BrowserOpfsKind;

typedef enum {
    BROWSER_OPFS_OK = 0,
    BROWSER_OPFS_UNAVAILABLE,
    BROWSER_OPFS_NOT_FOUND,
    BROWSER_OPFS_TYPE_MISMATCH,
    BROWSER_OPFS_ALREADY_EXISTS,
    BROWSER_OPFS_NOT_EMPTY,
    BROWSER_OPFS_QUOTA_EXCEEDED,
    BROWSER_OPFS_INVALID_PATH,
    BROWSER_OPFS_STALE
} BrowserOpfsResult;

typedef struct {
    BrowserOpfsKind kind;
    const unsigned char *data;
    size_t data_length;
    int64_t last_modified_ms;
    uint64_t generation;
} BrowserOpfsView;

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

#define BROWSER_OFFLINE_CACHE_ENTRY_LIMIT 32u

/* QuickJS bytecode is an internal compiler artifact, not a web-visible or
   Tilefinch-release ABI. Bump this only when the QuickJS serializer, compiler
   configuration, or local compiler patches change incompatibly. Offline apps
   keep their source response and simply recompile when this does not match. */
#if defined(PSP_BROWSER_BELLARD_QUICKJS)
#define TILEFINCH_QUICKJS_BYTECODE_ABI UINT32_C(0x514a5302)
#else
#define TILEFINCH_QUICKJS_BYTECODE_ABI UINT32_C(0x514a4e01)
#endif

typedef enum {
    BROWSER_OFFLINE_CACHE_GENERIC = 0,
    BROWSER_OFFLINE_CACHE_RESOURCE,
    BROWSER_OFFLINE_CACHE_MODULE
} BrowserOfflineCacheKind;

/* Borrowed immutable view used only while an explicit offline-app save is
   serializing the current same-origin working set. Executable/resource
   authority is carried with the bytes instead of being reconstructed from a
   URL-only disk cache. */
typedef struct {
    const char *url;
    const unsigned char *data;
    size_t length;
    const unsigned char *classic_script_bytecode;
    size_t classic_script_bytecode_length;
    const char *content_type;
    const char *response_url;
    const char *response_referrer_policy;
    BrowserOfflineCacheKind kind;
    TilefinchResourceGrant resource_grant;
    TilefinchCredentialsMode module_credentials;
    bool module_cors_validated;
    bool module_redirect_origin_tainted;
    bool module_javascript_mime_validated;
} BrowserOfflineCacheView;

typedef struct {
    char key[BROWSER_SITE_ADAPTER_STATE_KEY_LIMIT];
    unsigned char data[BROWSER_SITE_ADAPTER_STATE_DATA_LIMIT];
    size_t length;
    uint64_t stored_at_ns;
    uint64_t cookie_fingerprint;
    bool valid;
} BrowserSiteAdapterState;

typedef struct {
    char adapter[BROWSER_SITE_ADAPTER_DOCUMENT_CACHE_ADAPTER_LIMIT];
    char *key;
    unsigned char *data;
    size_t length;
    size_t source_bytes;
    size_t result_count;
    uint64_t stored_at_ns;
    uint64_t cookie_fingerprint;
    size_t last_used;
    uint32_t variant;
    long status_code;
    char server[64];
    char cf_mitigated[32];
    bool valid;
} BrowserSiteAdapterDocumentCacheEntry;

typedef struct {
    const unsigned char *data;
    size_t length;
    size_t source_bytes;
    size_t result_count;
    long status_code;
    const char *server;
    const char *cf_mitigated;
} BrowserSiteAdapterDocumentCacheView;

/* Accept-CH is an origin-scoped user-agent preference retained for the
   browsing session.  Keep only a tiny fixed LRU: this is transport identity,
   not page data, and must never grow with the number of visited origins. */
#define BROWSER_CLIENT_HINT_ORIGIN_LIMIT 4u
#define BROWSER_CLIENT_HINT_TOKEN_LIMIT 1024u
typedef struct {
    char origin[BROWSER_ORIGIN_LIMIT];
    char tokens[BROWSER_CLIENT_HINT_TOKEN_LIMIT];
    size_t stamp;
    bool valid;
} BrowserClientHintEntry;

/*
 * In-memory QuickJS bytecode for page scripts, in three tables of the same
 * shape: one for ES modules, one for classic external scripts, and one for
 * lazy webpack bundle records (BROWSER_SCRIPT_BYTECODE_LAZY_BUNDLE, below).
 *
 * A compiled record is keyed by everything that shapes it: the name QuickJS
 * bakes into the bytecode (a module's module-map name, a classic script's
 * file name), the request URL (classic) or response URL (module) it came
 * from, the network partition (top-level site) that fetched it, the segment
 * ordinal (classic ResourceLoader segments; 0 for a whole script), the
 * compile options, and the exact source (length plus SHA-256; the source is
 * not retained). A hit is therefore only ever the bytecode that compiling
 * those same bytes would produce, after every fetch, CSP, SRI, CORS and MIME
 * check has already admitted the bytes.
 *
 * Entries are compiler artifacts kept in RAM (the opt-in persistent tier,
 * browser_session_script_disk_configure, copies them to disk), charged to the
 * page Budget as SESSION memory under each table's own byte ceiling, and
 * dropped with the HTTP cache on clear or optional-memory reclaim, or by
 * the Budget reclaim hook before the page Budget refuses an allocation.
 * They outlive the HTTP responses they were compiled from: the source
 * digest, not the response entry, decides validity. Each table is
 * allocated on its first store, so a session that never caches a script
 * pays one pointer per table.
 */
#define BROWSER_SCRIPT_BYTECODE_ENTRIES 128u

typedef enum {
    BROWSER_SCRIPT_BYTECODE_MODULE = 0,
    BROWSER_SCRIPT_BYTECODE_CLASSIC,
    /* Not bytecode: lazy webpack bundle records (see
       browser_session_lazy_bundle_record_queue). The same table shape,
       keyed the same way, in a table of its own. */
    BROWSER_SCRIPT_BYTECODE_LAZY_BUNDLE,
    BROWSER_SCRIPT_BYTECODE_KINDS
} BrowserScriptBytecodeKind;

typedef struct {
    char *module_name;
    char *response_url;
    char *partition_key;
    BrowserSharedBody *bytecode;
    size_t source_length;
    /* Bytecode plus key strings, as charged against the ceiling. */
    size_t charged_bytes;
    size_t stamp;
    /* The document realm that last stored or restored this entry. Entries
       the current realm has used are not evicted to admit another script
       of the same load: that would trade a certain hit for a later miss. */
    uint32_t generation;
    uint32_t compile_flags;
    uint32_t ordinal;
    /* Unique per stored record (the table's serial clock): lets the
       persistent tier's writer tell a slot it is writing from its reuse. */
    uint32_t serial;
    /* BrowserScriptDiskState: whether the persistent tier still has to
       write this record. */
    uint8_t disk_state;
    uint8_t source_digest[32];
} BrowserScriptBytecodeEntry;

typedef enum {
    /* Not for the persistent tier (it is off, read-only, or the record
       could not be written). */
    BROWSER_SCRIPT_DISK_NONE = 0,
    /* Stored while the tier writes: idle work will write its pack. */
    BROWSER_SCRIPT_DISK_DIRTY,
    /* In a pack on disk (written or read this session). */
    BROWSER_SCRIPT_DISK_CLEAN
} BrowserScriptDiskState;

typedef struct BrowserScriptBytecodeTable {
    BrowserScriptBytecodeEntry entries[BROWSER_SCRIPT_BYTECODE_ENTRIES];
    size_t bytes;
    size_t clock;
    uint32_t serial_clock;
} BrowserScriptBytecodeTable;

/* Lookup key. The digest is computed at most once per key, and only when a
   stored entry matches every cheaper field; a key whose digest is already
   known (digest_ready) needs no source. For a classic script,
   module_name is the compile name and response_url the request URL (the
   HTTP cache key); `ordinal` is 1 + the ResourceLoader segment index, or 0.
   One record exists per (module_name, response_url, partition_key,
   ordinal): a store with other bytes or flags replaces it. */
typedef struct {
    const char *module_name;
    const char *response_url;
    const char *partition_key;
    const unsigned char *source;
    size_t source_length;
    uint32_t compile_flags;
    uint32_t ordinal;
    uint8_t source_digest[32];
    bool digest_ready;
} BrowserScriptBytecodeKey;

typedef struct BrowserSession {
    Budget *budget;
    /* Non-owning engine-lifetime request policy. */
    struct ContentBlocker *content_blocker;
    /* localStorage, sessionStorage and OPFS for every origin. Allocated on
       the first write (or when the Memory Stick tier is configured), so
       ordinary pages pay one pointer. */
    struct BrowserSiteStore *site_store;
    /* Outlives the store, so an OPFS writer from before a clear can never
       match a recreated file's generation. */
    uint64_t site_storage_generation;
    BrowserCookieEntry cookies[BROWSER_COOKIE_ENTRIES];
    BrowserCacheEntry cache[BROWSER_CACHE_ENTRIES];
    BrowserSiteAdapterState site_adapter_state;
    BrowserSiteAdapterDocumentCacheEntry site_adapter_document_cache[
        BROWSER_SITE_ADAPTER_DOCUMENT_CACHE_ENTRIES];
    BrowserClientHintEntry client_hints[BROWSER_CLIENT_HINT_ORIGIN_LIMIT];
    size_t cookie_bytes;
    size_t cookie_long_path_bytes;
    size_t cache_bytes;
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
    size_t site_adapter_document_clock;
    size_t client_hint_clock;
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
    /* A captive sign-in temporarily moves cookies/storage into an owned
       stash and uses the now-empty tables as an ephemeral partition. Cache
       access is disabled while this pointer is non-NULL. */
    struct BrowserCaptivePortalStash *captive_portal_stash;
    BudgetReservation accounting_reservation;
    size_t accounting_bytes;
    /* NULL unless an installed app restored classic scripts as bytecode
       only (browser_session_offline_deferred_*). */
    struct BrowserOfflineDeferredScripts *offline_deferred;
    /* On-demand source reads that failed (pack missing or corrupt). */
    size_t offline_deferred_failures;
    /* NULL until the first module bytecode store. */
    BrowserScriptBytecodeTable *module_bytecode;
    size_t maximum_module_bytecode_bytes;
    uint32_t module_bytecode_generation;
    size_t module_bytecode_evictions;
    /* Classic external scripts: the same table shape and rules, its own
       ceiling (set by browser_session_init to
       BROWSER_CLASSIC_BYTECODE_CACHE_BYTES, by the engine from its
       config). Realms share module_bytecode_generation. */
    BrowserScriptBytecodeTable *classic_bytecode;
    size_t maximum_classic_bytecode_bytes;
    size_t classic_bytecode_evictions;
    /* Advances whenever compiled scripts are cleared (the cache clear,
       teardown, or one site's data): a realm's deferred store or a bundle
       record queued before then is dropped, not stored. */
    uint32_t script_bytecode_epoch;
    /* The optional persistent compiled-script tier (NULL unless a
       directory is configured; see browser_session_script_disk_configure). */
    struct BrowserScriptDisk *script_disk;
    /* Lazy webpack bundle records (BROWSER_SCRIPT_BYTECODE_LAZY_BUNDLE):
       a table of their own with its own small ceiling, and the records
       still being digested at idle (NULL when none). */
    BrowserScriptBytecodeTable *lazy_bundle_records;
    size_t maximum_lazy_bundle_record_bytes;
    size_t lazy_bundle_record_evictions;
    struct BrowserLazyBundlePending *lazy_bundle_pending;
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
/* Retains normalized, supported high-entropy Accept-CH tokens for a
   trustworthy response origin. Captive-portal partitions neither consume nor
   modify the ordinary browsing cache. */
bool browser_session_client_hints_put(
    BrowserSession *session, const char *url, const char *tokens);
bool browser_session_client_hints_get(
    BrowserSession *session, const char *url, char *tokens,
    size_t tokens_capacity, char *origin, size_t origin_capacity);
/* Read-only view used while constructing a request envelope.  The returned
   pointers remain owned by the session and are valid until the next client
   hint policy update; transports snapshot them before enqueue returns. */
bool browser_session_client_hints_peek(
    const BrowserSession *session, const char *url,
    const char **tokens, const char **origin);
bool browser_session_set_mixed_content_site_allowed(
    BrowserSession *session, const char *url, bool allowed);
bool browser_session_mixed_content_site_allowed(
    const BrowserSession *session, const char *url);
bool browser_session_set_third_party_cookie_site_allowed(
    BrowserSession *session, const char *url, bool allowed);
/* Begins and ends a temporary captive-portal partition. The normal cookie and
   storage tables are restored byte-for-byte; portal data is destroyed. */
bool browser_session_captive_portal_begin(
    BrowserSession *session, const char *portal_url);
void browser_session_captive_portal_end(BrowserSession *session);
bool browser_session_captive_portal_active(const BrowserSession *session);
/* The detected entry origin and at most three redirect/user-navigation
   origins form the only HTTP/PNA exception available to the portal context. */
bool browser_session_captive_portal_url_allowed(
    const BrowserSession *session, const char *url);
bool browser_session_captive_portal_authorize_navigation(
    BrowserSession *session, const char *from_url, const char *target_url);
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
bool browser_session_site_adapter_document_cache_put(
    BrowserSession *session, const char *adapter, const char *key,
    uint32_t variant, const TilefinchRequestContext *authority_context,
    const void *data, size_t data_length,
    size_t source_bytes, size_t result_count, long status_code,
    const char *server, const char *cf_mitigated, uint64_t now_ns);
bool browser_session_site_adapter_document_cache_get(
    BrowserSession *session, const char *adapter, const char *key,
    uint32_t variant, const TilefinchRequestContext *authority_context,
    uint64_t now_ns, uint64_t maximum_age_ns,
    BrowserSiteAdapterDocumentCacheView *view);
void browser_session_site_adapter_document_cache_clear(
    BrowserSession *session);
void browser_session_site_adapter_document_cache_remove(
    BrowserSession *session, const char *adapter, const char *key);
bool browser_session_storage_get(const BrowserSession *session,
                                 const char *url, bool local,
                                 const char *key, const char **value,
                                 size_t *value_length);
size_t browser_session_storage_length(
    const BrowserSession *session, const char *url, bool local);
typedef struct {
    size_t cookie_count;
    size_t local_storage_count;
    size_t session_storage_count;
    size_t cookie_bytes;
    size_t storage_bytes;
    size_t opfs_entry_count;
    size_t opfs_bytes;
} BrowserSiteDataUsage;
/* Summarizes and clears only the origin/domain represented by url.  These
   operations remain available when ordinary site-data admission is disabled,
   because a user must always be able to inspect and remove retained data. */
bool browser_session_site_data_usage(
    const BrowserSession *session, const char *url,
    BrowserSiteDataUsage *usage);
bool browser_session_clear_site_data(
    BrowserSession *session, const char *url);
bool browser_session_storage_key(
    const BrowserSession *session, const char *url, bool local,
    size_t index, const char **key);
bool browser_session_storage_set(BrowserSession *session, const char *url,
                                 bool local, const char *key,
                                 const char *value, size_t value_length);
/* Removal from a site kept on the Memory Stick is logged there; when that
   write fails the entry stays (it would reappear at the next load) and
   these return false. */
bool browser_session_storage_remove(BrowserSession *session, const char *url,
                                    bool local, const char *key);
bool browser_session_storage_clear(BrowserSession *session, const char *url,
                                   bool local);
/* Clears every entry in one storage namespace, for every site, in RAM and
   on the Memory Stick. Sites keep their tier and standing choice. */
bool browser_session_storage_clear_all(BrowserSession *session, bool local);
/* A bounded origin-private file system in the site's storage. Paths are
   canonical, absolute OPFS paths (root is "/"); the native layer derives
   the origin from url and never accepts page-supplied authority. stat
   returns data only for a site kept in RAM; read always returns it, from a
   buffer valid until the next storage call on this session. */
BrowserOpfsResult browser_session_opfs_stat(
    const BrowserSession *session, const char *url, const char *path,
    BrowserOpfsView *view);
BrowserOpfsResult browser_session_opfs_read(
    const BrowserSession *session, const char *url, const char *path,
    BrowserOpfsView *view);
BrowserOpfsResult browser_session_opfs_create(
    BrowserSession *session, const char *url, const char *path,
    BrowserOpfsKind kind);
BrowserOpfsResult browser_session_opfs_write(
    BrowserSession *session, const char *url, const char *path,
    const unsigned char *data, size_t data_length);
BrowserOpfsResult browser_session_opfs_remove(
    BrowserSession *session, const char *url, const char *path,
    bool recursive);
BrowserOpfsResult browser_session_opfs_child(
    const BrowserSession *session, const char *url, const char *parent,
    size_t index, const char **name, BrowserOpfsView *view);
bool browser_session_opfs_clear_all(BrowserSession *session);

/* The Memory Stick tier. Until configured with an existing directory, all
   site storage stays in RAM and no offers are made. */
bool browser_session_site_storage_configure(BrowserSession *session,
                                            const char *directory);
/* The global "offer the Memory Stick" setting (on by default). */
void browser_session_site_storage_set_offers(BrowserSession *session,
                                             bool enabled);
/* Applies a site's standing choice. STICK moves any RAM data to the
   Memory Stick now (a site registered at boot loads on first use). ASK and
   MEMORY_ONLY turn an always-kept site into this-session-only: its data
   stays readable until exit and is then deleted. */
bool browser_session_site_storage_set_policy(
    BrowserSession *session, const char *url,
    BrowserSiteStoragePolicy policy);
/* The one pending offer, if a site ran out of RAM. Taking it leaves the
   site waiting for grant or decline; it is not offered again meanwhile. */
bool browser_session_site_storage_take_request(
    BrowserSession *session, BrowserSiteStorageRequest *request);
/* The user's answer. grant(always=false) moves the site's data to the
   Memory Stick for this session; always=true is set_policy(STICK). The
   write that ran out already failed; later writes can succeed. */
bool browser_session_site_storage_grant(BrowserSession *session,
                                        const char *url, bool always);
void browser_session_site_storage_decline(BrowserSession *session,
                                          const char *url);
/* Deletes the site's storage everywhere, removes its Memory Stick file and
   returns it to RAM with policy ASK. */
bool browser_session_site_storage_forget(BrowserSession *session,
                                         const char *url);
/* Cheap: no Memory Stick access. */
BrowserSiteStorageTier browser_session_site_storage_tier(
    const BrowserSession *session, const char *url);
/* Sites with storage or a standing choice, for the Storage menu. */
size_t browser_session_site_storage_count(const BrowserSession *session);
bool browser_session_site_storage_info(const BrowserSession *session,
                                       size_t index,
                                       BrowserSiteStorageInfo *info);
bool browser_session_site_storage_info_for(const BrowserSession *session,
                                           const char *url,
                                           BrowserSiteStorageInfo *info);
/* Boot cleanup: this-session-only files left by a run that did not exit
   cleanly. */
void browser_site_storage_remove_session_files(const char *directory);
bool browser_session_cookie_get(const BrowserSession *session,
                                const char *url, char *output,
                                size_t output_capacity);
bool browser_session_cookie_header(const BrowserSession *session,
                                   const char *url, char *output,
                                   size_t output_capacity);
bool browser_session_cookie_header_context(
    const BrowserSession *session, const TilefinchRequestContext *context,
    char *output, size_t output_capacity);
/* Hashes exactly the Cookie header authority visible to this request context,
   without materializing a potentially multi-kilobyte header. */
bool browser_session_cookie_header_fingerprint_context(
    const BrowserSession *session, const TilefinchRequestContext *context,
    uint64_t *fingerprint);
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
/* Classic-script bytecode attached to an HTTP response entry. Only an
   installed offline app's restore attaches it (the app's working set is
   reserved in the response cache for its launch); page scripts use the
   session's classic bytecode table (BrowserScriptBytecodeTable) instead. */
BrowserSharedBody *browser_session_classic_script_bytecode_acquire(
    BrowserSession *session, const char *request_url,
    const unsigned char *source, size_t source_length);
bool browser_session_classic_script_bytecode_put(
    BrowserSession *session, const char *request_url,
    const unsigned char *source, size_t source_length,
    const unsigned char *bytecode, size_t bytecode_length);
void browser_session_classic_script_bytecode_invalidate(
    BrowserSession *session, const char *request_url,
    const unsigned char *source, size_t source_length);
/* Whether a Cache-Control value carries the no-store directive. */
bool browser_session_cache_control_no_store(const char *cache_control);

/* Classic-script bytecode ceilings: browser_session_init applies the
   first, and the engine's profiles one or the other
   (BrowserConfig.classic_bytecode_cache_limit). Bytes, not entries, bind:
   on the second census at 1.5 MiB a ticked revisit hit 327 of 492 classic
   lookups with 101 + 172 stores skipped for a full table; at 3 MiB it hits
   457 of 495 with none skipped (4 MiB adds nothing there) and the table
   never held more than 83 entries. The table is optional memory: the
   Budget reclaim hook and the script pressure checks evict it before the
   page needs the room. See docs/STORAGE.md and
   docs/engineering/MEMORY_EXPERIMENTS.md. */
#define BROWSER_CLASSIC_BYTECODE_CACHE_BYTES (3072u * 1024u)
#define BROWSER_CLASSIC_BYTECODE_CACHE_STRICT_BYTES (1536u * 1024u)

/*
 * Lazy webpack bundle records (src/session_lazy_bundle.c).
 *
 * A lazily split webpack bundle (src/js_lazy_webpack.c) costs a whole-
 * bundle pass on every load before any factory runs: the planner's lexer
 * pass that finds the factories. A record keeps its outcome, the factory
 * table, for one exact byte sequence. It vouches for no factory's syntax:
 * each factory is compiled, and checked, when it first runs. The engine
 * (js_lazy_webpack.c) owns the record's format; the session stores it as
 * an opaque payload in the LAZY_BUNDLE table, keyed like a classic script
 * (a fixed record name, the request URL, the top-level site, and the length
 * and SHA-256 of the exact bytes), so the persistent compiled-script tier
 * keeps records across restarts when it is on, and the cache clear, site
 * clear, optional-memory reclaim, Budget reclaim hook and teardown drop
 * them with the bytecode.
 *
 * A lookup digests the bytes only when a record for the same site, URL and
 * length exists, so a first visit never hashes. A store needs the digest,
 * so it is queued here with a reference to the response body and digested
 * at idle, BROWSER_LAZY_BUNDLE_HASH_SLICE bytes per call, off the load's
 * critical path. The clears above drop queued records. The reclaims go to
 * them first: a queued record whose reference is the last one on its body
 * has its digest finished there and then (CPU only) and releases the body,
 * so memory pressure costs the hash it deferred, never the record.
 */
#define BROWSER_LAZY_BUNDLE_RECORD_DEFAULT_BYTES (96u * 1024u)
#define BROWSER_LAZY_BUNDLE_PENDING_LIMIT 16u
/* Body bytes all queued records may reference together. */
#define BROWSER_LAZY_BUNDLE_PENDING_BYTES (8u * 1024u * 1024u)
#define BROWSER_LAZY_BUNDLE_HASH_SLICE (64u * 1024u)

/* Queues a record for `body` (exactly the key's source bytes): retains the
   body and copies the key strings and the payload. With the key's digest
   already known (digest_ready) the record is stored at once. False, with
   nothing retained, when the session cannot keep it (table off, captive
   sign-in, queue full, allocation refused). */
bool browser_session_lazy_bundle_record_queue(
    BrowserSession *session, const BrowserScriptBytecodeKey *key,
    BrowserSharedBody *body, uint32_t generation,
    const unsigned char *record, size_t record_length);
/* One slice of idle work: digests up to BROWSER_LAZY_BUNDLE_HASH_SLICE
   bytes of the oldest queued record, storing it when complete. True if it
   did work. */
bool browser_session_lazy_bundle_record_maintenance(BrowserSession *session);
size_t browser_session_lazy_bundle_pending_count(
    const BrowserSession *session);
/* Drops queued records (of one top-level site, or all with NULL). */
void browser_session_lazy_bundle_pending_clear(BrowserSession *session,
                                               const char *partition);
/* Releases the bodies of queued records that hold their last reference,
   oldest first, until `target_bytes` are released, finishing each one's
   digest first so the record itself is kept (stored at the next idle
   turn). Returns the body bytes released. Frees only, allocates nothing:
   the Budget reclaim hook calls it. */
size_t browser_session_lazy_bundle_pending_reclaim(BrowserSession *session,
                                                   size_t target_bytes);

/*
 * Script bytecode tables (BrowserScriptBytecodeTable), by kind. The
 * browser_session_module_bytecode_* functions below are the MODULE table's.
 */
/* Sets a table's ceiling; zero disables it. Lowering it evicts least-
   recently-used entries until the table fits. */
void browser_session_script_bytecode_set_limit(
    BrowserSession *session, BrowserScriptBytecodeKind kind,
    size_t maximum_bytes);
/* A retained reference to the bytecode for exactly this key, or NULL. A hit
   marks the entry as used by `generation`. */
BrowserSharedBody *browser_session_script_bytecode_acquire(
    BrowserSession *session, BrowserScriptBytecodeKind kind,
    BrowserScriptBytecodeKey *key, uint32_t generation);
/* Whether a store of `bytecode_length` bytes could be admitted now without
   evicting an entry `generation` has used: an admission hint. */
bool browser_session_script_bytecode_may_fit(
    const BrowserSession *session, BrowserScriptBytecodeKind kind,
    const BrowserScriptBytecodeKey *key, size_t bytecode_length,
    uint32_t generation);
/* Copies the bytecode in, replacing the entry for the same record. Returns
   false (and changes nothing) when it does not fit or an allocation is
   refused. */
bool browser_session_script_bytecode_put(
    BrowserSession *session, BrowserScriptBytecodeKind kind,
    BrowserScriptBytecodeKey *key, uint32_t generation,
    const unsigned char *bytecode, size_t bytecode_length);
/* Drops the entry for this key's record (after a failed restore). */
void browser_session_script_bytecode_invalidate(
    BrowserSession *session, BrowserScriptBytecodeKind kind,
    BrowserScriptBytecodeKey *key);
/* Releases compiled-script memory until `target_bytes` are released or
   nothing is left: the bundle-record queue, then least-recently-used
   classic and module entries (from the larger table first), then bundle
   records. Returns the bytes released. Frees only entries (never a table)
   and allocates nothing, so it is safe as the engine's Budget reclaim hook
   while an allocation is in progress anywhere, including inside a table. */
size_t browser_session_script_bytecode_reclaim(BrowserSession *session,
                                               size_t target_bytes);
size_t browser_session_script_bytecode_bytes(
    const BrowserSession *session, BrowserScriptBytecodeKind kind);
size_t browser_session_script_bytecode_entries(
    const BrowserSession *session, BrowserScriptBytecodeKind kind);
#ifndef TILEFINCH_NO_TRACE
/* Site-census diagnosis: "hit", "miss-no-record", "miss-changed-source",
   "miss-flags" or "ineligible-*" for this key, without touching LRU. */
const char *browser_session_script_bytecode_diagnose(
    BrowserSession *session, BrowserScriptBytecodeKind kind,
    BrowserScriptBytecodeKey *key);
#endif

/* Sets the module bytecode ceiling; zero disables the cache. Lowering it
   evicts least-recently-used entries until the cache fits. */
void browser_session_module_bytecode_set_limit(BrowserSession *session,
                                               size_t maximum_bytes);
/* A fresh nonzero generation for one document realm. */
uint32_t browser_session_module_bytecode_generation(BrowserSession *session);
/* Whether a store of `bytecode_length` bytes could be admitted now without
   evicting an entry `generation` has used. An admission hint for skipping
   serialization, not a reservation. */
bool browser_session_module_bytecode_may_fit(
    const BrowserSession *session, const BrowserScriptBytecodeKey *key,
    size_t bytecode_length, uint32_t generation);
/* Copies the bytecode into the cache, replacing any entry for the same
   module name, response URL and partition. Returns false (and changes
   nothing) when it does not fit the ceiling or an allocation is refused. */
bool browser_session_module_bytecode_put(
    BrowserSession *session, BrowserScriptBytecodeKey *key,
    uint32_t generation, const unsigned char *bytecode,
    size_t bytecode_length);
/*
 * The persistent compiled-script tier (off by default): QuickJS bytecode of
 * page scripts, classic and module, kept across browser restarts in one
 * directory (src/session_script_disk.c). The PSP application turns it on
 * with Settings > Device & storage > Site data & storage > Keep compiled
 * scripts (data/script-cache); boot.cfg's module_cache_dir names another
 * directory for development, and the lab takes --script-cache-dir.
 *
 * - One pack file per record group (table kind, compile name, URL and
 *   top-level site): every RAM record of that group, each with its ordinal,
 *   compile flags, source length and source SHA-256, and a CRC-32 of the
 *   whole file. A file is verified completely before any of it is used, so
 *   a truncated, corrupt or foreign file is a miss, never bytecode handed
 *   to the engine; one that fails (or whose bytecode the engine cannot
 *   restore) is removed when the tier writes.
 * - File names and the group hash carry this engine build's fingerprint,
 *   pointer width and TILEFINCH_QUICKJS_BYTECODE_ABI: another build never
 *   reads a pack, and the sweep removes it.
 * - An index file is read once, on first use; a key it does not name costs
 *   no card access. A RAM miss reads the named pack whole (at most once per
 *   page load) and copies its records into the RAM table, where the lookup
 *   that follows finds them. Reads come after the same fetch, CSP, SRI,
 *   CORS and MIME admission as a RAM hit.
 * - Writes are idle work only (browser_session_script_disk_maintenance):
 *   a pack is written from the RAM table's records stored while the tier
 *   writes, at most BROWSER_SCRIPT_DISK_WRITE_SLICE bytes per call, to a
 *   temporary name renamed into place when complete. Least recently used
 *   packs are removed first to stay under the size ceiling (default
 *   BROWSER_SCRIPT_DISK_DEFAULT_BYTES) and the file-count ceiling, one per
 *   call. The first maintenance calls of a writing session sweep the
 *   directory, removing other builds' packs, stray temporary files, the
 *   retired per-module tier's files and packs the index does not name.
 * - Nothing is written during a captive sign-in or while site data is not
 *   allowed, and no-store responses, data:/blob: scripts and opaque-origin
 *   realms never reach the RAM tables the tier writes from. Clearing the
 *   cache empties the directory (a read-only tier stops reading instead);
 *   a site's data clear removes its packs.
 * - A host build refuses a PSP device path (anything before a ':'), so no
 *   test or lab run can reach the Memory Stick.
 */
#define BROWSER_SCRIPT_DISK_DEFAULT_BYTES (8u * 1024u * 1024u)
#define BROWSER_SCRIPT_DISK_FILE_LIMIT (2u * 1024u * 1024u)
#define BROWSER_SCRIPT_DISK_FILE_COUNT_LIMIT 256u
#define BROWSER_SCRIPT_DISK_WRITE_SLICE (16u * 1024u)
#define BROWSER_SCRIPT_DISK_SCAN_SLICE 8u
#define BROWSER_SCRIPT_DISK_SCAN_LIMIT 4096u
#define BROWSER_SCRIPT_DISK_DIRECTORY_LIMIT 160u

typedef struct BrowserScriptDisk BrowserScriptDisk;

typedef struct {
    size_t files;
    uint64_t bytes;
    /* Index reads and writes; pack reads (whole files) and their bytes;
       records copied into RAM; lookups the index did not name. */
    size_t index_reads;
    size_t index_writes;
    size_t index_misses;
    size_t reads;
    uint64_t read_bytes;
    size_t promoted;
    /* Packs refused (verification or restore) and files removed. */
    size_t rejects;
    size_t removed;
    size_t evictions;
    /* Packs written, all bytes written (packs and index), groups not
       written (too large or refused by the card), restarted and failed
       writes, clears. */
    size_t writes;
    uint64_t written_bytes;
    size_t skipped;
    size_t write_restarts;
    size_t write_failures;
    size_t clears;
    uint64_t read_ns;
    uint64_t verify_ns;
    uint64_t write_ns;
} BrowserScriptDiskStats;

/* Sets (or, with an empty or NULL directory, turns off) the tier. Turning
   it off removes nothing; clear first to empty it. A zero maximum selects
   the default. False if the directory is refused or the tier's state
   (about 10 KiB) cannot be allocated. */
bool browser_session_script_disk_configure(BrowserSession *session,
                                           const char *directory, bool write,
                                           size_t maximum_bytes);
/* Saves pending index changes and frees the tier (teardown). */
void browser_session_script_disk_release(BrowserSession *session);
bool browser_session_script_disk_enabled(const BrowserSession *session);
/* Writes allowed now: on, writable, site data allowed, no captive sign-in. */
bool browser_session_script_disk_writable(const BrowserSession *session);
/* On a RAM miss for `key`: reads the pack its group's index entry names,
   at most once per load (document realm), verifies it and copies its
   records into the RAM table, cooperating at bounded read/verify boundaries
   and between promotions; cancellation permits a same-generation retry
   under `generation`. True if the key's own record (by ordinal) was
   copied; the caller then looks it up in RAM, where the digest decides. */
bool browser_session_script_disk_promote(BrowserSession *session,
                                         BrowserScriptBytecodeKind kind,
                                         const BrowserScriptBytecodeKey *key,
                                         uint32_t generation);
/* The engine could not restore a record that came from this key's pack:
   remove the pack (writes on) or stop reading it this session. */
void browser_session_script_disk_discard(BrowserSession *session,
                                         BrowserScriptBytecodeKind kind,
                                         const BrowserScriptBytecodeKey *key);
/* One bounded slice of idle work: the index load, a pack write slice, the
   sweep, an index save, an eviction or a new pack. True if it did work. */
bool browser_session_script_disk_maintenance(BrowserSession *session);
/* Explicit user clear: every pack and the index. False on incomplete
   removal (the tier then stops reading for the session). */
bool browser_session_script_disk_clear(BrowserSession *session);
/* Removes the packs of one top-level site (a partition key from
   tilefinch_url_site_key). A read-only tier cannot remove them: it stops
   reading for the session instead, as an explicit clear does. */
bool browser_session_script_disk_clear_site(BrowserSession *session,
                                            const char *partition);
/* Bytes and packs on disk, loading the index if needed; false when the
   tier is off. */
bool browser_session_script_disk_usage(BrowserSession *session,
                                       uint64_t *bytes, size_t *files);
void browser_session_script_disk_stats(const BrowserSession *session,
                                       BrowserScriptDiskStats *stats);
/* Removes every RAM record of one top-level site, in every table and in
   the bundle-record queue, and advances script_bytecode_epoch so no
   realm's deferred store queued before the clear writes that site's
   scripts back. */
void browser_session_script_bytecode_clear_partition(
    BrowserSession *session, const char *partition);
/* Drops the entry for this key (after a failed restore). */
void browser_session_module_bytecode_invalidate(
    BrowserSession *session, BrowserScriptBytecodeKey *key);
/* Bytes and entries currently held. */
size_t browser_session_module_bytecode_bytes(const BrowserSession *session);
size_t browser_session_module_bytecode_entries(const BrowserSession *session);
/* Resolves the exact authorized response once and retains either requested
   RAM-only compiler artifact. A NULL output skips that artifact. Returns
   whether the exact backing response exists and leaves room for an artifact;
   this is an admission hint, not a reservation or a guarantee of storage. */
bool browser_session_stylesheet_artifacts_acquire(
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
/* Drops the cached decoded copy that is this body (a page replaced it with
   a surface of another size), so no later page adopts a stale size. */
void browser_session_decoded_image_forget(BrowserSession *session,
                                          const BrowserSharedBody *pixels);
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
/* Evicts least-recently-used response entries, then module bytecode, until
   at least target_bytes of the shared page Budget has actually become
   available, or the optional caches are empty. Shared bodies still leased by
   another subsystem may therefore be evicted without contributing to the
   returned physical-byte count. */
size_t browser_session_cache_reclaim(BrowserSession *session,
                                     size_t target_bytes);
/* Evicts older HTTP entries until a bounded working set can be inserted
   without evicting its own earliest members. */
bool browser_session_cache_reserve_working_set(
    BrowserSession *session, size_t required_bytes);
/* Raise, but never lower, the bounded live-cache admission ceiling. Installed
   offline applications use this when their complete working set is larger
   than the user's ordinary transient-cache preference. This does not allocate
   memory. */
bool browser_session_cache_ensure_maximum_bytes(
    BrowserSession *session, size_t minimum_bytes);
/* Changes the live cache ceiling. A smaller ceiling evicts least-recently
   used entries before returning; zero and values beyond the shared Budget
   ceiling are rejected without changing the session. */
bool browser_session_cache_set_maximum_bytes(BrowserSession *session,
                                              size_t maximum_bytes);
void browser_session_cache_clear(BrowserSession *session);
size_t browser_session_cache_collect_offline_same_origin(
    BrowserSession *session, const char *document_url,
    BrowserOfflineCacheView *views, size_t capacity,
    size_t maximum_bytes, size_t *total_bytes, bool *complete);
bool browser_session_cache_restore_offline(
    BrowserSession *session, const char *document_url,
    const BrowserOfflineCacheView *view);
/* Installed-app classic scripts restored as bytecode only. Each record has
   the response's authority (grant, type, response URL) and its bytecode, but
   not its body: the source stays in the app pack at `pack_path` and is read
   on demand, verified against `source_hash`
   (browser_session_script_source_hash), by anything that needs the bytes.
   Reading one restores it as an ordinary cache entry with its bytecode.
   One app's set at a time: begin replaces any earlier set. */
#define BROWSER_SESSION_SOURCE_HASH_SEED UINT64_C(1469598103934665603)
uint64_t browser_session_script_source_hash(uint64_t hash, const void *data,
                                            size_t length);
bool browser_session_offline_deferred_begin(
    BrowserSession *session, const char *document_url,
    const char *pack_path);
/* `view->data` is ignored; `view->length` is the source length. The set
   takes over the caller's reference to `bytecode` on success. */
bool browser_session_offline_deferred_add(
    BrowserSession *session, const BrowserOfflineCacheView *view,
    uint64_t source_hash, uint64_t offset, BrowserSharedBody *bytecode);
void browser_session_offline_deferred_clear(BrowserSession *session);
/* Deferred classic scripts whose source has not been read (0 without a
   set); each on-demand read removes one. */
size_t browser_session_offline_deferred_pending(const BrowserSession *session);
/* A fresh hit for a deferred classic script under `context`, without
   reading its source. */
bool browser_session_offline_script_match(
    BrowserSession *session, const char *url,
    const TilefinchRequestContext *context, size_t *length);
/* Retained bytecode of a deferred script, or NULL. */
BrowserSharedBody *browser_session_offline_script_bytecode(
    BrowserSession *session, const char *url, size_t length);
/* Retained source body of a (possibly deferred) restored classic script,
   reading and restoring it if needed; NULL when unavailable. */
BrowserSharedBody *browser_session_offline_script_source(
    BrowserSession *session, const char *url, size_t length);
void browser_session_destroy(BrowserSession *session);

#endif
