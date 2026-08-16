static uint64_t navigation_elapsed_us(uint64_t started, uint64_t finished)
{
    return finished >= started ? finished - started : 0;
}

static void navigation_record_blocking_script_sample(
    NavigationSession *session,
    const NavigationBlockingScriptSample *sample)
{
    if (session == NULL || sample == NULL) return;
    NavigationPerformance *performance = &session->performance;
    performance->parser_script_compile_us += sample->compile_us;
    performance->parser_script_execute_us += sample->execute_us;
    performance->parser_script_rescan_us +=
        sample->metadata_us + sample->stylesheet_fingerprint_us;
    if (sample->dom_mutations
        > SIZE_MAX - performance->parser_script_mutations) {
        performance->parser_script_mutations = SIZE_MAX;
    } else {
        performance->parser_script_mutations += sample->dom_mutations;
    }
    if (performance->blocking_script_sample_count
        < NAVIGATION_BLOCKING_SCRIPT_SAMPLE_LIMIT) {
        performance->blocking_script_samples[
            performance->blocking_script_sample_count++] = *sample;
    } else {
        performance->blocking_script_samples_dropped++;
    }
}

static bool navigation_stream_retire_failed_script_runtime(
    NavigationDocumentStream *stream)
{
    if (stream == NULL || stream->runtime == NULL
        || stream->session == NULL || stream->session->cancelled) {
        return false;
    }
    /* Script fetch/evaluation infrastructure is optional to document
       parsing. Once a parser-time realm can no longer make progress, retire
       it rather than converting a late author failure into a blank target.
       The parser continues and EOF builds an authoritative stylesheet and
       layout from the complete DOM. */
    script_runtime_set_synchronous_layout_callback(
        stream->runtime, NULL, NULL);
    script_runtime_set_layout(stream->runtime, NULL, 0);
    script_runtime_set_stylesheet(stream->runtime, NULL);
    script_runtime_destroy(stream->runtime);
    stream->runtime = NULL;
    stream->script_disabled_after_failure = true;
    stream->script_result.success = false;
    snprintf(stream->script_result.summary,
             sizeof(stream->script_result.summary),
             "JavaScript stopped; page retained");
    return true;
}

static bool navigation_stream_element_closed(void *opaque,
                                             PocDocument *document,
                                             lxb_dom_node_t *element)
{
    NavigationDocumentStream *stream = opaque;
    if (stream->closed_element_count != SIZE_MAX) {
        stream->closed_element_count++;
    }
    size_t name_length = 0;
    const char *name = document_element_name(element, &name_length);
    if (name != NULL && name_length == 4
        && memcmp(name, "head", 4) == 0) {
        stream->head_closed = true;
        if (!navigation_stream_scan_metadata(stream)) return false;
        stream->preview_checkpoint_checks = 0;
        navigation_stream_try_pre_eof_preview(stream);
        return true;
    }
    if (name != NULL && name_length == 4
        && memcmp(name, "body", 4) == 0) {
        /* Body-close is the strongest streaming checkpoint, but ordinary
           browsers may already have painted after the head and a useful body
           prefix. Keep it as a final pre-EOF opportunity. */
        stream->body_closed = true;
        stream->preview_checkpoint_checks = 0;
        navigation_stream_try_pre_eof_preview(stream);
        return true;
    }
    if (name == NULL || name_length != 6
        || memcmp(name, "script", 6) != 0) return true;
    if (stream->session == NULL
        || !stream->session->scripts_enabled
        || !stream->session->document_scripts_enabled
        || stream->script_disabled_after_failure) {
        return true;
    }
    uint64_t callback_started = navigation_now_us();
    bool parser_blocking = document_script_is_parser_blocking(element);
    size_t source_attribute_length = 0;
    const char *source_attribute = parser_blocking
        ? document_attribute(element, "src", &source_attribute_length)
        : NULL;
    if (parser_blocking
        && (source_attribute == NULL || source_attribute_length == 0)
        && element->ns == LXB_NS_SVG) {
        source_attribute = document_attribute(
            element, "href", &source_attribute_length);
    }
    bool external = source_attribute != NULL
        && source_attribute_length != 0;
    size_t inline_source_bytes = 0;
    if (parser_blocking && !external) {
        for (lxb_dom_node_t *child = element->first_child;
             child != NULL; child = child->next) {
            size_t length = 0;
            const char *text = document_text_data(child, &length);
            if (text == NULL) continue;
            if (length > SIZE_MAX - inline_source_bytes) {
                inline_source_bytes = SIZE_MAX;
                break;
            }
            inline_source_bytes += length;
        }
    }
    size_t script_ordinal = stream->scripts.early.parser_blocking + 1u;
    NavigationSession *session = stream->session;

    /* Once the realm's cumulative executable count is exhausted, this
       script cannot observe an intermediate stylesheet or metadata state.
       Still pass it through the loader so discovery/skipped metrics and the
       parser-executed set remain authoritative; EOF performs the final style
       build for the complete document. */
    if (parser_blocking && stream->runtime != NULL
        && !script_runtime_script_slot_available(stream->runtime)
        /* Before first paint, a later non-executing script can still be the
           first stable checkpoint after newly parsed CSS. Preserve that one
           opportunity; after paint, or without a compositor, its
           intermediate sheet is unobservable and may be skipped. */
        && (session->progressive_paint == NULL
            || stream->pre_eof_preview_painted
            || stream->pre_eof_preview_attempted)) {
        ScriptRuntimeTelemetry telemetry_before = {0};
        if (!script_runtime_parser_mutation_checkpoint(stream->runtime)) {
            return false;
        }
        script_runtime_telemetry(stream->runtime, &telemetry_before);
        size_t source_bytes_before = stream->scripts.early.bytes;
        uint64_t script_started = navigation_now_us();
        bool processed = document_scripts_process_closed(
            stream->runtime, session->budget, session->browser_session,
            stream->resource_base_url, stream->document_url,
            stream->referrer_policy,
            &stream->parser->document.content_security_policy,
            session->maximum_scripts, session->maximum_script_bytes,
            session->maximum_script_file_bytes, session->script_timeout_ms,
            stream->scheduler, element, &stream->scripts);
        uint64_t callback_finished = navigation_now_us();
        ScriptRuntimeTelemetry telemetry_after = {0};
        script_runtime_telemetry(stream->runtime, &telemetry_after);
        size_t source_bytes =
            stream->scripts.early.bytes >= source_bytes_before
            ? stream->scripts.early.bytes - source_bytes_before : 0;
        if (!external && source_bytes == 0) {
            source_bytes = inline_source_bytes;
        }
        size_t mutations =
            telemetry_after.dom_mutations >= telemetry_before.dom_mutations
            ? telemetry_after.dom_mutations
                - telemetry_before.dom_mutations : 0;
        uint64_t host_callback_us =
            telemetry_after.host_callback_total_us
                >= telemetry_before.host_callback_total_us
            ? telemetry_after.host_callback_total_us
                - telemetry_before.host_callback_total_us : 0;
        uint64_t process_us = navigation_elapsed_us(
            script_started, callback_finished);
        NavigationBlockingScriptSample sample = {
            .ordinal = script_ordinal,
            .node_count = document == NULL ? 0 : document->node_count,
            .source_bytes = source_bytes,
            .dom_mutations = mutations,
            .total_us = navigation_elapsed_us(
                callback_started, callback_finished),
            .process_us = process_us,
            .host_callback_us = host_callback_us,
            .execute_us = process_us,
            .external = external,
            .succeeded = processed
        };
        navigation_record_blocking_script_sample(session, &sample);
        session->performance.parser_script_us += process_us;
        session->performance.parser_callback_us += sample.total_us;
        if (processed) {
            navigation_stream_launch_preloads(stream);
            if (!navigation_stream_has_active_style_speculation(stream)) {
                navigation_stream_progress_preloads(stream, false);
            }
            stream->preview_checkpoint_checks = 0;
            navigation_stream_try_pre_eof_preview(stream);
        }
        return processed
            || navigation_stream_retire_failed_script_runtime(stream);
    }

    uint64_t metadata_started = callback_started;
    if (!navigation_stream_scan_metadata(stream)) return false;
    uint64_t metadata_finished = navigation_now_us();
    stream->session->performance.parser_metadata_us +=
        metadata_finished >= metadata_started
            ? metadata_finished - metadata_started : 0;
    /* Only a parser-blocking script can synchronously observe stylesheet
       state here. Rebuilding and refetching for defer/async/module elements
       is both unnecessary and harmful: a bounded stylesheet failure would
       otherwise be retried at every non-blocking closing tag while the
       top-level response's wall-clock deadline continues to run. */
    if (!parser_blocking) {
        navigation_stream_launch_preloads(stream);
        if (!navigation_stream_has_active_style_speculation(stream)) {
            navigation_stream_progress_preloads(stream, false);
        }
        uint64_t callback_finished = navigation_now_us();
        stream->session->performance.parser_callback_us +=
            callback_finished >= callback_started
                ? callback_finished - callback_started : 0;
        return true;
    }
    uint64_t runtime_started = metadata_finished;
    if (!navigation_stream_prepare_runtime(stream, true)) return false;
    uint64_t runtime_finished = navigation_now_us();
    stream->session->performance.parser_runtime_startup_us +=
        runtime_finished >= runtime_started
            ? runtime_finished - runtime_started : 0;
    uint64_t stylesheet_started = runtime_finished;
    /* Snapshot parser-owned links before the first author callback.  Any
       stylesheet link inserted by that callback remains absent from this
       baseline and receives its eventual load/error event after commit. */
    navigation_stream_adopt_stylesheet_event_baseline(stream);
    uint64_t fingerprint_started = navigation_now_us();
    uint64_t fingerprint = 0;
    if (!navigation_stream_stylesheet_fingerprint(
            stream, document, &fingerprint)) return false;
    uint64_t fingerprint_finished = navigation_now_us();
    session->performance.blocking_stylesheet_fingerprint_us +=
        fingerprint_finished >= fingerprint_started
            ? fingerprint_finished - fingerprint_started : 0;
    bool reuse = stream->blocking_stylesheet_ready
        && stream->blocking_stylesheet_fingerprint_valid
        && stream->blocking_stylesheet_fingerprint == fingerprint;
    if (reuse) {
        session->performance.blocking_stylesheet_reuses++;
    } else {
        bool continued = navigation_stream_try_continue_stylesheet(
            stream, document, fingerprint);
        if (!continued
            && !navigation_stream_build_blocking_stylesheet(
                   stream, document, fingerprint)) return false;
    }
    script_runtime_set_stylesheet(stream->runtime,
                                  &stream->blocking_stylesheet);
    script_runtime_invalidate_document_base(stream->runtime);
    if (!script_runtime_parser_mutation_checkpoint(stream->runtime)) {
        return false;
    }
    uint64_t stylesheet_finished = navigation_now_us();
    session->performance.parser_stylesheet_us +=
        stylesheet_finished >= stylesheet_started
            ? stylesheet_finished - stylesheet_started : 0;

    /* An external classic script may spend an appreciable interval waiting
       for its already-discovered response and then compiling/executing it.
       Once a closed-head body prefix and its stylesheet checkpoint are
       usable, present that immutable prefix before entering those blocking
       stages. Inline scripts have no network wait and usually execute more
       cheaply than a speculative layout, so painting before them would add
       work and author-visible flashing rather than improve first paint. */
    if (external && !stream->pre_eof_preview_attempted
        && !stream->pre_eof_preview_painted) {
        lxb_dom_node_t *body = document_body_node(document);
        if (body != NULL && body->first_child != NULL) {
            /* Entering the body insertion mode implicitly closes an omitted
               </head>. Record that parser fact so the ordinary preview gate
               need not special-case malformed-but-common document syntax. */
            stream->head_closed = true;
            session->performance.streaming_preview_pre_script_checks++;
            stream->preview_checkpoint_checks = 0;
            navigation_stream_try_pre_eof_preview(stream);
            if (stream->pre_eof_preview_painted) {
                session->performance.streaming_preview_pre_script_paints++;
            }
        }
    }
    ScriptRuntimeTelemetry telemetry_before = {0};
    script_runtime_telemetry(stream->runtime, &telemetry_before);
    size_t source_bytes_before = stream->scripts.early.bytes;
    uint64_t script_started = stylesheet_finished;
    if (external && source_attribute_length < NAVIGATION_URL_LIMIT) {
        char source_reference[NAVIGATION_URL_LIMIT];
        memcpy(source_reference, source_attribute, source_attribute_length);
        source_reference[source_attribute_length] = '\0';
        navigation_stream_settle_script_preload(
            stream, source_reference);
    }
    bool processed = document_scripts_process_closed(
        stream->runtime, session->budget, session->browser_session,
        stream->resource_base_url, stream->document_url,
        stream->referrer_policy,
        &stream->parser->document.content_security_policy,
        session->maximum_scripts, session->maximum_script_bytes,
        session->maximum_script_file_bytes, session->script_timeout_ms,
        stream->scheduler, element, &stream->scripts);
    /* Parser-blocking work owns the scarce transport pool first. Once its
       checkpoint is satisfied, opportunistic preloads may use the remaining
       scheduler capacity without starving CSS or the blocking script. */
    if (processed) {
        navigation_stream_launch_preloads(stream);
        if (!navigation_stream_has_active_style_speculation(stream)) {
            navigation_stream_progress_preloads(stream, false);
        }
    }
    uint64_t callback_finished = navigation_now_us();
    ScriptRuntimeTelemetry telemetry_after = {0};
    script_runtime_telemetry(stream->runtime, &telemetry_after);
    uint64_t compile_us =
        telemetry_after.host_compile_total_us
            >= telemetry_before.host_compile_total_us
        ? telemetry_after.host_compile_total_us
            - telemetry_before.host_compile_total_us
        : 0;
    uint64_t process_us = navigation_elapsed_us(
        script_started, callback_finished);
    size_t source_bytes =
        stream->scripts.early.bytes >= source_bytes_before
        ? stream->scripts.early.bytes - source_bytes_before : 0;
    if (!external && source_bytes == 0) {
        source_bytes = inline_source_bytes;
    }
    size_t mutations =
        telemetry_after.dom_mutations >= telemetry_before.dom_mutations
        ? telemetry_after.dom_mutations - telemetry_before.dom_mutations : 0;
    uint64_t host_callback_us =
        telemetry_after.host_callback_total_us
            >= telemetry_before.host_callback_total_us
        ? telemetry_after.host_callback_total_us
            - telemetry_before.host_callback_total_us : 0;
    NavigationBlockingScriptSample sample = {
        .ordinal = script_ordinal,
        .node_count = document == NULL ? 0 : document->node_count,
        .source_bytes = source_bytes,
        .dom_mutations = mutations,
        .total_us = navigation_elapsed_us(
            callback_started, callback_finished),
        .metadata_us = navigation_elapsed_us(
            metadata_started, metadata_finished),
        .runtime_startup_us = navigation_elapsed_us(
            runtime_started, runtime_finished),
        .stylesheet_us = navigation_elapsed_us(
            stylesheet_started, stylesheet_finished),
        .stylesheet_fingerprint_us = navigation_elapsed_us(
            fingerprint_started, fingerprint_finished),
        .process_us = process_us,
        .compile_us = compile_us,
        .host_callback_us = host_callback_us,
        .execute_us = process_us >= compile_us
            ? process_us - compile_us : 0,
        .external = external,
        .succeeded = processed
    };
    navigation_record_blocking_script_sample(session, &sample);
    session->performance.parser_script_us +=
        process_us;
    session->performance.parser_callback_us +=
        sample.total_us;
    if (processed) {
        /* This is the strongest parser checkpoint: the closing script has
           executed in order, and no later token has been consumed. Recheck
           the stylesheet fingerprint because the script itself may have
           changed a style/link element synchronously. */
        stream->preview_checkpoint_checks = 0;
        navigation_stream_try_pre_eof_preview(stream);
    }
    return processed
        || navigation_stream_retire_failed_script_runtime(stream);
}
