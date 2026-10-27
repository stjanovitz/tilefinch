/* Streaming document staging: teardown and release of a document stream,
   shedding its optional work at the memory limit, and preparing the
   stream's script realm. */

static void navigation_stream_destroy_staging(
    NavigationDocumentStream *stream)
{
    if (stream == NULL) return;
    /* Commit already detached staging callbacks before transferring the
       realm. The page may have retired it after a later allocation refusal;
       the stream's borrowed identity must never be dereferenced again. */
    if (stream->runtime != NULL && !stream->runtime_transferred) {
        script_runtime_set_synchronous_layout_callback(
            stream->runtime, NULL, NULL);
        script_runtime_set_layout(
            stream->runtime, NULL, stream->viewport.css_height);
    }
    if (stream->cssom_layout_ready) {
        layout_destroy(&stream->cssom_layout);
        memset(&stream->cssom_layout, 0, sizeof(stream->cssom_layout));
        stream->cssom_layout_ready = false;
    }
    layout_reuse_cache_destroy(stream->preview_layout_reuse);
    stream->preview_layout_reuse = NULL;
    navigation_stream_release_blocking_stylesheet(stream);
    document_body_snapshot_destroy(&stream->server_body_snapshot);
    budget_free(stream->session == NULL ? NULL : stream->session->budget,
                stream->stylesheet_inputs);
    stream->stylesheet_inputs = NULL;
    stream->stylesheet_input_count = 0;
    stylesheet_document_resources_destroy(&stream->stylesheet_resources);
    images_destroy(&stream->canvas_images);
    if (!stream->runtime_transferred) {
        script_runtime_destroy(stream->runtime);
    }
    if (!stream->scheduler_transferred) {
        fetch_scheduler_destroy(stream->scheduler);
    }
    fetch_scheduler_domain_release(stream->fetch_domain);
    for (size_t i = 0; i < stream->preload_count; i++) {
        budget_free(stream->session == NULL ? NULL : stream->session->budget,
                    stream->preloads[i].url);
        stream->preloads[i].url = NULL;
    }
    stream->runtime = NULL;
    stream->scheduler = NULL;
    stream->fetch_domain = NULL;
}

static void navigation_stream_release(NavigationDocumentStream *stream)
{
    if (stream == NULL) return;
    Budget *budget = stream->session == NULL ? NULL : stream->session->budget;
    navigation_stream_destroy_staging(stream);
    budget_free(budget, stream);
}

static void navigation_stream_shed_optional_work(
    NavigationDocumentStream *stream, const char *reason,
    bool shed_resources)
{
    if (stream == NULL || stream->optional_work_shed) return;
    NavigationSession *session = stream->session;
    if (stream->runtime != NULL) {
        script_runtime_set_synchronous_layout_callback(
            stream->runtime, NULL, NULL);
        script_runtime_destroy(stream->runtime);
        stream->runtime = NULL;
    }
    if (shed_resources) {
        fetch_scheduler_destroy(stream->scheduler);
        stream->scheduler = NULL;
        for (size_t i = 0; i < stream->preload_count; i++) {
            NavigationPreload *preload = &stream->preloads[i];
            if (!preload->complete) {
                preload->request_id = 0;
                preload->complete = true;
                if (session != NULL) session->preloads_deferred++;
            }
        }
    }
    stream->script_disabled_after_failure = true;
    stream->optional_work_shed = true;
    stream->script_result.success = false;
    snprintf(stream->script_result.summary,
             sizeof(stream->script_result.summary), "%s",
             reason == NULL ? "Optional page work stopped at its memory limit"
                            : reason);
    if (session != NULL) session->performance.optional_work_sheds++;
}

static bool navigation_stream_prepare_runtime(NavigationDocumentStream *stream,
                                              bool create_runtime)
{
    NavigationSession *session = stream->session;
    bool needs_runtime = session->scripts_enabled
                         && session->document_scripts_enabled;
    bool needs_scheduler = needs_runtime
                           || session->external_resources_enabled
                           || session->web_fonts_enabled;
    if (stream->optional_work_shed) return true;
    if (stream->scheduler == NULL && needs_scheduler) {
        size_t maximum_file = navigation_stream_maximum_file_bytes(session);
        /* Every request reserves its whole declared file limit from enqueue
           until completion, so the byte pool has to name each lane that can
           hold a reservation at the same time rather than trusting one round
           number:
             - NAVIGATION_PRELOAD_CONCURRENCY file limits for the speculative
               preload lane, which by construction keeps that many requests
               outstanding across a slow link;
             - four file limits for the authoritative lanes - the blocking
               stylesheet checkpoint, the parser-blocking script behind it,
               and the ordered resource/async-bridge fetches that follow.
           Sizing for the authoritative lanes alone (the historical 4x) let
           the two lanes overlap on a real network and refused the next
           authoritative enqueue outright with "response reservation exceeds
           scheduler byte bound". This is reservation accounting, not resident
           memory: with a 512 KiB file limit, the pool states a 3 MiB ceiling
           instead of 2 MiB. */
        const size_t preload_lanes = (size_t) NAVIGATION_PRELOAD_CONCURRENCY;
        const size_t authoritative_lanes = 4u;
        const size_t lanes = preload_lanes + authoritative_lanes;
        size_t reserved = maximum_file <= SIZE_MAX / lanes
            ? maximum_file * lanes : maximum_file;
        /* An SPA can hold several concurrent async chunk fetches while a
           synchronous module-loader fetch also needs a slot; capacity must
           cover the async bridge's native limit plus that headroom. */
        bool refuse_scheduler = false;
#if !defined(__PSP__)
        refuse_scheduler =
            navigation_test_consume_stream_scheduler_refusal();
#endif
        stream->scheduler = refuse_scheduler ? NULL
            : fetch_scheduler_create_in_domain(
                  stream->fetch_domain, 12, reserved);
        if (stream->scheduler == NULL) {
            navigation_stream_shed_optional_work(
                stream, "Optional resources stopped at the page memory limit",
                true);
            return true;
        }
        /* Six streaming worker lanes match the scheduler's documented two
           speculative plus four authoritative reservations. On PSP this
           moves transport production off the browser thread without reducing
           intended request overlap; host/replay leaves the scheduler alone. */
        (void) fetch_scheduler_enable_background_transport(
            stream->scheduler, true);
    }
    if (!create_runtime || !needs_runtime || stream->runtime != NULL) {
        return true;
    }
    stream->runtime = navigation_create_runtime(
        session, &stream->parser->document,
        stream->document_url[0] == '\0' ? "about:blank"
                                         : stream->document_url,
        stream->document_referrer, stream->started_us,
        stream->referrer_policy, true,
        &stream->viewport,
        SCRIPT_DOCUMENT_SCOPE_TOP_LEVEL, stream->fetch_domain,
        NULL, &stream->canvas_images, stream->fonts,
        &stream->script_result);
    if (stream->runtime != NULL) {
        if (stream->response_headers_elapsed_us != 0) {
            const ScriptNavigationTiming navigation_timing = {
                .response_start_us = stream->response_headers_elapsed_us,
                .response_status = stream->response_status,
                .next_hop_protocol = stream->next_hop_protocol,
                .content_type = stream->response_content_type
            };
            (void) script_runtime_record_navigation_timing(
                stream->runtime, &navigation_timing);
        }
        script_runtime_set_synchronous_layout_callback(
            stream->runtime, navigation_stream_flush_cssom_layout, stream);
    }
    if (stream->runtime == NULL) {
        navigation_stream_shed_optional_work(
            stream, "JavaScript stopped at the page memory limit", false);
        return true;
    }
    return true;
}
