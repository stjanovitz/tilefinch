static void scheduled_capture_finish(FetchScheduler *scheduler,
                                     ScheduledFetch *item,
                                     bool external_cancel);

static char *scheduler_copy(Budget *budget, const char *value, size_t limit)
{
    if (value == NULL) return NULL;
    size_t length = strlen(value);
    if (length > limit) return NULL;
    char *copy = budget_malloc(budget, length + 1);
    if (copy != NULL) memcpy(copy, value, length + 1);
    return copy;
}

static char *scheduler_copy_bytes(Budget *budget, const char *value,
                                  size_t length, size_t limit)
{
    if (value == NULL) return NULL;
    if (length > limit) return NULL;
    char *copy = budget_malloc(budget, length + 1);
    if (copy != NULL) {
        if (length != 0) memcpy(copy, value, length);
        copy[length] = '\0';
    }
    return copy;
}

/* Trace records key the logical fetch invocation, so redirects remain one
   record.  For replay, derive the same initial wire Cookie without retaining
   the caller's BrowserSession or TilefinchRequestContext. */
static bool scheduled_trace_request_shape(
    const char *url, const FetchRequest *source, FetchRequest *shape,
    char *cookie, size_t cookie_capacity)
{
    if (shape == NULL || cookie == NULL || cookie_capacity == 0) return false;
    cookie[0] = '\0';
    if (source == NULL) return true;
    *shape = *source;
    shape->cookie = NULL;
    shape->cookie_session = NULL;
    shape->cookie_context = NULL;
    if (source->credentials == FETCH_CREDENTIALS_OMIT
        || (source->credentials == FETCH_CREDENTIALS_SAME_ORIGIN
            && !tilefinch_url_same_origin(source->credential_origin, url))) {
        return true;
    }
    if (source->cookie_session != NULL) {
        const TilefinchRequestContext *context = source->cookie_context;
        TilefinchRequestContext secured_context;
        char target[TILEFINCH_URL_SERIALIZED_LIMIT];
        const char *method = source->method == NULL ? "GET" : source->method;
        TilefinchCredentialsMode expected =
            source->credentials == FETCH_CREDENTIALS_OMIT
                ? TILEFINCH_CREDENTIALS_OMIT
                : (source->credentials == FETCH_CREDENTIALS_SAME_ORIGIN
                       ? TILEFINCH_CREDENTIALS_SAME_ORIGIN
                       : TILEFINCH_CREDENTIALS_INCLUDE);
        if (context == NULL) return false;
        secured_context = *context;
        secured_context.target_url = url;
        if (!tilefinch_url_request_key(
                   secured_context.target_url, target, sizeof(target))
            || strcmp(target, url) != 0
            || strcasecmp(
                   context->method == NULL ? "GET" : context->method,
                   method) != 0
            || context->credentials != expected
            || !browser_session_cookie_header_context(
                   source->cookie_session, &secured_context,
                   cookie, cookie_capacity)) {
            return false;
        }
    } else if (source->cookie != NULL) {
        size_t length = strlen(source->cookie);
        if (length >= cookie_capacity) return false;
        memcpy(cookie, source->cookie, length + 1);
    }
    shape->cookie = cookie[0] == '\0' ? NULL : cookie;
    return true;
}

static void fetch_scheduler_domain_release_slot(
    FetchSchedulerDomain *domain)
{
    if (domain != NULL && domain->active_slots != 0) {
        domain->active_slots--;
    }
}

static void scheduled_fetch_clear(FetchScheduler *scheduler,
                                  ScheduledFetch **slot)
{
    if (scheduler == NULL || slot == NULL || *slot == NULL) return;
    ScheduledFetch *item = *slot;
    if (item->background_request_id != 0) {
        (void) fetch_background_transport_cancel(
            item->background_request_id, "scheduler request released");
        item->background_request_id = 0;
    }
    if (scheduler != NULL && item != NULL && item->active
        && fetch_trace.mode == FETCH_TRACE_CAPTURE
        && item->has_trace_sequence && !item->capture_finalized) {
        item->success = false;
        snprintf(item->result.error, sizeof(item->result.error),
                 "request cancelled during scheduler teardown");
        scheduled_capture_finish(scheduler, item, true);
    }
    if (item->easy != NULL) {
        (void) curl_multi_remove_handle(scheduler->multi, item->easy);
        curl_easy_cleanup(item->easy);
    }
    curl_slist_free_all(item->headers);
    fetch_hop_destroy(item->hop);
    if (item->writer.capture_body != NULL) {
        (void) fclose(item->writer.capture_body);
        item->writer.capture_body = NULL;
    }
    budget_free(scheduler->budget, item->writer.capture_body_path);
    item->writer.capture_body_path = NULL;
    if (item->replay_file != NULL) {
        (void) fclose(item->replay_file);
        item->replay_file = NULL;
    }
    budget_free(scheduler->budget, item->writer.pending);
    budget_free(scheduler->budget, item->replay_buffer);
    fetch_result_destroy(&item->result);
    budget_free(scheduler->budget, item->client_hint_origin);
    budget_free(scheduler->budget, item->client_hint_tokens);
    budget_free(scheduler->budget, item->fetch_site);
    budget_free(scheduler->budget, item->if_none_match);
    budget_free(scheduler->budget, item->if_modified_since);
    budget_free(scheduler->budget, item->user_agent);
    budget_free(scheduler->budget, item->credential_origin);
    budget_free(scheduler->budget, item->referrer_policy);
    budget_free(scheduler->budget, item->referrer_source);
    budget_free(scheduler->budget, item->initiator_url);
    budget_free(scheduler->budget, item->page_top_level_url);
    budget_free(scheduler->budget, item->fetch_mode);
    budget_free(scheduler->budget, item->fetch_dest);
    budget_free(scheduler->budget, item->accept);
    budget_free(scheduler->budget, item->origin);
    budget_free(scheduler->budget, item->referer);
    budget_free(scheduler->budget, item->trace_cookie);
    budget_free(scheduler->budget, item->cookie);
    budget_free(scheduler->budget, item->extra_headers);
    budget_free(scheduler->budget, item->content_type);
    budget_free(scheduler->budget, item->body);
    budget_free(scheduler->budget, item->method);
    budget_free(scheduler->budget, item->logical_url);
    budget_free(scheduler->budget, item->url);
    if (scheduler->reserved_bytes >= item->reserved_bytes) {
        scheduler->reserved_bytes -= item->reserved_bytes;
    }
    bool domain_slot_reserved = item->domain_slot_reserved;
    budget_free(scheduler->budget, item);
    *slot = NULL;
    if (domain_slot_reserved) {
        fetch_scheduler_domain_release_slot(scheduler->domain);
    }
}

FetchSchedulerDomain *fetch_scheduler_domain_create(
    Budget *budget, size_t maximum_active_slots)
{
    if (budget == NULL || maximum_active_slots == 0
        || maximum_active_slots == SIZE_MAX) return NULL;
    FetchSchedulerDomain *domain = budget_calloc(
        budget, 1, sizeof(*domain));
    if (domain == NULL) return NULL;
    domain->budget = budget;
    domain->references = 1;
    domain->maximum_active_slots = maximum_active_slots;
    return domain;
}

FetchSchedulerDomain *fetch_scheduler_domain_retain(
    FetchSchedulerDomain *domain)
{
    if (domain == NULL || domain->references == 0
        || domain->references == SIZE_MAX) return NULL;
    domain->references++;
    return domain;
}

void fetch_scheduler_domain_release(FetchSchedulerDomain *domain)
{
    if (domain == NULL || domain->references == 0) return;
    domain->references--;
    if (domain->references == 0) {
        Budget *budget = domain->budget;
        budget_free(budget, domain);
    }
}

bool fetch_scheduler_domain_metrics(
    const FetchSchedulerDomain *domain,
    FetchSchedulerDomainMetrics *metrics)
{
    if (domain == NULL || metrics == NULL) return false;
    *metrics = (FetchSchedulerDomainMetrics) {
        .maximum_active_slots = domain->maximum_active_slots,
        .active_slots = domain->active_slots,
        .peak_active_slots = domain->peak_active_slots,
        .rejected_enqueues = domain->rejected_enqueues,
        .active_views = domain->active_views,
        .peak_active_views = domain->peak_active_views
    };
    return true;
}

static void fetch_scheduler_set_error(FetchScheduler *scheduler,
                                      const char *message)
{
    if (scheduler == NULL) return;
    snprintf(scheduler->last_error, sizeof(scheduler->last_error), "%s",
             message == NULL ? "" : message);
}

static void fetch_scheduler_copy_error(char *error, size_t error_size,
                                       const char *message)
{
    if (error == NULL || error_size == 0) return;
    snprintf(error, error_size, "%s", message == NULL ? "" : message);
}

static FetchScheduler *fetch_scheduler_create_in_domain_ex(
    FetchSchedulerDomain *domain, size_t maximum_concurrent,
    size_t maximum_reserved_bytes, char *error, size_t error_size)
{
    fetch_scheduler_copy_error(error, error_size, "");
    if (domain == NULL || domain->budget == NULL
        || maximum_concurrent == 0
        || maximum_concurrent > FETCH_SCHEDULER_LIMIT
        || maximum_reserved_bytes == 0
        || maximum_reserved_bytes == SIZE_MAX) {
        fetch_scheduler_copy_error(
            error, error_size, "invalid scheduler limits");
        return NULL;
    }
    if (!curl_global_acquire(domain->budget)) {
        fetch_scheduler_copy_error(
            error, error_size,
            curl_global_error[0] == '\0'
                ? "network runtime or curl memory reservation unavailable"
                : curl_global_error);
        return NULL;
    }
    Budget *budget = domain->budget;
    FetchScheduler *scheduler = budget_calloc(budget, 1,
                                              sizeof(*scheduler));
    CURLM *multi = curl_multi_init();
    if (scheduler == NULL || multi == NULL
        || fetch_scheduler_domain_retain(domain) == NULL) {
        fetch_scheduler_copy_error(
            error, error_size,
            scheduler == NULL ? "scheduler state exceeded page memory"
            : (multi == NULL ? "curl multi handle exceeded network memory"
                             : "scheduler domain retain failed"));
        if (multi != NULL) curl_multi_cleanup(multi);
        budget_free(budget, scheduler);
        curl_global_release();
        return NULL;
    }
#if defined(TILEFINCH_PSP_HTTP2)
    if (curl_multi_setopt(
            multi, CURLMOPT_PIPELINING, CURLPIPE_MULTIPLEX) != CURLM_OK) {
        fetch_scheduler_copy_error(
            error, error_size, "curl HTTP/2 multiplex setup failed");
        fetch_scheduler_domain_release(domain);
        curl_multi_cleanup(multi);
        budget_free(budget, scheduler);
        curl_global_release();
        return NULL;
    }
#endif
    scheduler->budget = budget;
    scheduler->multi = multi;
    scheduler->domain = domain;
    scheduler->capacity = maximum_concurrent;
    scheduler->maximum_reserved_bytes = maximum_reserved_bytes;
    scheduler->next_id = 1;
    scheduler->global_acquired = true;
    domain->active_views++;
    if (domain->active_views > domain->peak_active_views) {
        domain->peak_active_views = domain->active_views;
    }
    return scheduler;
}

FetchScheduler *fetch_scheduler_create_in_domain(
    FetchSchedulerDomain *domain, size_t maximum_concurrent,
    size_t maximum_reserved_bytes)
{
    return fetch_scheduler_create_in_domain_ex(
        domain, maximum_concurrent, maximum_reserved_bytes, NULL, 0);
}

FetchScheduler *fetch_scheduler_create(Budget *budget,
                                       size_t maximum_concurrent,
                                       size_t maximum_reserved_bytes)
{
    return fetch_scheduler_create_ex(
        budget, maximum_concurrent, maximum_reserved_bytes, NULL, 0);
}

FetchScheduler *fetch_scheduler_create_ex(
    Budget *budget, size_t maximum_concurrent,
    size_t maximum_reserved_bytes, char *error, size_t error_size)
{
    fetch_scheduler_copy_error(error, error_size, "");
    if (budget == NULL || maximum_concurrent == 0
        || maximum_concurrent > FETCH_SCHEDULER_LIMIT
        || maximum_reserved_bytes == 0
        || maximum_reserved_bytes == SIZE_MAX) {
        fetch_scheduler_copy_error(
            error, error_size, "invalid scheduler limits");
        return NULL;
    }
    FetchSchedulerDomain *domain = fetch_scheduler_domain_create(
        budget, maximum_concurrent);
    if (domain == NULL) {
        fetch_scheduler_copy_error(
            error, error_size, "scheduler domain exceeded page memory");
        return NULL;
    }
    FetchScheduler *scheduler = fetch_scheduler_create_in_domain_ex(
        domain, maximum_concurrent, maximum_reserved_bytes,
        error, error_size);
    fetch_scheduler_domain_release(domain);
    return scheduler;
}

bool fetch_scheduler_enable_background_transport(
    FetchScheduler *scheduler, bool enabled)
{
    if (scheduler == NULL || scheduler->pending != 0
        || scheduler->reserved_bytes != 0) return false;
    if (enabled
        && (!fetch_background_transport_available()
            || !fetch_background_transport_initialize(
                   scheduler->budget))) return false;
    if (enabled && scheduler->background_chunk == NULL) {
        scheduler->background_chunk_capacity =
            FETCH_BACKGROUND_STREAM_BUFFER_BYTES;
        scheduler->background_chunk = budget_malloc_category(
            scheduler->budget, BUDGET_CATEGORY_RESOURCE,
            scheduler->background_chunk_capacity);
        if (scheduler->background_chunk == NULL) {
            scheduler->background_chunk_capacity = 0;
            return false;
        }
    }
    scheduler->background_transport_enabled = enabled;
    return true;
}

static bool scheduled_fetch_prepare_failure(
    ScheduledFetch *item, const char *stage, CURLcode code)
{
    if (item == NULL) return false;
    BudgetConcurrentPoolMetrics metrics = {0};
    (void) budget_concurrent_pool_metrics(
        &curl_concurrent_pool, &metrics);
    snprintf(
        item->result.error, sizeof(item->result.error),
        "curl setup failed at %s code=%d pool=%zu/%zu failures=%zu",
        stage == NULL ? "unknown" : stage, (int) code,
        metrics.current, metrics.limit, metrics.failure_count);
    return false;
}

static FetchRequest scheduled_background_hop_shape(
    ScheduledFetch *item)
{
    FetchHopState *hop = item == NULL ? NULL : item->hop;
    const FetchRequest *base = item == NULL ? NULL : &item->request;
    return (FetchRequest) {
        .method = hop == NULL ? NULL : hop->method,
        .body = hop != NULL && hop->body_enabled ? item->body : NULL,
        .body_length = hop != NULL && hop->body_enabled
            ? item->request.body_length : 0,
        .content_type = hop != NULL && hop->body_enabled
            ? item->content_type : NULL,
        .extra_headers = hop == NULL ? NULL : hop->extra_headers,
        .cookie = hop == NULL || hop->cookie_header[0] == '\0'
            ? NULL : hop->cookie_header,
        .if_none_match = base == NULL ? NULL : base->if_none_match,
        .if_modified_since = base == NULL ? NULL : base->if_modified_since,
        .allow_http_errors = base != NULL && base->allow_http_errors,
        .send_client_hints = base != NULL && base->send_client_hints
            && hop != NULL && !hop->client_hints_tainted,
        .client_hint_tokens = base == NULL ? NULL : base->client_hint_tokens,
        .client_hint_origin = base == NULL ? NULL : base->client_hint_origin,
        .send_low_client_hints = base != NULL
            && base->send_low_client_hints,
        .referer = hop == NULL ? NULL : hop->referer,
        .origin = hop == NULL ? NULL : hop->origin,
        .accept = base == NULL ? NULL : base->accept,
        .sec_fetch_dest = base == NULL ? NULL : base->sec_fetch_dest,
        .sec_fetch_mode = base == NULL ? NULL : base->sec_fetch_mode,
        .sec_fetch_site = hop == NULL ? NULL : hop->fetch_site,
        .sec_fetch_user = base != NULL && base->sec_fetch_user,
        .upgrade_insecure_requests = base != NULL
            && base->upgrade_insecure_requests,
        .identity_encoding = base != NULL && base->identity_encoding,
        .user_agent = base == NULL ? NULL : base->user_agent,
        .connect_timeout_ms = base == NULL ? 0 : base->connect_timeout_ms,
        .block_private_network = base != NULL
            && base->block_private_network,
        /* The cookie/referrer/CORS policies have already produced the fixed
           wire shape above. The worker is a single-hop byte transport and
           receives no browser-owned policy or session pointers. */
        .credentials = FETCH_CREDENTIALS_INCLUDE
    };
}

static bool scheduled_background_hop_supported(ScheduledFetch *item)
{
    if (item == NULL || !item->background_allowed || item->hop == NULL
        || fetch_trace.mode != FETCH_TRACE_OFF
        || !fetch_background_transport_available()
        || !fetch_background_transport_initialize(item->result.budget)
        || !fetch_hop_configure_cookie(
               NULL, item->hop, &item->request,
               item->hop->redirects == 0)) return false;
    FetchRequest shape = scheduled_background_hop_shape(item);
    long remaining_ms = fetch_hop_remaining_ms(item->hop);
    return remaining_ms > 0
        && fetch_background_transport_hop_supported(
               item->hop->url, &shape, item->reserved_bytes,
               remaining_ms);
}

static bool scheduled_background_start_hop(ScheduledFetch *item)
{
    if (item == NULL || !item->background_backend
        || item->background_request_id != 0 || item->hop == NULL) {
        return false;
    }
    if (!fetch_hop_configure_cookie(
            NULL, item->hop, &item->request,
            item->hop->redirects == 0)) return false;
    FetchRequest shape = scheduled_background_hop_shape(item);
    long remaining_ms = fetch_hop_remaining_ms(item->hop);
    if (remaining_ms <= 0) {
        item->result.timed_out = true;
        snprintf(item->result.error, sizeof(item->result.error), "%s",
                 "request deadline expired before background hop");
        return false;
    }
    item->background_request_id =
        fetch_background_transport_enqueue_hop_stream(
            item->hop->url, &shape, item->reserved_bytes, remaining_ms);
    return item->background_request_id != 0;
}

static bool scheduled_fetch_prepare(FetchScheduler *scheduler,
                                    ScheduledFetch *item, const char *url,
                                    const char *logical_url,
                                    const FetchRequest *source,
                                    size_t maximum_bytes, long timeout_ms)
{
    bool diagnostic_mobile_safari =
        getenv("TILEFINCH_DIAGNOSTIC_MOBILE_SAFARI") != NULL;
    const char *method = source == NULL || source->method == NULL
                         ? "GET" : source->method;
    item->url = scheduler_copy(scheduler->budget, url, 4095);
    item->logical_url = scheduler_copy(
        scheduler->budget, logical_url, 4095);
    item->method = scheduler_copy(scheduler->budget, method, 16);
    item->body = scheduler_copy_bytes(
        scheduler->budget, source == NULL ? NULL : source->body,
        source == NULL ? 0 : source->body_length, 64u * 1024u);
    item->content_type = scheduler_copy(
        scheduler->budget, source == NULL ? NULL : source->content_type, 255);
    item->extra_headers = scheduler_copy(
        scheduler->budget, source == NULL ? NULL : source->extra_headers,
        FETCH_EXTRA_HEADERS_MAX);
    item->cookie = scheduler_copy(
        scheduler->budget, source == NULL ? NULL : source->cookie, 4095);
    item->referer = scheduler_copy(
        scheduler->budget, source == NULL ? NULL : source->referer, 4095);
    item->origin = scheduler_copy(
        scheduler->budget, source == NULL ? NULL : source->origin, 2047);
    item->accept = scheduler_copy(
        scheduler->budget, source == NULL ? NULL : source->accept, 255);
    item->fetch_dest = scheduler_copy(
        scheduler->budget, source == NULL ? NULL : source->sec_fetch_dest, 63);
    item->fetch_mode = scheduler_copy(
        scheduler->budget, source == NULL ? NULL : source->sec_fetch_mode, 63);
    item->fetch_site = scheduler_copy(
        scheduler->budget, source == NULL ? NULL : source->sec_fetch_site, 63);
    item->client_hint_tokens = scheduler_copy(
        scheduler->budget,
        source == NULL ? NULL : source->client_hint_tokens, 1023);
    item->client_hint_origin = scheduler_copy(
        scheduler->budget,
        source == NULL ? NULL : source->client_hint_origin, 2047);
    item->if_none_match = scheduler_copy(
        scheduler->budget, source == NULL ? NULL : source->if_none_match, 191);
    item->if_modified_since = scheduler_copy(
        scheduler->budget, source == NULL ? NULL
                                          : source->if_modified_since, 127);
    item->user_agent = scheduler_copy(
        scheduler->budget, source == NULL ? NULL : source->user_agent, 511);
    item->credential_origin = scheduler_copy(
        scheduler->budget,
        source == NULL ? NULL : source->credential_origin, 2047);
    item->initiator_url = scheduler_copy(
        scheduler->budget,
        source == NULL ? NULL : source->initiator_url, 4095);
    item->page_top_level_url = scheduler_copy(
        scheduler->budget,
        source == NULL || source->page_context == NULL
            ? NULL : source->page_context->top_level_url,
        4095);
    item->referrer_source = scheduler_copy(
        scheduler->budget,
        source == NULL ? NULL : source->referrer_source, 4095);
    item->referrer_policy = scheduler_copy(
        scheduler->budget,
        source == NULL ? NULL : source->referrer_policy, 63);
    if (item->url == NULL || item->logical_url == NULL
        || item->method == NULL
        || (source != NULL && source->body != NULL && item->body == NULL)
        || (source != NULL && source->content_type != NULL
            && item->content_type == NULL)
        || (source != NULL && source->extra_headers != NULL
            && item->extra_headers == NULL)
        || (source != NULL && source->cookie != NULL && item->cookie == NULL)
        || (source != NULL && source->referer != NULL
            && item->referer == NULL)
        || (source != NULL && source->origin != NULL
            && item->origin == NULL)
        || (source != NULL && source->accept != NULL
            && item->accept == NULL)
        || (source != NULL && source->sec_fetch_dest != NULL
            && item->fetch_dest == NULL)
        || (source != NULL && source->sec_fetch_mode != NULL
            && item->fetch_mode == NULL)
        || (source != NULL && source->sec_fetch_site != NULL
            && item->fetch_site == NULL)
        || (source != NULL && source->client_hint_tokens != NULL
            && item->client_hint_tokens == NULL)
        || (source != NULL && source->client_hint_origin != NULL
            && item->client_hint_origin == NULL)
        || (source != NULL && source->if_none_match != NULL
            && item->if_none_match == NULL)
        || (source != NULL && source->if_modified_since != NULL
            && item->if_modified_since == NULL)
        || (source != NULL && source->user_agent != NULL
            && item->user_agent == NULL)
        || (source != NULL && source->credential_origin != NULL
            && item->credential_origin == NULL)
        || (source != NULL && source->initiator_url != NULL
            && item->initiator_url == NULL)
        || (source != NULL && source->page_context != NULL
            && source->page_context->top_level_url != NULL
            && item->page_top_level_url == NULL)
        || (source != NULL && source->referrer_source != NULL
            && item->referrer_source == NULL)
        || (source != NULL && source->referrer_policy != NULL
            && item->referrer_policy == NULL)) {
        return scheduled_fetch_prepare_failure(
            item, "request-copy", CURLE_OUT_OF_MEMORY);
    }
    if (source != NULL && source->page_context != NULL) {
        item->page_context = *source->page_context;
        item->page_context.target_url = item->url;
        item->page_context.initiator_url = item->initiator_url;
        item->page_context.top_level_url = item->page_top_level_url;
        item->page_context.method = item->method;
    }
    item->request = (FetchRequest) {
        .method = item->method, .body = item->body,
        .body_length = item->body == NULL || source == NULL
                       ? 0 : source->body_length,
        .content_type = item->content_type,
        .extra_headers = item->extra_headers, .cookie = item->cookie,
        .cors_preflight = source != NULL && source->cors_preflight,
        .allow_http_errors = source != NULL && source->allow_http_errors,
        .send_client_hints = source != NULL && source->send_client_hints,
        .client_hint_tokens = item->client_hint_tokens,
        .client_hint_origin = item->client_hint_origin,
        .send_low_client_hints = source != NULL
                                 && source->send_low_client_hints,
        .referer = item->referer, .origin = item->origin,
        .accept = item->accept,
        .sec_fetch_dest = item->fetch_dest,
        .sec_fetch_mode = item->fetch_mode,
        .sec_fetch_site = item->fetch_site,
        .sec_fetch_user = source != NULL && source->sec_fetch_user,
        .upgrade_insecure_requests =
            source != NULL && source->upgrade_insecure_requests,
        .identity_encoding = source != NULL && source->identity_encoding,
        .connect_timeout_ms = source == NULL ? 0 : source->connect_timeout_ms,
        .block_private_network = source != NULL
            && fetch_request_private_network_protected(source, url),
        .if_none_match = item->if_none_match,
        .if_modified_since = item->if_modified_since,
        .user_agent = item->user_agent,
        .credentials = source == NULL ? FETCH_CREDENTIALS_INCLUDE
                                      : source->credentials,
        .credential_origin = item->credential_origin,
        .initiator_url = item->initiator_url,
        .referrer_source = item->referrer_source,
        .referrer_policy = item->referrer_policy,
        .enforce_cors = source != NULL && source->enforce_cors,
        .cors_response_check_deferred = source != NULL
            && source->cors_response_check_deferred,
        .redirect_same_origin_only =
            source != NULL && source->redirect_same_origin_only,
        .redirect_policy = source == NULL
            ? FETCH_REDIRECT_DEFAULT : source->redirect_policy,
        .redirect_url_validator = source == NULL
            ? NULL : source->redirect_url_validator,
        .cors_cached_response_validated =
            source != NULL && source->cors_cached_response_validated,
        .cookie_session = source == NULL ? NULL : source->cookie_session,
        .cookie_context = source == NULL ? NULL : source->cookie_context,
        .page_context = source == NULL || source->page_context == NULL
            ? NULL : &item->page_context,
        .content_security_policy = source == NULL
            ? NULL : source->content_security_policy,
        .content_blocker = source == NULL ? NULL
            : (source->content_blocker != NULL
                   ? source->content_blocker
                   : (source->cookie_session == NULL ? NULL
                       : source->cookie_session->content_blocker)),
        .prepared_page_version = source == NULL
            ? 0 : source->prepared_page_version
    };
    item->security_session = source == NULL
        ? NULL : (BrowserSession *) source->cookie_session;
    item->result.budget = scheduler->budget;
    item->writer = (WriteContext) {
        .result = &item->result,
        .maximum_bytes = maximum_bytes,
        .credentials = item->request.credentials,
        .credential_origin = item->credential_origin,
        .diagnostic_mobile_safari = diagnostic_mobile_safari
    };
    item->hop = fetch_hop_create(
        scheduler->budget, item->url, item->logical_url,
        &item->request, timeout_ms);
    if (item->hop == NULL) {
        return scheduled_fetch_prepare_failure(
            item, "request-policy", CURLE_OUT_OF_MEMORY);
    }
    item->hop->private_network.required =
        item->request.block_private_network;
    /* The hop owns a cookie snapshot and copied immutable context. Do not
       retain a caller's possibly stack-backed TilefinchRequestContext. */
    item->request.cookie_session = NULL;
    item->request.cookie_context = NULL;
    item->writer.hop = item->hop;
    item->writer.credentials_tainted = item->hop->credentials_tainted;
    if (scheduled_background_hop_supported(item)) {
        item->background_backend = true;
        /* A full descriptor is transient backpressure, not permission to run
           curl on the browser thread. Later pumps retry the fixed worker
           queue while preserving the scheduler/domain reservation. */
        (void) scheduled_background_start_hop(item);
        return true;
    }
    item->easy = curl_easy_init();
    if (item->easy == NULL) {
        return scheduled_fetch_prepare_failure(
            item, "easy-init", CURLE_OUT_OF_MEMORY);
    }
    item->writer.easy = item->easy;
#if defined(TILEFINCH_CURL_IMPERSONATE)
    if (curl_easy_impersonate(item->easy,
                              TILEFINCH_CURL_IMPERSONATE_TARGET, 1)
        != CURLE_OK) return false;
#endif
    if (!curl_easy_attach_shared_state(item->easy)) {
        return scheduled_fetch_prepare_failure(
            item, "shared-state", CURLE_FAILED_INIT);
    }
    if (!fetch_configure_tls(item->easy)) {
        return scheduled_fetch_prepare_failure(
            item, "tls", CURLE_FAILED_INIT);
    }
    if (!fetch_configure_http_version(item->easy)) {
        return scheduled_fetch_prepare_failure(
            item, "http-version", CURLE_FAILED_INIT);
    }
    if (!fetch_configure_private_network(
            item->easy, &item->hop->private_network)
        || curl_easy_setopt(item->easy, CURLOPT_URL, item->url) != CURLE_OK
        || curl_easy_setopt(
               item->easy, CURLOPT_FOLLOWLOCATION, 0L) != CURLE_OK
        || curl_easy_setopt(
               item->easy,
               CURLOPT_SUPPRESS_CONNECT_HEADERS, 1L) != CURLE_OK
        || curl_easy_setopt(
               item->easy, TILEFINCH_CURL_PROTOCOLS_OPT,
               TILEFINCH_CURL_HTTP_PROTOCOLS) != CURLE_OK
        || curl_easy_setopt(
               item->easy, TILEFINCH_CURL_REDIR_PROTOCOLS_OPT,
               TILEFINCH_CURL_HTTP_PROTOCOLS) != CURLE_OK
        || curl_easy_setopt(
               item->easy, CURLOPT_ACCEPT_ENCODING,
               item->request.identity_encoding
                   ? "identity"
                   : fetch_accept_encoding(
                         diagnostic_mobile_safari)) != CURLE_OK
        || curl_easy_setopt(
               item->easy, CURLOPT_USERAGENT,
               fetch_effective_user_agent(
                   &item->request,
                   diagnostic_mobile_safari)) != CURLE_OK
        || curl_easy_setopt(
               item->easy, CURLOPT_CONNECTTIMEOUT_MS,
               fetch_connect_timeout_ms(
                   &item->request, timeout_ms)) != CURLE_OK
        || curl_easy_setopt(
               item->easy, CURLOPT_TIMEOUT_MS, timeout_ms) != CURLE_OK
        || curl_easy_setopt(
               item->easy, CURLOPT_NOSIGNAL, 1L) != CURLE_OK
        || curl_easy_setopt(
               item->easy, CURLOPT_FAILONERROR,
               item->request.allow_http_errors ? 0L : 1L) != CURLE_OK
        || curl_easy_setopt(
               item->easy, CURLOPT_ERRORBUFFER, item->error) != CURLE_OK
        || curl_easy_setopt(
               item->easy, CURLOPT_WRITEFUNCTION, receive_data) != CURLE_OK
        || curl_easy_setopt(
               item->easy, CURLOPT_WRITEDATA, &item->writer) != CURLE_OK
        || curl_easy_setopt(
               item->easy, CURLOPT_HEADERFUNCTION,
               receive_header) != CURLE_OK
        || curl_easy_setopt(
               item->easy, CURLOPT_HEADERDATA, &item->writer) != CURLE_OK
        || curl_easy_setopt(
               item->easy, CURLOPT_PRIVATE, item) != CURLE_OK) {
        return scheduled_fetch_prepare_failure(
            item, "easy-options", CURLE_FAILED_INIT);
    }
    if (!fetch_hop_configure_cookie(
            item->easy, item->hop, &item->request, true)) {
        return scheduled_fetch_prepare_failure(
            item, "cookie", CURLE_FAILED_INIT);
    }
    if (!configure_easy_method(
            item->easy, item->method, &item->request, true)) {
        return scheduled_fetch_prepare_failure(
            item, "method", CURLE_FAILED_INIT);
    }
    if (item->referer != NULL && item->referer[0] != '\0'
        && curl_easy_setopt(
               item->easy, CURLOPT_REFERER, item->referer) != CURLE_OK) {
        return scheduled_fetch_prepare_failure(
            item, "referer", CURLE_FAILED_INIT);
    }
    if (!append_request_headers(
            scheduler->budget, &item->headers, &item->request,
            item->url, item->origin, item->fetch_site,
            item->hop->cookie_header, item->extra_headers,
            true, false,
            diagnostic_mobile_safari)) {
        return scheduled_fetch_prepare_failure(
            item, "headers", CURLE_OUT_OF_MEMORY);
    }
    if (curl_easy_setopt(item->easy, CURLOPT_HTTPHEADER, item->headers)
        != CURLE_OK) {
        return scheduled_fetch_prepare_failure(
            item, "header-list", CURLE_FAILED_INIT);
    }
    if (curl_multi_add_handle(scheduler->multi, item->easy) != CURLM_OK) {
        return scheduled_fetch_prepare_failure(
            item, "multi-add", CURLE_FAILED_INIT);
    }
    return true;
}

static uint64_t fetch_scheduler_enqueue_internal(
    FetchScheduler *scheduler, const char *url, const FetchRequest *request,
    size_t maximum_bytes, long timeout_ms,
    const FetchStreamOptions *stream)
{
    char normalized_logical_url[TILEFINCH_URL_SERIALIZED_LIMIT];
    char canonical_url[TILEFINCH_URL_SERIALIZED_LIMIT];
    const char *logical_url = normalized_logical_url;
    bool diagnostic_mobile_safari =
        getenv("TILEFINCH_DIAGNOSTIC_MOBILE_SAFARI") != NULL;
    if (scheduler == NULL) return 0;
    fetch_scheduler_set_error(scheduler, "");
    if (url == NULL || maximum_bytes == 0 || maximum_bytes == SIZE_MAX
        || timeout_ms <= 0) {
        fetch_scheduler_set_error(scheduler, "invalid request limits");
        return 0;
    }
    if (!fetch_request_validate(request, NULL)) {
        fetch_scheduler_set_error(scheduler, "request policy is invalid");
        return 0;
    }
    if (!tilefinch_url_normalize(
            url, normalized_logical_url, sizeof(normalized_logical_url))
        || !tilefinch_url_request_key(
               normalized_logical_url, canonical_url,
               sizeof(canonical_url))) {
        fetch_scheduler_set_error(scheduler, "request URL is invalid");
        return 0;
    }
    if (!page_request_target_matches(request, canonical_url)) {
        fetch_scheduler_set_error(
            scheduler, "page request context target mismatch");
        return 0;
    }
    /* Security preparation first snapshots its input into a private
       normalized buffer, so using the logical URL as both input and output
       is supported.  Avoid keeping a third 2 KiB URL alive in this already
       request-envelope-heavy frame. */
    if (!fetch_request_prepare_security_url(
            request, normalized_logical_url, true, normalized_logical_url)
        || !tilefinch_url_request_key(
               normalized_logical_url, canonical_url,
               sizeof(canonical_url))) {
        fetch_scheduler_set_error(scheduler, "request security upgrade failed");
        return 0;
    }
    if (!fetch_request_target_allowed(request, canonical_url)) {
        fetch_scheduler_set_error(scheduler, "request target is not allowed");
        return 0;
    }
    if (stream != NULL
        && (stream->on_body == NULL || stream->chunk_bytes > 65536
            || stream->irregular_max_chunk_bytes > 65536)) {
        fetch_scheduler_set_error(
            scheduler, "stream callback limits are invalid");
        return 0;
    }
    if (maximum_bytes > scheduler->maximum_reserved_bytes
                              - scheduler->reserved_bytes) {
        fetch_scheduler_set_error(
            scheduler, "response reservation exceeds scheduler byte bound");
        return 0;
    }
    if (fetch_origin_blocked(canonical_url)
        || fetch_request_content_blocked(request, canonical_url)) {
        fetch_scheduler_set_error(scheduler, "request blocked by policy");
        return 0;
    }
    url = canonical_url;
    FetchRequest replay_shape;
    char replay_cookie[FETCH_SET_COOKIE_LIMIT];
    const FetchRequest *trace_request = request;
    if (fetch_trace.mode == FETCH_TRACE_REPLAY && request != NULL) {
        if (!scheduled_trace_request_shape(
                url, request, &replay_shape,
                replay_cookie, sizeof(replay_cookie))) return 0;
        trace_request = &replay_shape;
    }
    ScheduledFetch **item_slot = NULL;
    for (size_t i = 0; i < scheduler->capacity; i++) {
        if (scheduler->items[i] == NULL) {
            item_slot = &scheduler->items[i];
            break;
        }
    }
    if (item_slot == NULL) {
        fetch_scheduler_set_error(scheduler, "scheduler request slots are full");
        return 0;
    }
    FetchSchedulerDomain *domain = scheduler->domain;
    if (domain == NULL
        || domain->active_slots >= domain->maximum_active_slots) {
        if (domain != NULL && domain->rejected_enqueues != SIZE_MAX) {
            domain->rejected_enqueues++;
        }
        fetch_scheduler_set_error(
            scheduler, "shared page request slots are full");
        return 0;
    }
    ScheduledFetch *item = budget_calloc(
        scheduler->budget, 1, sizeof(*item));
    if (item == NULL) {
        fetch_scheduler_set_error(
            scheduler, "request state exceeded page memory");
        return 0;
    }
    *item_slot = item;
    domain->active_slots++;
    if (domain->active_slots > domain->peak_active_slots) {
        domain->peak_active_slots = domain->active_slots;
    }
    item->domain_slot_reserved = true;
    item->id = scheduler->next_id++;
    if (item->id == 0) item->id = scheduler->next_id++;
    item->reserved_bytes = maximum_bytes;
    item->background_allowed =
        scheduler->background_transport_enabled;
    scheduler->reserved_bytes += maximum_bytes;
    item->result.budget = scheduler->budget;
    if (stream != NULL) {
        item->streaming = true;
        item->stream = *stream;
    }
    if (tilefinch_fetch_consume_injected_failure(&item->result)) {
        item->success = false;
        item->complete = true;
        return item->id;
    }
    if (fetch_trace.mode == FETCH_TRACE_REPLAY) {
        item->writer = (WriteContext) {
            .result = &item->result,
            .maximum_bytes = maximum_bytes,
            .stream = item->streaming ? &item->stream : NULL,
            .metrics = item->streaming ? &item->stream_metrics : NULL,
            .pump_quota_enabled = true,
            .defer_replay_body = true
        };
        item->replay_source_success = trace_replay_response(
            scheduler->budget,
            request == NULL || request->method == NULL
                ? "GET" : request->method,
            logical_url, trace_request, fetch_effective_user_agent(
                              trace_request, diagnostic_mobile_safari),
            diagnostic_mobile_safari,
            maximum_bytes, &item->result, &item->writer,
            &item->replay_terminal_matched);
        item->replay_file = item->writer.deferred_replay_body;
        item->replay_length = item->writer.deferred_replay_length;
        item->writer.deferred_replay_body = NULL;
        item->writer.deferred_replay_length = 0;
        if (item->replay_source_success) {
            size_t capacity = item->streaming
                ? (item->stream.irregular_seed != 0
                    ? item->stream.irregular_max_chunk_bytes
                    : item->stream.chunk_bytes)
                : 16384u;
            if (capacity == 0) capacity = 16384;
            if (capacity > item->replay_length
                && item->replay_length != 0) {
                capacity = item->replay_length;
            }
            if (capacity == 0) capacity = 1;
            item->replay_buffer = budget_malloc(scheduler->budget, capacity);
            if (item->replay_buffer == NULL) {
                item->replay_source_success = false;
                snprintf(item->result.error, sizeof(item->result.error),
                         "replay body buffer exceeded shared memory budget");
            } else {
                item->replay_buffer_capacity = capacity;
                if (item->streaming) {
                    item->stream_metrics.peak_buffered_bytes = capacity;
                }
            }
        }
        item->success = item->replay_source_success;
        item->replay_retained_external_cancel =
            fetch_trace.replay_response_keyed
            && item->replay_terminal_matched
            && item->result.trace_external_cancel;
        if (item->replay_source_success) {
            item->result.length = 0;
            item->replay_random = item->streaming
                ? item->stream.irregular_seed : 0;
        }
        item->active = true;
        item->replay_pumps_remaining =
            item->result.trace_external_cancel
                && !item->replay_retained_external_cancel
              ? UINT_MAX
              : (unsigned) (!item->replay_source_success
                            && !item->replay_terminal_matched
                              ? 1
                              : (item->result.trace_delay_pumps == 0
                                  ? 1 : item->result.trace_delay_pumps));
        scheduler->pending++;
        return item->id;
    }
    if (!scheduled_fetch_prepare(
            scheduler, item, url, logical_url, request,
                                 maximum_bytes, timeout_ms)) {
        fetch_scheduler_set_error(
            scheduler,
            item->result.error[0] == '\0'
                ? "curl request setup exceeded policy or memory"
                : item->result.error);
        scheduled_fetch_clear(scheduler, item_slot);
        return 0;
    }
    /* Preparation can fail under the bounded allocator. Count the request
       only after it owns a fully configured easy handle, before any pump can
       put it on the wire. */
    if (!trace_activate_for_request(request, url)) {
        scheduled_fetch_clear(scheduler, item_slot);
        return 0;
    }
    item->writer.pump_quota_enabled = true;
    if (item->streaming) {
        item->writer.stream = &item->stream;
        item->writer.metrics = &item->stream_metrics;
    }
    if (fetch_trace.mode == FETCH_TRACE_CAPTURE) {
        if (item->hop->cookie_header[0] != '\0') {
            item->trace_cookie = scheduler_copy(
                scheduler->budget, item->hop->cookie_header,
                sizeof(item->hop->cookie_header) - 1);
            if (item->trace_cookie == NULL) {
                trace_capture_invalidate(
                    "scheduled Cookie snapshot allocation failed",
                    fetch_trace.sequence);
                scheduled_fetch_clear(scheduler, item_slot);
                return 0;
            }
        }
        size_t sequence = fetch_trace.sequence;
        if (sequence >= FETCH_TRACE_RECORD_LIMIT) {
            trace_capture_invalidate("record limit exceeded", sequence);
            scheduled_fetch_clear(scheduler, item_slot);
            return 0;
        }
        if (item->streaming) {
            item->writer.capture_body_path = budget_malloc(
                scheduler->budget, FETCH_TRACE_PATH_CAPACITY);
            if (item->writer.capture_body_path == NULL) {
                trace_capture_invalidate(
                    "scheduled stream capture path allocation failed",
                    sequence);
                scheduled_fetch_clear(scheduler, item_slot);
                return 0;
            }
            if (!trace_path(item->writer.capture_body_path,
                            FETCH_TRACE_PATH_CAPACITY,
                            sequence, "body")) {
                trace_capture_invalidate(
                    "scheduled stream capture path is too long", sequence);
                scheduled_fetch_clear(scheduler, item_slot);
                return 0;
            }
            item->writer.capture_body = fopen(
                item->writer.capture_body_path, "wb");
            if (item->writer.capture_body == NULL) {
                trace_capture_invalidate(
                    "scheduled stream capture body could not be opened",
                    sequence);
                scheduled_fetch_clear(scheduler, item_slot);
                return 0;
            }
            item->writer.capture_sequence = sequence;
            item->writer.has_capture_sequence = true;
        }
        item->trace_sequence = sequence;
        item->has_trace_sequence = true;
        item->enqueue_pump_generation = scheduler->pump_generation;
        fetch_trace.sequence++;
    }
    item->active = true;
    scheduler->pending++;
    return item->id;
}

uint64_t fetch_scheduler_enqueue(FetchScheduler *scheduler, const char *url,
                                 const FetchRequest *request,
                                 size_t maximum_bytes, long timeout_ms)
{
    return fetch_scheduler_enqueue_internal(
        scheduler, url, request, maximum_bytes, timeout_ms, NULL);
}

uint64_t fetch_scheduler_enqueue_stream(
    FetchScheduler *scheduler, const char *url, const FetchRequest *request,
    size_t maximum_bytes, long timeout_ms,
    const FetchStreamOptions *stream)
{
    return fetch_scheduler_enqueue_internal(
        scheduler, url, request, maximum_bytes, timeout_ms, stream);
}

bool fetch_scheduler_enqueue_would_block(
    const FetchScheduler *scheduler, size_t maximum_bytes)
{
    if (scheduler == NULL || scheduler->domain == NULL
        || maximum_bytes == 0 || maximum_bytes == SIZE_MAX
        || maximum_bytes > scheduler->maximum_reserved_bytes) {
        return false;
    }
    bool has_local_slot = false;
    for (size_t i = 0; i < scheduler->capacity; i++) {
        if (scheduler->items[i] == NULL) {
            has_local_slot = true;
            break;
        }
    }
    bool reservation_busy =
        maximum_bytes > scheduler->maximum_reserved_bytes
                            - scheduler->reserved_bytes;
    bool domain_busy =
        scheduler->domain->active_slots
            >= scheduler->domain->maximum_active_slots;
    bool capacity_busy = !has_local_slot
        || reservation_busy || domain_busy;
    bool work_can_release_capacity = scheduler->pending != 0
        || scheduler->reserved_bytes != 0
        || scheduler->domain->active_slots != 0;
    return capacity_busy && work_can_release_capacity;
}

const char *fetch_scheduler_last_error(const FetchScheduler *scheduler)
{
    return scheduler == NULL ? "scheduler unavailable"
                             : scheduler->last_error;
}

static uint64_t fetch_pump_elapsed_us(uint64_t started_us)
{
    uint64_t now = tilefinch_platform_monotonic_time_us();
    return now >= started_us ? now - started_us : 0;
}

static void scheduled_capture_finish(FetchScheduler *scheduler,
                                     ScheduledFetch *item,
                                     bool external_cancel)
{
    /* A new hop's status line resets per-response metadata. If that hop then
       fails before its header terminator (or cannot be rescheduled), preserve
       the chain-wide redirect taint from the hop state before returning or
       capturing the terminal result. */
    if (item != NULL && item->hop != NULL) {
        item->result.redirect_origin_tainted =
            item->result.redirect_origin_tainted
            || item->hop->origin_tainted;
    }
    if (fetch_trace.mode != FETCH_TRACE_CAPTURE || item->capture_finalized) {
        return;
    }
    item->capture_finalized = true;
    if (item->writer.capture_body != NULL) {
        if (fclose(item->writer.capture_body) != 0) {
            item->writer.capture_failed = true;
            item->success = false;
            snprintf(item->result.error, sizeof(item->result.error),
                     "HTTP stream capture finalization failed");
        }
        item->writer.capture_body = NULL;
    }
    FetchRequest trace_request = item->request;
    trace_request.cookie = item->trace_cookie;
    bool captured = item->has_trace_sequence
        && !item->writer.capture_failed
        && trace_capture_response_at(
            item->trace_sequence, item->method, item->url,
            item->logical_url, &trace_request,
            fetch_effective_user_agent(
                &trace_request, item->writer.diagnostic_mobile_safari),
            item->writer.diagnostic_mobile_safari,
            &item->result, item->success,
            scheduler->pump_generation - item->enqueue_pump_generation,
            external_cancel, item->streaming);
    if (!captured) {
        trace_capture_invalidate(
            "scheduled record write failed",
            item->has_trace_sequence ? item->trace_sequence : 0);
    }
}

static void scheduled_mark_complete(FetchScheduler *scheduler,
                                    ScheduledFetch *item, bool success,
                                    const char *error,
                                    bool external_cancel)
{
    if (item->background_request_id != 0) {
        (void) fetch_background_transport_cancel(
            item->background_request_id,
            error == NULL ? "scheduler request complete" : error);
        item->background_request_id = 0;
    }
    if (item->easy != NULL) {
        (void) curl_multi_remove_handle(scheduler->multi, item->easy);
        curl_easy_cleanup(item->easy);
        item->easy = NULL;
    }
    item->success = success;
    if (!success && error != NULL && error[0] != '\0') {
        snprintf(item->result.error, sizeof(item->result.error), "%s", error);
    }
    scheduled_capture_finish(scheduler, item, external_cancel);
    if (!success) fetch_result_discard_all_cookie_storage(&item->result);
    item->active = false;
    item->complete = true;
    if (scheduler->pending != 0) scheduler->pending--;
}

static bool scheduled_replay_headers(ScheduledFetch *item)
{
    if (item->replay_headers_delivered) return true;
    if (item->stream.on_headers != NULL
        && !item->stream.on_headers(item->stream.opaque, &item->result)) {
        snprintf(item->result.error, sizeof(item->result.error),
                 "stream consumer rejected response headers");
        return false;
    }
    item->replay_headers_delivered = true;
    item->stream_metrics.headers_delivered = true;
    return true;
}

static bool scheduled_replay_body(FetchScheduler *scheduler,
                                  ScheduledFetch *item,
                                  size_t *bytes_remaining,
                                  size_t *callbacks_remaining,
                                  FetchPumpMetrics *pump,
                                  uint64_t started_us,
                                  uint64_t maximum_time_us)
{
    item->writer.result = &item->result;
    item->writer.maximum_bytes = item->reserved_bytes;
    item->writer.stream = item->streaming ? &item->stream : NULL;
    item->writer.metrics = item->streaming ? &item->stream_metrics : NULL;
    item->writer.pump_quota_enabled = true;
    item->writer.pump_bytes_remaining = bytes_remaining;
    item->writer.pump_callbacks_remaining = callbacks_remaining;
    item->writer.pump_metrics = pump;
    while (item->replay_offset < item->replay_length
           && *bytes_remaining != 0 && *callbacks_remaining != 0
           && (maximum_time_us == 0
               || fetch_pump_elapsed_us(started_us) < maximum_time_us)) {
        if (item->stream.cancel_after_bytes != 0
            && item->replay_offset >= item->stream.cancel_after_bytes) {
            item->stream_metrics.cancelled = true;
            snprintf(item->result.error, sizeof(item->result.error),
                     "injected stream cancellation at byte %zu",
                     item->replay_offset);
            return false;
        }
        if (item->stream.truncate_after_bytes != 0
            && item->replay_offset >= item->stream.truncate_after_bytes) {
            item->stream_metrics.truncated = true;
            snprintf(item->result.error, sizeof(item->result.error),
                     "injected truncated response at byte %zu",
                     item->replay_offset);
            return false;
        }
        if (item->replay_chunk_remaining == 0) {
            size_t maximum = item->stream.irregular_seed != 0
                ? item->stream.irregular_max_chunk_bytes
                : item->stream.chunk_bytes;
            if (maximum == 0) maximum = 16384;
            size_t wanted = maximum;
            if (item->stream.irregular_seed != 0) {
                item->replay_random ^= item->replay_random << 13;
                item->replay_random ^= item->replay_random >> 7;
                item->replay_random ^= item->replay_random << 17;
                wanted = 1 + (size_t) (item->replay_random % maximum);
            }
            size_t left = item->replay_length - item->replay_offset;
            item->replay_chunk_remaining = wanted < left ? wanted : left;
        }
        size_t wanted = item->replay_chunk_remaining;
        if (wanted > *bytes_remaining) wanted = *bytes_remaining;
        if (item->stream.cancel_after_bytes > item->replay_offset
            && wanted > item->stream.cancel_after_bytes
                        - item->replay_offset) {
            wanted = item->stream.cancel_after_bytes - item->replay_offset;
        }
        if (item->stream.truncate_after_bytes > item->replay_offset
            && wanted > item->stream.truncate_after_bytes
                        - item->replay_offset) {
            wanted = item->stream.truncate_after_bytes - item->replay_offset;
        }
        if (wanted == 0) continue;
        if (wanted > item->replay_buffer_capacity
            || item->replay_file == NULL
            || fread(item->replay_buffer, 1, wanted,
                     item->replay_file) != wanted) {
            snprintf(item->result.error, sizeof(item->result.error),
                     "HTTP replay body is corrupt");
            return false;
        }
        item->writer.replay_body_hash = trace_body_hash_with_offset(
            (const char *) item->replay_buffer, wanted,
            item->writer.replay_body_hash);
        if (!write_deliver_chunk(
                &item->writer, item->replay_buffer, wanted)) {
            if (item->result.error[0] == '\0') {
                snprintf(item->result.error, sizeof(item->result.error),
                         "stream consumer rejected response body");
            }
            return false;
        }
        item->replay_offset += wanted;
        item->replay_chunk_remaining -= wanted;
        if (item->stream.stall_every_chunks != 0
            && item->replay_offset < item->replay_length
            && item->stream_metrics.chunks_received
                   % item->stream.stall_every_chunks == 0) {
            item->stream_metrics.replay_stalls++;
            if (item->stream.on_stall != NULL
                && !item->stream.on_stall(item->stream.opaque)) {
                snprintf(item->result.error, sizeof(item->result.error),
                         "stream consumer rejected replay stall");
                return false;
            }
        }
    }
    (void) scheduler;
    return true;
}

static bool scheduled_drain_pending(FetchScheduler *scheduler,
                                    ScheduledFetch *item,
                                    size_t *bytes_remaining,
                                    size_t *callbacks_remaining,
                                    FetchPumpMetrics *pump)
{
    WriteContext *writer = &item->writer;
    if (writer->pending_offset >= writer->pending_length) return true;
    if (*bytes_remaining == 0 || *callbacks_remaining == 0) return true;
    size_t wanted = writer->pending_length - writer->pending_offset;
    if (wanted > *bytes_remaining) wanted = *bytes_remaining;
    bool injected = false;
    wanted = stream_injected_boundary(writer, wanted, &injected);
    if (wanted != 0
        && !write_deliver_chunk(
            writer, writer->pending + writer->pending_offset, wanted)) {
        return false;
    }
    writer->pending_offset += wanted;
    if (injected) {
        stream_mark_injected_fault(writer);
        return false;
    }
    if (writer->pending_offset != writer->pending_length) return true;
    budget_free(scheduler->budget, writer->pending);
    writer->pending = NULL;
    writer->pending_offset = 0;
    writer->pending_length = 0;
    (void) pump;
    return true;
}

static bool scheduled_retry_tls12_compatibility(
    FetchScheduler *scheduler, ScheduledFetch *item, CURLcode result)
{
    if (scheduler == NULL || item == NULL || item->easy == NULL
        || item->hop->private_network.blocked
        || item->writer.headers_delivered
        || item->result.received_body_bytes != 0
        || !fetch_try_tls12_compatibility(
               item->easy, result, &item->tls12_retry_attempted,
               fetch_hop_remaining_ms(item->hop), &item->request)) {
        return false;
    }
    if (curl_multi_remove_handle(scheduler->multi, item->easy) != CURLM_OK) {
        return false;
    }
    item->result.tls12_compatibility_retry = true;
    item->error[0] = '\0';
    return curl_multi_add_handle(scheduler->multi, item->easy) == CURLM_OK;
}

/* A bounded pump may accept transport bytes faster than it is allowed to
   expose them to the response consumer.  libcurl can consequently report the
   transfer complete while a suffix is still staged.  Finalizing at that point
   would return a truncated buffered response (or end a stream early), so keep
   the easy handle and completion result alive until later pumps drain it. */
static bool scheduled_finish_transport(FetchScheduler *scheduler,
                                       ScheduledFetch *item,
                                       CURLcode transport_result)
{
    item->transport_complete = false;
    item->result.transport_code = (long) transport_result;
    fetch_result_set_transport_info(item->easy, &item->result);
    bool redirect_callback_abort =
        tilefinch_fetch_redirect_callback_abort_expected(
            transport_result, item->writer.redirect_response,
            item->writer.redirect_callback_aborted,
            redirect_callback_failed(&item->writer));
    item->success = transport_result == CURLE_OK
        || redirect_callback_abort;
    item->result.timed_out = transport_result == CURLE_OPERATION_TIMEDOUT;
    if (item->success) {
        char *effective = NULL, *type = NULL;
        (void) curl_easy_getinfo(item->easy, CURLINFO_RESPONSE_CODE,
                                &item->result.status_code);
        (void) curl_easy_getinfo(item->easy, CURLINFO_EFFECTIVE_URL,
                                &effective);
        (void) curl_easy_getinfo(item->easy, CURLINFO_CONTENT_TYPE, &type);
        /* One sample per completed transport hop, including each redirect
           hop this scheduler reschedules onto the same easy handle. */
        fetch_tls_counters_record(item->easy, &scheduler->tls_counters);
        if (item->security_session != NULL) {
            (void) browser_session_hsts_observe_metadata(
                item->security_session,
                item->hop->url, &item->result.security);
        }
        write_context_set_effective_url(&item->writer, effective);
        if (type != NULL) {
            snprintf(item->result.content_type,
                     sizeof(item->result.content_type), "%s", type);
        }
        if (item->writer.redirect_response) {
            bool redirected = false;
            if (item->hop->redirects >= 5) {
                snprintf(item->result.error, sizeof(item->result.error),
                         "response exceeded the five-hop redirect limit");
            } else {
                fetch_hop_stage_response_cookies(
                    item->hop, &item->result,
                    item->writer.response_cookie_start);
            }
            if (item->result.error[0] != '\0') {
                /* Preserve an earlier explicit redirect error. */
            } else if (!fetch_hop_advance(item->hop, &item->result)) {
                snprintf(item->result.error, sizeof(item->result.error),
                         "response redirect target is invalid");
            } else if (curl_multi_remove_handle(
                           scheduler->multi, item->easy) != CURLM_OK) {
                snprintf(item->result.error, sizeof(item->result.error),
                         "redirect transport could not be rescheduled");
            } else if (!fetch_configure_redirect_hop(
                           item->easy, &item->request, item->hop,
                           &item->writer, &item->headers)) {
                item->result.timed_out =
                    fetch_hop_remaining_ms(item->hop) <= 0;
                snprintf(
                    item->result.error, sizeof(item->result.error), "%s",
                    item->result.timed_out
                        ? "redirect chain exceeded the request deadline"
                        : "redirect request headers exceeded policy or memory budget");
            } else if (curl_multi_add_handle(
                           scheduler->multi, item->easy) != CURLM_OK) {
                snprintf(item->result.error, sizeof(item->result.error),
                         "redirect transport could not be rescheduled");
            } else {
                item->writer.redirect_response = false;
                item->writer.redirect_callback_aborted = false;
                item->writer.interim_response = false;
                item->writer.headers_delivered = false;
                item->error[0] = '\0';
                redirected = true;
            }
            if (redirected) return false;
            item->success = false;
        }
        if (item->success && !item->streaming
            && item->result.data == NULL) {
            item->result.data = budget_malloc(scheduler->budget, 1);
            item->success = item->result.data != NULL;
            if (item->success) {
                item->result.data[0] = '\0';
                item->result.capacity = 1;
            }
        }
    }
    if (!item->success) {
        const char *error = item->result.error;
        if (item->hop->private_network.blocked) {
            error = "blocked private network target";
        } else if (error[0] == '\0' && item->writer.cancelled) {
            error = "request cancelled";
        } else if (error[0] == '\0' && item->writer.limit_exceeded) {
            error = "response quota exceeded";
        } else if (error[0] == '\0' && item->writer.allocation_failed) {
            error = "shared memory budget exceeded";
        } else if (error[0] == '\0' && item->writer.capture_failed) {
            error = "HTTP stream capture write failed";
        } else if (error[0] == '\0' && item->writer.consumer_failed) {
            error = "stream consumer rejected response";
        } else if (error[0] == '\0' && item->error[0] != '\0') {
            error = item->error;
        } else if (error[0] == '\0') {
            error = curl_easy_strerror(transport_result);
        }
        snprintf(item->result.error, sizeof(item->result.error), "%s", error);
    }
    fetch_result_log_transport(&item->result);
    scheduled_mark_complete(
        scheduler, item, item->success, item->result.error, false);
    return true;
}

static void scheduled_background_abort(
    FetchScheduler *scheduler, ScheduledFetch *item, const char *error)
{
    if (scheduler == NULL || item == NULL) return;
    if (item->background_request_id != 0) {
        (void) fetch_background_transport_cancel(
            item->background_request_id,
            error == NULL ? "background scheduler abort" : error);
        item->background_request_id = 0;
    }
    scheduled_mark_complete(
        scheduler, item, false,
        error == NULL ? "background transport failed" : error, false);
}

static bool scheduled_background_deliver_headers(
    FetchScheduler *scheduler, ScheduledFetch *item)
{
    if (scheduler == NULL || item == NULL || !item->background_backend
        || item->background_headers_delivered
        || item->background_request_id == 0) return true;
    if (!fetch_background_transport_take_headers(
            item->background_request_id, &item->result)) return true;
    item->result.redirect_origin_tainted = item->hop->origin_tainted;
    item->writer.response_cookie_start = item->result.set_cookie_count;
    char location[TILEFINCH_URL_SERIALIZED_LIMIT];
    bool has_location = fetch_response_header_value(
        &item->result, "location", location, sizeof(location));
    long status = item->result.status_code;
    item->writer.redirect_response =
        (status == 301 || status == 302 || status == 303
         || status == 307 || status == 308) && has_location;
    item->writer.has_response_location = has_location;
    item->writer.credentials_tainted = item->hop->credentials_tainted;
    if (item->writer.redirect_response
        && item->result.set_cookies_truncated) {
        char message[112];
        snprintf(
            message, sizeof(message),
            "redirect response exceeded the %u-cookie transport limit",
            (unsigned) FETCH_RESPONSE_COOKIE_CAPACITY);
        scheduled_background_abort(scheduler, item, message);
        return false;
    }
    if (!item->writer.redirect_response
        && status >= 400 && !item->request.allow_http_errors) {
        char message[96];
        snprintf(message, sizeof(message),
                 "HTTP response status %ld was rejected", status);
        scheduled_background_abort(scheduler, item, message);
        return false;
    }
    if (!enforce_response_cors_policy(&item->writer)
        || !enforce_response_credential_policy(&item->writer)) {
        scheduled_background_abort(
            scheduler, item,
            item->result.error[0] == '\0'
                ? "background response policy rejected headers"
                : item->result.error);
        return false;
    }
    if (!item->writer.redirect_response && item->streaming
        && item->stream.on_headers != NULL
        && !item->stream.on_headers(item->stream.opaque, &item->result)) {
        item->writer.consumer_failed = true;
        scheduled_background_abort(
            scheduler, item, "stream consumer rejected response headers");
        return false;
    }
    item->background_headers_delivered = true;
    item->writer.headers_delivered = true;
    if (item->streaming && !item->writer.redirect_response) {
        item->stream_metrics.headers_delivered = true;
    }
    return true;
}

static bool scheduled_background_deliver_chunk(
    FetchScheduler *scheduler, ScheduledFetch *item)
{
    if (scheduler == NULL || item == NULL || !item->background_backend
        || item->background_request_id == 0
        || !item->background_headers_delivered
        || item->writer.redirect_response
        || item->writer.pending_offset < item->writer.pending_length) {
        return true;
    }
    FetchBackgroundProgress progress = {0};
    if (!fetch_background_transport_progress(
            item->background_request_id, &progress)
        || progress.available_body_bytes == 0) return true;
    if (progress.available_body_bytes
            > scheduler->background_chunk_capacity) {
        snprintf(item->result.error, sizeof(item->result.error), "%s",
                 "background body chunk exceeded scheduler bound");
        return false;
    }
    size_t length = 0;
    if (!fetch_background_transport_take_chunk(
            item->background_request_id, scheduler->background_chunk,
            scheduler->background_chunk_capacity, &length)) return true;
    if (!write_deliver_chunk(
            &item->writer, scheduler->background_chunk, length)) {
        if (item->result.error[0] == '\0') {
            snprintf(item->result.error, sizeof(item->result.error), "%s",
                     "stream consumer rejected background response body");
        }
        return false;
    }
    return true;
}

static bool scheduled_background_finish_hop(
    FetchScheduler *scheduler, ScheduledFetch *item)
{
    if (scheduler == NULL || item == NULL || !item->background_backend
        || item->background_request_id == 0
        || item->writer.pending_offset < item->writer.pending_length) {
        return false;
    }
    FetchBackgroundProgress progress = {0};
    if (!fetch_background_transport_progress(
            item->background_request_id, &progress)
        || !progress.complete || progress.available_body_bytes != 0) {
        return false;
    }
    uint64_t completed_id = item->background_request_id;
    size_t consumed = item->streaming
        ? item->stream_metrics.bytes_received : item->result.length;
    bool success = fetch_background_transport_take_fetch_result_consumed(
        completed_id, scheduler->budget, &item->result, consumed);
    item->background_request_id = 0;
    FetchHandshakeSample handshake = {
        .classified = item->result.tls_connection_reuse_known,
        .measured = item->result.tls_handshake_measured,
        .handshake_us = item->result.tls_handshake_us,
        .reused = item->result.tls_connection_reused
    };
    fetch_tls_counters_add(&scheduler->tls_counters, &handshake);
    if (success && !enforce_response_credential_policy(&item->writer)) {
        success = false;
    }
    if (!success) {
        scheduled_mark_complete(
            scheduler, item, false,
            item->result.error[0] == '\0'
                ? "background transport failed" : item->result.error,
            false);
        return true;
    }
    if (item->security_session != NULL) {
        (void) browser_session_hsts_observe_metadata(
            item->security_session,
            item->hop->url, &item->result.security);
    }
    if (item->writer.redirect_response) {
        bool redirected = false;
        if (item->hop->redirects >= 5) {
            snprintf(item->result.error, sizeof(item->result.error), "%s",
                     "response exceeded the five-hop redirect limit");
        } else {
            fetch_hop_stage_response_cookies(
                item->hop, &item->result,
                item->writer.response_cookie_start);
            redirected = fetch_hop_advance(item->hop, &item->result);
            if (!redirected && item->result.error[0] == '\0') {
                snprintf(item->result.error, sizeof(item->result.error), "%s",
                         "response redirect target is invalid");
            }
        }
        if (redirected) {
            item->background_headers_delivered = false;
            item->writer.redirect_response = false;
            item->writer.has_response_location = false;
            item->writer.headers_delivered = false;
            reset_response_metadata(&item->result);
            if (scheduled_background_start_hop(item)) return false;
            if (item->result.timed_out || item->result.error[0] != '\0') {
                scheduled_mark_complete(
                    scheduler, item, false, item->result.error, false);
                return true;
            }
            /* No worker descriptor is free yet. Keep the browser-owned item
               pending; a later pump starts the already-authorized hop. */
            return false;
        }
        scheduled_mark_complete(
            scheduler, item, false,
            item->result.error[0] == '\0'
                ? "background redirect failed" : item->result.error,
            false);
        return true;
    }
    fetch_result_log_transport(&item->result);
    scheduled_mark_complete(scheduler, item, true, NULL, false);
    return true;
}

size_t fetch_scheduler_pump_bounded(
    FetchScheduler *scheduler, size_t maximum_completions,
    unsigned maximum_wait_ms, const FetchPumpQuota *quota,
    FetchPumpMetrics *metrics)
{
    if (metrics != NULL) memset(metrics, 0, sizeof(*metrics));
    if (scheduler == NULL || maximum_completions == 0) return 0;
    FetchPumpMetrics local = {0};
    FetchPumpMetrics *pump = metrics == NULL ? &local : metrics;
    size_t bytes_remaining = quota == NULL
        || quota->maximum_body_bytes == 0
            ? 64u * 1024u : quota->maximum_body_bytes;
    size_t callbacks_remaining = quota == NULL
        || quota->maximum_body_callbacks == 0
            ? 4u : quota->maximum_body_callbacks;
    uint64_t maximum_time_us = quota == NULL
        || quota->maximum_time_us == 0 ? UINT64_C(10000)
                                       : quota->maximum_time_us;
    if (maximum_wait_ms > 10) maximum_wait_ms = 10;
    uint64_t started_us = tilefinch_platform_monotonic_time_us();
    scheduler->pump_generation++;
    size_t stream_start[FETCH_SCHEDULER_LIMIT] = {0};
    bool active_seen[FETCH_SCHEDULER_LIMIT] = {0};
    bool stream_seen[FETCH_SCHEDULER_LIMIT] = {0};
    for (size_t i = 0; i < scheduler->capacity; i++) {
        ScheduledFetch *item = scheduler->items[i];
        if (item == NULL) continue;
        if (!item->active) continue;
        active_seen[i] = true;
        if (item->streaming) {
            stream_seen[i] = true;
            stream_start[i] = item->stream_metrics.bytes_received;
            item->stream_metrics.pump_calls++;
        }
        item->writer.pump_bytes_remaining = &bytes_remaining;
        item->writer.pump_callbacks_remaining = &callbacks_remaining;
        item->writer.pump_metrics = pump;
    }
    size_t completed = 0;
    if (fetch_trace.mode == FETCH_TRACE_REPLAY) {
        for (size_t visited = 0; visited < scheduler->capacity
                                 && completed < maximum_completions;
             visited++) {
            size_t i = (scheduler->pump_cursor + visited)
                     % scheduler->capacity;
            ScheduledFetch *item = scheduler->items[i];
            if (item == NULL) continue;
            if (!item->active || item->easy != NULL) continue;
            if (item->result.trace_external_cancel
                && !item->replay_retained_external_cancel) continue;
            if (item->replay_pumps_remaining != 0) {
                item->replay_pumps_remaining--;
                if (item->replay_pumps_remaining != 0) continue;
            }
            if (!item->replay_source_success) {
                scheduled_mark_complete(
                    scheduler, item, false, item->result.error, false);
                completed++;
                continue;
            }
            if (!scheduled_replay_headers(item)
                || !scheduled_replay_body(
                    scheduler, item, &bytes_remaining,
                    &callbacks_remaining, pump, started_us,
                    maximum_time_us)) {
                scheduled_mark_complete(
                    scheduler, item, false, item->result.error, false);
                completed++;
            } else if (item->replay_offset == item->replay_length) {
                bool intact = item->replay_file != NULL
                    && fgetc(item->replay_file) == EOF
                    && !ferror(item->replay_file);
                bool close_ok = item->replay_file != NULL
                    && fclose(item->replay_file) == 0;
                intact = intact && close_ok
                    && (!item->writer.replay_body_hash_required
                        || item->writer.replay_body_hash
                             == item->writer.replay_expected_body_hash);
                item->replay_file = NULL;
                if (!intact) {
                    snprintf(item->result.error,
                             sizeof(item->result.error),
                             "HTTP replay body is corrupt");
                }
                scheduled_mark_complete(
                    scheduler, item, intact,
                    intact ? NULL : item->result.error, false);
                completed++;
            }
        }
    } else {
        bool background_active = false;
        for (size_t visited = 0; visited < scheduler->capacity
                                 && completed < maximum_completions;
             visited++) {
            size_t i = (scheduler->pump_cursor + visited)
                     % scheduler->capacity;
            ScheduledFetch *item = scheduler->items[i];
            if (item == NULL || !item->active
                || !item->background_backend) continue;
            background_active = true;
            if (item->background_request_id == 0) {
                if (!scheduled_background_start_hop(item)
                    && (item->result.timed_out
                        || item->result.error[0] != '\0')) {
                    scheduled_mark_complete(
                        scheduler, item, false, item->result.error, false);
                    completed++;
                    continue;
                }
            }
            if (item->background_request_id == 0) continue;
            if (!scheduled_drain_pending(
                    scheduler, item, &bytes_remaining,
                    &callbacks_remaining, pump)) {
                scheduled_background_abort(
                    scheduler, item,
                    item->result.error[0] == '\0'
                        ? "stream consumer rejected staged response body"
                        : item->result.error);
                completed++;
                continue;
            }
            if (!scheduled_background_deliver_headers(scheduler, item)) {
                if (item->complete) completed++;
                continue;
            }
            if (!item->active) {
                completed++;
                continue;
            }
            if (!scheduled_background_deliver_chunk(scheduler, item)) {
                scheduled_background_abort(
                    scheduler, item,
                    item->result.error[0] == '\0'
                        ? "background response body delivery failed"
                        : item->result.error);
                completed++;
                continue;
            }
            if (scheduled_background_finish_hop(scheduler, item)) {
                completed++;
            }
        }
        for (size_t visited = 0; visited < scheduler->capacity; visited++) {
            size_t i = (scheduler->pump_cursor + visited)
                     % scheduler->capacity;
            ScheduledFetch *item = scheduler->items[i];
            if (item == NULL) continue;
            if (!item->active || item->easy == NULL) {
                continue;
            }
            if (!scheduled_drain_pending(
                    scheduler, item, &bytes_remaining,
                    &callbacks_remaining, pump)) {
                const char *error = item->result.error[0] == '\0'
                    ? "stream consumer rejected staged response body"
                    : item->result.error;
                scheduled_mark_complete(scheduler, item, false, error, false);
                completed++;
            } else if (item->transport_complete
                       && item->writer.pending_offset
                              >= item->writer.pending_length
                       && completed < maximum_completions
                       && scheduled_finish_transport(
                              scheduler, item, item->transport_result)) {
                completed++;
            }
        }
        bool regular_active = false;
        for (size_t i = 0; i < scheduler->capacity; i++) {
            ScheduledFetch *item = scheduler->items[i];
            if (item != NULL && item->active && item->easy != NULL) {
                regular_active = true;
                break;
            }
        }
        int running = 0;
        CURLMcode performed = CURLM_OK;
        if (regular_active && completed < maximum_completions
            && bytes_remaining != 0 && callbacks_remaining != 0
            && fetch_pump_elapsed_us(started_us) < maximum_time_us) {
            performed = curl_multi_perform(scheduler->multi, &running);
        }
        uint64_t elapsed = fetch_pump_elapsed_us(started_us);
        if (performed == CURLM_OK && running > 0 && maximum_wait_ms != 0
            && bytes_remaining != 0 && callbacks_remaining != 0
            && elapsed < maximum_time_us) {
            uint64_t left_us = maximum_time_us - elapsed;
            unsigned wait_ms = maximum_wait_ms;
            if ((uint64_t) wait_ms * 1000u > left_us) {
                /* This branch proves left_us < wait_ms * 1000 and wait_ms is
                   capped at ten above, so the cast is exact and avoids a
                   software 64-bit division in the browser-thread pump. */
                wait_ms = (unsigned) left_us / 1000u;
            }
            if (wait_ms != 0) {
                int descriptors = 0;
                if (curl_multi_poll(scheduler->multi, NULL, 0,
                                    (int) wait_ms, &descriptors) == CURLM_OK) {
                    (void) curl_multi_perform(scheduler->multi, &running);
                }
            }
        }
        /* The worker owns socket waits for background requests. Preserve the
           scheduler's blocking-pump contract without spinning the browser
           thread while no response chunk has been produced yet. */
        if (background_active && !regular_active && completed == 0
            && pump->body_callbacks == 0 && maximum_wait_ms != 0) {
            uint64_t background_elapsed =
                fetch_pump_elapsed_us(started_us);
            if (background_elapsed < maximum_time_us) {
                uint64_t left_us = maximum_time_us - background_elapsed;
                unsigned wait_ms = maximum_wait_ms;
                if ((uint64_t) wait_ms * 1000u > left_us)
                    wait_ms = (unsigned) left_us / 1000u;
                if (wait_ms != 0)
                    fetch_background_transport_wait(wait_ms);
            }
        }
        int remaining = 0;
        CURLMsg *message = NULL;
        while (completed < maximum_completions
               && (message = curl_multi_info_read(
                       scheduler->multi, &remaining)) != NULL) {
            if (message->msg != CURLMSG_DONE) continue;
            ScheduledFetch *item = NULL;
            (void) curl_easy_getinfo(message->easy_handle,
                                     CURLINFO_PRIVATE, &item);
            if (item == NULL || !item->active) continue;
            if (scheduled_retry_tls12_compatibility(
                    scheduler, item, message->data.result)) {
                continue;
            }
            if (message->data.result == CURLE_OK
                && item->writer.pending_offset
                       < item->writer.pending_length) {
                item->transport_complete = true;
                item->transport_result = message->data.result;
                continue;
            }
            if (scheduled_finish_transport(
                    scheduler, item, message->data.result)) completed++;
        }
    }
    pump->elapsed_us = fetch_pump_elapsed_us(started_us);
    pump->completions = completed;
    bool active_bounded = false;
    for (size_t i = 0; i < scheduler->capacity; i++) {
        ScheduledFetch *item = scheduler->items[i];
        if (item == NULL) continue;
        if (stream_seen[i]) {
            size_t bytes = item->stream_metrics.bytes_received
                         - stream_start[i];
            if (bytes > item->stream_metrics.maximum_pump_bytes) {
                item->stream_metrics.maximum_pump_bytes = bytes;
            }
            if (pump->elapsed_us > item->stream_metrics.maximum_pump_us) {
                item->stream_metrics.maximum_pump_us = pump->elapsed_us;
            }
        }
        if (active_seen[i]) {
            item->writer.pump_bytes_remaining = NULL;
            item->writer.pump_callbacks_remaining = NULL;
            item->writer.pump_metrics = NULL;
        }
        if (item->active) active_bounded = true;
    }
    pump->quota_yielded = active_bounded
        && (bytes_remaining == 0 || callbacks_remaining == 0
            || pump->elapsed_us >= maximum_time_us);
    if (pump->quota_yielded) {
        for (size_t i = 0; i < scheduler->capacity; i++) {
            ScheduledFetch *item = scheduler->items[i];
            if (item == NULL) continue;
            if (item->active && item->streaming) {
                item->stream_metrics.quota_yields++;
            }
        }
    }
    if (scheduler->capacity != 0) {
        scheduler->pump_cursor = (scheduler->pump_cursor + 1u)
                               % scheduler->capacity;
    }
    return completed;
}

size_t fetch_scheduler_pump(FetchScheduler *scheduler,
                            size_t maximum_completions,
                            unsigned maximum_wait_ms)
{
    return fetch_scheduler_pump_bounded(
        scheduler, maximum_completions, maximum_wait_ms, NULL, NULL);
}

bool fetch_scheduler_take(FetchScheduler *scheduler, uint64_t request_id,
                          bool *success, FetchResult *result)
{
    if (scheduler == NULL || request_id == 0 || success == NULL
        || result == NULL) return false;
    for (size_t i = 0; i < scheduler->capacity; i++) {
        ScheduledFetch *item = scheduler->items[i];
        if (item == NULL) continue;
        if (!item->complete || item->id != request_id) continue;
        if (item->result.received_body_bytes < item->result.length) {
            item->result.received_body_bytes = item->result.length;
        }
        *success = item->success;
        *result = item->result;
        memset(&item->result, 0, sizeof(item->result));
        scheduled_fetch_clear(scheduler, &scheduler->items[i]);
        return true;
    }
    return false;
}

bool fetch_scheduler_take_stream(
    FetchScheduler *scheduler, uint64_t request_id, bool *success,
    FetchStreamMetrics *metrics, FetchResult *result)
{
    if (scheduler == NULL || request_id == 0 || success == NULL
        || metrics == NULL || result == NULL) return false;
    for (size_t i = 0; i < scheduler->capacity; i++) {
        ScheduledFetch *item = scheduler->items[i];
        if (item == NULL) continue;
        if (!item->complete || item->id != request_id
            || !item->streaming) continue;
        if (item->result.received_body_bytes
                < item->stream_metrics.bytes_received) {
            item->result.received_body_bytes =
                item->stream_metrics.bytes_received;
        }
        *metrics = item->stream_metrics;
        *success = item->success;
        *result = item->result;
        memset(&item->result, 0, sizeof(item->result));
        scheduled_fetch_clear(scheduler, &scheduler->items[i]);
        return true;
    }
    return false;
}

bool fetch_scheduler_stream_metrics(
    const FetchScheduler *scheduler, uint64_t request_id,
    FetchStreamMetrics *metrics)
{
    if (scheduler == NULL || request_id == 0 || metrics == NULL) return false;
    for (size_t i = 0; i < scheduler->capacity; i++) {
        const ScheduledFetch *item = scheduler->items[i];
        if (item == NULL) continue;
        if (item->id != request_id || !item->streaming
            || (!item->active && !item->complete)) continue;
        *metrics = item->stream_metrics;
        return true;
    }
    return false;
}

bool fetch_scheduler_request_progress(
    const FetchScheduler *scheduler, uint64_t request_id,
    FetchRequestProgress *progress)
{
    if (scheduler == NULL || request_id == 0 || progress == NULL) {
        return false;
    }
    for (size_t i = 0; i < scheduler->capacity; i++) {
        const ScheduledFetch *item = scheduler->items[i];
        if (item == NULL || item->id != request_id) continue;
        size_t received = item->result.received_body_bytes;
        if (received < item->result.length) received = item->result.length;
        if (received < item->writer.accepted) received = item->writer.accepted;
        FetchBackgroundProgress background = {0};
        if (item->background_request_id != 0
            && fetch_background_transport_progress(
                   item->background_request_id, &background)
            && received < background.received_body_bytes) {
            received = background.received_body_bytes;
        }
        *progress = (FetchRequestProgress) {
            .received_body_bytes = received,
            .status_code = item->result.status_code,
            .active = item->active,
            .complete = item->complete
        };
        return true;
    }
    return false;
}

bool fetch_scheduler_request(FetchScheduler *scheduler, const char *url,
                             const FetchRequest *request,
                             size_t maximum_bytes, long timeout_ms,
                             FetchResult *result)
{
    if (result == NULL) return false;
    Budget *result_budget = scheduler == NULL
        ? result->budget : scheduler->budget;
    memset(result, 0, sizeof(*result));
    /*
     * Heap-owned FetchResults retain the allocator needed by
     * fetch_result_free() even when enqueue fails before a ScheduledFetch can
     * transfer its initialized result.  Losing this pointer leaked one result
     * object per rejected module dependency under script-memory pressure.
     * Stack-owned results get the same scheduler budget for their response
     * body and cookie cleanup.
     */
    result->budget = result_budget;
    uint64_t id = fetch_scheduler_enqueue(scheduler, url, request,
                                          maximum_bytes, timeout_ms);
    if (id == 0) return false;
    for (;;) {
        bool success = false;
        if (fetch_scheduler_take(scheduler, id, &success, result)) {
            /* The synchronous convenience API transfers a response body only
               on success. libcurl may have filled a bounded partial buffer
               before a size, timeout, or transport failure; release it here
               so callers that correctly return immediately on false cannot
               leak that ownership. Keep status and error diagnostics. */
            if (!success) {
                budget_free(result->budget, result->data);
                result->data = NULL;
                result->length = 0;
                result->capacity = 0;
            }
            return success;
        }
        (void) fetch_scheduler_pump(scheduler, 1, 10);
        FetchRequestProgress progress = {0};
        (void) fetch_scheduler_request_progress(
            scheduler, id, &progress);
        if (!tilefinch_platform_cooperate(
                "fetch", progress.received_body_bytes)) {
            (void) fetch_scheduler_cancel(
                scheduler, id, "request cancelled by platform");
            bool ignored_success = false;
            (void) fetch_scheduler_take(
                scheduler, id, &ignored_success, result);
            budget_free(result->budget, result->data);
            result->data = NULL;
            result->length = 0;
            result->capacity = 0;
            return false;
        }
    }
}

bool fetch_scheduler_cancel(FetchScheduler *scheduler, uint64_t request_id,
                            const char *reason)
{
    if (scheduler == NULL || request_id == 0) return false;
    for (size_t i = 0; i < scheduler->capacity; i++) {
        ScheduledFetch *item = scheduler->items[i];
        if (item == NULL) continue;
        if (item->id != request_id || item->complete || !item->active) {
            continue;
        }
        const char *message = reason == NULL || reason[0] == '\0'
            ? "request aborted" : reason;
        if (item->streaming) item->stream_metrics.cancelled = true;
        scheduled_mark_complete(
            scheduler, item, false, message, true);
        return true;
    }
    return false;
}

size_t fetch_scheduler_cancel_all(
    FetchScheduler *scheduler, const char *reason)
{
    if (scheduler == NULL) return 0;
    const char *message = reason == NULL || reason[0] == '\0'
        ? "requests aborted" : reason;
    size_t cancelled = 0;
    for (size_t i = 0; i < scheduler->capacity; i++) {
        ScheduledFetch *item = scheduler->items[i];
        if (item == NULL || item->complete || !item->active) continue;
        if (item->streaming) item->stream_metrics.cancelled = true;
        scheduled_mark_complete(
            scheduler, item, false, message, true);
        cancelled++;
    }
    return cancelled;
}

bool fetch_scheduler_discard(FetchScheduler *scheduler,
                             uint64_t request_id)
{
    if (scheduler == NULL || request_id == 0) return false;
    for (size_t i = 0; i < scheduler->capacity; i++) {
        ScheduledFetch *item = scheduler->items[i];
        if (item == NULL) continue;
        if (item->id != request_id || !item->complete) continue;
        scheduled_fetch_clear(scheduler, &scheduler->items[i]);
        return true;
    }
    return false;
}

size_t fetch_scheduler_pending(const FetchScheduler *scheduler)
{
    return scheduler == NULL ? 0 : scheduler->pending;
}

bool fetch_scheduler_tls_handshake_counters(
    const FetchScheduler *scheduler, FetchTlsHandshakeCounters *counters)
{
    if (counters == NULL) return false;
    memset(counters, 0, sizeof(*counters));
    if (scheduler == NULL) return false;
    *counters = scheduler->tls_counters;
    return true;
}

void fetch_scheduler_reservation_state(const FetchScheduler *scheduler,
                                       size_t *reserved_bytes,
                                       size_t *maximum_reserved_bytes,
                                       size_t *domain_active_slots,
                                       size_t *domain_maximum_slots)
{
    if (reserved_bytes != NULL) {
        *reserved_bytes = scheduler == NULL ? 0 : scheduler->reserved_bytes;
    }
    if (maximum_reserved_bytes != NULL) {
        *maximum_reserved_bytes =
            scheduler == NULL ? 0 : scheduler->maximum_reserved_bytes;
    }
    if (domain_active_slots != NULL) {
        *domain_active_slots = scheduler == NULL || scheduler->domain == NULL
            ? 0 : scheduler->domain->active_slots;
    }
    if (domain_maximum_slots != NULL) {
        *domain_maximum_slots = scheduler == NULL || scheduler->domain == NULL
            ? 0 : scheduler->domain->maximum_active_slots;
    }
}

bool fetch_scheduler_reservation_available(const FetchScheduler *scheduler,
                                           size_t maximum_bytes,
                                           size_t reserve_bytes)
{
    if (scheduler == NULL || maximum_bytes == 0
        || maximum_bytes == SIZE_MAX) return false;
    if (scheduler->reserved_bytes > scheduler->maximum_reserved_bytes) {
        return false;
    }
    /* Mirrors the enqueue bound at the head of fetch_scheduler_enqueue so a
       caller which clears this predicate is not refused a moment later. */
    size_t available =
        scheduler->maximum_reserved_bytes - scheduler->reserved_bytes;
    if (maximum_bytes > available) return false;
    return available - maximum_bytes >= reserve_bytes;
}

void fetch_scheduler_debug_dump(const FetchScheduler *scheduler,
                                const char *label)
{
    if (scheduler == NULL) return;
    for (size_t i = 0; i < scheduler->capacity; i++) {
        const ScheduledFetch *item = scheduler->items[i];
        if (item == NULL) continue;
        fprintf(stderr,
                "scheduler-slot label=%s slot=%zu id=%llu active=%d "
                "complete=%d success=%d reserved=%zu url=\"%.200s\"\n",
                label == NULL ? "" : label, i,
                (unsigned long long) item->id, (int) item->active,
                (int) item->complete, (int) item->success,
                item->reserved_bytes,
                item->url == NULL ? "" : item->url);
    }
}

bool fetch_scheduler_uses_virtual_replay(const FetchScheduler *scheduler)
{
    return scheduler != NULL && fetch_trace.mode == FETCH_TRACE_REPLAY;
}

void fetch_scheduler_destroy(FetchScheduler *scheduler)
{
    if (scheduler == NULL) return;
    Budget *budget = scheduler->budget;
    FetchSchedulerDomain *domain = scheduler->domain;
    for (size_t i = 0; i < scheduler->capacity; i++) {
        scheduled_fetch_clear(scheduler, &scheduler->items[i]);
    }
    curl_multi_cleanup(scheduler->multi);
    bool acquired = scheduler->global_acquired;
    budget_free(budget, scheduler->background_chunk);
    budget_free(budget, scheduler);
    if (acquired) curl_global_release();
    if (domain != NULL) {
        if (domain->active_views != 0) domain->active_views--;
        fetch_scheduler_domain_release(domain);
    }
}

FetchResult *fetch_result_create(Budget *budget)
{
    if (budget == NULL) return NULL;
    FetchResult *result = budget_calloc(budget, 1, sizeof(*result));
    if (result != NULL) result->budget = budget;
    return result;
}
