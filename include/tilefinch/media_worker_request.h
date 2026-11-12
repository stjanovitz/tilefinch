#ifndef TILEFINCH_MEDIA_WORKER_REQUEST_H
#define TILEFINCH_MEDIA_WORKER_REQUEST_H

#include <stdbool.h>
#include <stddef.h>

#include "tilefinch/content_security_policy.h"
#include "tilefinch/fetch.h"
#include "tilefinch/request_context.h"
#include "tilefinch/session.h"
#include "tilefinch/url.h"

/*
 * Request authority for native media requests that run on the PSP transport
 * worker without FetchScheduler (HLS playlists and segments). The worker
 * holds no BrowserSession, so everything page-specific is decided here, on
 * the browser thread, and the worker receives only a flattened request.
 *
 * Page media (`page_redirects`) asks the worker for one hop at a time. A
 * redirect comes back unfollowed and media_worker_redirect() admits the next
 * hop under the page's own per-hop rules (fetch_page_redirect_next), so the
 * target may be on another origin. Native provider streams leave it false:
 * the worker follows their redirects itself and only within the origin.
 */

/* One more than this many redirects of one request is refused. Equal to the
   scheduler's own limit. */
#define MEDIA_WORKER_REDIRECT_LIMIT 8u

typedef struct {
    BrowserSession *session;
    const char *document_url;
    TilefinchRequestMode mode;
    TilefinchCredentialsMode credentials;
    /* The opening document's CSP, or NULL when it sent none. */
    const TilefinchContentSecurityPolicy *content_security_policy;
    bool page_redirects;
    /* An audio-only stream: its hops carry TILEFINCH_DESTINATION_AUDIO. */
    bool audio;
} MediaWorkerAuthority;

/* The current hop of one logical request. Start with media_worker_hop_init. */
typedef struct {
    char url[TILEFINCH_URL_SERIALIZED_LIMIT];
    /* A Referrer-Policy delivered by a redirect governs the later hops. */
    char referrer_policy[FETCH_REFERRER_POLICY_LIMIT];
    unsigned redirects;
    /* Set for a redirect hop whose target must not be a private-network
       peer; the worker enforces it on the resolved address. */
    bool block_private_network;
} MediaWorkerHop;

typedef enum {
    /* Not a redirect this layer follows: admit or refuse it as a response. */
    MEDIA_WORKER_RESPONSE = 0,
    /* `hop` now names the admitted next target; issue it. */
    MEDIA_WORKER_REDIRECT_FOLLOW,
    /* A redirect that must not be followed; fail the request. */
    MEDIA_WORKER_REDIRECT_REFUSED,
    /* No scratch storage to decide with: a memory failure of this request,
       not a refusal of its target. */
    MEDIA_WORKER_REDIRECT_NO_MEMORY
} MediaWorkerRedirectStatus;

bool media_worker_hop_init(MediaWorkerHop *hop, const char *url);

/* HTTP(S) only, the validator the worker applies to every URL it fetches. */
bool media_worker_http_url_supported(const char *url);

typedef enum {
    MEDIA_WORKER_HOP_ADMITTED = 0,
    /* The page's policy refuses the target; it fails the same way on every
       retry. */
    MEDIA_WORKER_HOP_REFUSED,
    /* No scratch storage; nothing was decided. */
    MEDIA_WORKER_HOP_NO_MEMORY
} MediaWorkerHopAdmission;

/*
 * Admits the first hop of a page media request before anything is issued:
 * the same gate a redirect target meets (CSP media-src, mixed content with
 * HSTS and passive auto-upgrade, captive portal, blocked origins, the
 * content blocker), with the private-network flag the scheduler would set.
 * An admitted hop may have its URL upgraded in place. Provider streams and
 * redirect hops pass through unchanged. `scratch` is caller storage for the
 * hop's policy envelope.
 */
MediaWorkerHopAdmission media_worker_hop_admit(
    const MediaWorkerAuthority *authority, MediaWorkerHop *hop,
    FetchPreparedPageRequest *scratch);

/*
 * Builds the worker request for `hop->url` through the page request
 * boundary: the cookie and Referer are those of this hop's own target, never
 * carried over from an earlier hop. `transport` holds transport-only fields
 * (Accept, User-Agent, timeouts, encoding). `prepared` is caller storage
 * that owns the strings `worker_request` points into; it must outlive the
 * enqueue call that copies them.
 */
bool media_worker_request_prepare(
    const MediaWorkerAuthority *authority, const MediaWorkerHop *hop,
    const FetchRequest *transport, FetchPreparedPageRequest *prepared,
    FetchRequest *worker_request);

/*
 * Classifies the response header snapshot of `hop`. A followed redirect
 * advances `hop` in place. A CORS-mode request does not leave its origin on
 * this path: an opaque redirect would need the tainted-origin handling only
 * the scheduler implements. `scratch` is caller storage for the hop's
 * policy envelope.
 */
MediaWorkerRedirectStatus media_worker_redirect(
    const MediaWorkerAuthority *authority, MediaWorkerHop *hop,
    const FetchResult *headers, FetchPreparedPageRequest *scratch);

#endif
