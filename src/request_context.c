#include "tilefinch/request_context.h"

#include <string.h>
#include <strings.h>

#include "tilefinch/url.h"

static const char *top_level_url(const TilefinchRequestContext *context)
{
    return context->top_level_url != NULL ? context->top_level_url
        : context->initiator_url;
}

static bool request_destination_valid(TilefinchRequestDestination destination)
{
    switch (destination) {
        case TILEFINCH_DESTINATION_DOCUMENT:
        case TILEFINCH_DESTINATION_FRAME:
        case TILEFINCH_DESTINATION_SCRIPT:
        case TILEFINCH_DESTINATION_STYLE:
        case TILEFINCH_DESTINATION_IMAGE:
        case TILEFINCH_DESTINATION_FETCH:
        case TILEFINCH_DESTINATION_OTHER:
        case TILEFINCH_DESTINATION_FONT:
        case TILEFINCH_DESTINATION_MEDIA:
            return true;
    }
    return false;
}

static bool parsed_same_origin(const TilefinchUrl *left,
                               const TilefinchUrl *right)
{
    return left != NULL && right != NULL
        && left->scheme == right->scheme && left->port == right->port
        && left->host_length == right->host_length
        && strncasecmp(left->value + left->host_offset,
                       right->value + right->host_offset,
                       left->host_length) == 0;
}

static bool request_method_is_safe(const char *method)
{
    method = method == NULL ? "GET" : method;
    return strcasecmp(method, "GET") == 0 || strcasecmp(method, "HEAD") == 0
        || strcasecmp(method, "OPTIONS") == 0
        || strcasecmp(method, "TRACE") == 0;
}

static bool request_context_parse(const TilefinchRequestContext *context,
                                  TilefinchUrl *target,
    TilefinchUrl *initiator)
{
    TilefinchUrl top;
    const char *top_url = context == NULL ? NULL : top_level_url(context);
    if (context == NULL || target == NULL || initiator == NULL
        || context->target_url == NULL
        || (unsigned) context->mode > TILEFINCH_REQUEST_MODE_NO_CORS
        || (unsigned) context->credentials > TILEFINCH_CREDENTIALS_INCLUDE
        || !request_destination_valid(context->destination)
        || !tilefinch_url_parse(context->target_url, target)
        || (context->initiator_url != NULL
            && !tilefinch_url_parse(context->initiator_url, initiator))
        || (top_url != NULL && top_url != context->initiator_url
            && !tilefinch_url_parse(top_url, &top))) {
        return false;
    }
    bool navigation = context->mode == TILEFINCH_REQUEST_MODE_NAVIGATE;
    bool navigation_destination =
        context->destination == TILEFINCH_DESTINATION_DOCUMENT
        || context->destination == TILEFINCH_DESTINATION_FRAME;
    if (navigation != navigation_destination) return false;
    if (context->top_level_navigation
        && context->destination != TILEFINCH_DESTINATION_DOCUMENT) {
        return false;
    }
    if (context->user_activated && !context->top_level_navigation) {
        return false;
    }
    return true;
}

bool tilefinch_request_context_analyze(
    const TilefinchRequestContext *context, TilefinchRequestFacts *facts)
{
    if (facts == NULL) return false;
    *facts = (TilefinchRequestFacts) {.source = context};
    TilefinchUrl target;
    TilefinchUrl initiator;
    if (!request_context_parse(context, &target, &initiator)) return false;
    const char *top_url = top_level_url(context);

    facts->valid = true;
    facts->safe_method = request_method_is_safe(context->method);
    facts->same_origin = !context->initiator_opaque
        && context->initiator_url != NULL
        && parsed_same_origin(&initiator, &target);
    if (!context->initiator_opaque && top_url != NULL) {
        char top_site[TILEFINCH_ORIGIN_SERIALIZED_LIMIT];
        char target_site[TILEFINCH_ORIGIN_SERIALIZED_LIMIT];
        facts->same_site = tilefinch_url_site_key(
                               top_url, top_site, sizeof(top_site))
            && tilefinch_url_site_key(
                   context->target_url, target_site, sizeof(target_site))
            && strcmp(top_site, target_site) == 0;
    }
    facts->sends_credentials =
        context->credentials != TILEFINCH_CREDENTIALS_OMIT
        && (context->credentials == TILEFINCH_CREDENTIALS_INCLUDE
            || context->initiator_url == NULL || facts->same_origin);
    facts->allows_lax_cookie = facts->same_site
        || (context->top_level_navigation && facts->safe_method);
    facts->site = context->initiator_url == NULL
        ? TILEFINCH_REQUEST_SITE_NONE
        : (facts->same_origin ? TILEFINCH_REQUEST_SITE_SAME_ORIGIN
           : facts->same_site ? TILEFINCH_REQUEST_SITE_SAME_SITE
                              : TILEFINCH_REQUEST_SITE_CROSS_SITE);
    return true;
}

bool tilefinch_request_context_valid(const TilefinchRequestContext *context)
{
    TilefinchUrl target;
    TilefinchUrl initiator;
    return request_context_parse(context, &target, &initiator);
}

bool tilefinch_request_same_origin(const TilefinchRequestContext *context)
{
    TilefinchUrl target;
    TilefinchUrl initiator;
    return request_context_parse(context, &target, &initiator)
        && !context->initiator_opaque && context->initiator_url != NULL
        && parsed_same_origin(&initiator, &target);
}

bool tilefinch_request_same_site(const TilefinchRequestContext *context)
{
    TilefinchRequestFacts facts;
    return tilefinch_request_context_analyze(context, &facts)
        && facts.same_site;
}

bool tilefinch_request_safe_method(const TilefinchRequestContext *context)
{
    return request_method_is_safe(context == NULL ? NULL : context->method);
}

bool tilefinch_request_sends_credentials(const TilefinchRequestContext *context)
{
    if (!tilefinch_request_context_valid(context)
        || context->credentials == TILEFINCH_CREDENTIALS_OMIT) return false;
    return context->credentials == TILEFINCH_CREDENTIALS_INCLUDE
        || context->initiator_url == NULL
        || tilefinch_request_same_origin(context);
}

bool tilefinch_request_allows_lax_cookie(const TilefinchRequestContext *context)
{
    TilefinchRequestFacts facts;
    return tilefinch_request_context_analyze(context, &facts)
        && facts.allows_lax_cookie;
}

const char *tilefinch_request_fetch_site(const TilefinchRequestContext *context)
{
    TilefinchRequestFacts facts;
    if (!tilefinch_request_context_analyze(context, &facts)) {
        return context == NULL || context->initiator_url == NULL
            ? "none" : "cross-site";
    }
    return tilefinch_request_facts_fetch_site(&facts);
}

const char *tilefinch_request_facts_fetch_site(
    const TilefinchRequestFacts *facts)
{
    if (facts == NULL || !facts->valid) return "cross-site";
    switch (facts->site) {
        case TILEFINCH_REQUEST_SITE_NONE: return "none";
        case TILEFINCH_REQUEST_SITE_SAME_ORIGIN: return "same-origin";
        case TILEFINCH_REQUEST_SITE_SAME_SITE: return "same-site";
        case TILEFINCH_REQUEST_SITE_CROSS_SITE: return "cross-site";
    }
    return "cross-site";
}

const char *tilefinch_request_fetch_mode(const TilefinchRequestContext *context)
{
    if (context == NULL) return "no-cors";
    switch (context->mode) {
        case TILEFINCH_REQUEST_MODE_NAVIGATE: return "navigate";
        case TILEFINCH_REQUEST_MODE_SAME_ORIGIN: return "same-origin";
        case TILEFINCH_REQUEST_MODE_CORS: return "cors";
        case TILEFINCH_REQUEST_MODE_NO_CORS: return "no-cors";
    }
    return "no-cors";
}

const char *tilefinch_request_fetch_destination(
    const TilefinchRequestContext *context)
{
    if (context == NULL) return "empty";
    switch (context->destination) {
        case TILEFINCH_DESTINATION_DOCUMENT: return "document";
        case TILEFINCH_DESTINATION_FRAME: return "iframe";
        case TILEFINCH_DESTINATION_SCRIPT: return "script";
        case TILEFINCH_DESTINATION_STYLE: return "style";
        case TILEFINCH_DESTINATION_IMAGE: return "image";
        case TILEFINCH_DESTINATION_FETCH: return "empty";
        case TILEFINCH_DESTINATION_FONT: return "font";
        case TILEFINCH_DESTINATION_MEDIA: return "video";
        case TILEFINCH_DESTINATION_OTHER: return "empty";
    }
    return "empty";
}
