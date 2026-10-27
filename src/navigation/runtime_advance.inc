/* Blank-page recovery, the per-frame runtime advance, and
   script-initiated navigation. */

bool navigation_layout_is_visually_blank(
    const LayoutDocument *layout)
{
    if (layout == NULL) return false;
    /* Interaction tables are not paint evidence. visibility:hidden zeros
       their geometry only at the final visibility pass, while opacity:0
       deliberately preserves hit testing. In both cases a retained link or
       control can coexist with a completely blank framebuffer. Ordinary
       visible links and controls contribute draw commands below. */
    for (size_t i = 0; i < layout->count; i++) {
        const DrawCommand *command = &layout->commands[i];
        if (command->width <= 0 || command->height <= 0) continue;
        /* Visibility:hidden and opacity:0 retain geometry for stable layout,
           hit-test generations, and later author toggles, but rasterize no
           pixels. They cannot make an otherwise blank page useful. */
        if (command->opacity_scale == 0u) continue;
        /* A page-wide background rectangle is still a blank presentation.
           Text, images, outlines, shadows, and any second painted layer are
           observable output and must keep author runtime semantics. */
        if (command->type != DRAW_FILL_RECT
            || command->color != layout->page_background
            || draw_command_fill_gradient(command, NULL)
            || draw_command_backdrop_blur(command) != 0u
            || draw_command_filter_code(command) != 0u
            || draw_command_rotation_quadrants(command) != 0u
            || draw_command_blend_mode(command) != LAYOUT_MIX_BLEND_NORMAL
            || command->radius != 0
            || command->opacity_scale != 256u
            || command->x > 0 || command->y > 0
            || command->width < layout->viewport.css_width
            || command->height < layout->viewport.css_height) return false;
    }
    return true;
}

static bool navigation_recovery_semantic_element(lxb_dom_node_t *node)
{
    return node_name_is(node, "main") || node_name_is(node, "article")
        || node_name_is(node, "section") || node_name_is(node, "p")
        || node_name_is(node, "li") || node_name_is(node, "blockquote")
        || node_name_is(node, "pre") || node_name_is(node, "table")
        || node_name_is(node, "details") || node_name_is(node, "h1")
        || node_name_is(node, "h2") || node_name_is(node, "h3")
        || node_name_is(node, "h4") || node_name_is(node, "h5")
        || node_name_is(node, "h6");
}

static bool navigation_recovery_excluded_element(lxb_dom_node_t *node)
{
    if (node == NULL || node->type != LXB_DOM_NODE_TYPE_ELEMENT)
        return false;
    size_t length = 0;
    const char *aria_hidden = document_attribute(
        node, "aria-hidden", &length);
    return node_name_is(node, "script") || node_name_is(node, "style")
        || node_name_is(node, "template") || node_name_is(node, "noscript")
        || node_name_is(node, "svg") || node_name_is(node, "dialog")
        || node_name_is(node, "nav") || node_name_is(node, "aside")
        || node_name_is(node, "footer") || node_name_is(node, "form")
        || lxb_dom_element_has_attribute(
               lxb_dom_interface_element(node),
               (const lxb_char_t *) "hidden", 6u)
        || lxb_dom_element_has_attribute(
               lxb_dom_interface_element(node),
               (const lxb_char_t *) "inert", 5u)
        || (aria_hidden != NULL && length == 4u
            && strncasecmp(aria_hidden, "true", 4u) == 0);
}

/* This is evidence for static recovery, not a visibility override. Count a
   bounded amount of prose inside semantic content, excluding navigation,
   forms, dialogs, and authored hidden/inert subtrees. The subsequent style
   fallback remains responsible for its narrow, content-shaped reveal rules. */
static bool navigation_document_has_bounded_retained_content(
    PocDocument *document)
{
    lxb_dom_node_t *root = document_body_node(document);
    if (root == NULL) return false;
    enum { RETAINED_TEXT_SCAN_LIMIT = 8192 };
    size_t text_bytes = 0, semantic_text_bytes = 0, inspected_bytes = 0;
    lxb_dom_node_t *node = root->first_child;
    for (size_t visited = 0; node != NULL && visited < 1024u; visited++) {
        bool skip_children = false;
        if (node->type == LXB_DOM_NODE_TYPE_ELEMENT) {
            skip_children = navigation_recovery_excluded_element(node);
        } else if (node->type == LXB_DOM_NODE_TYPE_TEXT) {
            size_t length = 0;
            const char *text = document_text_data(node, &length);
            bool semantic = false;
            lxb_dom_node_t *ancestor = node->parent;
            for (size_t depth = 0;
                 ancestor != NULL && ancestor != root && depth < 64u;
                 depth++, ancestor = ancestor->parent) {
                if (navigation_recovery_semantic_element(ancestor)) {
                    semantic = true;
                    break;
                }
            }
            for (size_t at = 0; text != NULL && at < length
                 && inspected_bytes < RETAINED_TEXT_SCAN_LIMIT; at++) {
                inspected_bytes++;
                if (isspace((unsigned char) text[at])) continue;
                if (text_bytes != SIZE_MAX) text_bytes++;
                if (semantic && semantic_text_bytes != SIZE_MAX)
                    semantic_text_bytes++;
                if (text_bytes >= 128u && semantic_text_bytes >= 96u)
                    return true;
                /* Plain server-rendered prose frequently uses only divs.
                   Enough bounded non-whitespace text is useful recovery
                   evidence even without an article/main/p ancestor. */
                if (text_bytes >= 512u) return true;
            }
            if (inspected_bytes == RETAINED_TEXT_SCAN_LIMIT) return false;
        }
        if (!skip_children && node->first_child != NULL) {
            node = node->first_child;
            continue;
        }
        while (node != root && node->next == NULL) node = node->parent;
        node = node == root ? NULL : node->next;
    }
    return false;
}

static bool navigation_blank_recovery_has_useful_content(
    NavigationSession *session)
{
    if (session == NULL) return false;
    bool retained = navigation_document_has_bounded_retained_content(
        &session->page.document);
    ReaderDocumentAnalysis analysis = session->page.reader_analysis;
    if (analysis.prepared
        && (analysis.bounded_out || analysis.extraction_truncated)) {
        return false;
    }
    /* Avoid allocating a Reader clone on the ordinary complete-prose path:
       this predicate runs inside the allocator-refusal recovery boundary,
       where little memory is left. Conservatively decline documents beyond
       the extractor's floor (whole-document nodes and body text against
       what any extraction may emit), which holds however short memory is;
       the frontend may still offer an explicit Reader attempt later. */
    if (retained) {
        if (session->page.document.node_count > READER_EXTRACT_NODE_FLOOR
            || session->page.document.body_text_length
                   > READER_EXTRACT_BYTE_FLOOR) {
            return false;
        }
        return true;
    }
    /* A RAW analysis may have been cached before a late author mutation.
       RAW installs no markers or clone, so it is safe to analyze the settled
       document once more at this rare allocator-failure boundary. */
    bool analyzed_for_recovery =
        !analysis.prepared || analysis.kind == READER_PAGE_RAW;
    if (analyzed_for_recovery) {
        analysis = (ReaderDocumentAnalysis) {0};
        if (!reader_document_prepare_complete_with_stylesheet(
                &session->page.document, &session->page.stylesheet,
                &analysis)) {
            analysis = (ReaderDocumentAnalysis) {
                .prepared = true,
                .bounded_out = true
            };
        }
        session->page.reader_analysis = analysis;
    }
    /* Realm retirement is permanent. A bounded prefix can establish useful
       prose without proving that the uninspected tail is not a form or app
       shell, so never make that irreversible transition from a truncated
       extraction. Complete retained semantic text remains a valid fallback
       even when its score is below Reader's ARTICLE threshold. */
    if (analysis.bounded_out || analysis.extraction_truncated) {
        if (analyzed_for_recovery) {
            /* This strict preflight installed no tree. Leave explicit Reader
               free to perform its separately permitted shortened extraction
               later rather than caching analysis-only as presentation-ready. */
            session->page.reader_analysis.prepared = false;
        }
        return false;
    }
    return analysis.kind != READER_PAGE_RAW;
}

/* A committed page whose realm was refused memory during this turn and
   whose body ended degraded (most of its text or every action gone): an app
   that ran out of memory tore the server-rendered page down. Put the body
   back as it was when memory ran out (the realm's memory-rescue copy,
   without scripts), through the live realm so its node handles and the
   resource bookkeeping see an ordinary replacement, lay it out, then retire
   the realm. A page that keeps its content is left to the ordinary checks.
   The copy is always consumed. */
static bool navigation_rescue_exhausted_page(NavigationSession *session,
                                             size_t heap_rejections_before)
{
    ScriptRuntime *runtime = session->page.runtime;
    DocumentBodySnapshot rescue = {0};
    if (runtime == NULL
        || !script_runtime_take_memory_rescue(runtime, &rescue)) return false;
    bool rescued = false;
    if (script_runtime_heap_rejections(runtime) > heap_rejections_before
        && navigation_restore_server_body_if_degraded(session, &rescue)) {
        session->page.script_degradation_observed = true;
        (void) navigation_relayout(session);
        if (navigation_retire_exhausted_script_realms(session, true)) {
            session->page.script_memory_rescued = true;
            (void) navigation_relayout_static_fallback(session);
        }
        rescued = true;
    }
    document_body_snapshot_destroy(&rescue);
    return rescued;
}

bool navigation_page_memory_rescued(const NavigationSession *session)
{
    return session != NULL && session->page.script_memory_rescued;
}

bool navigation_advance_runtime(NavigationSession *session,
                                unsigned elapsed_ms,
                                size_t callback_budget)
{
    if (session == NULL || !session->page.loaded) return false;
    /* A declarative refresh runs on the page clock whether or not the page
       has an author realm. A page-script navigation already requested wins
       the tick; a deferring owner takes the refresh itself. */
    if (navigation_refresh_advance(session, elapsed_ms)
        && !session->defer_script_navigation
        && (session->page.runtime == NULL
            || !script_runtime_has_pending_navigation(
                   session->page.runtime))) {
        char refresh_url[NAVIGATION_URL_LIMIT];
        navigation_refresh_follow(session, refresh_url, sizeof(refresh_url));
        navigation_retire_large_incumbent_realm(session);
        uint64_t generation = navigation_begin(session);
        return navigation_load_url(session, generation, refresh_url,
                                   4 * 1024 * 1024, 30000,
                                   session->viewport.device_width,
                                   session->fonts, session->images, false);
    }
    /* A readable page whose bounded author realm was retired has no script
       work to advance. Treat that as a successful idle tick so the browser
       shell, background resources, scrolling, and navigation remain live. */
    if (session->page.runtime == NULL) return true;
    uint64_t runtime_started = navigation_now_us();
    size_t heap_rejections_before = script_runtime_heap_rejections(
        session->page.runtime);
    /* A copy left from memory refused outside a turn (an event handler) is
       not this turn's. */
    (void) script_runtime_take_memory_rescue(session->page.runtime, NULL);
    if (session->committed_scroll_reflection_pending) {
        session->committed_scroll_reflection_pending = false;
        const NavigationEntry *entry = navigation_current(session);
        if (entry != NULL) {
            /* Reflection can invoke an author-defined viewport setter. Do it
               only in an owner turn, never in the layout-publication return:
               a destructive setter must be settled before any paint or other
               author task can use that display list. */
            (void) script_runtime_set_page_scroll(session->page.runtime,
                                                  entry->scroll_y);
            if (navigation_retire_failed_document_refresh(
                    session, session->page.runtime)) return false;
            if (script_runtime_consume_relayout(session->page.runtime)
                && !navigation_relayout(session)) return false;
        }
    }
    bool advanced = script_runtime_advance(
        session->page.runtime, elapsed_ms, callback_budget,
        &session->page.script_result);
    uint64_t page_script_finished = navigation_now_us();
    if (page_script_finished > runtime_started)
        session->performance.advance_page_script_us +=
            page_script_finished - runtime_started;
    /* A top-level realm owns the document transition once it requests one.
       Do not let refresh retirement or stale child-frame work from the
       outgoing document veto that committed replacement in the same tick. */
    if (script_runtime_has_pending_navigation(session->page.runtime)) {
        goto consume_script_navigation;
    }
    if (navigation_retire_failed_document_refresh(
            session, session->page.runtime)) return false;
    if (!advanced) {
        capture_cached_script_error(session,
                                    session->page.script_result.error);
        /*
         * Once the authoritative document is committed, a later author task
         * exhausting its bounded realm must not take the browser shell or
         * readable page down with it. Parser-time failures still fail closed
         * because their DOM may be incomplete; this path is safe to degrade
         * to the already-committed static document.
         */
        if (navigation_rescue_exhausted_page(
                session, heap_rejections_before)) {
            session->performance.runtime_us += navigation_record_slice(
                session, NAVIGATION_SLICE_RUNTIME, runtime_started,
                callback_budget);
            return true;
        }
        if (navigation_retire_exhausted_script_realms(session, false)) {
            /* Retirement enables the static-content cascade after the last
               script-driven layout may already have hidden the page shell.
               Rebuild immediately while the old layout is still retained;
               a refused rebuild leaves that committed layout usable. */
            (void) navigation_relayout_static_fallback(session);
            session->performance.runtime_us += navigation_record_slice(
                session, NAVIGATION_SLICE_RUNTIME, runtime_started,
                callback_budget);
            return true;
        }
#if NAVIGATION_CAPABILITY_TRACES_COMPILED
        /* Lab error context (last_script_error_context is read only by the
           host lab): decode a generated function's source into it. */
        unsigned long generated_line = 0, generated_column = 0;
        bool captured_generated_source = false;
        if (tilefinch_trace_eval_source()
            && stack_generated_location(session->page.script_result.error,
                                        &generated_line,
                                        &generated_column)) {
            char original_error[sizeof(session->page.script_result.error)];
            snprintf(original_error, sizeof(original_error), "%s",
                     session->page.script_result.error);
            char diagnostic[640];
            snprintf(diagnostic, sizeof(diagnostic),
                     "(()=>{const a=globalThis.__tilefinchEvalSources;"
                     "if(!Array.isArray(a)){globalThis.pocSummary="
                     "'eval-sources-unavailable';return;}for(let i=a.length-1;"
                     "i>=0;i--){const s=a[i];let at=0,ok=true;"
                     "for(let line=1;line<%lu;line++){const next=s.indexOf('\\n',at);"
                     "if(next<0){ok=false;break;}at=next+1;}if(!ok)continue;"
                     "const end=s.indexOf('\\n',at),limit=end<0?s.length:end;"
                     "if(%lu-1>limit-at)continue;at+=%lu-1;"
                     "globalThis.pocSummary=s.slice(Math.max(0,at-900),"
                     "Math.min(s.length,at+900));return;}"
                     "globalThis.pocSummary='eval-source-no-match count='+a.length+"
                     "' lengths='+a.map(s=>s.length).join(',');})()",
                     generated_line, generated_column, generated_column);
            if (script_runtime_evaluate_diagnostic(
                    session->page.runtime, diagnostic,
                    "<generated-eval-context>",
                    &session->page.script_result)) {
                snprintf(session->last_script_error_context,
                         sizeof(session->last_script_error_context),
                         "generated-eval %lu:%lu: %s", generated_line,
                         generated_column,
                         session->page.script_result.summary);
                captured_generated_source = true;
            }
            snprintf(session->page.script_result.error,
                     sizeof(session->page.script_result.error), "%s",
                     original_error);
            session->page.script_result.success = false;
        }
        char function_name[64] = {0};
        if (!captured_generated_source
            && stack_generated_function_name(session->page.script_result.error,
                                          function_name,
                                          sizeof(function_name))) {
            char original_error[sizeof(session->page.script_result.error)];
            snprintf(original_error, sizeof(original_error), "%s",
                     session->page.script_result.error);
            char diagnostic[320];
            snprintf(diagnostic, sizeof(diagnostic),
                     "try{globalThis.pocSummary=String("
                     "__tilefinchDiagnosticLookup('%s'))}catch(error){"
                     "globalThis.pocSummary='lookup-error:'+String(error)}",
                     function_name);
            if (script_runtime_evaluate_diagnostic(
                    session->page.runtime, diagnostic,
                    "<generated-function-source>",
                    &session->page.script_result)) {
                snprintf(session->last_script_error_context,
                         sizeof(session->last_script_error_context),
                         "generated-function %s: %s", function_name,
                         session->page.script_result.summary);
            }
            snprintf(session->page.script_result.error,
                     sizeof(session->page.script_result.error), "%s",
                     original_error);
            session->page.script_result.success = false;
        }
#endif
        page_capability_trace_report(session);
        snprintf(session->last_error, sizeof(session->last_error), "%s",
                 session->page.script_result.error[0] == '\0'
                   ? "JavaScript scheduler tick failed"
                   : session->page.script_result.error);
        return false;
    }
    if (tilefinch_trace_callback_source()
        && session->last_script_error_context[0] == '\0'
        && session->page.script_result.last_uncaught_callback_error[0]
            != '\0') {
        collect_document_stack_source_context_into(
            &session->page.document,
            session->page.script_result.last_uncaught_callback_error,
            session->last_script_error_context,
            sizeof(session->last_script_error_context));
    }
    const char *current_url = navigation_active_document_url(session);
    if (session->document_scripts_enabled && current_url != NULL
        && !navigation_load_frames(session, current_url)) {
        snprintf(session->last_error, sizeof(session->last_error),
                 "dynamic frame loading failed: %.440s",
                 session->page.script_result.error);
        return false;
    }
    if (!navigation_progress_frames(session)) {
        snprintf(session->last_error, sizeof(session->last_error),
                 "dynamic frame completion failed: %.430s",
                 session->page.script_result.error);
        return false;
    }
    if (NAVIGATION_TRACES_FRAME_CAPABILITIES(session)
        && session->page.frame_count != 0) {
        (void) navigation_collect_frame_capability_trace(session);
    }
    uint64_t frames_started = navigation_now_us();
    if (frames_started > page_script_finished)
        session->performance.advance_frame_setup_us +=
            frames_started - page_script_finished;
    for (size_t i = 0; i < session->page.frame_count; i++) {
        NavigationFrame *frame = &session->page.frames[i];
        if (!frame->loaded || frame->runtime == NULL) continue;
        uint64_t frame_script_started = navigation_now_us();
        bool frame_advanced = script_runtime_advance(
            frame->runtime, elapsed_ms, callback_budget,
            &frame->script_result);
        uint64_t frame_script_finished = navigation_now_us();
        if (frame_script_finished > frame_script_started)
            session->performance.advance_frame_script_us +=
                frame_script_finished - frame_script_started;
        /* A child callback may fail after the top realm requested a reload.
           The child belongs to the page being replaced and must not suppress
           that already-published top-level navigation. */
        if (script_runtime_has_pending_navigation(session->page.runtime)) {
            goto consume_script_navigation;
        }
        if (navigation_retire_failed_document_refresh(
                session, frame->runtime)) return false;
        if (!frame_advanced) {
#if NAVIGATION_CAPABILITY_TRACES_COMPILED
            /* reject_source_context is read only by the host lab. */
            collect_stack_source_context(frame, frame->script_result.error);
#endif
            if (NAVIGATION_TRACES_FRAME_CAPABILITIES(session)) {
                char name[64] = {0};
                if (stack_function_name(frame->script_result.error, name,
                                        sizeof(name))) {
                    char diagnostic[1024];
                    snprintf(diagnostic, sizeof(diagnostic),
                        "try{const s=String(__tilefinchDiagnosticLookup('%s')),"
                        "o=[...s.matchAll(/[A-Za-z0-9_$]+={(.*?)}[,;]/g)],"
                        "m=s.match(/},[A-Za-z0-9_$]+=([A-Za-z0-9_$]+)[,;]/),"
                        "d=__tilefinchDiagnosticLookup(m[1]);"
                        "decoded=o.map((x,i)=>i+':'+[...x[1].matchAll(/([A-Za-z0-9_$]+):([0-9.e]+)/g)].map(v=>v[1]+'='+d(Number(v[2]))).join(',')).join('|'),"
                        "missing=globalThis.__tilefinchCapabilityTrace?[...__tilefinchCapabilityTrace.missing].sort().slice(0,64):[];"
                        "globalThis.pocSummary=decoded+'|missing='+missing.join(',')}catch(e){globalThis.pocSummary='decode-error:'+String(e)}",
                        name);
                    (void) script_runtime_evaluate_diagnostic(
                        frame->runtime, diagnostic, "<frame-error-decode>",
                        &frame->script_result);
                }
            }
            snprintf(session->last_error, sizeof(session->last_error),
                     "frame scheduler failed: %.470s",
                     frame->script_result.error);
            return false;
        }
        if (script_runtime_consume_relayout(frame->runtime)) {
            if (!navigation_refresh_frame_presentation_or_defer(
                    session, frame)) return false;
        }
        if (tilefinch_trace_callback_source()
            && frame->callback_error_source_context[0] == '\0'
            && frame->script_result.last_uncaught_callback_error[0] != '\0') {
            collect_stack_source_context_into(
                frame, frame->script_result.last_uncaught_callback_error,
                frame->callback_error_source_context,
                sizeof(frame->callback_error_source_context));
        }
    }
    uint64_t messages_started = navigation_now_us();
    if (messages_started > frames_started)
        session->performance.advance_frames_us +=
            messages_started - frames_started;
    bool messages_delivered = navigation_deliver_frame_messages(session);
    uint64_t messages_finished = navigation_now_us();
    if (messages_finished > messages_started)
        session->performance.advance_messages_us +=
            messages_finished - messages_started;
    if (!messages_delivered) {
        if (navigation_retire_exhausted_script_realms(session, false)) {
            session->performance.frame_message_soft_failures++;
            (void) navigation_relayout_static_fallback(session);
            session->performance.runtime_us += navigation_record_slice(
                session, NAVIGATION_SLICE_RUNTIME, runtime_started,
                callback_budget);
            return true;
        }
        snprintf(session->last_error, sizeof(session->last_error),
                 "cross-frame message delivery failed: %.450s",
                 session->page.script_result.error);
        return false;
    }
    /* Retry completion tasks deferred by an incomplete checkpoint even if
       that checkpoint made no DOM changes. Keep this before navigation,
       scroll and relayout consumption: a load handler may destroy the DOM
       or navigate, so its effects must settle before returning to paint. */
    if (!script_runtime_has_pending_mutations(session->page.runtime)
        && !navigation_sweep_stylesheet_events(session)) return false;
consume_script_navigation:
    ;
    if (session->defer_script_navigation
        && script_runtime_has_pending_navigation(session->page.runtime)) {
        session->performance.runtime_us += navigation_record_slice(
            session, NAVIGATION_SLICE_RUNTIME, runtime_started,
            callback_budget);
        return true;
    }
    char navigation_url[NAVIGATION_URL_LIMIT];
    bool replace = false;
    bool user_activated = false;
    if (script_runtime_consume_navigation(session->page.runtime,
                                          navigation_url,
                                          sizeof(navigation_url),
                                          &replace, &user_activated)) {
        if (NAVIGATION_TRACES_FRAME_CAPABILITIES(session)) {
            (void) navigation_collect_frame_capability_trace(session);
        }
        if (NAVIGATION_TRACES_PAGE_CAPABILITIES(session)) {
            page_capability_trace_report(session);
            snprintf(session->previous_page_trace,
                     sizeof(session->previous_page_trace), "%s",
                     session->last_page_trace);
        }
        navigation_note_script_initiator(session, navigation_url,
                                         user_activated, replace);
        navigation_retire_large_incumbent_realm(session);
        uint64_t generation = navigation_begin(session);
        session->performance.runtime_us += navigation_record_slice(
            session, NAVIGATION_SLICE_RUNTIME, runtime_started,
            callback_budget);
        return navigation_load_url(session, generation, navigation_url,
                                   4 * 1024 * 1024, 30000,
                                   session->viewport.device_width,
                                   session->fonts, session->images,
                                   !replace);
    }
    int scroll_y = 0;
    if (script_runtime_consume_scroll(session->page.runtime, &scroll_y)) {
        int maximum = navigation_max_scroll(session);
        navigation_set_scroll(session, scroll_y < maximum ? scroll_y : maximum);
    }
    size_t heap_rejections_after = script_runtime_heap_rejections(
        session->page.runtime);
    bool relayout_requested =
        script_runtime_consume_relayout(session->page.runtime);
    /* A page task may catch an actual QuickJS allocation refusal, leaving the
       scheduler alive even though its last mutation hid the entire authored
       shell. Distinguish that allocator fact from an author merely throwing
       text containing "out of memory", and degrade only a blank, text-rich
       committed page. */
    bool allocator_rejected =
        heap_rejections_after > heap_rejections_before;
    bool ok = true;
    if (relayout_requested) {
        uint64_t relayout_started = navigation_now_us();
        ok = navigation_relayout(session);
        uint64_t relayout_finished = navigation_now_us();
        if (relayout_finished > relayout_started)
            session->performance.advance_relayout_us +=
                relayout_finished - relayout_started;
    }
    /* Inspect the successfully settled layout, not the pre-mutation command
       count. Only a real allocator rejection plus blank paint/control output
       and bounded useful content may retire a realm. Hard relayout/security
       failures still return above their ordinary fail-closed path. */
    bool rescued = ok && allocator_rejected
        && navigation_rescue_exhausted_page(session, heap_rejections_before);
    if (!rescued && ok && allocator_rejected
        && navigation_layout_is_visually_blank(&session->page.layout)
        && navigation_blank_recovery_has_useful_content(session)
        && navigation_retire_exhausted_script_realms(session, true)) {
        (void) navigation_relayout_static_fallback(session);
    }
    /* An unused memory-rescue copy does not outlive its turn. */
    (void) script_runtime_take_memory_rescue(session->page.runtime, NULL);
    session->performance.runtime_us += navigation_record_slice(
        session, NAVIGATION_SLICE_RUNTIME, runtime_started, callback_budget);
    return ok;
}

bool navigation_take_script_navigation(NavigationSession *session,
                                       char *url, size_t capacity,
                                       bool *record_history)
{
    if (session == NULL || url == NULL || capacity == 0
        || !session->defer_script_navigation || !session->page.loaded
        || session->page.runtime == NULL) return false;
    bool replace = false;
    bool user_activated = false;
    if (!script_runtime_consume_navigation(session->page.runtime, url,
                                           capacity, &replace,
                                           &user_activated)) return false;
    navigation_note_script_initiator(session, url, user_activated, replace);
    if (record_history != NULL) *record_history = !replace;
    return true;
}
