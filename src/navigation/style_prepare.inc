/* A parser-blocking script runs against the stylesheet its document has
   at its end tag, so that tag's checkpoint waits for render-blocking style
   responses and builds the sheet. Done inside the parser callback, that is
   one indivisible step: a page whose head links a few hundred kilobytes of
   CSS spent seconds of PSP time in a single parser pump.

   Instead, the parser pump stops feeding just before the end tag of an
   open parser-blocking script (the DOM is then exactly the one the
   checkpoint will see) and prepares the sheet across later pumps, each
   bounded by the parser's own time quota: the script realm, the
   render-blocking preloads (polled, never waited on), then the build, with
   each large external sheet parsed in slices (StylesheetParseDeferral).
   The end tag is fed once the sheet is ready, and the checkpoint reuses it
   by fingerprint. Anything unexpected abandons the preparation and leaves
   the checkpoint's ordinary synchronous path to do the work. */

/* External sheets at least this large are parsed in slices. Smaller ones
   cost less than a slice to parse whole. */
#define NAVIGATION_STYLE_PREPARE_SLICED_BYTES (16u * 1024u)

static void navigation_stream_style_prepare_abandon(
    NavigationDocumentStream *stream, bool sheet_partial)
{
    lxb_dom_node_t *script = stream->style_prepare.script;
    navigation_stream_style_prepare_cancel(stream);
    if (script != NULL) stream->style_prepare.declined = script;
    else stream->style_prepare.preview_declined = true;
    stream->session->performance.blocking_stylesheet_prepare_abandoned++;
    /* A partly built sheet is never left looking usable. */
    if (sheet_partial) navigation_stream_release_blocking_stylesheet(stream);
}

/* Whether `script`, whose end tag is next, would make the checkpoint
   create the script realm or build or extend the blocking stylesheet. */
static bool navigation_stream_style_prepare_wanted(
    NavigationDocumentStream *stream, lxb_dom_node_t *script,
    uint64_t *fingerprint)
{
    NavigationSession *session = stream->session;
    PocDocument *document = &stream->parser->document;
    if (!session->scripts_enabled || !session->document_scripts_enabled
        || stream->script_disabled_after_failure
        || stream->parser_script_circuit_open || stream->optional_work_shed
        || stream->runtime_transferred
        || !document_script_is_parser_blocking(script)
        || document_parser_script_was_truncated(stream->parser, script)
        || (stream->runtime != NULL
            && !script_runtime_script_slot_available(stream->runtime))
        || document_adopted_sheets_signature(document) != 0
        || navigation_test_parser_checkpoint_fault_armed()) return false;
    /* The first such script also creates the realm: a step of its own. */
    if (stream->runtime == NULL) return true;
    if (!navigation_stream_stylesheet_fingerprint(
            stream, document, fingerprint)) return false;
    return !(stream->blocking_stylesheet_ready
             && stream->blocking_stylesheet_fingerprint_valid
             && stream->blocking_stylesheet_fingerprint == *fingerprint);
}

/* The inputs to append when the ready sheet's inputs are an unchanged
   prefix of the document's (the conditions of
   navigation_stream_continue_stylesheet_suffix). Takes `current`. */
static bool navigation_stream_style_prepare_continuation(
    NavigationDocumentStream *stream, NavigationStylesheetInput *current,
    size_t current_count)
{
    if (!stream->blocking_stylesheet_ready
        || stream->blocking_stylesheet.user_css_changes_context
        || stream->stylesheet_input_overflow
        || stream->blocking_stylesheet.scopes.adopted_source_count != 0)
        return false;
    if (stream->blocking_stylesheet.viewport_width
            != stream->viewport.css_width
        || stream->blocking_stylesheet.viewport_height
            != stream->viewport.css_height) {
        stream->session->performance
            .blocking_stylesheet_viewport_rebuilds++;
        return false;
    }
    if (stream->blocking_stylesheet.prefers_dark
        != stylesheet_prefers_dark_color_scheme()
        || stream->blocking_stylesheet_user_css
            != navigation_stylesheet_user_css_hash(stream->session))
        return false;
    if (current_count <= stream->stylesheet_input_count) return false;
#ifndef TILEFINCH_NO_TRACE
    if (getenv("TILEFINCH_DISABLE_STYLESHEET_CONTINUATION") != NULL)
        return false;
#endif
    for (size_t i = 0; i < stream->stylesheet_input_count; i++) {
        const NavigationStylesheetInput *before =
            &stream->stylesheet_inputs[i];
        if (before->node != current[i].node
            || before->hash != current[i].hash
            || before->style != current[i].style) return false;
    }
    return true;
}

/* Starts from the cached sheet's <head> part when this document's <head>
   sources have its key: the head inputs become the ready prefix and the
   remaining inputs append as a continuation (the APPEND phase), exactly
   as a fresh pass would parse them after the head. False leaves the
   preparation to build the sheet. */
static bool navigation_stream_style_prepare_take_head(
    NavigationDocumentStream *stream, uint64_t fingerprint)
{
    NavigationSession *session = stream->session;
    NavigationStylePrepare *prepare = &stream->style_prepare;
    PocDocument *document = &stream->parser->document;
    if (!session->external_resources_enabled
        || !session->compiled_stylesheet_cache_ready
        || !session->compiled_stylesheet_cache_head_valid) return false;
    uint64_t key = 0;
    size_t head_inputs = 0, body_inputs = 0;
    bool body_inline = false;
    if (!navigation_stream_stylesheet_head_key(
            stream, document, &key, &head_inputs, &body_inputs,
            &body_inline)
        || key != session->compiled_stylesheet_cache_head_key) return false;
    NavigationStylesheetInput *inputs = NULL;
    size_t count = 0;
    bool overflow = false;
    if (!navigation_stream_collect_stylesheet_inputs(
            stream, document, &inputs, &count, &overflow)
        || overflow || count != head_inputs + body_inputs) {
        budget_free(session->budget, inputs);
        return false;
    }
    NavigationStylesheetInput *prefix = head_inputs == 0 ? NULL
        : budget_malloc(session->budget, head_inputs * sizeof(*prefix));
    if (head_inputs != 0 && prefix == NULL) {
        budget_free(session->budget, inputs);
        return false;
    }
    if (!navigation_compiled_stylesheet_cache_take_head(
            session, key, &stream->blocking_stylesheet,
            &stream->blocking_stylesheets)) {
        budget_free(session->budget, prefix);
        budget_free(session->budget, inputs);
        return false;
    }
    Stylesheet *sheet = &stream->blocking_stylesheet;
    sheet->noscript_rendered = document->noscript_rendered;
    sheet->font_text_pages = document->font_text_pages;
    if (head_inputs != 0)
        memcpy(prefix, inputs, head_inputs * sizeof(*prefix));
    navigation_stream_replace_stylesheet_inputs(
        stream, prefix, head_inputs, false);
    /* The take added the session's user stylesheet after the cut. */
    stream->blocking_stylesheet_user_css =
        navigation_stylesheet_user_css_hash(session);
    prepare->continuation = true;
    prepare->inputs = inputs;
    prepare->input_count = count;
    prepare->next_input = head_inputs;
    prepare->fingerprint = fingerprint;
    prepare->rules_before = sheet->count;
    stream->blocking_stylesheet_ready = false;
    stream->blocking_stylesheet_fingerprint_valid = false;
    prepare->phase = NAVIGATION_STYLE_PREPARE_APPEND;
    return true;
}

/* Starts the build: an append to the ready sheet when its inputs are a
   prefix, else a fresh sheet from the compiled cache (whole, or its <head>
   part with the rest appended) or a sliced full pass. False abandons the preparation. */
static bool navigation_stream_style_prepare_build(
    NavigationDocumentStream *stream)
{
    NavigationSession *session = stream->session;
    NavigationStylePrepare *prepare = &stream->style_prepare;
    PocDocument *document = &stream->parser->document;
    uint64_t fingerprint = 0;
    if (!navigation_stream_stylesheet_fingerprint(
            stream, document, &fingerprint)) return false;
    if (stream->blocking_stylesheet_ready
        && stream->blocking_stylesheet_fingerprint_valid
        && stream->blocking_stylesheet_fingerprint == fingerprint) {
        prepare->phase = NAVIGATION_STYLE_PREPARE_IDLE;
        return true;
    }
    NavigationStylesheetInput *inputs = NULL;
    size_t count = 0;
    bool overflow = false;
    if (!navigation_stream_collect_stylesheet_inputs(
            stream, document, &inputs, &count, &overflow)) return false;
    if (overflow) {
        budget_free(session->budget, inputs);
        return false;
    }
    if (session->external_resources_enabled
        && navigation_stream_style_prepare_continuation(
               stream, inputs, count)) {
        bool inline_only = true;
        for (size_t i = stream->stylesheet_input_count; i < count; i++)
            inline_only = inline_only && inputs[i].style;
        if (inline_only) {
            /* <style> blocks append in one parse, as the checkpoint
               does; there is no response to wait for. */
            budget_free(session->budget, inputs);
            prepare->phase = NAVIGATION_STYLE_PREPARE_IDLE;
            return navigation_stream_continue_stylesheet_suffix(
                stream, document, fingerprint, false);
        }
        prepare->continuation = true;
        prepare->inputs = inputs;
        prepare->input_count = count;
        prepare->next_input = stream->stylesheet_input_count;
        prepare->fingerprint = fingerprint;
        prepare->rules_before = stream->blocking_stylesheet.count;
        stream->blocking_stylesheet_ready = false;
        stream->blocking_stylesheet_fingerprint_valid = false;
        prepare->phase = NAVIGATION_STYLE_PREPARE_APPEND;
        return true;
    }
    budget_free(session->budget, inputs);
    /* A fresh sheet. Releasing the old one also clears this preparation's
       state, so the new state is set afterwards. */
    navigation_stream_release_blocking_stylesheet(stream);
    prepare->phase = NAVIGATION_STYLE_PREPARE_BUILD;
    prepare->continuation = false;
    prepare->from_cache = false;
    prepare->fingerprint = fingerprint;
    if (navigation_compiled_stylesheet_cache_take(
            session, fingerprint, &stream->blocking_stylesheet,
            &stream->blocking_stylesheets)) {
        prepare->from_cache = true;
        prepare->phase = NAVIGATION_STYLE_PREPARE_FINISH;
        return true;
    }
    if (navigation_stream_style_prepare_take_head(stream, fingerprint))
        return true;
    bool built = session->external_resources_enabled
        ? stylesheet_build_context_deferred(
              &stream->blocking_stylesheet, session->budget, document,
              &stream->viewport)
        : stylesheet_build_context(
              &stream->blocking_stylesheet, session->budget, document,
              &stream->viewport);
    if (!built) return false;
    if (!session->external_resources_enabled) {
        prepare->phase = NAVIGATION_STYLE_PREPARE_FINISH;
        return true;
    }
    if (!navigation_stream_collect_stylesheet_inputs(
            stream, document, &prepare->inputs, &prepare->input_count,
            &overflow) || overflow) {
        stylesheet_destroy(&stream->blocking_stylesheet);
        return false;
    }
    prepare->deferral.minimum_bytes = NAVIGATION_STYLE_PREPARE_SLICED_BYTES;
    if (!stylesheets_load_external_sliced(
            document, &stream->blocking_stylesheet, session->budget,
            stream->resource_base_url, stream->document_url,
            stream->referrer_policy, session->maximum_stylesheets,
            session->maximum_stylesheet_bytes,
            session->maximum_stylesheet_file_bytes,
            session->resource_timeout_ms, stream->scheduler,
            session->browser_session, &stream->stylesheet_resources,
            &stream->blocking_stylesheets, &prepare->deferral)) {
        stylesheet_parse_deferral_release(&prepare->deferral);
        stylesheet_destroy(&stream->blocking_stylesheet);
        return false;
    }
    return true;
}

/* Where the remaining sources start: after the input whose sheet was just
   deferred, or (a pass that ran to the end) after the last input. */
static bool navigation_stream_style_prepare_after_deferral(
    NavigationStylePrepare *prepare)
{
    if (prepare->deferral.parse == NULL) {
        prepare->next_input = prepare->input_count;
        return true;
    }
    for (size_t i = prepare->next_input; i < prepare->input_count; i++) {
        if (prepare->inputs[i].node == prepare->deferral.element) {
            prepare->next_input = i + 1u;
            return true;
        }
    }
    return false;
}

/* The sheet is complete: record it exactly as the checkpoint's build or
   continuation would. False abandons the preparation. */
static bool navigation_stream_style_prepare_finish(
    NavigationDocumentStream *stream)
{
    NavigationSession *session = stream->session;
    NavigationStylePrepare *prepare = &stream->style_prepare;
    PocDocument *document = &stream->parser->document;
    /* The rule batch a stopped pass left open. */
    if (stream->blocking_stylesheet.rule_batch_active
        && !stylesheet_end_rule_batch(&stream->blocking_stylesheet))
        return false;
    if (prepare->continuation) {
        session->performance.blocking_stylesheet_continuations++;
        session->performance.blocking_stylesheet_continuation_rules +=
            stream->blocking_stylesheet.count - prepare->rules_before;
        navigation_stream_replace_stylesheet_inputs(
            stream, prepare->inputs, prepare->input_count, false);
        prepare->inputs = NULL;
        layout_reuse_cache_destroy(stream->preview_layout_reuse);
        stream->preview_layout_reuse = NULL;
    } else if (prepare->from_cache) {
        stream->blocking_stylesheet_user_css =
            navigation_stylesheet_user_css_hash(session);
        navigation_stream_capture_stylesheet_inputs(stream, document);
    } else {
        if (session->user_css_length != 0) {
            if (!stylesheet_add_user_css(
                    &stream->blocking_stylesheet, session->user_css,
                    session->user_css_length)) return false;
        }
        stream->blocking_stylesheet_user_css =
            navigation_stylesheet_user_css_hash(session);
        navigation_stream_capture_stylesheet_inputs(stream, document);
        session->performance.blocking_stylesheet_builds++;
    }
    stream->blocking_stylesheet_ready = true;
    stream->blocking_stylesheet_fingerprint = prepare->fingerprint;
    stream->blocking_stylesheet_fingerprint_valid = true;
    if (stream->runtime != NULL) {
        script_runtime_set_stylesheet(
            stream->runtime, &stream->blocking_stylesheet);
        script_runtime_invalidate_document_base(stream->runtime);
    }
    navigation_stream_style_prepare_cancel(stream);
    prepare->fresh = true;
    session->performance.blocking_stylesheet_prepared++;
    preview_policy_reopen_checks(&stream->preview_policy);
    return true;
}

/* A streaming preview whose sheet is stale prepares it across the coming
   parser pumps instead of building it now; true when that started (the
   preview then waits for a later feed). */
static bool navigation_stream_style_prepare_for_preview(
    NavigationDocumentStream *stream)
{
    NavigationStylePrepare *prepare = &stream->style_prepare;
    if (!prepare->gated || prepare->preview_declined
        || navigation_test_parser_checkpoint_fault_armed()
        || prepare->phase != NAVIGATION_STYLE_PREPARE_IDLE
        || stream->parser == NULL
        || document_adopted_sheets_signature(&stream->parser->document) != 0)
        return false;
    prepare->script = NULL;
    prepare->phase = NAVIGATION_STYLE_PREPARE_SETTLE;
    return true;
}

/* Advances the preparation until it finishes or `deadline_us` passes. */
static void navigation_stream_style_prepare_run(
    NavigationDocumentStream *stream, uint64_t deadline_us)
{
    NavigationSession *session = stream->session;
    NavigationStylePrepare *prepare = &stream->style_prepare;
    PocDocument *document = &stream->parser->document;
    while (prepare->phase != NAVIGATION_STYLE_PREPARE_IDLE) {
        uint64_t step_started = navigation_now_us();
        switch (prepare->phase) {
        case NAVIGATION_STYLE_PREPARE_RUNTIME:
            /* The checkpoint's own order: metadata settles the viewport
               the sheet is built for, then the realm. */
            if (navigation_stream_metadata_checkpoint(stream)
                    != NAVIGATION_PARSER_CHECKPOINT_OK
                || !navigation_stream_prepare_runtime(stream, true)
                || stream->runtime == NULL) {
                navigation_stream_style_prepare_abandon(stream, false);
                return;
            }
            prepare->phase = NAVIGATION_STYLE_PREPARE_SETTLE;
            break;
        case NAVIGATION_STYLE_PREPARE_SETTLE:
            if (session->external_resources_enabled
                && !navigation_stream_poll_style_preloads(stream))
                return;   /* Waiting on the network: yield the pump. */
            prepare->phase = NAVIGATION_STYLE_PREPARE_BUILD;
            if (!navigation_stream_style_prepare_build(stream)) {
                navigation_stream_style_prepare_abandon(stream, true);
                return;
            }
            if (prepare->phase == NAVIGATION_STYLE_PREPARE_BUILD) {
                if (!navigation_stream_style_prepare_after_deferral(
                        prepare)) {
                    navigation_stream_style_prepare_abandon(stream, true);
                    return;
                }
                prepare->phase = prepare->deferral.parse != NULL
                    ? NAVIGATION_STYLE_PREPARE_PARSE
                    : NAVIGATION_STYLE_PREPARE_APPEND;
            }
            break;
        case NAVIGATION_STYLE_PREPARE_PARSE: {
            StyleCssSliceResult result = stylesheet_parse_deferral_step(
                &prepare->deferral, deadline_us,
                &stream->blocking_stylesheets);
            if (result == STYLE_CSS_SLICE_FAILED) {
                navigation_stream_style_prepare_abandon(stream, true);
                return;
            }
            if (result == STYLE_CSS_SLICE_DONE) {
                stylesheet_parse_deferral_release(&prepare->deferral);
                prepare->phase = NAVIGATION_STYLE_PREPARE_APPEND;
            }
            break;
        }
        case NAVIGATION_STYLE_PREPARE_APPEND: {
            if (prepare->next_input >= prepare->input_count) {
                prepare->phase = NAVIGATION_STYLE_PREPARE_FINISH;
                break;
            }
            lxb_dom_node_t *nodes[NAVIGATION_STYLESHEET_INPUT_LIMIT];
            size_t node_count = prepare->input_count - prepare->next_input;
            for (size_t i = 0; i < node_count; i++)
                nodes[i] = prepare->inputs[prepare->next_input + i].node;
            size_t consumed = 0;
            prepare->deferral.minimum_bytes =
                NAVIGATION_STYLE_PREPARE_SLICED_BYTES;
            if (!stylesheets_append_ordered_suffix_sliced(
                    &stream->blocking_stylesheet, session->budget, nodes,
                    node_count, stream->resource_base_url,
                    stream->document_url, stream->referrer_policy,
                    &document->content_security_policy,
                    session->maximum_stylesheets,
                    session->maximum_stylesheet_bytes,
                    session->maximum_stylesheet_file_bytes,
                    session->resource_timeout_ms, stream->scheduler,
                    session->browser_session, &stream->stylesheet_resources,
                    &stream->blocking_stylesheets, &prepare->deferral,
                    &consumed)) {
                navigation_stream_style_prepare_abandon(stream, true);
                return;
            }
            prepare->next_input += consumed;
            if (prepare->deferral.parse != NULL)
                prepare->phase = NAVIGATION_STYLE_PREPARE_PARSE;
            break;
        }
        case NAVIGATION_STYLE_PREPARE_FINISH:
        case NAVIGATION_STYLE_PREPARE_BUILD:
            if (!navigation_stream_style_prepare_finish(stream)) {
                navigation_stream_style_prepare_abandon(stream, true);
                return;
            }
            break;
        case NAVIGATION_STYLE_PREPARE_IDLE:
            break;
        }
        uint64_t now = navigation_now_us();
        uint64_t step = navigation_elapsed_us(step_started, now);
        if (step > session->performance
                       .blocking_stylesheet_prepare_max_step_us) {
            session->performance.blocking_stylesheet_prepare_max_step_us =
                step;
        }
        if (now >= deadline_us) return;
    }
}

/* Case-insensitive offset of "</script" in data, or length. */
static size_t navigation_find_script_end_tag(const unsigned char *data,
                                             size_t length, size_t from)
{
    static const char tag[] = "</script";
    const size_t tag_length = sizeof(tag) - 1u;
    for (size_t at = from; at + tag_length <= length; at++) {
        if (data[at] != '<') continue;
        size_t matched = 1;
        while (matched < tag_length
               && (data[at + matched] | 0x20u) == (unsigned char) tag[matched])
            matched++;
        if (matched == tag_length) return at;
    }
    return length;
}

/* Bytes at the end of data that begin "</script" without completing it. */
static size_t navigation_partial_script_end_tag(const unsigned char *data,
                                                size_t length)
{
    static const char tag[] = "</script";
    const size_t tag_length = sizeof(tag) - 1u;
    for (size_t keep = tag_length - 1u; keep != 0; keep--) {
        if (keep > length) continue;
        const unsigned char *tail = data + length - keep;
        size_t matched = 0;
        while (matched < keep
               && (matched == 0 ? tail[0] == '<'
                                : (tail[matched] | 0x20u)
                                      == (unsigned char) tag[matched]))
            matched++;
        if (matched == keep) return keep;
    }
    return 0;
}

/* Called by the parser pump before it feeds `*length` bytes of `data`.
   Runs a preparation in progress, or starts one when the window opens with
   the end tag of a parser-blocking script that needs a new sheet; either
   way `*length` becomes 0 and this pump feeds nothing. Otherwise the window
   is cut before the next "</script" so that tag starts a later window. */
static void navigation_stream_style_prepare_gate(
    NavigationDocumentStream *stream, const unsigned char *data,
    size_t *length, size_t staged_length, uint64_t deadline_us)
{
    if (stream == NULL || stream->session == NULL || stream->parser == NULL
        || *length == 0) return;
    NavigationSession *session = stream->session;
    NavigationStylePrepare *prepare = &stream->style_prepare;
#ifndef TILEFINCH_NO_TRACE
    /* A/B switch: the checkpoint (and preview) build in place, as before. */
    if (getenv("TILEFINCH_DISABLE_STYLESHEET_PREPARE") != NULL) return;
#endif
    prepare->gated = true;
    if (prepare->phase == NAVIGATION_STYLE_PREPARE_IDLE) {
        if (!session->scripts_enabled || !session->document_scripts_enabled
            || stream->optional_work_shed
            || stream->parser_script_circuit_open
            || stream->script_disabled_after_failure) return;
        size_t from = 0;
        if (navigation_find_script_end_tag(data, *length, 0) == 0) {
            lxb_dom_node_t *script =
                document_parser_open_script(stream->parser);
            uint64_t fingerprint = 0;
            if (script != NULL && script != prepare->declined
                && navigation_stream_style_prepare_wanted(
                       stream, script, &fingerprint)) {
                prepare->phase = NAVIGATION_STYLE_PREPARE_RUNTIME;
                prepare->script = script;
            } else {
                from = 1;
            }
        }
        if (prepare->phase == NAVIGATION_STYLE_PREPARE_IDLE) {
            size_t end = navigation_find_script_end_tag(data, *length, from);
            if (end < *length) {
                *length = end;
            } else if (*length < staged_length) {
                size_t partial =
                    navigation_partial_script_end_tag(data, *length);
                if (partial < *length) *length -= partial;
            }
            return;
        }
    }
    *length = 0;
    session->performance.blocking_stylesheet_prepare_pumps++;
    uint64_t started = navigation_now_us();
    navigation_stream_style_prepare_run(stream, deadline_us);
    session->performance.blocking_stylesheet_prepare_us +=
        navigation_elapsed_us(started, navigation_now_us());
}
