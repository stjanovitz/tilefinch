static bool navigation_stream_body(void *opaque, const unsigned char *data,
                                   size_t length)
{
    NavigationDocumentStream *stream = opaque;
    if (stream != NULL && stream->session != NULL
        && stream->session->performance.first_body_byte_us == 0
        && stream->started_us != 0 && length != 0) {
        stream->session->performance.first_body_byte_us =
            navigation_now_us() - stream->started_us;
    }
    /* The fetch scheduler must exist before parsing can discover preloads,
       but the realm is created at the first parser-blocking script after
       metadata scanning has resolved the page's layout viewport. */
    if (!navigation_stream_prepare_runtime(stream, false)) return false;
    /* Head/body close checkpoints are needed for progressive layout even
       when author JavaScript is disabled. The callback itself gates script
       execution on the session policy. */
    document_parser_set_element_closed_callback(
        stream->parser, navigation_stream_element_closed, stream);
    uint64_t started = navigation_now_us();
    /* Account the bounded feed before entering Lexbor so a close-element
       checkpoint can report the response prefix containing it. The callback
       may occur anywhere within this chunk; the value is therefore an honest
       chunk-boundary upper bound, never an invented byte offset. */
    if (length > SIZE_MAX - stream->parsed_body_bytes) {
        stream->parsed_body_bytes = SIZE_MAX;
    } else {
        stream->parsed_body_bytes += length;
    }
    uint64_t preview_before =
        stream->session->performance.streaming_preview_layout_us
        + stream->session->performance.streaming_preview_paint_us
        + stream->session->performance.streaming_preview_style_refresh_us;
    /* The main parser must stop at an author script, but a browser preload
       scanner may inspect already-received bytes beyond that checkpoint.
       Start only fetches here; the normal DOM-derived stylesheet pipeline
       still parses and applies rules in document order. */
    navigation_stream_scan_style_preloads(stream, data, length);
    navigation_stream_launch_preloads(stream);
    navigation_stream_progress_preloads(stream, false);
    if (!document_parser_feed(stream->parser, (const char *) data, length)) {
        return false;
    }
    uint64_t preview_after =
        stream->session->performance.streaming_preview_layout_us
        + stream->session->performance.streaming_preview_paint_us
        + stream->session->performance.streaming_preview_style_refresh_us;
    uint64_t preview_elapsed = preview_after >= preview_before
        ? preview_after - preview_before : 0;
    uint64_t finished = navigation_now_us();
    uint64_t adjusted_started = preview_elapsed <= finished - started
        ? started + preview_elapsed : finished;
    uint64_t elapsed = navigation_record_slice(
        stream->session, NAVIGATION_SLICE_PARSER, adjusted_started, length);
    stream->parse_us += elapsed;
    stream->session->performance.parser_feed_us += elapsed;
    if (stream->session->performance.first_dom_us == 0) {
        lxb_dom_node_t *body = document_body_node(&stream->parser->document);
        if (body != NULL && body->first_child != NULL) {
            stream->session->performance.first_dom_us =
                navigation_now_us() - stream->started_us;
        }
    }
    navigation_stream_try_pre_eof_preview(stream);
    return true;
}

static bool navigation_stream_headers(void *opaque,
                                      const FetchResult *metadata)
{
    NavigationDocumentStream *stream = opaque;
    if (stream != NULL && stream->session != NULL
        && stream->session->performance.response_headers_us == 0
        && stream->started_us != 0) {
        stream->session->performance.response_headers_us =
            navigation_now_us() - stream->started_us;
    }
    if (metadata->effective_url[0] != '\0') {
        snprintf(stream->document_url, sizeof(stream->document_url), "%s",
                 metadata->effective_url);
        snprintf(stream->resource_base_url,
                 sizeof(stream->resource_base_url), "%s",
                 metadata->effective_url);
    }
    const char *security_document_url =
        metadata->effective_url[0] == '\0'
            ? stream->document_url : metadata->effective_url;
    bool security_snapshot_truncated =
        metadata->security.version
                == FETCH_RESPONSE_SECURITY_METADATA_VERSION
        ? metadata->security.csp_snapshot_state
              == FETCH_SECURITY_FIELD_TRUNCATED
        : metadata->response_security_headers_truncated;
    if (!tilefinch_csp_parse_response_headers(
            &stream->parser->document.content_security_policy,
            security_document_url, metadata->response_headers,
            metadata->response_headers_length,
            security_snapshot_truncated)) {
        snprintf(stream->session->last_error,
                 sizeof(stream->session->last_error),
                 "response security policy is invalid or exceeds limits");
        return false;
    }
    stream->referrer_policy[0] = '\0';
    bool referrer_policy_header_present = false;
    return fetch_response_referrer_policy(
        metadata, &referrer_policy_header_present,
        stream->referrer_policy);
}

static bool navigation_stream_stall(void *opaque)
{
    NavigationDocumentStream *stream = opaque;
    if (stream->session != NULL) {
        stream->session->last_stream.parser_pauses++;
    }
    return true;
}

static bool navigation_same_origin(const char *left, const char *right)
{
    return tilefinch_url_same_origin(left, right);
}

typedef enum {
    NAVIGATION_FINALIZE_PARSE = 0,
    NAVIGATION_FINALIZE_BUILD,
    NAVIGATION_FINALIZE_COMMIT,
    NAVIGATION_FINALIZE_DONE
} NavigationFinalizePhase;

struct NavigationLoad {
    NavigationSession *session;
    uint64_t generation;
    uint64_t started_us;
    FetchScheduler *scheduler;
    uint64_t request_id;
    FetchResult response;
    FetchStreamOptions stream_options;
    TilefinchRequestContext request_context;
    DocumentParser parser;
    NavigationDocumentStream *document_stream;
    LayoutBuildJob *layout_job;
    SectionRouteStream section_router;
    CompressedSectionStore *section_store;
    SectionStoreStreamBuilder *section_builder;
    const FontSet *fonts;
    const ImageResources *images;
    char requested_url[NAVIGATION_URL_LIMIT];
    char request_url[NAVIGATION_URL_LIMIT];
    char effective_url[NAVIGATION_URL_LIMIT];
    char effective_history_url[NAVIGATION_URL_LIMIT];
    char source_url[NAVIGATION_URL_LIMIT];
    char client_hint_tokens[1024];
    char client_hint_origin[TILEFINCH_ORIGIN_SERIALIZED_LIMIT];
    char method[17];
    char content_type[256];
    char *body;
    size_t body_length;
    size_t maximum_bytes;
    size_t section_block_bytes;
    size_t section_maximum_bytes;
    long timeout_ms;
    int viewport_width;
    size_t initial_section;
    NavigationLoadStatus status;
    NavigationLoadMetrics metrics;
    bool record_history;
    bool sectioned;
    bool parser_active;
    bool response_taken;
    bool response_cookies_accepted;
    bool client_hint_retried;
    bool client_hint_retry_pending;
    bool zero_body_retry_attempted;
    bool staging_released;
    unsigned char *staged_body;
    size_t staged_body_offset;
    size_t staged_body_length;
    size_t staged_body_capacity;
    bool staged_stall;
    NavigationFinalizePhase finalize_phase;
    bool layout_preview_candidate;
    PocDocument finalized_document;
    NavigationSession *candidate_session;
    char commit_url[NAVIGATION_URL_LIMIT];
    char response_referrer_policy[128];
    bool low_memory_replacement;
    bool incumbent_retired;
    bool replacement_outcome_reported;
    ViewportContext incumbent_viewport;
    const FontSet *incumbent_fonts;
    const ImageResources *incumbent_images;
    bool incumbent_images_page_owned;
    uint64_t incumbent_resource_fingerprint;
    bool incumbent_style_rebuild_required;
    bool incumbent_relayout_damage_valid;
    int incumbent_relayout_damage_left;
    int incumbent_relayout_damage_top;
    int incumbent_relayout_damage_right;
    int incumbent_relayout_damage_bottom;
};

static bool navigation_load_reset_for_retry(NavigationLoad *load);
static uint64_t navigation_load_enqueue_fetch_request(NavigationLoad *load);

static void navigation_load_saturating_add(size_t *value, size_t amount)
{
    if (value == NULL) return;
    *value = amount > SIZE_MAX - *value ? SIZE_MAX : *value + amount;
}

static void navigation_load_set_error(NavigationLoad *load,
                                      NavigationLoadStatus status,
                                      const char *error)
{
    if (load == NULL) return;
    load->status = status;
    if (load->session != NULL) {
        snprintf(load->session->last_error,
                 sizeof(load->session->last_error), "%s",
                 error == NULL || error[0] == '\0'
                   ? "navigation request failed" : error);
    }
}

static bool navigation_load_headers(void *opaque,
                                    const FetchResult *metadata)
{
    NavigationLoad *load = opaque;
    if (load == NULL) return false;
    const char *effective = metadata->effective_url[0] == '\0'
        ? load->request_url : metadata->effective_url;
    if (!tilefinch_url_normalize(effective, load->effective_history_url,
                              sizeof(load->effective_history_url))
        || !tilefinch_url_request_key(effective, load->effective_url,
                                sizeof(load->effective_url))) {
        snprintf(load->session->last_error,
                 sizeof(load->session->last_error), "%s",
                 "response effective URL is invalid");
        return false;
    }
    /* Set-Cookie takes effect at the response-header boundary.  The parser
       may synchronously execute author code as body chunks arrive, so
       deferring this until transfer completion makes document.cookie lag the
       network response and breaks valid parser-blocking scripts. */
    navigation_accept_response_cookies(
        load->session->browser_session, &load->request_context, metadata,
        effective);
    load->response_cookies_accepted = true;
    /* Critical-CH is known at the response-header boundary.  The first
       representation is not page-visible: do not feed it to the HTML
       parser, execute parser-blocking scripts, discover preloads, route
       sections, or produce progressive paint.  The completed response is
       still retained long enough to accept its cookies before the request
       is retried with the requested client hints. */
    load->client_hint_tokens[0] = '\0';
    load->client_hint_retry_pending =
        tilefinch_url_potentially_trustworthy(effective)
        && strcasecmp(load->method, "GET") == 0
        && !load->client_hint_retried
        && fetch_accepted_critical_client_hints(
            metadata->accept_ch, metadata->critical_ch,
            load->client_hint_tokens,
            sizeof(load->client_hint_tokens))
        && load->client_hint_tokens[0] != '\0';
    if (load->client_hint_retry_pending) return true;
    if (!load->sectioned) {
        bool accepted = navigation_stream_headers(
            load->document_stream, metadata);
        if (!accepted && load->session->last_error[0] == '\0') {
            snprintf(load->session->last_error,
                     sizeof(load->session->last_error), "%s",
                     "response metadata is invalid or exceeds limits");
        }
        return accepted;
    }
    return true;
}

static bool navigation_load_body(void *opaque, const unsigned char *data,
                                 size_t length)
{
    NavigationLoad *load = opaque;
    if (load == NULL) return false;
    if (load->client_hint_retry_pending) return true;
    if (length == 0) return true;
    if (data == NULL || length > load->maximum_bytes
        || load->staged_body_length > load->maximum_bytes - length) {
        return false;
    }
    size_t needed = load->staged_body_length + length;
    size_t tail = load->staged_body_capacity - load->staged_body_offset;
    if (needed > tail && load->staged_body_offset != 0) {
        memmove(load->staged_body,
                load->staged_body + load->staged_body_offset,
                load->staged_body_length);
        navigation_load_saturating_add(
            &load->session->performance.staged_body_compactions, 1);
        navigation_load_saturating_add(
            &load->session->performance.staged_body_compaction_bytes,
            load->staged_body_length);
        load->staged_body_offset = 0;
        tail = load->staged_body_capacity;
    }
    if (needed > tail) {
        size_t capacity = load->staged_body_capacity == 0
            ? 4096 : load->staged_body_capacity;
        while (capacity < needed && capacity <= load->maximum_bytes / 2u) {
            capacity *= 2u;
        }
        if (capacity < needed || capacity > load->maximum_bytes) {
            capacity = needed;
        }
        unsigned char *grown = budget_realloc(
            load->session->budget, load->staged_body, capacity);
        if (grown == NULL) return false;
        load->staged_body = grown;
        load->staged_body_capacity = capacity;
    }
    memcpy(load->staged_body + load->staged_body_offset
               + load->staged_body_length,
           data, length);
    load->staged_body_length = needed;
    load->metrics.staged_body_bytes += length;
    if (needed > load->metrics.maximum_staged_body_bytes) {
        load->metrics.maximum_staged_body_bytes = needed;
    }
    return true;
}

static bool navigation_load_stall(void *opaque)
{
    NavigationLoad *load = opaque;
    if (load == NULL) return false;
    if (load->client_hint_retry_pending) return true;
    load->staged_stall = true;
    return true;
}

static bool navigation_load_process_staged_body(
    NavigationLoad *load, const NavigationLoadQuota *quota)
{
    if (load == NULL) return false;
    size_t maximum_bytes = quota == NULL
        || quota->maximum_parser_body_bytes == 0
            ? 64u * 1024u : quota->maximum_parser_body_bytes;
    uint64_t maximum_time_us = quota == NULL
        || quota->maximum_parser_time_us == 0
            ? UINT64_C(10000) : quota->maximum_parser_time_us;
    size_t length = load->staged_body_length < maximum_bytes
        ? load->staged_body_length : maximum_bytes;
    uint64_t started = navigation_now_us();
    bool ok = true;
    if (length != 0) {
        if (load->sectioned) {
            ok = section_route_stream_body(
                &load->section_router,
                load->staged_body + load->staged_body_offset, length);
            if (ok && load->section_store != NULL) {
                load->metrics.safe_sections_ready =
                    load->section_store->sealed_section_count;
                load->metrics.safe_section_source_bytes =
                    load->section_store->sealed_source_bytes;
            }
        } else {
            ok = navigation_stream_body(
                load->document_stream,
                load->staged_body + load->staged_body_offset, length);
        }
        if (ok) {
            load->staged_body_length -= length;
            if (load->staged_body_length != 0) {
                load->staged_body_offset += length;
                navigation_load_saturating_add(
                    &load->session->performance.staged_body_slides_avoided,
                    1);
                navigation_load_saturating_add(
                    &load->session->performance
                       .staged_body_slide_bytes_avoided,
                    load->staged_body_length);
            } else {
                load->staged_body_offset = 0;
            }
        }
    }
    if (ok && load->staged_body_length == 0 && load->staged_stall) {
        if (!load->sectioned) {
            ok = navigation_stream_stall(load->document_stream);
        }
        if (ok) load->staged_stall = false;
    }
    uint64_t elapsed = navigation_now_us() - started;
    load->metrics.parser_pump_us += elapsed;
    if (elapsed > load->metrics.maximum_parser_pump_us) {
        load->metrics.maximum_parser_pump_us = elapsed;
    }
    if (elapsed > maximum_time_us) {
        load->metrics.parser_time_quota_exceeded = true;
    }
    if (load->staged_body_length != 0) {
        load->metrics.parser_quota_yields++;
    }
    return ok;
}

static bool navigation_load_retry_zero_body_deadline(NavigationLoad *load)
{
    if (load == NULL || load->session == NULL
        || load->zero_body_retry_attempted
        || strcasecmp(load->method, "GET") != 0
        || load->metrics.stream.bytes_received != 0
        || strcmp(load->response.error,
                  "request deadline expired before background hop") != 0) {
        return false;
    }
    load->zero_body_retry_attempted = true;
    load->session->performance.zero_body_navigation_retries++;
    if (!navigation_load_reset_for_retry(load)) return false;
    fetch_result_destroy(&load->response);
    load->request_id = navigation_load_enqueue_fetch_request(load);
    if (load->request_id == 0) return false;
    memset(&load->session->last_stream, 0,
           sizeof(load->session->last_stream));
    return true;
}

static void navigation_load_reset_preload_metrics(NavigationLoad *load)
{
    if (load == NULL || load->session == NULL) return;
    NavigationSession *session = load->session;
    session->preloads_discovered = 0;
    session->preloads_launched = 0;
    session->preloads_completed = 0;
    session->preloads_cache_hits = 0;
    session->preloads_failed = 0;
    session->preloads_deferred = 0;
    session->preloads_headroom_skipped = 0;
}

static bool navigation_load_initialize_document_stream(
    NavigationLoad *load, NavigationDocumentStream *stream)
{
    if (load == NULL || stream == NULL) return false;
    memset(stream, 0, sizeof(*stream));
    stream->session = load->session;
    stream->stylesheet_resources.budget = load->session->budget;
    stream->parser = &load->parser;
    stream->started_us = load->started_us;
    stream->fonts = load->fonts;
    stream->images = load->images;
    stream->viewport_width = load->viewport_width;
    stream->isolates_remote_document = true;
    snprintf(stream->document_url, sizeof(stream->document_url), "%s",
             load->request_url);
    snprintf(stream->resource_base_url,
             sizeof(stream->resource_base_url), "%s", load->request_url);
    if (!viewport_context_init(
            &stream->viewport, load->viewport_width, 272,
            load->viewport_width, 272)) return false;
    /* A transactional candidate joins the incumbent page's domain, so their
       independent scheduler views cannot multiply the per-session bound.
       Initial and post-retirement loads establish a fresh page domain. */
    stream->fetch_domain = load->session->page.fetch_domain == NULL
        ? fetch_scheduler_domain_create(
              load->session->budget, NAVIGATION_PAGE_FETCH_SLOT_LIMIT)
        : fetch_scheduler_domain_retain(
              load->session->page.fetch_domain);
    return stream->fetch_domain != NULL;
}

static bool navigation_load_prepare_document_stream(NavigationLoad *load)
{
    load->document_stream = budget_calloc(
        load->session->budget, 1, sizeof(*load->document_stream));
    return load->document_stream != NULL
        && navigation_load_initialize_document_stream(
               load, load->document_stream);
}

static bool navigation_load_begin_parser(NavigationLoad *load)
{
    if (!document_parser_begin(&load->parser, load->session->budget)) {
        return false;
    }
    load->parser_active = true;
    if (!document_parser_set_scripting(
            &load->parser, load->session->document_scripts_enabled)) {
        return false;
    }
    return true;
}

static bool navigation_load_prepare_dom(NavigationLoad *load)
{
    return navigation_load_begin_parser(load)
        && navigation_load_prepare_document_stream(load);
}

static bool navigation_load_prepare_sections(
    NavigationLoad *load, size_t block_bytes,
    size_t maximum_section_bytes)
{
    return section_route_stream_begin(
        &load->section_router, load->section_builder, load->section_store,
        load->session->budget, block_bytes, maximum_section_bytes, true);
}

static void navigation_load_release_candidate(NavigationLoad *load)
{
    if (load == NULL) return;
    layout_build_job_destroy(load->layout_job);
    load->layout_job = NULL;
    if (load->candidate_session != NULL) {
        if (load->session != NULL) {
            navigation_compiled_stylesheet_cache_move(
                load->session, load->candidate_session);
        }
        page_destroy_value(load->candidate_session,
                           &load->candidate_session->page);
        budget_free(load->session == NULL ? NULL : load->session->budget,
                    load->candidate_session);
        load->candidate_session = NULL;
    }
    document_destroy(&load->finalized_document);
}

static void navigation_load_release_staging(NavigationLoad *load,
                                            bool abort_sections)
{
    if (load == NULL) return;
    navigation_load_release_candidate(load);
    if (load->staging_released) return;
    NavigationDocumentStream *stream = load->document_stream;
    /* Runtime, style, layout, and preload consumers retain DOM identities.
       Destroy them before the parser's owner rollback reclaims raw Lexbor
       allocations, then release the now-empty stream shell. */
    navigation_stream_destroy_staging(stream);
    if (load->parser_active) {
        document_parser_abort(&load->parser);
        load->parser_active = false;
    }
    budget_free(load->session == NULL ? NULL : load->session->budget,
                stream);
    load->document_stream = NULL;
    if (abort_sections) section_route_stream_abort(&load->section_router);
    budget_free(load->session == NULL ? NULL : load->session->budget,
                load->staged_body);
    load->staged_body = NULL;
    load->staged_body_offset = 0;
    load->staged_body_length = 0;
    load->staged_body_capacity = 0;
    load->staged_stall = false;
    load->staging_released = true;
}

static bool navigation_load_retire_incumbent(NavigationLoad *load)
{
    if (load == NULL || !load->low_memory_replacement
        || load->incumbent_retired) return load != NULL;
    NavigationSession *session = load->session;
    if (session == NULL || !page_has_owned_state(&session->page)) {
        return false;
    }

    size_t before = session->budget->current;
    bool frozen = session->replacement_freeze != NULL
        && session->replacement_freeze(
               session->replacement_hook_opaque, session);
    /* page_destroy only retires native ownership; it deliberately does not
       synthesize beforeunload/unload/pagehide author events. Destroying each
       runtime also drains its callback queues before their DOM can vanish. */
    page_destroy(session);
    (void) navigation_rebind_top_level_remote_document(session, NULL);
    size_t after = session->budget->current;
    size_t released = before >= after ? before - after : 0;
    session->performance.low_memory_replacements_started++;
    session->performance.low_memory_last_released_bytes = released;
    if (released
        > session->performance.low_memory_peak_released_bytes) {
        session->performance.low_memory_peak_released_bytes = released;
    }
    session->performance.low_memory_last_rollback_available = false;
    session->performance.low_memory_last_frozen_frame = frozen;
    session->replacement_failure_overlay_active = false;
    session->replacement_failure_message[0] = '\0';
    load->incumbent_retired = true;
    return true;
}

static void navigation_load_report_replacement_outcome(
    NavigationLoad *load, bool success)
{
    if (load == NULL || !load->incumbent_retired
        || load->replacement_outcome_reported) return;
    NavigationSession *session = load->session;
    load->replacement_outcome_reported = true;
    if (success) {
        session->performance.low_memory_replacements_succeeded++;
        session->replacement_failure_overlay_active = false;
        session->replacement_failure_message[0] = '\0';
    } else {
        session->performance.low_memory_replacements_failed++;
        session->viewport = load->incumbent_viewport;
        session->fonts = load->incumbent_fonts;
        session->images = load->incumbent_images_page_owned
            ? NULL : load->incumbent_images;
        session->resource_fingerprint =
            load->incumbent_resource_fingerprint;
        session->style_rebuild_required =
            load->incumbent_style_rebuild_required;
        session->relayout_damage_valid =
            load->incumbent_relayout_damage_valid;
        session->relayout_damage_left =
            load->incumbent_relayout_damage_left;
        session->relayout_damage_top =
            load->incumbent_relayout_damage_top;
        session->relayout_damage_right =
            load->incumbent_relayout_damage_right;
        session->relayout_damage_bottom =
            load->incumbent_relayout_damage_bottom;
        session->replacement_failure_overlay_active = true;
        snprintf(session->replacement_failure_message,
                 sizeof(session->replacement_failure_message), "%s",
                 session->last_error[0] == '\0'
                   ? "replacement navigation failed; retry available"
                   : session->last_error);
    }
    if (session->replacement_outcome != NULL) {
        session->replacement_outcome(
            session->replacement_hook_opaque, session, success);
    }
}

static bool navigation_load_parse_after_incumbent_retirement(
    NavigationLoad *load)
{
    if (load == NULL || !load->low_memory_replacement) return false;
    if (!navigation_load_retire_incumbent(load)
        || !navigation_load_begin_parser(load)
        || !navigation_stream_headers(
               load->document_stream, &load->response)) return false;

    const size_t chunk_bytes = 64u * 1024u;
    size_t offset = 0;
    uint64_t started = navigation_now_us();
    while (offset < load->staged_body_length) {
        size_t remaining = load->staged_body_length - offset;
        size_t part = remaining < chunk_bytes ? remaining : chunk_bytes;
        if (!navigation_stream_body(
                load->document_stream,
                load->staged_body + load->staged_body_offset + offset,
                part)) {
            return false;
        }
        offset += part;
        if (offset < load->staged_body_length) {
            load->metrics.parser_quota_yields++;
        }
    }
    if (load->staged_stall
        && !navigation_stream_stall(load->document_stream)) return false;
    uint64_t elapsed = navigation_now_us() - started;
    load->metrics.parser_pump_us += elapsed;
    if (elapsed > load->metrics.maximum_parser_pump_us) {
        load->metrics.maximum_parser_pump_us = elapsed;
    }
    budget_free(load->session->budget, load->staged_body);
    load->staged_body = NULL;
    load->staged_body_offset = 0;
    load->staged_body_length = 0;
    load->staged_body_capacity = 0;
    load->staged_stall = false;
    return true;
}

static bool navigation_load_reset_for_retry(NavigationLoad *load)
{
    load->client_hint_retry_pending = false;
    load->response_cookies_accepted = false;
    load->staged_body_length = 0;
    load->staged_body_offset = 0;
    load->staged_stall = false;
    navigation_load_reset_preload_metrics(load);
    snprintf(load->effective_url, sizeof(load->effective_url), "%s",
             load->request_url);
    snprintf(load->effective_history_url,
             sizeof(load->effective_history_url), "%s",
             load->requested_url);
    if (load->sectioned) {
        section_route_stream_abort(&load->section_router);
        load->staging_released = false;
        return navigation_load_prepare_sections(
            load, load->section_block_bytes,
            load->section_maximum_bytes);
    }
    navigation_stream_destroy_staging(load->document_stream);
    /* Leave the retained stream shell destructible even if the replacement
       parser allocation fails.  Every owned preload/resource pointer was
       released above; clearing the shell also prevents a retry from
       observing stale node identities, counts, or deadlines. */
    memset(load->document_stream, 0, sizeof(*load->document_stream));
    load->document_stream->session = load->session;
    if (load->parser_active) document_parser_abort(&load->parser);
    load->parser_active = false;
    if (load->low_memory_replacement && !load->incumbent_retired) {
        return navigation_load_initialize_document_stream(
            load, load->document_stream);
    }
    if (!document_parser_begin(&load->parser, load->session->budget)) {
        return false;
    }
    load->parser_active = true;
    if (!document_parser_set_scripting(
            &load->parser, load->session->document_scripts_enabled)) {
        return false;
    }
    NavigationDocumentStream *stream = load->document_stream;
    return navigation_load_initialize_document_stream(load, stream);
}

static uint64_t navigation_load_enqueue_fetch_request(NavigationLoad *load)
{
    if (load == NULL || load->session == NULL || load->scheduler == NULL) {
        return 0;
    }
    bool contextual = load->source_url[0] != '\0';
    FetchPageSecurityContext security = {
        .document_url = contextual ? load->source_url : NULL,
        .top_level_url = contextual ? load->source_url : load->request_url,
        .referrer_policy = load->session->page.referrer_policy,
        .session = load->session->browser_session
    };
    FetchPageRequestDescriptor descriptor = {
        .target_url = load->request_url,
        .method = load->method,
        .mode = TILEFINCH_REQUEST_MODE_NAVIGATE,
        .credentials = TILEFINCH_CREDENTIALS_INCLUDE,
        .destination = TILEFINCH_DESTINATION_DOCUMENT,
        .top_level_navigation = true,
        .user_activated = !contextual,
        .transport = {
            .body = load->body, .body_length = load->body_length,
            .content_type = load->content_type[0] == '\0'
                ? NULL : load->content_type,
            .allow_http_errors = true, .send_low_client_hints = true,
            .send_client_hints = load->client_hint_tokens[0] != '\0',
            .client_hint_tokens = load->client_hint_tokens[0] == '\0'
                ? NULL : load->client_hint_tokens,
            .client_hint_origin = load->client_hint_origin[0] == '\0'
                ? NULL : load->client_hint_origin,
            .accept = "text/html,application/xhtml+xml,application/xml;q=0.9,*/*;q=0.8",
            .upgrade_insecure_requests = true
        }
    };
    /* The scheduler takes owned copies of the request and its context before
       returning. Keep the 7 KiB string envelope transient instead of pinning
       it in NavigationLoad for the entire parser/layout lifetime. */
    FetchPreparedPageRequest prepared;
    if (!fetch_prepare_page_request(
            &security, &descriptor, &prepared, NULL)) return 0;
    const FetchRequest *request = fetch_prepared_page_request(&prepared);
    const TilefinchRequestContext *context =
        fetch_prepared_page_request_context(&prepared);
    if (request == NULL || context == NULL) return 0;
    load->request_context = *context;
    return fetch_scheduler_enqueue_stream(
        load->scheduler, load->request_url, request,
        load->maximum_bytes, load->timeout_ms, &load->stream_options);
}

static bool navigation_load_retarget_client_hint_retry(NavigationLoad *load)
{
    if (load == NULL || load->session == NULL) return false;
    char retry_url[NAVIGATION_URL_LIMIT];
    const char *effective = load->response.effective_url[0] == '\0'
        ? load->effective_url : load->response.effective_url;
    if (!tilefinch_url_request_key(effective, retry_url, sizeof(retry_url))) {
        return false;
    }
    if (load->client_hint_tokens[0] == '\0'
        || !tilefinch_url_potentially_trustworthy(retry_url)
        || !tilefinch_url_origin(retry_url, load->client_hint_origin,
                              sizeof(load->client_hint_origin))) return false;

    /* The final response origin requested these hints.  Restart it directly;
       restarting the pre-redirect URL would disclose high-entropy hints to a
       redirector and would build the Cookie header for the wrong path/origin. */
    snprintf(load->request_url, sizeof(load->request_url), "%s", retry_url);
    return true;
}

static NavigationLoad *navigation_load_begin_common(
    NavigationSession *session, uint64_t generation, const char *url,
    const char *method, const char *body, size_t body_length,
    const char *content_type, size_t maximum_bytes, long timeout_ms,
    int viewport_width, const FontSet *fonts,
    const ImageResources *images, bool record_history,
    CompressedSectionStore *store, SectionStoreStreamBuilder *builder,
    size_t block_bytes, size_t maximum_section_bytes,
    size_t initial_section)
{
    if (session == NULL || session->budget == NULL || generation == 0
        || generation != session->generation || session->cancelled
        || url == NULL || maximum_bytes == 0 || timeout_ms <= 0
        || viewport_width <= 0 || body_length > 64u * 1024u
        || (body == NULL && body_length != 0)
        || ((store == NULL) != (builder == NULL))) return NULL;
    /* The compiled sheet is an optional RAM acceleration. Do not carry it
       into candidate construction when the page budget has lost its reserve;
       this is the sole pressure-eviction point and never touches persistence. */
    navigation_compiled_stylesheet_cache_reclaim_under_pressure(session);
    NavigationLoad *load = budget_calloc(
        session->budget, 1, sizeof(*load));
    if (load == NULL) return NULL;
    load->session = session;
    load->generation = generation;
    load->started_us = navigation_now_us();
    load->maximum_bytes = maximum_bytes;
    load->timeout_ms = timeout_ms;
    load->viewport_width = viewport_width;
    load->fonts = fonts;
    load->images = images;
    load->record_history = record_history;
    load->sectioned = store != NULL;
    bool low_memory_requested = store == NULL
        && session->replacement_mode == NAVIGATION_REPLACEMENT_LOW_MEMORY
        && page_has_owned_state(&session->page);
    /* Keep the demonstrated complex-runtime safety boundary without forcing
       a small progressively-enhanced document onto the overlapping path. A
       live AWS challenge with a large external-script state exposed Bellard
       exception-backtrace corruption during retirement. Small bounded states
       are exercised through the same drain-and-destroy path as inline-only
       pages; larger static or dynamic graphs retain transactional rollback. */
    const ScriptResult *incumbent_result = &session->page.script_result;
    bool external_script_incumbent = session->script_loaded != 0
        || session->script_bytes != 0;
    bool bounded_external_script_incumbent = external_script_incumbent
        && session->script_loaded <= 2
        && session->script_bytes <= 128u * 1024u
        && incumbent_result->dynamic_scripts_completed <= 4
        && incumbent_result->dynamic_script_bytes <= 128u * 1024u;
    load->low_memory_replacement = low_memory_requested
        && (!external_script_incumbent || bounded_external_script_incumbent);
    if (low_memory_requested && !load->low_memory_replacement) {
        session->performance.low_memory_transactional_fallbacks++;
    }
    if (load->low_memory_replacement) {
        load->incumbent_viewport = session->viewport;
        load->incumbent_fonts = session->fonts;
        load->incumbent_images = session->images;
        load->incumbent_images_page_owned =
            session->images == &session->page.images;
        load->incumbent_resource_fingerprint =
            session->resource_fingerprint;
        load->incumbent_style_rebuild_required =
            session->style_rebuild_required;
        load->incumbent_relayout_damage_valid =
            session->relayout_damage_valid;
        load->incumbent_relayout_damage_left =
            session->relayout_damage_left;
        load->incumbent_relayout_damage_top =
            session->relayout_damage_top;
        load->incumbent_relayout_damage_right =
            session->relayout_damage_right;
        load->incumbent_relayout_damage_bottom =
            session->relayout_damage_bottom;
    }
    load->section_store = store;
    load->section_builder = builder;
    load->section_block_bytes = block_bytes;
    load->section_maximum_bytes = maximum_section_bytes;
    load->initial_section = initial_section;
    load->response.budget = session->budget;
    const char *actual_method = method == NULL ? "GET" : method;
    if (strlen(actual_method) >= sizeof(load->method)
        || !tilefinch_url_normalize(url, load->requested_url,
                                 sizeof(load->requested_url))
        || !tilefinch_url_request_key(load->requested_url, load->request_url,
                                   sizeof(load->request_url))) {
        navigation_load_set_error(
            load, NAVIGATION_LOAD_FAILED,
            "navigation URL or method failed canonical policy");
        navigation_load_destroy(load);
        return NULL;
    }
    snprintf(load->effective_url, sizeof(load->effective_url), "%s",
             load->request_url);
    snprintf(load->effective_history_url,
             sizeof(load->effective_history_url), "%s",
             load->requested_url);
    snprintf(load->method, sizeof(load->method), "%s", actual_method);
    if (content_type != NULL) {
        if (strlen(content_type) >= sizeof(load->content_type)) {
            navigation_load_set_error(load, NAVIGATION_LOAD_FAILED,
                                      "navigation content type is too long");
            navigation_load_destroy(load);
            return NULL;
        }
        snprintf(load->content_type, sizeof(load->content_type), "%s",
                 content_type);
    }
    if (body_length != 0) {
        load->body = budget_malloc(session->budget, body_length + 1);
        if (load->body == NULL) {
            navigation_load_set_error(load, NAVIGATION_LOAD_FAILED,
                                      "navigation body allocation failed");
            navigation_load_destroy(load);
            return NULL;
        }
        memcpy(load->body, body, body_length);
        load->body[body_length] = '\0';
        load->body_length = body_length;
    }
    bool contextual_navigation =
        session->pending_navigation_referer[0] != '\0';
    if (contextual_navigation
        && !tilefinch_url_request_key(session->pending_navigation_referer,
                                   load->source_url,
                                   sizeof(load->source_url))) {
        navigation_load_set_error(load, NAVIGATION_LOAD_FAILED,
                                  "navigation source URL is invalid");
        navigation_load_destroy(load);
        return NULL;
    }
    session->pending_navigation_referer[0] = '\0';
    session->pending_navigation_same_origin = false;
    load->request_context = (TilefinchRequestContext) {
        .target_url = load->request_url,
        .initiator_url = contextual_navigation ? load->source_url : NULL,
        .top_level_url = contextual_navigation
            ? load->source_url : load->request_url,
        .method = load->method,
        .mode = TILEFINCH_REQUEST_MODE_NAVIGATE,
        .credentials = TILEFINCH_CREDENTIALS_INCLUDE,
        .destination = TILEFINCH_DESTINATION_DOCUMENT,
        .top_level_navigation = true,
        .user_activated = !contextual_navigation
    };
    session->performance.first_dom_us = 0;
    session->performance.first_layout_us = 0;
    session->performance.first_paint_us = 0;
    session->performance.completion_us = 0;
    session->performance.partial_layouts = 0;
    session->performance.partial_paints = 0;
    navigation_load_reset_preload_metrics(load);
    bool staging_ready = load->sectioned
        ? navigation_load_prepare_sections(
            load, block_bytes, maximum_section_bytes)
        : (load->low_memory_replacement
             ? navigation_load_prepare_document_stream(load)
             : navigation_load_prepare_dom(load));
    char scheduler_error[96] = {0};
    load->scheduler = fetch_scheduler_create_ex(
        session->budget, 1, maximum_bytes,
        scheduler_error, sizeof(scheduler_error));
    if (!staging_ready || load->scheduler == NULL) {
        char startup_error[160];
        snprintf(
            startup_error, sizeof(startup_error),
            "navigation streaming startup failed: %s",
            !staging_ready ? "document staging exceeded page memory"
                           : (scheduler_error[0] == '\0'
                                  ? "scheduler unavailable"
                                  : scheduler_error));
        navigation_load_set_error(
            load, NAVIGATION_LOAD_FAILED, startup_error);
        navigation_load_destroy(load);
        return NULL;
    }
    /* A document navigation owns one serial redirect chain, so moving its
       DNS/TLS/body transport off the browser thread cannot reduce resource
       parallelism. The page-resource schedulers use the same service through
       their separate six-lane opt-in; this scheduler still needs only one. */
    (void) fetch_scheduler_enable_background_transport(
        load->scheduler, true);
    load->stream_options = (FetchStreamOptions) {
        .on_headers = navigation_load_headers,
        .on_body = navigation_load_body,
        .on_stall = navigation_load_stall,
        .opaque = load,
        .chunk_bytes = session->stream_delivery.chunk_bytes,
        .irregular_seed = session->stream_delivery.irregular_seed,
        .irregular_max_chunk_bytes =
            session->stream_delivery.irregular_max_chunk_bytes,
        .stall_every_chunks = session->stream_delivery.stall_every_chunks,
        .cancel_after_bytes = session->stream_delivery.cancel_after_bytes,
        .truncate_after_bytes = session->stream_delivery.truncate_after_bytes
    };
    load->request_id = navigation_load_enqueue_fetch_request(load);
    if (load->request_id == 0) {
        char enqueue_error[160];
        snprintf(
            enqueue_error, sizeof(enqueue_error),
            "navigation request could not be queued: %s",
            fetch_scheduler_last_error(load->scheduler));
        navigation_load_set_error(load, NAVIGATION_LOAD_FAILED,
                                  enqueue_error);
        navigation_load_destroy(load);
        return NULL;
    }
    memset(&session->last_stream, 0, sizeof(session->last_stream));
    load->status = NAVIGATION_LOAD_PENDING;
    return load;
}

NavigationLoad *navigation_load_begin_request(
    NavigationSession *session, uint64_t generation, const char *url,
    const char *method, const char *body, size_t body_length,
    const char *content_type, size_t maximum_bytes, long timeout_ms,
    int viewport_width, const FontSet *fonts,
    const ImageResources *images, bool record_history)
{
    return navigation_load_begin_common(
        session, generation, url, method, body, body_length, content_type,
        maximum_bytes, timeout_ms, viewport_width, fonts, images,
        record_history, NULL, NULL, 0, 0, 0);
}

NavigationLoad *navigation_load_begin_url(
    NavigationSession *session, uint64_t generation, const char *url,
    size_t maximum_bytes, long timeout_ms, int viewport_width,
    const FontSet *fonts, const ImageResources *images,
    bool record_history)
{
    return navigation_load_begin_request(
        session, generation, url, "GET", NULL, 0, NULL, maximum_bytes,
        timeout_ms, viewport_width, fonts, images, record_history);
}

NavigationLoad *navigation_load_begin_sectioned(
    NavigationSession *session, uint64_t generation, const char *url,
    size_t maximum_bytes, long timeout_ms, int viewport_width,
    const FontSet *fonts, const ImageResources *images, bool record_history,
    CompressedSectionStore *store, SectionStoreStreamBuilder *builder,
    size_t block_bytes, size_t maximum_section_bytes,
    size_t initial_section)
{
    return navigation_load_begin_common(
        session, generation, url, "GET", NULL, 0, NULL, maximum_bytes,
        timeout_ms, viewport_width, fonts, images, record_history, store,
        builder, block_bytes, maximum_section_bytes, initial_section);
}

NavigationLoadStatus navigation_load_pump(
    NavigationLoad *load, const NavigationLoadQuota *quota)
{
    if (load == NULL || load->status != NAVIGATION_LOAD_PENDING) {
        return load == NULL ? NAVIGATION_LOAD_FAILED : load->status;
    }
    NavigationSession *session = load->session;
    if (session->cancelled || session->generation != load->generation) {
        navigation_load_cancel(load, "navigation generation was cancelled");
        return load->status;
    }
    bool defer_candidate_parse = load->low_memory_replacement
        && !load->incumbent_retired;
    uint64_t started = navigation_now_us();
    load->metrics.pump_calls++;
    if (!defer_candidate_parse
        && (load->staged_body_length != 0 || load->staged_stall)) {
        if (!navigation_load_process_staged_body(load, quota)) {
            navigation_load_set_error(
                load, NAVIGATION_LOAD_FAILED,
                load->sectioned
                  ? "section parser rejected staged response body"
                  : "HTML parser rejected staged response body");
            navigation_load_release_staging(load, true);
            return load->status;
        }
        uint64_t elapsed = navigation_now_us() - started;
        load->metrics.total_pump_us += elapsed;
        if (elapsed > load->metrics.maximum_pump_us) {
            load->metrics.maximum_pump_us = elapsed;
        }
        if (load->staged_body_length != 0 || load->staged_stall) {
            return load->status;
        }
        /* Give control back after parser work instead of immediately entering
           transport again. This is the cooperative boundary callers can rely
           on even when a parser dependency overran its observed threshold. */
        load->metrics.quota_yields++;
        return load->status;
    }
    FetchPumpMetrics pump = {0};
    (void) fetch_scheduler_pump_bounded(
        load->scheduler, 1, 10, quota == NULL ? NULL : &quota->fetch, &pump);
    session->performance.network_us += navigation_record_slice(
        session, NAVIGATION_SLICE_NETWORK, started, pump.body_bytes);
    load->metrics.body_bytes += pump.body_bytes;
    load->metrics.body_callbacks += pump.body_callbacks;
    if (pump.peak_buffered_bytes > load->metrics.peak_buffered_bytes) {
        load->metrics.peak_buffered_bytes = pump.peak_buffered_bytes;
    }
    if (pump.quota_yielded) load->metrics.quota_yields++;
    (void) fetch_scheduler_stream_metrics(
        load->scheduler, load->request_id, &load->metrics.stream);
    if (load->sectioned && load->section_store != NULL) {
        load->metrics.safe_sections_ready =
            load->section_store->sealed_section_count;
        load->metrics.safe_section_source_bytes =
            load->section_store->sealed_source_bytes;
    }
    if (!defer_candidate_parse
        && !navigation_load_process_staged_body(load, quota)) {
        navigation_load_set_error(
            load, NAVIGATION_LOAD_FAILED,
            load->sectioned ? "section parser rejected staged response body"
                            : "HTML parser rejected staged response body");
        navigation_load_release_staging(load, true);
        return load->status;
    }
    uint64_t total_elapsed = navigation_now_us() - started;
    load->metrics.total_pump_us += total_elapsed;
    if (total_elapsed > load->metrics.maximum_pump_us) {
        load->metrics.maximum_pump_us = total_elapsed;
    }
    /* Completion cannot be consumed until every callback byte has crossed
       the separately-budgeted parser boundary. */
    if (!defer_candidate_parse
        && (load->staged_body_length != 0 || load->staged_stall)) {
        return load->status;
    }
    bool success = false;
    FetchStreamMetrics stream_metrics = {0};
    if (!fetch_scheduler_take_stream(
            load->scheduler, load->request_id, &success, &stream_metrics,
            &load->response)) return load->status;
    load->request_id = 0;
    load->metrics.stream = stream_metrics;
    session->last_stream = stream_metrics;
    session->last_http_status = load->response.status_code;
    session->last_transport_code = load->response.transport_code;
    session->last_tls_verify_result = load->response.tls_verify_result;
    session->last_transport_timed_out = load->response.timed_out;
    session->last_tls12_compatibility_retry =
        load->response.tls12_compatibility_retry;
    snprintf(session->last_tls_version,
             sizeof(session->last_tls_version), "%s",
             load->response.tls_version);
    if (!success) {
        if (navigation_load_retry_zero_body_deadline(load)) {
            return load->status;
        }
        const char *detail = load->response.error;
        if (strcmp(detail, "stream consumer rejected response") == 0
            && session->last_error[0] != '\0') {
            detail = session->last_error;
        }
        navigation_load_set_error(
            load, NAVIGATION_LOAD_FAILED,
            detail[0] == '\0' ? "navigation request failed" : detail);
        navigation_load_release_staging(load, true);
        return load->status;
    }
    snprintf(session->last_cf_mitigated,
             sizeof(session->last_cf_mitigated), "%s",
             load->response.cf_mitigated);
    snprintf(session->last_server, sizeof(session->last_server), "%s",
             load->response.server);
    if (!load->response_cookies_accepted) {
        navigation_accept_response_cookies(
            session->browser_session, &load->request_context, &load->response,
            load->response.effective_url[0] == '\0'
              ? load->request_url : load->response.effective_url);
        load->response_cookies_accepted = true;
    }
    if (load->client_hint_retry_pending) {
        if (!navigation_load_retarget_client_hint_retry(load)) {
            navigation_load_set_error(
                load, NAVIGATION_LOAD_FAILED,
                "client-hint retry request context is invalid");
            navigation_load_release_staging(load, true);
            return load->status;
        }
        load->client_hint_retried = true;
        session->client_hint_retries++;
        if (!navigation_load_reset_for_retry(load)) {
            navigation_load_set_error(
                load, NAVIGATION_LOAD_FAILED,
                "HTML parser retry startup failed within the page budget");
            navigation_load_release_staging(load, true);
            return load->status;
        }
        fetch_result_destroy(&load->response);
        load->request_id = navigation_load_enqueue_fetch_request(load);
        if (load->request_id == 0) {
            navigation_load_set_error(load, NAVIGATION_LOAD_FAILED,
                                      "client-hint retry could not be queued");
            navigation_load_release_staging(load, true);
        }
        memset(&session->last_stream, 0, sizeof(session->last_stream));
        return load->status;
    }
    const char *effective = load->response.effective_url[0] == '\0'
        ? load->request_url : load->response.effective_url;
    if (!tilefinch_url_normalize(effective, load->effective_history_url,
                              sizeof(load->effective_history_url))
        || !tilefinch_url_request_key(effective, load->effective_url,
                                sizeof(load->effective_url))) {
        navigation_load_set_error(load, NAVIGATION_LOAD_FAILED,
                                  "effective navigation URL is invalid");
        navigation_load_release_staging(load, true);
        return load->status;
    }
    load->response_taken = true;
    load->status = NAVIGATION_LOAD_READY_TO_FINISH;
    return load->status;
}

static bool navigation_load_history_url(
    const NavigationLoad *load, char output[NAVIGATION_URL_LIMIT])
{
    if (!tilefinch_url_normalize(load->effective_history_url, output,
                              NAVIGATION_URL_LIMIT)) return false;
    const char *fragment = strchr(load->requested_url, '#');
    if (fragment != NULL && strchr(output, '#') == NULL) {
        size_t used = strlen(output), length = strlen(fragment);
        if (length >= NAVIGATION_URL_LIMIT - used) return false;
        memcpy(output + used, fragment, length + 1);
    }
    return true;
}

static bool navigation_load_finalize_parse(NavigationLoad *load)
{
    NavigationSession *session = load->session;
    load->response_referrer_policy[0] = '\0';
    bool referrer_policy_header_present = false;
    if (!fetch_response_referrer_policy(
            &load->response, &referrer_policy_header_present,
            load->response_referrer_policy)) {
        navigation_load_set_error(
            load, NAVIGATION_LOAD_FAILED,
            "response Referrer-Policy metadata is invalid");
        return false;
    }
    if (!load->sectioned) {
        if (load->low_memory_replacement
            && !navigation_load_parse_after_incumbent_retirement(load)) {
            navigation_load_set_error(
                load, NAVIGATION_LOAD_FAILED,
                "low-memory replacement parser startup failed after "
                "incumbent retirement");
            return false;
        }
        NavigationDocumentStream *stream = load->document_stream;
        /* Speculative preloads are opportunistic. Never wait for them at the
           transactional EOF boundary; blocking resources are resolved by the
           candidate pipeline with their normal priority and failure rules. */
        uint64_t metadata_started = navigation_now_us();
        if (!navigation_stream_scan_metadata(stream)) {
            load->status = NAVIGATION_LOAD_FAILED;
            return false;
        }
        uint64_t metadata_finished = navigation_now_us();
        session->performance.parser_eof_metadata_us +=
            metadata_finished >= metadata_started
                ? metadata_finished - metadata_started : 0;
        uint64_t preload_started = metadata_finished;
        navigation_stream_launch_preloads(stream);
        navigation_stream_settle_preloads(stream);
        uint64_t preload_finished = navigation_now_us();
        session->performance.parser_eof_preload_us +=
            preload_finished >= preload_started
                ? preload_finished - preload_started : 0;
        /* The stream begins with the response policy and applies each valid
           document metadata update in tree order.  Persist that final policy
           even when the response header was nonempty. */
        snprintf(load->response_referrer_policy,
                 sizeof(load->response_referrer_policy), "%s",
                 stream->referrer_policy);
        if (!tilefinch_platform_cooperate(
                "navigation-eof", load->metrics.body_bytes)) {
            navigation_load_set_error(
                load, NAVIGATION_LOAD_CANCELLED,
                "navigation cancelled before parser finalization");
            return false;
        }
        uint64_t started = navigation_now_us();
        if (!document_parser_finish(&load->parser,
                                    &load->finalized_document)) {
            navigation_load_set_error(
                load, NAVIGATION_LOAD_FAILED,
                "HTML parser finalization failed within the page budget");
            return false;
        }
        load->parser_active = false;
        uint64_t elapsed = navigation_record_slice(
            session, NAVIGATION_SLICE_PARSER, started,
            load->finalized_document.node_count);
        session->performance.parser_finish_us += elapsed;
        session->performance.parse_us += stream->parse_us + elapsed;
        load->metrics.finalize_work_units =
            load->finalized_document.node_count;
        stylesheet_document_resources_open_final_retry(
            &stream->stylesheet_resources);
        /* A fully successful parser sheet is authoritative at EOF and may
           be adopted by the final page. Preserve the existing rebuild path
           after any parser-time CSS failure so the final bounded retry is
           still attempted once transport pressure has cleared. */
        if (stream->blocking_stylesheets.failed != 0) {
            navigation_stream_release_blocking_stylesheet(stream);
        }
    } else {
        if (!section_route_stream_finish(&load->section_router)) {
            navigation_load_set_error(load, NAVIGATION_LOAD_FAILED,
                                      "section stream finalization failed");
            return false;
        }
        const char *html = (const char *) load->section_router.buffer;
        size_t html_length = load->section_router.length;
        char *extracted = NULL;
        if (load->section_router.sectioned) {
            if (load->section_store->section_count == 0
                || load->initial_section >= load->section_store->section_count
                || !section_store_extract_html(
                    load->section_store, load->initial_section,
                    &extracted, &html_length)) {
                navigation_load_set_error(load, NAVIGATION_LOAD_FAILED,
                                          "requested section is unavailable");
                return false;
            }
            html = extracted;
            load->metrics.safe_sections_ready =
                load->section_store->section_count;
            load->metrics.safe_section_source_bytes =
                load->section_store->source_length;
        }
        uint64_t started = navigation_now_us();
        bool parsed = document_parse(
            &load->finalized_document, session->budget, html, html_length,
            512);
        budget_free(session->budget, extracted);
        if (!parsed) {
            navigation_load_set_error(
                load, NAVIGATION_LOAD_FAILED,
                "section HTML parsing failed within the page budget");
            return false;
        }
        if (!navigation_scan_document_referrer_policy(
                &load->finalized_document,
                load->response_referrer_policy)) {
            navigation_load_set_error(
                load, NAVIGATION_LOAD_FAILED,
                "section referrer metadata traversal failed");
            return false;
        }
        session->performance.parse_us += navigation_record_slice(
            session, NAVIGATION_SLICE_PARSER, started, html_length);
        load->metrics.finalize_work_units = html_length;
        section_route_stream_abort(&load->section_router);
        load->staging_released = true;
    }
    load->finalize_phase = NAVIGATION_FINALIZE_BUILD;
    load->status = NAVIGATION_LOAD_FINALIZING;
    return true;
}

static void navigation_load_close_stream_after_build(NavigationLoad *load)
{
    if (load->document_stream != NULL) {
        navigation_stream_release(load->document_stream);
        load->document_stream = NULL;
    }
    load->parser_active = false;
    load->staging_released = true;
    budget_free(load->session->budget, load->staged_body);
    load->staged_body = NULL;
    load->staged_body_offset = 0;
    load->staged_body_length = 0;
    load->staged_body_capacity = 0;
    load->staged_stall = false;
}

static bool navigation_load_finalize_build(NavigationLoad *load)
{
    NavigationSession *session = load->session;
    NavigationSession *candidate = load->candidate_session;
    bool built = true;
    if (candidate == NULL) {
        candidate = budget_malloc(session->budget, sizeof(*candidate));
        if (candidate == NULL) {
            navigation_load_set_error(
                load, NAVIGATION_LOAD_FAILED,
                "candidate session staging exceeded the page budget");
            return false;
        }
        *candidate = *session;
        memset(&candidate->compiled_stylesheet_cache, 0,
               sizeof(candidate->compiled_stylesheet_cache));
        memset(&candidate->compiled_stylesheet_cache_external_stats, 0,
               sizeof(candidate->compiled_stylesheet_cache_external_stats));
        candidate->compiled_stylesheet_cache_fingerprint = 0;
        candidate->compiled_stylesheet_cache_resource_signature = 0;
        candidate->compiled_stylesheet_cache_retained_bytes = 0;
        candidate->compiled_stylesheet_cache_ready = false;
        navigation_compiled_stylesheet_cache_move(candidate, session);
        memset(&candidate->page, 0, sizeof(candidate->page));
        candidate->candidate_prepare = NULL;
        candidate->candidate_abort = NULL;
        candidate->candidate_commit = NULL;
        candidate->candidate_commit_opaque = NULL;
        snprintf(candidate->pending_response_referrer_policy,
                 sizeof(candidate->pending_response_referrer_policy), "%s",
                 load->response_referrer_policy);
        load->candidate_session = candidate;
        built = navigation_commit_document(
            candidate, load->generation, load->commit_url,
            &load->finalized_document, load->viewport_width,
            load->fonts, load->images, false,
            load->sectioned ? NULL : load->document_stream,
            load->sectioned ? session->page.fetch_domain : NULL,
            true, true, &load->layout_job,
            &load->layout_preview_candidate);
        if (built && load->layout_job != NULL) return true;
    } else if (load->layout_job != NULL) {
        LayoutBuildStatus layout_status =
            layout_build_job_pump(load->layout_job);
        if (layout_status == LAYOUT_BUILD_PENDING) return true;
        if (layout_status != LAYOUT_BUILD_COMPLETE
            || !layout_build_job_take(
                   load->layout_job, &candidate->page.layout)) {
            snprintf(candidate->last_error, sizeof(candidate->last_error),
                     "resumable layout construction failed");
            built = false;
        } else {
            layout_build_job_destroy(load->layout_job);
            load->layout_job = NULL;
            uint64_t final_layout_elapsed =
                navigation_record_layout_slices_elapsed(
                    candidate, candidate->page.layout.performance.total_us,
                    &candidate->page.layout);
            candidate->performance.layout_us += final_layout_elapsed;
            candidate->performance.final_layout_us += final_layout_elapsed;
            const ImageResources *candidate_images =
                candidate->external_resources_enabled
                    ? &candidate->page.images : load->images;
            built = navigation_finish_authoritative_layout(
                candidate, load->commit_url,
                load->sectioned ? NULL : load->document_stream,
                NULL, load->layout_preview_candidate,
                load->fonts, candidate_images);
            if (built) {
                candidate->replacement_failure_overlay_active = false;
                candidate->replacement_failure_message[0] = '\0';
                candidate->loads_committed++;
            }
        }
    }
    load->metrics.finalize_work_units = candidate->page.document.node_count;
    if (candidate->page.layout.count
        <= SIZE_MAX - load->metrics.finalize_work_units) {
        load->metrics.finalize_work_units += candidate->page.layout.count;
    } else {
        load->metrics.finalize_work_units = SIZE_MAX;
    }
    if (!built) {
        snprintf(session->last_error, sizeof(session->last_error), "%s",
                 candidate->last_error[0] == '\0'
                   ? "candidate page construction failed"
                   : candidate->last_error);
        snprintf(session->last_script_error_context,
                 sizeof(session->last_script_error_context), "%s",
                 candidate->last_script_error_context);
        navigation_load_release_candidate(load);
        return false;
    }
    navigation_load_close_stream_after_build(load);
    load->finalize_phase = NAVIGATION_FINALIZE_COMMIT;
    return true;
}

static void navigation_page_relocate_runtime_storage(
    NavigationSession *session)
{
    if (session == NULL) return;
    if (session->page.runtime != NULL) {
        script_runtime_relocate_document_storage(
            session->page.runtime, &session->page.document);
        script_runtime_set_layout(session->page.runtime,
                                  &session->page.layout,
                                  session->viewport.css_height);
        script_runtime_set_stylesheet(session->page.runtime,
                                      &session->page.stylesheet);
        script_runtime_relocate_messaging_opaque(session->page.runtime,
                                                 session);
    }
    for (size_t i = 0; i < session->page.frame_count; i++) {
        NavigationFrame *frame = &session->page.frames[i];
        script_runtime_relocate_document_storage(frame->runtime,
                                                 &frame->document);
        script_runtime_relocate_messaging_opaque(frame->runtime, session);
    }
}

static bool navigation_load_finalize_commit(NavigationLoad *load)
{
    NavigationSession *session = load->session;
    NavigationSession *candidate = load->candidate_session;
    if (candidate == NULL || !candidate->page.loaded) {
        navigation_load_set_error(load, NAVIGATION_LOAD_FAILED,
                                  "candidate page is not prepared");
        return false;
    }
    /* Managed verification pages sometimes navigate themselves to a second
       challenge response whose script cannot complete on this platform.  A
       one-command, visually empty candidate must not replace the already
       useful verification screen: the transactional incumbent is both more
       informative and still owns the live challenge runtime.  This is a
       presentation safeguard, not a challenge bypass; a candidate with any
       visible text or image commits normally. */
    if (strcmp(load->response.cf_mitigated, "challenge") == 0
        && session->page.loaded
        && !navigation_preview_has_visible_content(
               &candidate->page.layout,
               candidate->viewport.css_width,
               candidate->viewport.css_height)) {
        navigation_load_set_error(
            load, NAVIGATION_LOAD_FAILED,
            "verification response did not produce a visible page");
        return false;
    }
    NavigationCandidatePrepareCallback prepare = session->candidate_prepare;
    NavigationCandidateFinalizeCallback abort = session->candidate_abort;
    NavigationCandidateFinalizeCallback commit = session->candidate_commit;
    void *hook_opaque = session->candidate_commit_opaque;
    bool hook_prepared = false;
    lxb_html_document_t *candidate_document = candidate->page.document.html;
    ScriptRuntime *candidate_runtime = candidate->page.runtime;
    DrawCommand *candidate_commands = candidate->page.layout.commands;
    FetchScheduler *candidate_scheduler =
        candidate->page.resource_scheduler;
    FetchSchedulerDomain *candidate_domain = candidate->page.fetch_domain;
    if (prepare != NULL) {
        hook_prepared = true;
        if (!prepare(hook_opaque, candidate)
            || session->generation != load->generation
            || session->cancelled
            || candidate->page.document.html != candidate_document
            || candidate->page.runtime != candidate_runtime
            || candidate->page.layout.commands != candidate_commands
            || candidate->page.resource_scheduler != candidate_scheduler
            || candidate->page.fetch_domain != candidate_domain) {
            abort(hook_opaque);
            snprintf(session->last_error, sizeof(session->last_error), "%s",
                     "candidate commit preparation failed or changed "
                     "candidate");
            navigation_load_set_error(
                load, NAVIGATION_LOAD_FAILED,
                "candidate commit preparation failed or changed candidate");
            return false;
        }
    }
    if (load->record_history
        && !history_push(session, load->commit_url,
                         candidate->page.document.title)) {
        if (hook_prepared) abort(hook_opaque);
        navigation_load_set_error(load, NAVIGATION_LOAD_FAILED,
                                  "history entry allocation failed");
        return false;
    }
    if (!load->sectioned) {
        /* The streamed candidate was built without incumbent sectional
           callbacks. Clear the incumbent binding only after every fallible
           step above has succeeded. */
        (void) navigation_rebind_top_level_remote_document(session, NULL);
    }
    NavigationEntry *history = session->history;
    size_t history_capacity = session->history_capacity;
    size_t history_count = session->history_count;
    size_t history_index = session->history_index;
    size_t history_pruned = session->history_pruned;
    bool candidate_uses_page_images =
        candidate->images == &candidate->page.images;
    /* Charge incumbent teardown to the candidate's accumulated performance
       record, then move the fully prepared page in one no-fail boundary. */
    navigation_compiled_stylesheet_cache_store(candidate, &session->page);
    page_destroy_value(candidate, &session->page);
    Budget *budget = session->budget;
    memcpy(session, candidate, sizeof(*session));
    session->history = history;
    session->history_capacity = history_capacity;
    session->history_count = history_count;
    session->history_index = history_index;
    session->history_pruned = history_pruned;
    if (candidate_uses_page_images) {
        /* images is a convenience pointer used by later relayouts. The
           candidate's inline NavigationPage changes address when its session
           is memcpy-promoted, even though the owned item allocations do not. */
        session->images = &session->page.images;
    }
    session->candidate_prepare = prepare;
    session->candidate_abort = abort;
    session->candidate_commit = commit;
    session->candidate_commit_opaque = hook_opaque;
    memset(&candidate->page, 0, sizeof(candidate->page));
    budget_free(budget, candidate);
    load->candidate_session = NULL;
    navigation_page_relocate_runtime_storage(session);
    /* The shell was staged against candidate's address. Publish the moved
       NavigationSession first, then let the no-fail commit callback repair
       its owner pointers before exposing the promoted shell. */
    if (hook_prepared) commit(hook_opaque);
    session->performance.completion_us = navigation_now_us()
                                         - load->started_us;
    load->finalize_phase = NAVIGATION_FINALIZE_DONE;
    load->status = NAVIGATION_LOAD_SUCCEEDED;
    navigation_load_report_replacement_outcome(load, true);
    return true;
}

static bool navigation_load_finish_step(NavigationLoad *load)
{
    switch (load->finalize_phase) {
    case NAVIGATION_FINALIZE_PARSE:
        return navigation_load_finalize_parse(load);
    case NAVIGATION_FINALIZE_BUILD:
        return navigation_load_finalize_build(load);
    case NAVIGATION_FINALIZE_COMMIT:
        return navigation_load_finalize_commit(load);
    case NAVIGATION_FINALIZE_DONE:
        return true;
    default:
        return false;
    }
}

bool navigation_load_finish(NavigationLoad *load,
                            const NavigationLoadQuota *quota)
{
    if (load == NULL) return false;
    if (load->status == NAVIGATION_LOAD_SUCCEEDED) return true;
    if (load->status != NAVIGATION_LOAD_READY_TO_FINISH
        && load->status != NAVIGATION_LOAD_FINALIZING) return false;
    if (load->session->cancelled
        || load->session->generation != load->generation) {
        navigation_load_cancel(load, "navigation generation was cancelled");
        return false;
    }
    if (load->commit_url[0] == '\0'
        && !navigation_load_history_url(load, load->commit_url)) {
        navigation_load_set_error(load, NAVIGATION_LOAD_FAILED,
                                  "history URL canonicalization failed");
        navigation_load_release_staging(load, true);
        return false;
    }
    do {
        uint64_t started = navigation_now_us();
        size_t before_work = load->metrics.finalize_work_units;
        bool advanced = navigation_load_finish_step(load);
        uint64_t elapsed = navigation_now_us() - started;
        load->metrics.finalize_us += elapsed;
        load->metrics.finalize_slices++;
        load->metrics.finalize_phase = (size_t) load->finalize_phase;
        size_t phase_work = load->metrics.finalize_work_units >= before_work
            ? load->metrics.finalize_work_units - before_work
            : load->metrics.finalize_work_units;
        if (quota != NULL
            && ((quota->maximum_finalize_time_us != 0
                 && elapsed > quota->maximum_finalize_time_us)
                || (quota->maximum_finalize_work_units != 0
                    && phase_work
                       > quota->maximum_finalize_work_units))) {
            load->metrics.finalize_quota_exceeded = true;
        }
        if (!advanced) {
            if (load->status != NAVIGATION_LOAD_CANCELLED) {
                load->status = NAVIGATION_LOAD_FAILED;
                navigation_load_release_staging(load, true);
            }
            if (load->session->last_script_error_context[0] == '\0') {
                capture_cached_script_error(
                    load->session, load->session->last_error);
            }
            navigation_load_report_replacement_outcome(load, false);
            return false;
        }
        if (load->status == NAVIGATION_LOAD_SUCCEEDED) return true;
        if (quota != NULL) {
            load->metrics.finalize_yields++;
            return false;
        }
    } while (load->status == NAVIGATION_LOAD_FINALIZING);
    return load->status == NAVIGATION_LOAD_SUCCEEDED;
}

NavigationLoadStatus navigation_load_status(const NavigationLoad *load)
{
    return load == NULL ? NAVIGATION_LOAD_FAILED : load->status;
}

bool navigation_load_metrics(const NavigationLoad *load,
                             NavigationLoadMetrics *metrics)
{
    if (load == NULL || metrics == NULL) return false;
    *metrics = load->metrics;
    if (load->status == NAVIGATION_LOAD_PENDING && load->request_id != 0) {
        (void) fetch_scheduler_stream_metrics(
            load->scheduler, load->request_id, &metrics->stream);
    }
    if (load->status == NAVIGATION_LOAD_SUCCEEDED) {
        metrics->completion_per_mille = 1000;
    } else if (metrics->finalize_slices != 0
               || load->status == NAVIGATION_LOAD_FINALIZING) {
        static const size_t phase_progress[] = {740, 830, 940, 990};
        size_t phase = metrics->finalize_phase;
        if (phase >= sizeof(phase_progress) / sizeof(phase_progress[0]))
            phase = sizeof(phase_progress) / sizeof(phase_progress[0]) - 1u;
        metrics->completion_per_mille = phase_progress[phase];
    } else if (load->status == NAVIGATION_LOAD_READY_TO_FINISH) {
        metrics->completion_per_mille = 720;
    } else {
        size_t received = metrics->stream.downloaded_bytes != 0
            ? metrics->stream.downloaded_bytes : metrics->body_bytes;
        size_t total = metrics->stream.download_total_bytes;
        size_t network_progress = 0;
        if (total != 0) {
            if (received > total) received = total;
            network_progress = received > SIZE_MAX / 650u
                ? 650u : received * 650u / total;
        } else if (received != 0) {
            /* An honest bounded estimate for chunked/unknown-length bodies:
               it approaches, but never claims, the end of the network phase. */
            const size_t horizon = 128u * 1024u;
            size_t denominator = received > SIZE_MAX - horizon
                ? SIZE_MAX : received + horizon;
            network_progress = received > SIZE_MAX / 600u
                ? 600u : received * 600u / denominator;
        }
        metrics->completion_per_mille =
            (metrics->stream.headers_delivered ? 50u : 10u)
            + network_progress;
        if (metrics->completion_per_mille > 700u)
            metrics->completion_per_mille = 700u;
    }
    return true;
}

void navigation_load_cancel(NavigationLoad *load, const char *reason)
{
    if (load == NULL || load->status == NAVIGATION_LOAD_SUCCEEDED
        || load->status == NAVIGATION_LOAD_FAILED
        || load->status == NAVIGATION_LOAD_CANCELLED) return;
    const char *message = reason == NULL || reason[0] == '\0'
        ? "navigation cancelled" : reason;
    if (load->request_id != 0) {
        (void) fetch_scheduler_cancel(load->scheduler, load->request_id,
                                     message);
        bool ignored = false;
        FetchStreamMetrics stream = {0};
        (void) fetch_scheduler_take_stream(
            load->scheduler, load->request_id, &ignored, &stream,
            &load->response);
        load->metrics.stream = stream;
        load->session->last_stream = stream;
        load->request_id = 0;
    }
    if (load->session->generation == load->generation
        && !load->session->cancelled) {
        navigation_cancel(load->session, load->generation);
    }
    navigation_load_set_error(load, NAVIGATION_LOAD_CANCELLED, message);
    navigation_load_release_staging(load, true);
    navigation_load_report_replacement_outcome(load, false);
}

void navigation_load_destroy(NavigationLoad *load)
{
    if (load == NULL) return;
    Budget *budget = load->session == NULL ? NULL : load->session->budget;
    if (load->status == NAVIGATION_LOAD_PENDING
        || load->status == NAVIGATION_LOAD_READY_TO_FINISH
        || load->status == NAVIGATION_LOAD_FINALIZING) {
        navigation_load_cancel(load, "navigation load was released");
    }
    navigation_load_release_staging(
        load, load->status != NAVIGATION_LOAD_SUCCEEDED);
    if (load->status != NAVIGATION_LOAD_SUCCEEDED) {
        navigation_load_report_replacement_outcome(load, false);
    }
    fetch_scheduler_destroy(load->scheduler);
    fetch_result_destroy(&load->response);
    budget_free(budget, load->body);
    budget_free(budget, load->staged_body);
    budget_free(budget, load);
}

bool navigation_load_request(NavigationSession *session,
                             uint64_t generation, const char *url,
                             const char *method, const char *body,
                             size_t body_length, const char *content_type,
                             size_t maximum_bytes, long timeout_ms,
                             int viewport_width, const FontSet *fonts,
                             const ImageResources *images,
                             bool record_history)
{
    NavigationLoad *load = navigation_load_begin_request(
        session, generation, url, method, body, body_length, content_type,
        maximum_bytes, timeout_ms, viewport_width, fonts, images,
        record_history);
    if (load == NULL) return false;
    while (navigation_load_status(load) == NAVIGATION_LOAD_PENDING) {
        (void) navigation_load_pump(load, NULL);
    }
    bool success = navigation_load_status(load)
                       == NAVIGATION_LOAD_READY_TO_FINISH
        && navigation_load_finish(load, NULL);
    navigation_load_destroy(load);
    return success;
}

bool navigation_load_url(NavigationSession *session, uint64_t generation,
                         const char *url, size_t maximum_bytes,
                         long timeout_ms, int viewport_width,
                         const FontSet *fonts,
                         const ImageResources *images,
                         bool record_history)
{
    return navigation_load_request(session, generation, url, "GET", NULL, 0,
                                   NULL, maximum_bytes, timeout_ms,
                                   viewport_width, fonts, images,
                                   record_history);
}
