/* PSP link stub for libcurl.
 *
 * The script EBOOT replays traces hermetically: fetch.c short-circuits into
 * the replay store before issuing any network transfer, so the only curl
 * calls that can execute are global init/cleanup bookkeeping.  Linking the
 * SDK's static libcurl (+ mbedtls, zlib) costs ~1MB of text that the PSP's
 * 64MB must otherwise absorb.  This TU satisfies the same undefined symbols
 * with allocation-free no-ops: init succeeds, handle creation fails, so any
 * unexpected network path degrades into an ordinary fetch error instead of
 * an undefined reference or a dead transfer engine.
 */
#include <stddef.h>
#include <curl/curl.h>

/* curl.h wraps these two in type-checking macros under GCC; the stub
   defines the real functions. */
#undef curl_easy_setopt
#undef curl_easy_getinfo

CURLcode curl_global_init_mem(long flags, curl_malloc_callback m,
                              curl_free_callback f, curl_realloc_callback r,
                              curl_strdup_callback s, curl_calloc_callback c)
{
    (void) flags; (void) m; (void) f; (void) r; (void) s; (void) c;
    return CURLE_OK;
}

void curl_global_cleanup(void) {}

static int stub_share_token;
CURLSH *curl_share_init(void) { return (CURLSH *) &stub_share_token; }

CURLSHcode curl_share_setopt(CURLSH *share, CURLSHoption option, ...)
{
    (void) share; (void) option;
    return CURLSHE_OK;
}

CURLSHcode curl_share_cleanup(CURLSH *share)
{
    (void) share;
    return CURLSHE_OK;
}

curl_version_info_data *curl_version_info(CURLversion version)
{
    (void) version;
    return NULL;
}

CURL *curl_easy_init(void) { return NULL; }
void curl_easy_cleanup(CURL *handle) { (void) handle; }

CURLcode curl_easy_setopt(CURL *handle, CURLoption option, ...)
{
    (void) handle; (void) option;
    return CURLE_FAILED_INIT;
}

CURLcode curl_easy_getinfo(CURL *handle, CURLINFO info, ...)
{
    (void) handle; (void) info;
    return CURLE_FAILED_INIT;
}

CURLcode curl_easy_perform(CURL *handle)
{
    (void) handle;
    return CURLE_FAILED_INIT;
}

const char *curl_easy_strerror(CURLcode code)
{
    (void) code;
    return "libcurl stubbed out for hermetic replay";
}

/* fetch_scheduler_create builds a multi handle even when every request is
   served from the replay store, so init must succeed; the handle is a
   static token that every other multi stub accepts and ignores. */
static int stub_multi_token;
CURLM *curl_multi_init(void) { return (CURLM *) &stub_multi_token; }

CURLMcode curl_multi_cleanup(CURLM *multi)
{
    (void) multi;
    return CURLM_OK;
}

CURLMcode curl_multi_add_handle(CURLM *multi, CURL *handle)
{
    (void) multi; (void) handle;
    return CURLM_BAD_HANDLE;
}

CURLMcode curl_multi_remove_handle(CURLM *multi, CURL *handle)
{
    (void) multi; (void) handle;
    return CURLM_BAD_HANDLE;
}

CURLMcode curl_multi_perform(CURLM *multi, int *running_handles)
{
    (void) multi;
    if (running_handles != NULL) *running_handles = 0;
    return CURLM_BAD_HANDLE;
}

CURLMcode curl_multi_wait(CURLM *multi, struct curl_waitfd *extra_fds,
                          unsigned int extra_nfds, int timeout_ms,
                          int *numfds)
{
    (void) multi; (void) extra_fds; (void) extra_nfds; (void) timeout_ms;
    if (numfds != NULL) *numfds = 0;
    return CURLM_BAD_HANDLE;
}

CURLMsg *curl_multi_info_read(CURLM *multi, int *msgs_in_queue)
{
    (void) multi;
    if (msgs_in_queue != NULL) *msgs_in_queue = 0;
    return NULL;
}

struct curl_slist *curl_slist_append(struct curl_slist *list,
                                     const char *string)
{
    (void) list; (void) string;
    return NULL;
}

void curl_slist_free_all(struct curl_slist *list) { (void) list; }
