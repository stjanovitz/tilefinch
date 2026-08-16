#ifndef TILEFINCH_FETCH_REDIRECT_ABORT_H
#define TILEFINCH_FETCH_REDIRECT_ABORT_H

#include <stdbool.h>

#include <curl/curl.h>

/* Returning a short count from a libcurl body or header callback is how the
   transport deliberately stops a redirect representation before following
   its Location.  Backends do not all report that callback termination with
   the same CURLcode (notably across HTTP/1.1 and HTTP/2), so the callback's
   explicit state is authoritative.  A separately recorded callback failure
   always wins and must never be normalized into a successful redirect hop. */
static inline bool tilefinch_fetch_redirect_callback_abort_expected(
    CURLcode transport_result, bool redirect_response,
    bool callback_aborted, bool callback_failed)
{
    return transport_result != CURLE_OK
        && redirect_response
        && callback_aborted
        && !callback_failed;
}

#endif
