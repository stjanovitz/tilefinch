#include "tilefinch/media_worker_request.h"

#include <stdio.h>
#include <string.h>

bool media_worker_hop_init(MediaWorkerHop *hop, const char *url)
{
    if (hop == NULL) return false;
    memset(hop, 0, sizeof(*hop));
    if (url == NULL) return false;
    size_t length = strlen(url);
    if (length >= sizeof(hop->url)) return false;
    memcpy(hop->url, url, length + 1u);
    return true;
}

bool media_worker_http_url_supported(const char *value)
{
    TilefinchUrl url;
    return tilefinch_url_parse(value, &url)
        && (url.scheme == TILEFINCH_URL_SCHEME_HTTP
            || url.scheme == TILEFINCH_URL_SCHEME_HTTPS);
}

static const FetchRequest *media_worker_page_request(
    const MediaWorkerAuthority *authority, const MediaWorkerHop *hop,
    const FetchRequest *transport, FetchPreparedPageRequest *prepared)
{
    TilefinchRequestContext context = {
        .target_url = hop->url,
        .initiator_url = authority->document_url,
        .top_level_url = authority->document_url,
        .method = "GET",
        .mode = authority->mode,
        .credentials = authority->credentials,
        .destination = authority->audio
            ? TILEFINCH_DESTINATION_AUDIO : TILEFINCH_DESTINATION_MEDIA
    };
    FetchRequestValidationError validation_error;
    BrowserSession *session = authority->session;
    if (!fetch_prepare_page_request_context(
            &context, authority->document_url,
            hop->referrer_policy[0] == '\0' ? NULL : hop->referrer_policy,
            session, authority->content_security_policy,
            session == NULL ? NULL : session->content_blocker,
            transport, prepared, &validation_error)) return NULL;
    return fetch_prepared_page_request(prepared);
}

MediaWorkerHopAdmission media_worker_hop_admit(
    const MediaWorkerAuthority *authority, MediaWorkerHop *hop,
    FetchPreparedPageRequest *scratch)
{
    if (authority == NULL || hop == NULL || authority->document_url == NULL)
        return MEDIA_WORKER_HOP_REFUSED;
    /* A provider stream is native work with no page policy, and a redirect
       hop was admitted by media_worker_redirect. */
    if (!authority->page_redirects || hop->redirects != 0)
        return MEDIA_WORKER_HOP_ADMITTED;
    if (scratch == NULL) return MEDIA_WORKER_HOP_NO_MEMORY;
    FetchRequest policy_shape = {.redirect_same_origin_only = true};
    const FetchRequest *page_request = media_worker_page_request(
        authority, hop, &policy_shape, scratch);
    char target[TILEFINCH_URL_SERIALIZED_LIMIT];
    memcpy(target, hop->url, strlen(hop->url) + 1u);
    bool block_private_network = true;
    if (page_request == NULL
        || !fetch_page_request_first_hop(
               page_request, target, sizeof(target), &block_private_network)
        || !media_worker_http_url_supported(target))
        return MEDIA_WORKER_HOP_REFUSED;
    memcpy(hop->url, target, strlen(target) + 1u);
    hop->block_private_network = block_private_network;
    return MEDIA_WORKER_HOP_ADMITTED;
}

bool media_worker_request_prepare(
    const MediaWorkerAuthority *authority, const MediaWorkerHop *hop,
    const FetchRequest *transport, FetchPreparedPageRequest *prepared,
    FetchRequest *worker_request)
{
    if (authority == NULL || hop == NULL || prepared == NULL
        || worker_request == NULL || authority->document_url == NULL)
        return false;
    FetchRequest shape = transport == NULL ? (FetchRequest) {0} : *transport;
    shape.redirect_same_origin_only = true;
    const FetchRequest *authorized = media_worker_page_request(
        authority, hop, &shape, prepared);
    if (authorized == NULL) return false;
    /* The prepared request owns the page authority and this hop's cookie
       snapshot. The worker has no BrowserSession on its thread, so it gets
       the flattened wire shape plus a syntactic HTTP(S) validator. A provider
       stream lets the worker follow same-origin redirects; a page hop is
       issued single-hop and its redirects return to media_worker_redirect. */
    *worker_request = *authorized;
    worker_request->redirect_url_validator = media_worker_http_url_supported;
    worker_request->redirect_same_origin_only = true;
    worker_request->block_private_network = hop->block_private_network;
    worker_request->page_context = NULL;
    worker_request->content_security_policy = NULL;
    worker_request->content_blocker = NULL;
    worker_request->cookie_session = NULL;
    worker_request->cookie_context = NULL;
    worker_request->prepared_page_version = 0;
    return true;
}

MediaWorkerRedirectStatus media_worker_redirect(
    const MediaWorkerAuthority *authority, MediaWorkerHop *hop,
    const FetchResult *headers, FetchPreparedPageRequest *scratch)
{
    if (authority == NULL || hop == NULL || headers == NULL)
        return MEDIA_WORKER_REDIRECT_REFUSED;
    long status = headers->status_code;
    bool redirect_status = status == 301 || status == 302 || status == 303
        || status == 307 || status == 308;
    /* Presence only; fetch_page_redirect_next reads the whole value. */
    char location[8];
    /* A native provider stream's redirects never reach this layer: the
       worker follows them under its fixed-origin rule. A 3xx without a
       Location is an ordinary response either way. */
    if (!authority->page_redirects || !redirect_status
        || !fetch_response_header_value(
               headers, "location", location, sizeof(location))) {
        return MEDIA_WORKER_RESPONSE;
    }
    if (hop->redirects >= MEDIA_WORKER_REDIRECT_LIMIT)
        return MEDIA_WORKER_REDIRECT_REFUSED;
    if (scratch == NULL) return MEDIA_WORKER_REDIRECT_NO_MEMORY;
    FetchRequest policy_shape = {.redirect_same_origin_only = true};
    const FetchRequest *page_request = media_worker_page_request(
        authority, hop, &policy_shape, scratch);
    char next[TILEFINCH_URL_SERIALIZED_LIMIT];
    bool block_private_network = true;
    if (page_request == NULL
        || !fetch_page_redirect_next(
               page_request, hop->url, headers, next, sizeof(next),
               &block_private_network)
        || (authority->mode == TILEFINCH_REQUEST_MODE_CORS
            && !tilefinch_url_same_origin(hop->url, next))) {
        return MEDIA_WORKER_REDIRECT_REFUSED;
    }
    bool policy_present = false;
    char policy[FETCH_REFERRER_POLICY_LIMIT];
    if (fetch_response_referrer_policy(headers, &policy_present, policy)
        && policy_present && policy[0] != '\0') {
        snprintf(hop->referrer_policy, sizeof(hop->referrer_policy), "%s",
                 policy);
    }
    memcpy(hop->url, next, strlen(next) + 1u);
    hop->block_private_network = block_private_network;
    hop->redirects++;
    return MEDIA_WORKER_REDIRECT_FOLLOW;
}
