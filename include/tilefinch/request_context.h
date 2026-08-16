#ifndef TILEFINCH_REQUEST_CONTEXT_H
#define TILEFINCH_REQUEST_CONTEXT_H

#include <stdbool.h>

typedef enum {
    TILEFINCH_REQUEST_MODE_NAVIGATE = 0,
    TILEFINCH_REQUEST_MODE_SAME_ORIGIN,
    TILEFINCH_REQUEST_MODE_CORS,
    TILEFINCH_REQUEST_MODE_NO_CORS
} TilefinchRequestMode;

typedef enum {
    TILEFINCH_CREDENTIALS_OMIT = 0,
    TILEFINCH_CREDENTIALS_SAME_ORIGIN,
    TILEFINCH_CREDENTIALS_INCLUDE
} TilefinchCredentialsMode;

typedef enum {
    TILEFINCH_DESTINATION_DOCUMENT = 0,
    TILEFINCH_DESTINATION_FRAME = 1,
    TILEFINCH_DESTINATION_SCRIPT = 2,
    TILEFINCH_DESTINATION_STYLE = 3,
    TILEFINCH_DESTINATION_IMAGE = 4,
    TILEFINCH_DESTINATION_FETCH = 5,
    /* Preserve the established public value while extending the enum. */
    TILEFINCH_DESTINATION_OTHER = 6,
    TILEFINCH_DESTINATION_FONT = 7
} TilefinchRequestDestination;

/* All security decisions for a request are derived from this immutable
   context. A NULL initiator denotes a user/externally initiated navigation;
   a NULL top-level URL falls back conservatively to the initiator. */
typedef struct {
    const char *target_url;
    const char *initiator_url;
    const char *top_level_url;
    const char *method;
    TilefinchRequestMode mode;
    TilefinchCredentialsMode credentials;
    TilefinchRequestDestination destination;
    /* A sandboxed document without allow-same-origin keeps its committed URL
       for resolution and referrers, but that URL must never confer native
       same-origin, same-site, cookie, storage, or response-read authority. */
    bool initiator_opaque;
    bool top_level_navigation;
    bool user_activated;
} TilefinchRequestContext;

typedef enum {
    TILEFINCH_REQUEST_SITE_NONE = 0,
    TILEFINCH_REQUEST_SITE_SAME_ORIGIN,
    TILEFINCH_REQUEST_SITE_SAME_SITE,
    TILEFINCH_REQUEST_SITE_CROSS_SITE
} TilefinchRequestSite;

/* A builder-owned snapshot of the URL and policy facts derived from one
   immutable request context. It is intentionally not retained by
   FetchRequest: callers that build several headers/cookie decisions at once
   can avoid reparsing the same URLs without increasing every queued request. */
typedef struct {
    const TilefinchRequestContext *source;
    TilefinchRequestSite site;
    bool valid;
    bool same_origin;
    bool same_site;
    bool safe_method;
    bool sends_credentials;
    bool allows_lax_cookie;
} TilefinchRequestFacts;

/* A resource body can carry this typed final-response grant across the
   transport/cache boundary. The current executable-script integration uses
   it authoritatively; other resource classes can adopt the same compact,
   allocation-free representation as their response gates are completed. */
typedef enum {
    TILEFINCH_RESOURCE_DENIED_NONE = 0,
    TILEFINCH_RESOURCE_DENIED_CONTEXT,
    TILEFINCH_RESOURCE_DENIED_TRUNCATED_SECURITY_HEADER,
    TILEFINCH_RESOURCE_DENIED_MALFORMED_SECURITY_HEADER,
    TILEFINCH_RESOURCE_DENIED_CORP,
    TILEFINCH_RESOURCE_DENIED_MIME
} TilefinchResourceDeniedReason;

typedef enum {
    TILEFINCH_CORP_UNSPECIFIED = 0,
    TILEFINCH_CORP_SAME_ORIGIN,
    TILEFINCH_CORP_SAME_SITE,
    TILEFINCH_CORP_CROSS_ORIGIN
} TilefinchCrossOriginResourcePolicy;

typedef struct {
    TilefinchRequestDestination destination;
    TilefinchRequestMode mode;
    TilefinchCredentialsMode credentials;
    TilefinchCrossOriginResourcePolicy corp;
    bool initiator_opaque;
    bool final_same_origin;
    bool final_same_site;
    bool cors_validated;
    bool nosniff;
    bool mime_validated;
} TilefinchResourceGrant;

bool tilefinch_request_context_valid(const TilefinchRequestContext *context);
bool tilefinch_request_context_analyze(
    const TilefinchRequestContext *context, TilefinchRequestFacts *facts);
bool tilefinch_request_same_origin(const TilefinchRequestContext *context);
bool tilefinch_request_same_site(const TilefinchRequestContext *context);
bool tilefinch_request_safe_method(const TilefinchRequestContext *context);
bool tilefinch_request_sends_credentials(const TilefinchRequestContext *context);
bool tilefinch_request_allows_lax_cookie(const TilefinchRequestContext *context);
const char *tilefinch_request_fetch_site(const TilefinchRequestContext *context);
const char *tilefinch_request_facts_fetch_site(
    const TilefinchRequestFacts *facts);
const char *tilefinch_request_fetch_mode(const TilefinchRequestContext *context);
const char *tilefinch_request_fetch_destination(
    const TilefinchRequestContext *context);

#endif
