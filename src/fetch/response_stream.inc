static void curl_free_hook(void *pointer)
{
    budget_concurrent_pool_free(&curl_concurrent_pool, pointer);
}

static void *curl_realloc_hook(void *pointer, size_t size)
{
    return budget_concurrent_pool_realloc(
        &curl_concurrent_pool, pointer, size);
}

static char *curl_strdup_hook(const char *source)
{
    size_t length = strlen(source);
    char *copy = budget_concurrent_pool_malloc(
        &curl_concurrent_pool, length + 1);
    if (copy != NULL) memcpy(copy, source, length + 1);
    return copy;
}

static void *curl_calloc_hook(size_t count, size_t size)
{
    return budget_concurrent_pool_calloc(
        &curl_concurrent_pool, count, size);
}

#if defined(TILEFINCH_PSP_OWNED_TRANSPORT)
/*
 * Cross-boot TLS session resumption (docs/engineering/PSP_TRANSPORT.md).
 * The share's TLS session cache is
 * seeded from one process-resident bounded store right after the share is
 * created, and exported back into that memory at transport teardown, both
 * through a throwaway share-attached easy handle. Disk is touched only at the
 * first import and an explicit suspend/exit flush. curl_easy_ssls_export/import
 * require the vendored curl built
 * with USE_SSLS_EXPORT=ON; when the store path is unset the whole feature is
 * inert. Failures at any step are silent misses -- a transparent full
 * handshake, never a transfer error. The Mbed TLS version number pins the
 * blob: a mismatched crypto build makes the file a load-side miss.
 */
typedef struct {
    TlsSessionStore *store;
    int64_t now;
    uint64_t sequence;
} FetchTlsExportContext;

static CURLcode fetch_tls_session_export_cb(
    CURL *curl, void *userptr, const char *session_key,
    const unsigned char *shmac, size_t shmac_len,
    const unsigned char *sdata, size_t sdata_len,
    curl_off_t valid_until, int ietf_tls_id,
    const char *alpn, size_t earlydata_max)
{
    (void) curl; (void) shmac; (void) shmac_len;
    (void) alpn; (void) earlydata_max;
    FetchTlsExportContext *context = userptr;
    /* An incrementing sequence gives the store a deterministic recency order
       to break LRU ties within a single export. */
    (void) tls_session_store_add(
        context->store, session_key, sdata, sdata_len,
        (int64_t) valid_until, (int32_t) ietf_tls_id,
        context->now, ++context->sequence);
    return CURLE_OK;
}

static CURL *fetch_tls_share_attached_easy(void)
{
    if (curl_shared_state == NULL) return NULL;
    CURL *easy = curl_easy_init();
    if (easy == NULL) return NULL;
    if (curl_easy_setopt(easy, CURLOPT_SHARE, curl_shared_state) != CURLE_OK) {
        curl_easy_cleanup(easy);
        return NULL;
    }
    return easy;
}

static int64_t fetch_tls_now_seconds(void)
{
    time_t seconds = time(NULL);
    return seconds > 0 ? (int64_t) seconds : 0;
}

static void fetch_tls_import_apply(void *context, const char *key,
                                   const unsigned char *blob,
                                   size_t blob_length, int32_t ietf_tls_id)
{
    (void) ietf_tls_id; /* curl recovers it from the packed session */
    (void) curl_easy_ssls_import((CURL *) context, key, NULL, 0,
                                 blob, blob_length);
}

static void fetch_tls_session_memory_reset(void)
{
    tls_session_store_free(fetch_tls_session_memory);
    fetch_tls_session_memory = NULL;
    fetch_tls_session_load_attempted = false;
    fetch_tls_session_dirty = false;
}

static TlsSessionStore *fetch_tls_session_memory_ensure(void)
{
    if (!fetch_tls_session_load_attempted) {
        fetch_tls_session_load_attempted = true;
        if (fetch_tls_session_store_file[0] != '\0') {
            fetch_tls_session_memory = tls_session_store_load(
                fetch_tls_session_store_file,
                (uint32_t) MBEDTLS_VERSION_NUMBER,
                fetch_tls_now_seconds());
        }
    }
    if (fetch_tls_session_memory == NULL)
        fetch_tls_session_memory = tls_session_store_create();
    return fetch_tls_session_memory;
}

static void fetch_tls_sessions_import(void)
{
    if (!fetch_tls_session_persistence_enabled()
        || fetch_tls_session_store_file[0] == '\0') return;
    int64_t now = fetch_tls_now_seconds();
    TlsSessionStore *store = fetch_tls_session_memory_ensure();
    if (store == NULL) return;
    CURL *easy = fetch_tls_share_attached_easy();
    if (easy != NULL) {
        (void) tls_session_store_for_each(
            store, now, fetch_tls_import_apply, easy);
        curl_easy_cleanup(easy);
    }
}

static void fetch_tls_sessions_export(void)
{
    if (!fetch_tls_session_persistence_enabled()
        || fetch_tls_session_store_file[0] == '\0') return;
    CURL *easy = fetch_tls_share_attached_easy();
    if (easy == NULL) return;
    TlsSessionStore *store = fetch_tls_session_memory_ensure();
    if (store != NULL) {
        int64_t now = fetch_tls_now_seconds();
        FetchTlsExportContext context = {
            .store = store, .now = now,
            .sequence = (uint64_t) (now > 0 ? now : 1) << 16
        };
        if (curl_easy_ssls_export(
                easy, fetch_tls_session_export_cb, &context) == CURLE_OK) {
            fetch_tls_session_dirty = true;
        }
    }
    curl_easy_cleanup(easy);
}

bool fetch_tls_session_store_flush(void)
{
    if (!fetch_tls_session_persistence_enabled()
        || !fetch_tls_session_dirty
        || fetch_tls_session_store_file[0] == '\0')
        return true;
    if (fetch_tls_session_memory == NULL) return false;
    int64_t now = fetch_tls_now_seconds();
    if (tls_session_store_save(
            fetch_tls_session_memory, fetch_tls_session_store_file,
            (uint32_t) MBEDTLS_VERSION_NUMBER, now) != 0) return false;
    fetch_tls_session_dirty = false;
    return true;
}

bool fetch_tls_session_store_clear(void)
{
    fetch_tls_session_memory_reset();
    return fetch_tls_session_store_file[0] == '\0'
        || tls_session_store_remove(fetch_tls_session_store_file) == 0;
}
#endif /* TILEFINCH_PSP_OWNED_TRANSPORT */

#if !defined(TILEFINCH_PSP_OWNED_TRANSPORT)
bool fetch_tls_session_store_flush(void) { return true; }
bool fetch_tls_session_store_clear(void) { return true; }
#endif

static bool curl_global_acquire(Budget *budget)
{
    curl_global_error[0] = '\0';
    if (budget == NULL) {
        snprintf(curl_global_error, sizeof(curl_global_error), "%s",
                 "curl received no memory budget");
        return false;
    }
    if (curl_global_users != 0 && curl_budget != budget) {
        snprintf(curl_global_error, sizeof(curl_global_error), "%s",
                 "curl is active under a different page budget");
        return false;
    }
    if (curl_global_users == 0) {
        if (!fetch_transport_runtime_supported()) {
            snprintf(curl_global_error, sizeof(curl_global_error), "%s",
                     "curl transport is unavailable in this build");
            return false;
        }
        if (!budget_concurrent_pool_init(
                &curl_concurrent_pool, budget, BUDGET_CATEGORY_RESOURCE,
                FETCH_CURL_CONCURRENT_POOL_BYTES)) {
            snprintf(curl_global_error, sizeof(curl_global_error), "%s",
                     "curl 1 MiB memory pool reservation failed");
            return false;
        }
        curl_budget = budget;
        CURLcode code = curl_global_init_mem(
            CURL_GLOBAL_DEFAULT, curl_malloc_hook, curl_free_hook,
            curl_realloc_hook, curl_strdup_hook, curl_calloc_hook);
        if (code != CURLE_OK) {
            snprintf(curl_global_error, sizeof(curl_global_error),
                     "curl global initialization failed (%d)", (int) code);
            (void) budget_concurrent_pool_metrics(
                &curl_concurrent_pool, &curl_last_pool_metrics);
            if (budget_concurrent_pool_destroy(&curl_concurrent_pool))
                curl_budget = NULL;
            return false;
        }
        curl_shared_state = curl_share_init();
        /* Every share category is an optimization. A PSP curl build that
           cannot create or configure the share can still perform correct
           transfers through each easy/multi handle's own caches. Never turn
           an unavailable cache into "network request could not start." */
        if (curl_shared_state != NULL) {
            CURLSHcode dns_share = curl_share_setopt(
                curl_shared_state, CURLSHOPT_SHARE, CURL_LOCK_DATA_DNS);
            CURLSHcode tls_share = curl_share_setopt(
                curl_shared_state, CURLSHOPT_SHARE,
                CURL_LOCK_DATA_SSL_SESSION);
            if (dns_share != CURLSHE_OK || tls_share != CURLSHE_OK) {
#if defined(TILEFINCH_PSP_VALIDATION_LOG)
                char share_error[128];
                snprintf(
                    share_error, sizeof(share_error),
                    "tilefinch-fetch: curl-share=disabled dns=%d tls=%d",
                    (int) dns_share, (int) tls_share);
                tilefinch_platform_log_message(share_error);
#endif
                (void) curl_share_cleanup(curl_shared_state);
                curl_shared_state = NULL;
            } else {
                CURLSHcode connection_share = curl_share_setopt(
                    curl_shared_state, CURLSHOPT_SHARE,
                    CURL_LOCK_DATA_CONNECT);
#if defined(TILEFINCH_PSP_VALIDATION_LOG)
                if (connection_share != CURLSHE_OK) {
                    char share_error[128];
                    snprintf(
                        share_error, sizeof(share_error),
                        "tilefinch-fetch: curl-connect-share=disabled "
                        "result=%d", (int) connection_share);
                    tilefinch_platform_log_message(share_error);
                }
#else
                (void) connection_share;
#endif
            }
        }
#if defined(TILEFINCH_PSP_OWNED_TRANSPORT)
        /* Seed the fresh share's TLS session cache from the on-stick store so
           the first connection to a previously-visited host can resume. */
        fetch_tls_sessions_import();
#endif
    }
    curl_global_users++;
    return true;
}

static bool curl_easy_attach_shared_state(CURL *easy)
{
    return easy != NULL
        && (curl_shared_state == NULL
            || curl_easy_setopt(
                   easy, CURLOPT_SHARE, curl_shared_state) == CURLE_OK);
}

static bool curl_global_release(void)
{
    if (curl_global_users == 0) return false;
    curl_global_users--;
    if (curl_global_users == 0) {
#if defined(TILEFINCH_PSP_OWNED_TRANSPORT)
        /* Retain the session cache in bounded process memory. Controlled
           suspend/exit owns the only Memory Stick flush. */
        fetch_tls_sessions_export();
#endif
        if (curl_shared_state != NULL) {
            (void) curl_share_cleanup(curl_shared_state);
            curl_shared_state = NULL;
        }
        curl_global_cleanup();
        (void) budget_concurrent_pool_metrics(
            &curl_concurrent_pool, &curl_last_pool_metrics);
        if (!budget_concurrent_pool_destroy(&curl_concurrent_pool)) {
            char cleanup_error[192];
            snprintf(
                cleanup_error, sizeof(cleanup_error),
                "curl-memory-cleanup current=%zu active=%zu allocs=%zu "
                "frees=%zu status=FAIL",
                curl_last_pool_metrics.current,
                curl_last_pool_metrics.active_allocations,
                curl_last_pool_metrics.allocation_count,
                curl_last_pool_metrics.free_count);
            tilefinch_platform_log_message(cleanup_error);
            return false;
        }
        curl_budget = NULL;
    }
    return true;
}

static void stream_record_peak(WriteContext *context, size_t bytes)
{
    if (context->metrics != NULL
        && bytes > context->metrics->peak_buffered_bytes) {
        context->metrics->peak_buffered_bytes = bytes;
    }
    if (context->pump_metrics != NULL
        && bytes > context->pump_metrics->peak_buffered_bytes) {
        context->pump_metrics->peak_buffered_bytes = bytes;
    }
}

static bool stream_deliver_chunk(WriteContext *context,
                                 const unsigned char *data, size_t length)
{
    if (length == 0) return true;
    if (context->stream == NULL || context->stream->on_body == NULL
        || !context->stream->on_body(context->stream->opaque, data, length)) {
        context->consumer_failed = true;
        return false;
    }
    if (context->capture_body != NULL
        && fwrite(data, 1, length, context->capture_body) != length) {
        context->capture_failed = true;
        return false;
    }
    context->delivered += length;
    context->result->length = context->delivered;
    if (context->metrics != NULL) {
        context->metrics->bytes_received = context->delivered;
        context->metrics->chunks_received++;
    }
    if (context->pump_quota_enabled) {
        if (context->pump_bytes_remaining != NULL) {
            *context->pump_bytes_remaining -= length;
        }
        if (context->pump_callbacks_remaining != NULL) {
            (*context->pump_callbacks_remaining)--;
        }
        if (context->pump_metrics != NULL) {
            context->pump_metrics->body_bytes += length;
            context->pump_metrics->body_callbacks++;
        }
    }
    stream_record_peak(context, length);
    return true;
}

static bool stream_stage_pending(WriteContext *context,
                                 const unsigned char *data, size_t length)
{
    if (length == 0) return true;
    size_t retained = context->pending_length >= context->pending_offset
        ? context->pending_length - context->pending_offset : 0;
    if (length > SIZE_MAX - retained) {
        context->allocation_failed = true;
        return false;
    }
    size_t needed = retained + length;
    if (retained != 0 && context->pending_offset != 0) {
        memmove(context->pending,
                context->pending + context->pending_offset, retained);
    }
    unsigned char *pending = budget_realloc(
        context->result->budget, context->pending, needed);
    if (pending == NULL) {
        context->allocation_failed = true;
        return false;
    }
    memcpy(pending + retained, data, length);
    context->pending = pending;
    context->pending_offset = 0;
    context->pending_length = needed;
    stream_record_peak(context, needed);
    return true;
}

static size_t stream_injected_boundary(const WriteContext *context,
                                       size_t wanted, bool *fault)
{
    *fault = false;
    if (context->stream == NULL) return wanted;
    const FetchStreamOptions *options = context->stream;
    size_t boundary = 0;
    if (options->cancel_after_bytes > context->delivered) {
        boundary = options->cancel_after_bytes - context->delivered;
    } else if (options->cancel_after_bytes != 0) {
        *fault = true;
        return 0;
    }
    if (options->truncate_after_bytes > context->delivered) {
        size_t remaining = options->truncate_after_bytes
                           - context->delivered;
        if (boundary == 0 || remaining < boundary) boundary = remaining;
    } else if (options->truncate_after_bytes != 0) {
        *fault = true;
        return 0;
    }
    if (boundary != 0 && wanted > boundary) {
        wanted = boundary;
        *fault = true;
    }
    return wanted;
}

static void stream_mark_injected_fault(WriteContext *context)
{
    const FetchStreamOptions *options = context->stream;
    if (options != NULL && options->cancel_after_bytes != 0
        && context->delivered >= options->cancel_after_bytes) {
        context->cancelled = true;
        if (context->metrics != NULL) context->metrics->cancelled = true;
        snprintf(context->result->error, sizeof(context->result->error),
                 "injected stream cancellation at byte %zu",
                 context->delivered);
    } else {
        if (context->metrics != NULL) context->metrics->truncated = true;
        snprintf(context->result->error, sizeof(context->result->error),
                 "injected truncated response at byte %zu",
                 context->delivered);
    }
}

static bool buffered_deliver_chunk(WriteContext *context,
                                   const unsigned char *data, size_t length)
{
    FetchResult *result = context->result;
    if (length == 0) return true;
    if (result->length > SIZE_MAX - 1u
        || length > SIZE_MAX - 1u - result->length) {
        context->limit_exceeded = true;
        return false;
    }
    size_t required = result->length + length + 1u;
    if (required > result->capacity) {
        size_t capacity = result->capacity == 0 ? 4096 : result->capacity;
        while (capacity < required) {
            size_t grown = capacity > (context->maximum_bytes + 1u) / 2u
                ? context->maximum_bytes + 1u : capacity * 2u;
            if (grown <= capacity) return false;
            capacity = grown;
        }
        char *expanded = budget_realloc(result->budget, result->data,
                                        capacity);
        if (expanded == NULL) {
            context->allocation_failed = true;
            return false;
        }
        result->data = expanded;
        result->capacity = capacity;
    }
    memcpy(result->data + result->length, data, length);
    result->length += length;
    context->delivered = result->length;
    result->data[result->length] = '\0';
    if (context->metrics != NULL) {
        context->metrics->bytes_received = context->delivered;
        context->metrics->chunks_received++;
        context->metrics->peak_buffered_bytes = result->capacity;
    }
    if (context->pump_quota_enabled) {
        if (context->pump_bytes_remaining != NULL) {
            *context->pump_bytes_remaining -= length;
        }
        if (context->pump_callbacks_remaining != NULL) {
            (*context->pump_callbacks_remaining)--;
        }
        if (context->pump_metrics != NULL) {
            context->pump_metrics->body_bytes += length;
            context->pump_metrics->body_callbacks++;
        }
    }
    stream_record_peak(context, result->capacity);
    return true;
}

static bool write_deliver_chunk(WriteContext *context,
                                const unsigned char *data, size_t length)
{
    return context->stream != NULL && context->stream->on_body != NULL
        ? stream_deliver_chunk(context, data, length)
        : buffered_deliver_chunk(context, data, length);
}

static void write_context_set_effective_url(WriteContext *context,
                                            const char *wire_url)
{
    if (context == NULL || context->result == NULL) return;
    const char *logical_url = fetch_hop_logical_url(context->hop);
    const char *effective = logical_url != NULL && logical_url[0] != '\0'
        ? logical_url : wire_url;
    if (effective != NULL) {
        snprintf(context->result->effective_url,
                 sizeof(context->result->effective_url), "%s", effective);
    }
}

static size_t receive_data(char *data, size_t size, size_t count, void *opaque)
{
    WriteContext *context = opaque;
    FetchResult *result = context->result;
    if (size != 0 && count > SIZE_MAX / size) return 0;
    size_t received = size * count;
    /* A redirect representation is never page-visible.  Stop the transfer at
       its first body byte so a slow or unbounded redirect body cannot consume
       the request deadline; the easy and multi drivers recognize this one
       internal write abort and immediately advance the validated hop. */
    if (context->redirect_response) {
        context->redirect_callback_aborted = true;
        return 0;
    }
    if (context->interim_response) {
        return received;
    }
    if (context->easy != NULL) {
        char *effective = NULL, *content_type = NULL;
        (void) curl_easy_getinfo(context->easy, CURLINFO_RESPONSE_CODE,
                                &result->status_code);
        (void) curl_easy_getinfo(context->easy, CURLINFO_EFFECTIVE_URL,
                                &effective);
        (void) curl_easy_getinfo(context->easy, CURLINFO_CONTENT_TYPE,
                                &content_type);
        write_context_set_effective_url(context, effective);
        if (content_type != NULL) {
            snprintf(result->content_type, sizeof(result->content_type),
                     "%s", content_type);
        }
    }
    size_t accounted = context->pump_quota_enabled
        ? context->accepted : context->delivered;
    if (accounted > context->maximum_bytes
        || received > context->maximum_bytes - accounted) {
        context->limit_exceeded = true;
        return 0;
    }

    if (!context->pump_quota_enabled) {
        return write_deliver_chunk(
            context, (const unsigned char *) data, received) ? received : 0;
    }
    {
        context->accepted += received;
        size_t deliver = received;
        if (context->pump_bytes_remaining != NULL
            && deliver > *context->pump_bytes_remaining) {
            deliver = *context->pump_bytes_remaining;
        }
        if (context->pump_callbacks_remaining != NULL
            && *context->pump_callbacks_remaining == 0) deliver = 0;
        bool injected_fault = false;
        deliver = stream_injected_boundary(
            context, deliver, &injected_fault);
        bool quota_split = deliver < received && !injected_fault;
        if (quota_split
            && !stream_stage_pending(
                context, (const unsigned char *) data + deliver,
                received - deliver)) return 0;
        if (deliver != 0 && !write_deliver_chunk(
                context, (const unsigned char *) data, deliver)) return 0;
        if (injected_fault) {
            stream_mark_injected_fault(context);
            return 0;
        }
        return received;
    }
}

static void copy_header_value(char *output, size_t capacity,
                              const char *value, size_t length)
{
    while (length != 0 && (*value == ' ' || *value == '\t')) {
        value++; length--;
    }
    while (length != 0 && (value[length - 1] == '\r'
                            || value[length - 1] == '\n'
                            || isspace((unsigned char) value[length - 1]))) {
        length--;
    }
    if (length >= capacity) length = capacity - 1;
    memcpy(output, value, length);
    output[length] = '\0';
}

static bool append_header_list_value(char *output, size_t capacity,
                                     const char *value, size_t length)
{
    while (length != 0 && (*value == ' ' || *value == '\t')) {
        value++;
        length--;
    }
    while (length != 0
           && (value[length - 1] == '\r' || value[length - 1] == '\n'
               || isspace((unsigned char) value[length - 1]))) {
        length--;
    }
    if (length == 0) return true;
    size_t used = strlen(output);
    size_t separator = used == 0 ? 0 : 2;
    if (capacity == 0 || used >= capacity
        || separator > capacity - used - 1
        || length > capacity - used - separator - 1) {
        return false;
    }
    if (separator != 0) {
        output[used++] = ',';
        output[used++] = ' ';
    }
    memcpy(output + used, value, length);
    output[used + length] = '\0';
    return true;
}

static void reset_response_metadata(FetchResult *result)
{
    result->status_code = 0;
    result->effective_url[0] = '\0';
    result->content_type[0] = '\0';
    result->etag[0] = '\0';
    result->last_modified[0] = '\0';
    result->cf_mitigated[0] = '\0';
    result->accept_ch[0] = '\0';
    result->critical_ch[0] = '\0';
    result->server[0] = '\0';
    result->cf_ray[0] = '\0';
    result->tls_version[0] = '\0';
    result->redirect_origin_tainted = false;
    fetch_response_security_metadata_reset(&result->security);
    result->response_referrer_policy[0] = '\0';
    result->response_referrer_policy_header_present = false;
    result->response_referrer_policy_metadata_valid = true;
    result->response_security_headers_truncated = false;
    result->set_cookies_truncated = false;
    result->response_headers_length = 0;
    result->response_headers[0] = '\0';
}

static bool remember_response_cookie_urls(WriteContext *context,
                                          const char *response_url)
{
    FetchResult *result = context->result;
    if (response_url == NULL || response_url[0] == '\0'
        || strlen(response_url) >= sizeof(result->effective_url)) {
        return result->set_cookie_count == context->response_cookie_start;
    }
    size_t length = strlen(response_url);
    for (size_t i = context->response_cookie_start;
         i < result->set_cookie_count; i++) {
        if (result->set_cookie_urls[i] != NULL) continue;
        char *copy = budget_malloc(result->budget, length + 1);
        if (copy == NULL) {
            context->allocation_failed = true;
            return false;
        }
        memcpy(copy, response_url, length + 1);
        result->set_cookie_urls[i] = copy;
    }
    return true;
}

static bool enforce_response_credential_policy(WriteContext *context);
static bool enforce_response_cors_policy(WriteContext *context);

bool fetch_response_header_value(const FetchResult *result,
                                 const char *name,
                                 char *output, size_t output_size)
{
    if (result == NULL || name == NULL || output == NULL
        || output_size == 0) return false;
    output[0] = '\0';
    size_t name_length = strlen(name);
    size_t offset = 0;
    while (offset < result->response_headers_length) {
        const char *line = result->response_headers + offset;
        const char *newline = memchr(
            line, '\n', result->response_headers_length - offset);
        size_t length = newline == NULL
            ? result->response_headers_length - offset
            : (size_t) (newline - line);
        const char *colon = memchr(line, ':', length);
        if (colon != NULL && (size_t) (colon - line) == name_length
            && strncasecmp(line, name, name_length) == 0) {
            copy_header_value(output, output_size, colon + 1,
                              length - (size_t) (colon + 1 - line));
            return true;
        }
        offset += length + (newline == NULL ? 0 : 1);
    }
    return false;
}

static bool response_header_single_value(
    const FetchResult *result, const char *name, char *output,
    size_t output_size, bool *present)
{
    if (output == NULL || output_size == 0 || present == NULL) return false;
    output[0] = '\0';
    *present = false;
    if (result == NULL || name == NULL) return false;
    size_t name_length = strlen(name);
    size_t offset = 0;
    while (offset < result->response_headers_length) {
        const char *line = result->response_headers + offset;
        const char *newline = memchr(
            line, '\n', result->response_headers_length - offset);
        size_t length = newline == NULL
            ? result->response_headers_length - offset
            : (size_t) (newline - line);
        const char *colon = memchr(line, ':', length);
        if (colon != NULL && (size_t) (colon - line) == name_length
            && strncasecmp(line, name, name_length) == 0) {
            /* These policy fields are singletons. A combined or duplicated
               value is invalid and therefore contributes no authority. */
            if (*present) return false;
            *present = true;
            const char *value = colon + 1;
            size_t value_length = length - (size_t) (value - line);
            while (value_length != 0
                   && (*value == ' ' || *value == '\t')) {
                value++;
                value_length--;
            }
            while (value_length != 0
                   && (value[value_length - 1] == '\r'
                       || value[value_length - 1] == '\n'
                       || isspace((unsigned char)
                                  value[value_length - 1]))) {
                value_length--;
            }
            if (value_length == 0 || value_length >= output_size) {
                return false;
            }
            memcpy(output, value, value_length);
            output[value_length] = '\0';
        }
        offset += length + (newline == NULL ? 0 : 1);
    }
    return true;
}

static bool resource_final_same_site(
    const TilefinchRequestContext *context, const char *response_url)
{
    if (context == NULL || context->initiator_opaque
        || context->initiator_url == NULL || response_url == NULL) {
        return false;
    }
    char initiator_site[TILEFINCH_ORIGIN_SERIALIZED_LIMIT];
    char response_site[TILEFINCH_ORIGIN_SERIALIZED_LIMIT];
    return tilefinch_url_site_key(
               context->initiator_url, initiator_site,
               sizeof(initiator_site))
        && tilefinch_url_site_key(
               response_url, response_site, sizeof(response_site))
        && strcmp(initiator_site, response_site) == 0;
}

static bool fetch_resource_grant_create_internal(
    const FetchResult *result, const TilefinchRequestContext *context,
    const TilefinchResourceGrant *previous_grant,
    bool cors_validated, bool mime_matches_destination,
    bool require_strict_mime, TilefinchResourceGrant *grant,
    TilefinchResourceDeniedReason *denied_reason)
{
    if (grant != NULL) memset(grant, 0, sizeof(*grant));
    if (denied_reason != NULL) {
        *denied_reason = TILEFINCH_RESOURCE_DENIED_CONTEXT;
    }
    const char *response_url = result == NULL ? NULL : result->effective_url;
    TilefinchUrl parsed_response;
    if (result == NULL || context == NULL || grant == NULL
        || !tilefinch_request_context_valid(context)
        || response_url == NULL || response_url[0] == '\0'
        || !tilefinch_url_parse(response_url, &parsed_response)) {
        return false;
    }
    TilefinchRequestContext final_context = *context;
    final_context.target_url = response_url;
    if (!tilefinch_request_context_valid(&final_context)) return false;

    bool same_origin = tilefinch_request_same_origin(&final_context);
    bool same_site = resource_final_same_site(&final_context, response_url);
    if (final_context.mode == TILEFINCH_REQUEST_MODE_SAME_ORIGIN
        && !same_origin) return false;
    if (final_context.mode == TILEFINCH_REQUEST_MODE_CORS
        && !same_origin && !cors_validated) return false;
    if ((result->security.version
             == FETCH_RESPONSE_SECURITY_METADATA_VERSION
         && result->security.snapshot_truncated)
        || (result->security.version == 0
            && result->response_security_headers_truncated)) {
        if (denied_reason != NULL) {
            *denied_reason =
                TILEFINCH_RESOURCE_DENIED_TRUNCATED_SECURITY_HEADER;
        }
        return false;
    }

    bool typed = result->security.version
        == FETCH_RESPONSE_SECURITY_METADATA_VERSION;
    bool nosniff_present = false;
    bool nosniff = false;
    bool nosniff_invalid = false;
    if (typed) {
        nosniff_present = result->security.nosniff_state
            != FETCH_SECURITY_FIELD_ABSENT;
        nosniff_invalid = nosniff_present
            && result->security.nosniff_state != FETCH_SECURITY_FIELD_VALID;
        nosniff = nosniff_present ? result->security.nosniff
            : previous_grant != NULL && previous_grant->nosniff;
    } else {
        char nosniff_value[32];
        bool valid = response_header_single_value(
            result, "x-content-type-options", nosniff_value,
            sizeof(nosniff_value), &nosniff_present);
        nosniff_invalid = nosniff_present
            && (!valid || strcasecmp(nosniff_value, "nosniff") != 0);
        nosniff = nosniff_present ? !nosniff_invalid
            : previous_grant != NULL && previous_grant->nosniff;
    }
    if (nosniff_invalid) {
        if (denied_reason != NULL) {
            *denied_reason =
                TILEFINCH_RESOURCE_DENIED_MALFORMED_SECURITY_HEADER;
        }
        return false;
    }

    bool corp_present = false;
    bool corp_invalid = false;
    TilefinchCrossOriginResourcePolicy corp = TILEFINCH_CORP_UNSPECIFIED;
    if (typed) {
        corp_present = result->security.corp_state
            != FETCH_SECURITY_FIELD_ABSENT;
        corp_invalid = corp_present
            && result->security.corp_state != FETCH_SECURITY_FIELD_VALID;
        if (!corp_invalid && corp_present) corp = result->security.corp;
    } else {
        char corp_value[32];
        bool valid = response_header_single_value(
            result, "cross-origin-resource-policy", corp_value,
            sizeof(corp_value), &corp_present);
        if (!valid && corp_present) corp_invalid = true;
        else if (valid && corp_present) {
            if (strcasecmp(corp_value, "same-origin") == 0) {
                corp = TILEFINCH_CORP_SAME_ORIGIN;
            } else if (strcasecmp(corp_value, "same-site") == 0) {
                corp = TILEFINCH_CORP_SAME_SITE;
            } else if (strcasecmp(corp_value, "cross-origin") == 0) {
                corp = TILEFINCH_CORP_CROSS_ORIGIN;
            } else {
                corp_invalid = true;
            }
        }
    }
    if (corp_invalid) {
        if (denied_reason != NULL) {
            *denied_reason =
                TILEFINCH_RESOURCE_DENIED_MALFORMED_SECURITY_HEADER;
        }
        return false;
    } else if (previous_grant != NULL) {
        if (!corp_present) corp = previous_grant->corp;
    }

    bool subresource = final_context.destination
            != TILEFINCH_DESTINATION_DOCUMENT
        && final_context.destination != TILEFINCH_DESTINATION_FRAME
        && final_context.destination != TILEFINCH_DESTINATION_FETCH;
    bool cors_authorized = same_origin
        || (final_context.mode == TILEFINCH_REQUEST_MODE_CORS
            && cors_validated);
    bool corp_allowed = true;
    if (subresource && !cors_authorized) {
        if (corp == TILEFINCH_CORP_SAME_ORIGIN) {
            corp_allowed = same_origin;
        } else if (corp == TILEFINCH_CORP_SAME_SITE) {
            corp_allowed = same_site;
        }
    }
    if (!corp_allowed) {
        if (denied_reason != NULL) {
            *denied_reason = TILEFINCH_RESOURCE_DENIED_CORP;
        }
        return false;
    }

    bool strict_mime = require_strict_mime
        || (nosniff
            && (final_context.destination == TILEFINCH_DESTINATION_SCRIPT
                || final_context.destination
                       == TILEFINCH_DESTINATION_STYLE));
    if (strict_mime && !mime_matches_destination) {
        if (denied_reason != NULL) {
            *denied_reason = TILEFINCH_RESOURCE_DENIED_MIME;
        }
        return false;
    }

    *grant = (TilefinchResourceGrant) {
        .destination = final_context.destination,
        .mode = final_context.mode,
        .credentials = final_context.credentials,
        .corp = corp,
        .initiator_opaque = final_context.initiator_opaque,
        .final_same_origin = same_origin,
        .final_same_site = same_site,
        .cors_validated = cors_authorized,
        .nosniff = nosniff,
        .mime_validated = strict_mime
    };
    if (denied_reason != NULL) {
        *denied_reason = TILEFINCH_RESOURCE_DENIED_NONE;
    }
    return true;
}

bool fetch_resource_grant_create(
    const FetchResult *result, const TilefinchRequestContext *context,
    bool cors_validated, bool mime_matches_destination,
    bool require_strict_mime, TilefinchResourceGrant *grant,
    TilefinchResourceDeniedReason *denied_reason)
{
    return fetch_resource_grant_create_internal(
        result, context, NULL, cors_validated, mime_matches_destination,
        require_strict_mime, grant, denied_reason);
}

bool fetch_resource_grant_revalidate_304(
    const FetchResult *result, const TilefinchRequestContext *context,
    const TilefinchResourceGrant *previous_grant, bool cors_validated,
    bool mime_matches_destination, bool require_strict_mime,
    TilefinchResourceGrant *grant,
    TilefinchResourceDeniedReason *denied_reason)
{
    if (result == NULL || result->status_code != 304
        || previous_grant == NULL || context == NULL
        || previous_grant->destination != context->destination
        || previous_grant->mode != context->mode
        || previous_grant->credentials != context->credentials
        || previous_grant->initiator_opaque != context->initiator_opaque) {
        if (grant != NULL) memset(grant, 0, sizeof(*grant));
        if (denied_reason != NULL) {
            *denied_reason = TILEFINCH_RESOURCE_DENIED_CONTEXT;
        }
        return false;
    }
    return fetch_resource_grant_create_internal(
        result, context, previous_grant, cors_validated,
        mime_matches_destination, require_strict_mime, grant,
        denied_reason);
}

typedef struct {
    char allow_origin[TILEFINCH_ORIGIN_SERIALIZED_LIMIT];
    char allow_credentials[16];
    size_t origin_fields;
    size_t credential_fields;
} FetchCorsResponsePolicy;

static bool fetch_cors_response_policy(
    const FetchResult *result, FetchCorsResponsePolicy *policy)
{
    if (result == NULL || policy == NULL) return false;
    memset(policy, 0, sizeof(*policy));
    size_t offset = 0;
    while (offset < result->response_headers_length) {
        const char *line = result->response_headers + offset;
        const char *newline = memchr(
            line, '\n', result->response_headers_length - offset);
        size_t length = newline == NULL
            ? result->response_headers_length - offset
            : (size_t) (newline - line);
        const char *colon = memchr(line, ':', length);
        if (colon != NULL) {
            const char *value = colon + 1;
            const char *limit = line + length;
            while (value < limit
                   && (*value == ' ' || *value == '\t')) value++;
            while (limit > value
                   && (limit[-1] == ' ' || limit[-1] == '\t'
                       || limit[-1] == '\r')) limit--;
            size_t name_length = (size_t) (colon - line);
            size_t value_length = (size_t) (limit - value);
            char *output = NULL;
            size_t capacity = 0;
            if (header_name_equals(
                    line, name_length, "access-control-allow-origin")) {
                policy->origin_fields++;
                output = policy->allow_origin;
                capacity = sizeof(policy->allow_origin);
            } else if (header_name_equals(
                           line, name_length,
                           "access-control-allow-credentials")) {
                policy->credential_fields++;
                output = policy->allow_credentials;
                capacity = sizeof(policy->allow_credentials);
            }
            if (output != NULL) {
                if (value_length == 0 || value_length >= capacity) {
                    return false;
                }
                memcpy(output, value, value_length);
                output[value_length] = '\0';
            }
        }
        offset += length + (newline == NULL ? 0 : 1);
    }
    return true;
}

bool fetch_cors_response_allows(const FetchResult *result,
                                const char *request_origin,
                                bool credentials_included)
{
    if (request_origin == NULL || request_origin[0] == '\0'
        || result == NULL) {
        return false;
    }
    if (result->security.version
            == FETCH_RESPONSE_SECURITY_METADATA_VERSION) {
        if (result->security.allow_origin_state
                != FETCH_SECURITY_FIELD_VALID) return false;
        if (credentials_included) {
            return strcmp(result->security.allow_origin, request_origin) == 0
                && result->security.allow_credentials_state
                       == FETCH_SECURITY_FIELD_VALID
                && result->security.allow_credentials;
        }
        return strcmp(result->security.allow_origin, "*") == 0
            || strcmp(result->security.allow_origin, request_origin) == 0;
    }
    if (result->response_security_headers_truncated) return false;
    FetchCorsResponsePolicy policy;
    if (!fetch_cors_response_policy(result, &policy)
        || policy.origin_fields != 1) return false;
    if (credentials_included) {
        return strcmp(policy.allow_origin, request_origin) == 0
            && policy.credential_fields == 1
            && strcmp(policy.allow_credentials, "true") == 0;
    }
    return strcmp(policy.allow_origin, "*") == 0
        || strcmp(policy.allow_origin, request_origin) == 0;
}

static bool cors_response_allows_with_cached_304(
    const FetchResult *result, const char *request_origin,
    bool credentials_included, bool cached_response_validated)
{
    if (!cached_response_validated || result == NULL
        || result->status_code != 304) {
        return fetch_cors_response_allows(
            result, request_origin, credentials_included);
    }
    if (request_origin == NULL || request_origin[0] == '\0') return false;
    if (result->security.version
            == FETCH_RESPONSE_SECURITY_METADATA_VERSION) {
        FetchSecurityFieldState origin =
            result->security.allow_origin_state;
        FetchSecurityFieldState credentials =
            result->security.allow_credentials_state;
        bool origin_allowed = origin == FETCH_SECURITY_FIELD_ABSENT;
        if (origin == FETCH_SECURITY_FIELD_VALID) {
            origin_allowed = strcmp(
                    result->security.allow_origin, request_origin) == 0
                || (!credentials_included
                    && strcmp(result->security.allow_origin, "*") == 0);
        } else if (origin != FETCH_SECURITY_FIELD_ABSENT) {
            return false;
        }
        if (!origin_allowed || !credentials_included) return origin_allowed;
        return credentials == FETCH_SECURITY_FIELD_ABSENT
            || (credentials == FETCH_SECURITY_FIELD_VALID
                && result->security.allow_credentials);
    }
    if (result->response_security_headers_truncated) return false;
    FetchCorsResponsePolicy policy;
    if (!fetch_cors_response_policy(result, &policy)) return false;

    bool origin_allowed = policy.origin_fields == 0;
    if (policy.origin_fields == 1) {
        origin_allowed = strcmp(policy.allow_origin, request_origin) == 0
            || (!credentials_included
                && strcmp(policy.allow_origin, "*") == 0);
    }
    if (!origin_allowed) return false;
    if (!credentials_included) return true;
    /* An omitted ACAC inherits the cached validated field. If the 304 sends
       one, it replaces that field in the combined response and must itself be
       the single exact credential token. */
    return policy.credential_fields == 0
        || (policy.credential_fields == 1
            && strcmp(policy.allow_credentials, "true") == 0);
}

static void select_referrer_policy(const char *header,
                                   char output[
                                       FETCH_REFERRER_POLICY_LIMIT])
{
    snprintf(output, FETCH_REFERRER_POLICY_LIMIT, "%s",
             "strict-origin-when-cross-origin");
    if (header != NULL) {
        (void) update_referrer_policy_tokens(
            header, strlen(header), output);
    }
}

bool fetch_response_referrer_policy(
    const FetchResult *result, bool *header_present,
    char policy[FETCH_REFERRER_POLICY_LIMIT])
{
    if (result == NULL || header_present == NULL || policy == NULL) {
        return false;
    }
    *header_present = false;
    policy[0] = '\0';
    if (result->security.version
            == FETCH_RESPONSE_SECURITY_METADATA_VERSION) {
        if (!referrer_policy_normalized(result->security.referrer_policy)) {
            return false;
        }
        *header_present = result->security.referrer_policy_state
            != FETCH_SECURITY_FIELD_ABSENT;
        snprintf(policy, FETCH_REFERRER_POLICY_LIMIT, "%s",
                 result->security.referrer_policy);
        return true;
    }
    /* v8 replay records and a few synthetic unit-test results predate the
       direct metadata. Preserve their bounded snapshot behavior without
       allowing a modern live/v9 result to silently fall back after overflow. */
    size_t offset = 0;
    while (offset < result->response_headers_length) {
        const char *line = result->response_headers + offset;
        const char *newline = memchr(
            line, '\n', result->response_headers_length - offset);
        size_t length = newline == NULL
            ? result->response_headers_length - offset
            : (size_t) (newline - line);
        const char *colon = memchr(line, ':', length);
        if (colon != NULL
            && header_name_equals(
                line, (size_t) (colon - line), "referrer-policy")) {
            *header_present = true;
            (void) update_referrer_policy_tokens(
                colon + 1, length - (size_t) (colon + 1 - line), policy);
        }
        offset += length + (newline == NULL ? 0 : 1);
    }
    return true;
}

bool fetch_compute_referrer(const char *document_url,
                            const char *target_url,
                            const char *policy,
                            char *output, size_t output_size)
{
    if (document_url == NULL || target_url == NULL || output == NULL
        || output_size == 0) return false;
    output[0] = '\0';
    char selected[FETCH_REFERRER_POLICY_LIMIT];
    select_referrer_policy(policy, selected);
    bool same_origin = tilefinch_url_same_origin(document_url, target_url);
    bool downgrade = tilefinch_url_is_downgrade(document_url, target_url);
    bool full = false, origin = false;
    if (strcmp(selected, "no-referrer") == 0) {
        return true;
    } else if (strcmp(selected, "same-origin") == 0) {
        full = same_origin;
    } else if (strcmp(selected, "origin") == 0) {
        origin = true;
    } else if (strcmp(selected, "strict-origin") == 0) {
        origin = !downgrade;
    } else if (strcmp(selected, "origin-when-cross-origin") == 0) {
        full = same_origin;
        origin = !same_origin;
    } else if (strcmp(selected,
                      "strict-origin-when-cross-origin") == 0) {
        full = same_origin;
        origin = !same_origin && !downgrade;
    } else if (strcmp(selected, "unsafe-url") == 0) {
        full = true;
    } else {
        full = !downgrade;
    }
    if (full) {
        char normalized[TILEFINCH_URL_SERIALIZED_LIMIT];
        if (!tilefinch_url_normalize(document_url, normalized,
                                  sizeof(normalized))) return false;
        char *fragment = strchr(normalized, '#');
        if (fragment != NULL) *fragment = '\0';
        size_t length = strlen(normalized);
        if (length >= output_size) return false;
        memcpy(output, normalized, length + 1);
        return true;
    } else if (origin) {
        char serialized[TILEFINCH_ORIGIN_SERIALIZED_LIMIT];
        if (!tilefinch_url_origin(document_url, serialized,
                               sizeof(serialized))) return false;
        size_t length = strlen(serialized);
        if (length + 1 >= output_size) return false;
        memcpy(output, serialized, length);
        output[length++] = '/';
        output[length] = '\0';
        return true;
    } else {
        return true;
    }
}

static size_t receive_header(char *data, size_t size, size_t count,
                             void *opaque)
{
    WriteContext *context = opaque;
    if (size != 0 && count > SIZE_MAX / size) return 0;
    size_t length = size * count;
    const char *colon = memchr(data, ':', length);
    if (colon == NULL) {
        if (length >= 5 && strncasecmp(data, "HTTP/", 5) == 0) {
            reset_response_metadata(context->result);
            /* The negotiated protocol version is readable only while this
               transfer still owns its connection, so sample it here rather
               than from the completed handle. */
            fetch_result_capture_tls_version(context->easy, context->result);
            if (context->metrics != NULL) {
                context->metrics->download_total_bytes = 0;
                context->metrics->downloaded_bytes = 0;
            }
            context->response_cookie_start =
                context->result->set_cookie_count;
            context->response_cookies_truncated = false;
            context->has_response_location = false;
            context->redirect_response = false;
            context->redirect_callback_aborted = false;
            context->interim_response = false;
            const char *space = memchr(data, ' ', length);
            if (space != NULL) {
                context->result->status_code = strtol(space + 1, NULL, 10);
            }
            context->interim_response =
                context->result->status_code >= 100
                && context->result->status_code < 200;
            context->headers_delivered = false;
        } else if ((length == 2 && data[0] == '\r' && data[1] == '\n')
                   || (length == 1 && data[0] == '\n')) {
            if (context->easy != NULL) {
                char *effective = NULL, *content_type = NULL;
                (void) curl_easy_getinfo(
                    context->easy, CURLINFO_RESPONSE_CODE,
                    &context->result->status_code);
                (void) curl_easy_getinfo(
                    context->easy, CURLINFO_EFFECTIVE_URL, &effective);
                (void) curl_easy_getinfo(
                    context->easy, CURLINFO_CONTENT_TYPE, &content_type);
                write_context_set_effective_url(context, effective);
                if (content_type != NULL) {
                    snprintf(context->result->content_type,
                             sizeof(context->result->content_type), "%s",
                             content_type);
                }
            }
            /* 1xx fields describe only the informational response. They
               must never enter the page-visible result or influence a
               later redirect. The transport never enables libcurl's cookie
               engine, so discarding the staged field is sufficient. */
            if (context->interim_response) {
                fetch_result_discard_cookies_from(
                    context->result, context->response_cookie_start);
                reset_response_metadata(context->result);
                context->has_response_location = false;
                context->redirect_response = false;
                context->response_cookies_truncated = false;
                context->headers_delivered = false;
                return length;
            }
            const char *cookie_url = fetch_hop_wire_url(context->hop);
            if (!remember_response_cookie_urls(
                    context, cookie_url == NULL
                               ? context->result->effective_url : cookie_url)) {
                return 0;
            }
            long status = context->result->status_code;
            context->redirect_response = !context->single_hop
                && (status == 301 || status == 302 || status == 303
                    || status == 307 || status == 308)
                && context->has_response_location;
            if (context->response_cookies_truncated) {
                if (context->redirect_response) {
                    context->cookie_header_rejected = true;
                    snprintf(
                        context->result->error,
                        sizeof(context->result->error),
                        "redirect response exceeded the %u-cookie transport limit",
                        (unsigned) FETCH_RESPONSE_COOKIE_CAPACITY);
                    return 0;
                }
                context->result->set_cookies_truncated = true;
            }
            if (!enforce_response_cors_policy(context)) return 0;
            if (!enforce_response_credential_policy(context)) return 0;
            if (context->redirect_response) {
                context->redirect_callback_aborted = true;
                return 0;
            }
            if (!context->redirect_response && !context->interim_response
                && context->stream != NULL
                && context->stream->on_headers != NULL
                && !context->stream->on_headers(context->stream->opaque,
                                                 context->result)) {
                context->consumer_failed = true;
                return 0;
            }
            context->headers_delivered = true;
            if (context->metrics != NULL) {
                context->metrics->headers_delivered = true;
            }
        }
        return length;
    }
    size_t name_length = (size_t) (colon - data);
    const char *value = colon + 1;
    size_t value_length = length - name_length - 1;
    FetchResult *result = context->result;
    bool forbidden = (name_length == 10
                      && strncasecmp(data, "set-cookie", 10) == 0)
                     || (name_length == 11
                         && strncasecmp(data, "set-cookie2", 11) == 0);
    const char *trimmed = value;
    size_t trimmed_length = value_length;
    while (trimmed_length != 0
           && (*trimmed == ' ' || *trimmed == '\t')) {
        trimmed++; trimmed_length--;
    }
    while (trimmed_length != 0
           && (trimmed[trimmed_length - 1] == '\r'
               || trimmed[trimmed_length - 1] == '\n'
               || isspace((unsigned char) trimmed[trimmed_length - 1]))) {
        trimmed_length--;
    }
    size_t required = name_length + 2 + trimmed_length + 1;
    bool stored = false;
    if (!forbidden && name_length != 0
        && required < sizeof(result->response_headers)
                      - result->response_headers_length) {
        size_t at = result->response_headers_length;
        for (size_t i = 0; i < name_length; i++) {
            result->response_headers[at++] =
                (char) tolower((unsigned char) data[i]);
        }
        result->response_headers[at++] = ':';
        result->response_headers[at++] = ' ';
        memcpy(result->response_headers + at, trimmed, trimmed_length);
        at += trimmed_length;
        result->response_headers[at++] = '\n';
        result->response_headers[at] = '\0';
        result->response_headers_length = at;
        stored = true;
    }
    (void) fetch_response_security_metadata_collect(
        &result->security, data, name_length, trimmed, trimmed_length, stored);
    snprintf(result->response_referrer_policy,
             sizeof(result->response_referrer_policy), "%s",
             result->security.referrer_policy);
    result->response_referrer_policy_header_present =
        result->security.referrer_policy_state != FETCH_SECURITY_FIELD_ABSENT;
    result->response_security_headers_truncated =
        result->security.snapshot_truncated;
    if (name_length == 4 && strncasecmp(data, "etag", 4) == 0) {
        copy_header_value(result->etag, sizeof(result->etag), value,
                          value_length);
    } else if (name_length == 13
               && strncasecmp(data, "last-modified", 13) == 0) {
        copy_header_value(result->last_modified,
                          sizeof(result->last_modified), value,
                          value_length);
    } else if (name_length == 8
               && strncasecmp(data, "location", 8) == 0) {
        /* Presence is semantically distinct from a missing field.  An empty
           Location is a valid relative reference to the current URL. */
        context->has_response_location = true;
    } else if (name_length == 10
               && strncasecmp(data, "set-cookie", 10) == 0) {
        if (trimmed_length >= FETCH_SET_COOKIE_LIMIT) {
            context->cookie_header_rejected = true;
            snprintf(result->error, sizeof(result->error), "%s",
                     "response Set-Cookie exceeded 4095 bytes");
            return 0;
        }
        if (result->set_cookie_count >= FETCH_RESPONSE_COOKIE_CAPACITY) {
            /* Keep parsing so a later malformed or oversized field still
               fails.  Header completion decides whether this bounded loss is
               safe to expose (final response) or must reject (redirect). */
            context->response_cookies_truncated = true;
            return length;
        }
        char *copy = budget_malloc(result->budget, trimmed_length + 1);
        if (copy == NULL) {
            context->allocation_failed = true;
            context->cookie_header_rejected = true;
            snprintf(result->error, sizeof(result->error), "%s",
                     "response Set-Cookie exceeded shared memory budget");
            return 0;
        }
        memcpy(copy, trimmed, trimmed_length);
        copy[trimmed_length] = '\0';
        result->set_cookies[result->set_cookie_count] = copy;
        result->set_cookie_count++;
    } else if (name_length == 12
               && strncasecmp(data, "cf-mitigated", 12) == 0) {
        copy_header_value(result->cf_mitigated,
                          sizeof(result->cf_mitigated), value, value_length);
    } else if (name_length == 9
               && strncasecmp(data, "accept-ch", 9) == 0) {
        if (!append_header_list_value(
                result->accept_ch, sizeof(result->accept_ch),
                value, value_length)) {
            context->client_hint_header_rejected = true;
            snprintf(result->error, sizeof(result->error), "%s",
                     "combined Accept-CH response header exceeded 1023 bytes");
            return 0;
        }
    } else if (name_length == 11
               && strncasecmp(data, "critical-ch", 11) == 0) {
        if (!append_header_list_value(
                result->critical_ch, sizeof(result->critical_ch),
                value, value_length)) {
            context->client_hint_header_rejected = true;
            snprintf(result->error, sizeof(result->error), "%s",
                     "combined Critical-CH response header exceeded 1023 bytes");
            return 0;
        }
    } else if (name_length == 6
               && strncasecmp(data, "server", 6) == 0) {
        copy_header_value(result->server, sizeof(result->server), value,
                          value_length);
    } else if (name_length == 6
               && strncasecmp(data, "cf-ray", 6) == 0) {
        copy_header_value(result->cf_ray, sizeof(result->cf_ray), value,
                          value_length);
    }
    return length;
}

static bool redirect_callback_failed(const WriteContext *context)
{
    return context == NULL
        || context->cancelled
        || context->limit_exceeded
        || context->allocation_failed
        || context->consumer_failed
        || context->capture_failed
        || context->cookie_header_rejected
        || context->client_hint_header_rejected
        || context->cors_rejected
        || context->result == NULL
        || context->result->error[0] != '\0';
}

static int transfer_progress(void *opaque, curl_off_t download_total,
                             curl_off_t downloaded, curl_off_t upload_total,
                             curl_off_t uploaded)
{
    (void) upload_total; (void) uploaded;
    WriteContext *context = opaque;
    if (context->metrics != NULL) {
        if (download_total > 0) {
            uint64_t total = (uint64_t) download_total;
            context->metrics->download_total_bytes =
                total > SIZE_MAX ? SIZE_MAX : (size_t) total;
        }
        if (downloaded > 0) {
            uint64_t received = (uint64_t) downloaded;
            context->metrics->downloaded_bytes =
                received > SIZE_MAX ? SIZE_MAX : (size_t) received;
        }
    }
    if (context->cancel != NULL && context->cancel(context->cancel_opaque)) {
        context->cancelled = true;
        return 1;
    }
    return 0;
}

static bool enforce_response_credential_policy(WriteContext *context)
{
    if (context == NULL
        || context->credentials == FETCH_CREDENTIALS_INCLUDE) return true;
    const char *response_url = context->result->effective_url;
    bool response_eligible =
        context->credentials == FETCH_CREDENTIALS_SAME_ORIGIN
        && context->credential_origin != NULL
        && context->credential_origin[0] != '\0'
        && !context->credentials_tainted
        && response_url[0] != '\0'
        && tilefinch_url_same_origin(context->credential_origin, response_url);
    if (response_eligible) return true;
    context->credentials_tainted = true;
    fetch_result_discard_cookies_from(
        context->result, context->response_cookie_start);
    return true;
}

struct FetchHopState {
    Budget *budget;
    const FetchRequest *base;
    /* HSTS is session authority, not cookie mutation state. Scheduled fetches
       discard their live cookie-session field after snapshotting cookies, so
       retain this engine-lifetime pointer separately for redirect admission. */
    BrowserSession *security_session;
    /* url is the fragmentless HTTP/cache identity; logical_url is the
       normalized browser-visible URL whose fragment follows redirect rules. */
    char *url;
    char *logical_url;
    char *initiator;
    char *method;
    char *extra_headers;
    char *referer;
    char *referrer_source;
    char referrer_policy[FETCH_REFERRER_POLICY_LIMIT];
    char *origin;
    char *fetch_site;
    BrowserCookieOverlay *cookie_overlay;
    char *cookie_initiator;
    char *cookie_top_level;
    TilefinchRequestMode cookie_mode;
    TilefinchCredentialsMode cookie_credentials;
    TilefinchRequestDestination cookie_destination;
    bool cookie_top_level_navigation;
    bool cookie_user_activated;
    bool cookie_initiator_opaque;
    char cookie_header[4096];
    size_t redirects;
    uint64_t deadline_ns;
    bool body_enabled;
    bool fetch_site_none;
    bool origin_tainted;
    bool credentials_tainted;
    bool client_hints_tainted;
    FetchPrivateNetworkGuard private_network;
};

static const char *fetch_hop_logical_url(const FetchHopState *hop)
{
    return hop == NULL ? NULL : hop->logical_url;
}

static const char *fetch_hop_wire_url(const FetchHopState *hop)
{
    return hop == NULL ? NULL : hop->url;
}

static bool fetch_hop_cors_response_required(const FetchHopState *hop)
{
    return hop != NULL && hop->base != NULL && hop->base->enforce_cors
        && (hop->origin_tainted
            || !tilefinch_url_same_origin(hop->initiator, hop->url));
}

static bool fetch_hop_credentials_included(const FetchHopState *hop)
{
    if (hop == NULL || hop->base == NULL) return true;
    return hop->base->credentials == FETCH_CREDENTIALS_INCLUDE
        || (hop->base->credentials == FETCH_CREDENTIALS_SAME_ORIGIN
            && !hop->credentials_tainted);
}

static bool enforce_response_cors_policy(WriteContext *context)
{
    if (context == NULL || context->result == NULL) return false;
    FetchHopState *hop = context->hop;
    if (hop == NULL) return true;
    context->result->redirect_origin_tainted = hop->origin_tainted;
    if (!fetch_hop_cors_response_required(hop)) return true;
    bool cached_response_validated = hop->base != NULL
        && hop->base->cors_cached_response_validated
        && context->result->status_code == 304
        && !hop->origin_tainted && hop->redirects == 0;
    if (cors_response_allows_with_cached_304(
            context->result, hop->origin,
            fetch_hop_credentials_included(hop),
            cached_response_validated)) return true;
    context->cors_rejected = true;
    snprintf(context->result->error, sizeof(context->result->error),
             "CORS response denied by access-control policy");
    return false;
}

static char *hop_copy(Budget *budget, const char *value, size_t limit)
{
    if (value == NULL || value[0] == '\0') return NULL;
    size_t length = strlen(value);
    if (length > limit) return NULL;
    char *copy = budget_malloc(budget, length + 1);
    if (copy != NULL) memcpy(copy, value, length + 1);
    return copy;
}

static char *hop_normalized_url(Budget *budget, const char *value)
{
    if (budget == NULL || value == NULL) return NULL;
    char *copy = budget_malloc(budget, TILEFINCH_URL_SERIALIZED_LIMIT);
    if (copy == NULL) return NULL;
    if (!tilefinch_url_normalize(
            value, copy, TILEFINCH_URL_SERIALIZED_LIMIT)) {
        budget_free(budget, copy);
        return NULL;
    }
    size_t length = strlen(copy);
    char *trimmed = budget_realloc(budget, copy, length + 1);
    return trimmed == NULL ? copy : trimmed;
}
