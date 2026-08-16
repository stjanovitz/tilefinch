static void navigation_stream_release_blocking_stylesheet(
    NavigationDocumentStream *stream)
{
    if (stream == NULL) return;
    /* getComputedStyle() may have observed this temporary sheet while a
       parser-blocking script ran.  Clear the runtime bridge before reclaiming
       it so EOF finalization and error teardown never leave a dangling
       stylesheet pointer.  The candidate pipeline binds the final sheet
       before deferred/async work is resumed. */
    if (!stream->runtime_transferred) {
        script_runtime_set_stylesheet(stream->runtime, NULL);
    }
    stylesheet_destroy(&stream->blocking_stylesheet);
    stream->blocking_stylesheet_ready = false;
    stream->blocking_stylesheet_user_context_neutral = false;
    stream->blocking_stylesheet_fingerprint_valid = false;
}

static uint64_t navigation_stylesheet_fingerprint_field(
    uint64_t hash, const char *marker, const char *value, size_t length,
    bool present)
{
    hash = fingerprint_bytes(hash, marker, strlen(marker) + 1u);
    unsigned char presence = present ? 1u : 0u;
    hash = fingerprint_bytes(hash, (const char *) &presence, 1u);
    if (present) hash = fingerprint_bytes(hash, value, length);
    unsigned char terminator = 0xffu;
    return fingerprint_bytes(hash, (const char *) &terminator, 1u);
}

static bool navigation_stream_collect_stylesheet_inputs(
    NavigationDocumentStream *stream, const PocDocument *document,
    NavigationStylesheetInput **inputs, size_t *count, bool *overflow)
{
    if (inputs != NULL) *inputs = NULL;
    if (count != NULL) *count = 0;
    if (overflow != NULL) *overflow = false;
    if (stream == NULL || stream->session == NULL || document == NULL
        || document->html == NULL || inputs == NULL || count == NULL
        || overflow == NULL) return false;
    NavigationStylesheetInput *collected = NULL;
    size_t capacity = 0;
    lxb_dom_node_t *node = lxb_dom_interface_node(document->html);
    lxb_dom_node_t *boundary_parent = node->parent;
    size_t visited = 0;
    while (node != NULL && node != boundary_parent) {
        if (++visited > NAVIGATION_DOM_TRAVERSAL_NODE_LIMIT) {
            budget_free(stream->session->budget, collected);
            return false;
        }
        size_t name_length = 0;
        const char *name = document_element_name(node, &name_length);
        bool style = name != NULL && name_length == 5
            && memcmp(name, "style", 5) == 0;
        bool link = name != NULL && name_length == 4
            && memcmp(name, "link", 4) == 0;
        if (link) {
            size_t rel_length = 0;
            const char *rel = document_attribute(node, "rel", &rel_length);
            link = rel != NULL
                && navigation_token_contains(
                       rel, rel_length, "stylesheet");
            /* The raw preload scanner discovers links before Lexbor creates
               their nodes. A provisional stylesheet checkpoint can precede
               the broader metadata walk, so bind the already-walked node
               here as well. Otherwise settle ignores the speculative request
               and the authoritative loader starts a duplicate fetch. */
            if (link) {
                navigation_stream_confirm_stylesheet_preload(stream, node);
            }
        }
        if (style || link) {
            if (*count == NAVIGATION_STYLESHEET_INPUT_LIMIT) {
                *overflow = true;
            } else {
                if (*count == capacity) {
                    size_t next = capacity == 0 ? 8u : capacity * 2u;
                    if (next > NAVIGATION_STYLESHEET_INPUT_LIMIT) {
                        next = NAVIGATION_STYLESHEET_INPUT_LIMIT;
                    }
                    NavigationStylesheetInput *grown = budget_realloc(
                        stream->session->budget, collected,
                        next * sizeof(*grown));
                    if (grown == NULL) {
                        budget_free(stream->session->budget, collected);
                        return false;
                    }
                    collected = grown;
                    capacity = next;
                }
                uint64_t hash = UINT64_C(1469598103934665603);
                hash = navigation_stylesheet_fingerprint_field(
                    hash, "element", name, name_length, true);
                if (style) {
                    for (lxb_dom_node_t *child = node->first_child;
                         child != NULL; child = child->next) {
                        size_t length = 0;
                        const char *text = document_text_data(child, &length);
                        hash = navigation_stylesheet_fingerprint_field(
                            hash, "css", text, length, text != NULL);
                    }
                } else {
                    const char *attributes[] = {
                        "rel", "href", "media", "referrerpolicy"
                    };
                    for (size_t i = 0;
                         i < sizeof(attributes) / sizeof(attributes[0]);
                         i++) {
                        size_t length = 0;
                        const char *value = document_attribute(
                            node, attributes[i], &length);
                        hash = navigation_stylesheet_fingerprint_field(
                            hash, attributes[i], value, length,
                            value != NULL);
                    }
                }
                collected[*count] = (NavigationStylesheetInput) {
                    .node = node, .hash = hash, .style = style
                };
                (*count)++;
            }
        }
        if (node->first_child != NULL) {
            node = node->first_child;
            continue;
        }
        while (node != NULL && node != boundary_parent
               && node->next == NULL) node = node->parent;
        if (node != NULL && node != boundary_parent) node = node->next;
    }
    *inputs = collected;
    return true;
}

static void navigation_stream_replace_stylesheet_inputs(
    NavigationDocumentStream *stream, NavigationStylesheetInput *inputs,
    size_t count, bool overflow)
{
    if (stream == NULL || stream->session == NULL) return;
    budget_free(stream->session->budget, stream->stylesheet_inputs);
    stream->stylesheet_inputs = inputs;
    stream->stylesheet_input_count = count;
    stream->stylesheet_input_overflow = overflow;
}

static void navigation_stream_capture_stylesheet_inputs(
    NavigationDocumentStream *stream, const PocDocument *document)
{
    NavigationStylesheetInput *inputs = NULL;
    size_t count = 0;
    bool overflow = false;
    if (!navigation_stream_collect_stylesheet_inputs(
            stream, document, &inputs, &count, &overflow)) {
        navigation_stream_replace_stylesheet_inputs(
            stream, NULL, 0, true);
        return;
    }
    navigation_stream_replace_stylesheet_inputs(
        stream, inputs, count, overflow);
}

static bool navigation_stream_try_continue_stylesheet(
    NavigationDocumentStream *stream, PocDocument *document,
    uint64_t fingerprint)
{
#ifndef TILEFINCH_NO_TRACE
    if (getenv("TILEFINCH_DISABLE_STYLESHEET_CONTINUATION") != NULL) {
        return false;
    }
#endif
    if (stream == NULL || stream->session == NULL || document == NULL
        || !stream->blocking_stylesheet_ready
        || !stream->blocking_stylesheet_user_context_neutral
        || stream->stylesheet_input_overflow) return false;
    uint64_t discovery_started = navigation_now_us();
    NavigationStylesheetInput *current = NULL;
    size_t current_count = 0;
    bool current_overflow = false;
    if (!navigation_stream_collect_stylesheet_inputs(
            stream, document, &current, &current_count,
            &current_overflow)
        || current_overflow
        || current_count <= stream->stylesheet_input_count) {
        budget_free(stream->session->budget, current);
        return false;
    }
    bool prefix = true;
    for (size_t i = 0; i < stream->stylesheet_input_count; i++) {
        const NavigationStylesheetInput *before =
            &stream->stylesheet_inputs[i];
        const NavigationStylesheetInput *after = &current[i];
        if (before->node != after->node || before->hash != after->hash
            || before->style != after->style) {
            prefix = false;
            break;
        }
    }
    if (!prefix) {
        budget_free(stream->session->budget, current);
        return false;
    }
    lxb_dom_node_t *suffix[NAVIGATION_STYLESHEET_INPUT_LIMIT];
    size_t suffix_count = current_count - stream->stylesheet_input_count;
    for (size_t i = 0; i < suffix_count; i++) {
        suffix[i] =
            current[stream->stylesheet_input_count + i].node;
    }
    NavigationSession *session = stream->session;
    uint64_t started = navigation_now_us();
    session->performance.blocking_stylesheet_continuation_discovery_us +=
        started >= discovery_started ? started - discovery_started : 0;
    size_t rules_before = stream->blocking_stylesheet.count;
    uint64_t append_started = navigation_now_us();
    bool inline_only = true;
    for (size_t i = stream->stylesheet_input_count;
         i < current_count; i++) {
        if (!current[i].style) {
            inline_only = false;
            break;
        }
    }
    bool settle_suffix_preloads = true;
#ifndef TILEFINCH_NO_TRACE
    settle_suffix_preloads =
        getenv("TILEFINCH_DISABLE_STYLESHEET_SUFFIX_PRELOAD_SETTLE") == NULL;
#endif
    if (!inline_only && session->external_resources_enabled
        && settle_suffix_preloads) {
        navigation_stream_settle_style_preloads(stream);
    }
    bool appended = inline_only
        ? stylesheet_append_style_elements(
            &stream->blocking_stylesheet, suffix, suffix_count,
            &stream->parser->document.content_security_policy)
        : stylesheets_append_ordered_suffix_with_context(
            &stream->blocking_stylesheet, session->budget,
            suffix, suffix_count, stream->resource_base_url,
            stream->document_url, stream->referrer_policy,
            &stream->parser->document.content_security_policy,
            session->maximum_stylesheets,
            session->maximum_stylesheet_bytes,
            session->maximum_stylesheet_file_bytes,
            session->resource_timeout_ms, stream->scheduler,
            session->browser_session, &stream->stylesheet_resources,
            &stream->blocking_stylesheets);
    if (!appended) {
        session->performance.blocking_stylesheet_continuation_fallbacks++;
        budget_free(session->budget, current);
        return false;
    }
    uint64_t append_finished = navigation_now_us();
    session->performance.blocking_stylesheet_continuation_append_us +=
        append_finished >= append_started
            ? append_finished - append_started : 0;
    /* The unchanged-input-prefix proof above is the semantic guard. Every
       suffix is parsed in its original document order, after every rule and
       retained intern entry it followed in a clean build. Root variables are
       resolved from the final table, not folded into prior declarations, and
       layer identities retain first-occurrence order. Their monotonic growth
       therefore cannot reinterpret the already-compiled prefix. The former
       before/after context-signature check rejected both valid cases only
       after doing the append, forcing the caller to parse the whole sheet a
       second time in the same parser callback. */
    session->performance.blocking_stylesheet_continuations++;
    session->performance.blocking_stylesheet_continuation_prefix_inputs =
        stream->stylesheet_input_count;
    session->performance.blocking_stylesheet_continuation_suffix_inputs =
        suffix_count;
    session->performance.blocking_stylesheet_continuation_rules_before =
        rules_before;
    session->performance.blocking_stylesheet_continuation_rules +=
        stream->blocking_stylesheet.count - rules_before;
    uint64_t finished = navigation_now_us();
    session->performance.blocking_stylesheet_continuation_us +=
        finished >= started ? finished - started : 0;
    navigation_stream_replace_stylesheet_inputs(
        stream, current, current_count, false);
    stream->blocking_stylesheet_fingerprint = fingerprint;
    stream->blocking_stylesheet_fingerprint_valid = true;
    layout_reuse_cache_destroy(stream->preview_layout_reuse);
    stream->preview_layout_reuse = NULL;
    if (stream->runtime != NULL) {
        script_runtime_set_stylesheet(
            stream->runtime, &stream->blocking_stylesheet);
        script_runtime_invalidate_document_base(stream->runtime);
    }
    uint64_t returned = navigation_now_us();
    session->performance.blocking_stylesheet_continuation_us +=
        returned >= finished ? returned - finished : 0;
    return true;
}

/* Parser-blocking scripts need the stylesheet state visible at their exact
   checkpoint.  Hash only inputs which can alter the sheet itself: inline
   CSS, external stylesheet references/provenance, user CSS, resource bounds,
   the base URL, referrer policy, and viewport. Ordinary DOM/class/style
   mutations are resolved against the same sheet and must not force CSS
   reparsing. */
static bool navigation_stream_stylesheet_fingerprint(
    NavigationDocumentStream *stream, const PocDocument *document,
    uint64_t *fingerprint)
{
    if (stream == NULL || stream->session == NULL
        || document == NULL || document->html == NULL
        || fingerprint == NULL) return false;
    const NavigationSession *session = stream->session;
    if (session->user_css_length != 0 && session->user_css == NULL) {
        return false;
    }
    uint64_t hash = UINT64_C(1469598103934665603);
    hash = navigation_stylesheet_fingerprint_field(
        hash, "base-url", stream->resource_base_url,
        strlen(stream->resource_base_url), true);
    hash = navigation_stylesheet_fingerprint_field(
        hash, "referrer-policy", stream->referrer_policy,
        strlen(stream->referrer_policy), true);
    hash = fingerprint_bytes(
        hash, (const char *) &stream->viewport.css_width,
        sizeof(stream->viewport.css_width));
    hash = fingerprint_bytes(
        hash, (const char *) &stream->viewport.css_height,
        sizeof(stream->viewport.css_height));
    hash = fingerprint_bytes(
        hash, (const char *) &session->external_resources_enabled,
        sizeof(session->external_resources_enabled));
    hash = fingerprint_bytes(
        hash, (const char *) &session->maximum_stylesheets,
        sizeof(session->maximum_stylesheets));
    hash = fingerprint_bytes(
        hash, (const char *) &session->maximum_stylesheet_bytes,
        sizeof(session->maximum_stylesheet_bytes));
    hash = fingerprint_bytes(
        hash, (const char *) &session->maximum_stylesheet_file_bytes,
        sizeof(session->maximum_stylesheet_file_bytes));
    hash = navigation_stylesheet_fingerprint_field(
        hash, "user-css", session->user_css, session->user_css_length,
        session->user_css_length != 0);
    const TilefinchContentSecurityPolicy *csp =
        &document->content_security_policy;
    hash = navigation_stylesheet_fingerprint_field(
        hash, "csp-origin", csp->document_origin,
        strlen(csp->document_origin), csp->document_origin[0] != '\0');
    hash = fingerprint_bytes(
        hash, (const char *) &csp->header_present,
        sizeof(csp->header_present));
    hash = fingerprint_bytes(hash, (const char *) &csp->valid,
                             sizeof(csp->valid));
    hash = fingerprint_bytes(hash, (const char *) &csp->policy_count,
                             sizeof(csp->policy_count));
    hash = fingerprint_bytes(hash, (const char *) &csp->storage_used,
                             sizeof(csp->storage_used));
    if (csp->storage_used > sizeof(csp->storage)) return false;
    hash = navigation_stylesheet_fingerprint_field(
        hash, "csp", csp->storage, csp->storage_used,
        csp->storage_used != 0);
    if (csp->policy_count > TILEFINCH_CSP_POLICY_LIMIT) return false;
    for (size_t policy_index = 0;
         policy_index < csp->policy_count; policy_index++) {
        for (size_t directive_index = 0;
             directive_index < TILEFINCH_CSP_DIRECTIVE_COUNT;
             directive_index++) {
            const TilefinchCspDirectiveValue *directive =
                &csp->policies[policy_index].directives[directive_index];
            hash = fingerprint_bytes(
                hash, (const char *) &directive->offset,
                sizeof(directive->offset));
            hash = fingerprint_bytes(
                hash, (const char *) &directive->length,
                sizeof(directive->length));
            hash = fingerprint_bytes(
                hash, (const char *) &directive->present,
                sizeof(directive->present));
        }
    }

    lxb_dom_node_t *node = lxb_dom_interface_node(document->html);
    lxb_dom_node_t *boundary_parent = node->parent;
    size_t visited = 0;
    while (node != NULL && node != boundary_parent) {
        if (++visited > NAVIGATION_DOM_TRAVERSAL_NODE_LIMIT) return false;
        size_t name_length = 0;
        const char *name = document_element_name(node, &name_length);
        bool style = name != NULL && name_length == 5
            && memcmp(name, "style", 5) == 0;
        bool link = name != NULL && name_length == 4
            && memcmp(name, "link", 4) == 0;
        if (link) {
            size_t rel_length = 0;
            const char *rel = document_attribute(node, "rel", &rel_length);
            link = rel != NULL
                && navigation_token_contains(
                       rel, rel_length, "stylesheet");
        }
        if (style || link) {
            hash = navigation_stylesheet_fingerprint_field(
                hash, "element", name, name_length, true);
        }
        if (style) {
            size_t nonce_length = 0;
            const char *nonce = document_attribute(
                node, "nonce", &nonce_length);
            hash = navigation_stylesheet_fingerprint_field(
                hash, "nonce", nonce, nonce_length, nonce != NULL);
            for (lxb_dom_node_t *child = node->first_child; child != NULL;
                 child = child->next) {
                size_t length = 0;
                const char *text = document_text_data(child, &length);
                hash = navigation_stylesheet_fingerprint_field(
                    hash, "css", text, length, text != NULL);
            }
        } else if (link) {
            const char *attributes[] = {
                "rel", "href", "media", "referrerpolicy",
                "integrity", "crossorigin"
            };
            for (size_t i = 0;
                 i < sizeof(attributes) / sizeof(attributes[0]); i++) {
                size_t length = 0;
                const char *value = document_attribute(
                    node, attributes[i], &length);
                hash = navigation_stylesheet_fingerprint_field(
                    hash, attributes[i], value, length, value != NULL);
            }
        }
        if (node->first_child != NULL) {
            node = node->first_child;
            continue;
        }
        while (node != NULL && node != boundary_parent
               && node->next == NULL) {
            node = node->parent;
        }
        if (node != NULL && node != boundary_parent) node = node->next;
    }
    *fingerprint = hash;
    return true;
}

static bool navigation_stream_build_blocking_stylesheet_with_timeout(
    NavigationDocumentStream *stream, PocDocument *document,
    uint64_t fingerprint, long timeout_ms)
{
    if (stream == NULL || stream->session == NULL || document == NULL) {
        return false;
    }
    NavigationSession *session = stream->session;
    navigation_stream_release_blocking_stylesheet(stream);
    /* Speculative fetches never supply CSS directly. Settle only the style
       entries into the ordinary HTTP cache, then let the ordered stylesheet
       loader perform its normal provenance, media, parse, and cascade work. */
    if (session->external_resources_enabled) {
        navigation_stream_settle_style_preloads(stream);
    }
    if (navigation_compiled_stylesheet_cache_take(
            session, fingerprint, &stream->blocking_stylesheet,
            &stream->blocking_stylesheets)) {
        stream->blocking_stylesheet_ready = true;
        stream->blocking_stylesheet_user_context_neutral = true;
        navigation_stream_capture_stylesheet_inputs(stream, document);
        stream->blocking_stylesheet_fingerprint = fingerprint;
        stream->blocking_stylesheet_fingerprint_valid = true;
        if (stream->runtime != NULL) {
            script_runtime_set_stylesheet(
                stream->runtime, &stream->blocking_stylesheet);
            script_runtime_invalidate_document_base(stream->runtime);
        }
        return true;
    }
    bool built = session->external_resources_enabled
        ? stylesheet_build_context_deferred(
              &stream->blocking_stylesheet, session->budget, document,
              &stream->viewport)
        : stylesheet_build_context(
              &stream->blocking_stylesheet, session->budget, document,
              &stream->viewport);
    if (!built) {
        return false;
    }
    stream->blocking_stylesheet_ready = true;
    if (session->external_resources_enabled
        && !stylesheets_load_external_tracked_with_context(
            document, &stream->blocking_stylesheet, session->budget,
            stream->resource_base_url, stream->document_url,
            stream->referrer_policy, session->maximum_stylesheets,
            session->maximum_stylesheet_bytes,
            session->maximum_stylesheet_file_bytes,
            timeout_ms, stream->scheduler,
            session->browser_session, &stream->stylesheet_resources,
            &stream->blocking_stylesheets)) {
        if (!navigation_build_usable_stylesheet_fallback(
                &stream->blocking_stylesheet, session->budget,
                document, &stream->viewport)) {
            navigation_stream_release_blocking_stylesheet(stream);
            return false;
        }
        stream->blocking_stylesheet_ready = true;
        session->performance.stylesheet_pressure_fallbacks++;
    }
    stream->blocking_stylesheet_user_context_neutral = true;
    if (session->user_css_length != 0) {
        uint64_t context_before_user = stylesheet_parse_context_signature(
            &stream->blocking_stylesheet);
        if (!stylesheet_add_user_css(
                &stream->blocking_stylesheet, session->user_css,
                session->user_css_length)) {
            navigation_stream_release_blocking_stylesheet(stream);
            return false;
        }
        stream->blocking_stylesheet_user_context_neutral =
            stylesheet_parse_context_signature(
                &stream->blocking_stylesheet) == context_before_user;
    }
    navigation_stream_capture_stylesheet_inputs(stream, document);
    stream->blocking_stylesheet_fingerprint = fingerprint;
    stream->blocking_stylesheet_fingerprint_valid = true;
    session->performance.blocking_stylesheet_builds++;
    if (stream->runtime != NULL) {
        script_runtime_set_stylesheet(
            stream->runtime, &stream->blocking_stylesheet);
        script_runtime_invalidate_document_base(stream->runtime);
    }
    return true;
}

static bool navigation_stream_build_blocking_stylesheet(
    NavigationDocumentStream *stream, PocDocument *document,
    uint64_t fingerprint)
{
    if (stream == NULL || stream->session == NULL) return false;
    return navigation_stream_build_blocking_stylesheet_with_timeout(
        stream, document, fingerprint,
        stream->session->resource_timeout_ms);
}

/* Author scripts may synchronously read geometry or computed style before
   the streaming parser reaches EOF. Keep that standards boundary local to
   the staged document: refresh a changed sheet, build one authoritative
   in-memory layout, and never invoke JavaScript from the callback. The
   snapshot is replaced after each author mutation and released before the
   streaming runtime is transferred to committed page state. */
static bool navigation_stream_flush_cssom_layout(void *opaque)
{
    NavigationDocumentStream *stream = opaque;
    if (stream == NULL || stream->session == NULL || stream->runtime == NULL
        || stream->parser == NULL) return false;
    NavigationSession *session = stream->session;
    PocDocument *document = &stream->parser->document;

    script_runtime_set_layout(
        stream->runtime, NULL, stream->viewport.css_height);
    if (stream->cssom_layout_ready) {
        layout_destroy(&stream->cssom_layout);
        memset(&stream->cssom_layout, 0, sizeof(stream->cssom_layout));
        stream->cssom_layout_ready = false;
    }

    uint64_t fingerprint = 0;
    if (!navigation_stream_stylesheet_fingerprint(
            stream, document, &fingerprint)) return false;
    bool current = stream->blocking_stylesheet_ready
        && stream->blocking_stylesheet_fingerprint_valid
        && stream->blocking_stylesheet_fingerprint == fingerprint;
    if (!current) {
        bool refreshed = navigation_stream_try_continue_stylesheet(
            stream, document, fingerprint);
        if (!refreshed) {
            refreshed = navigation_stream_build_blocking_stylesheet(
                stream, document, fingerprint);
        }
        if (!refreshed) return false;
    }

    LayoutDocument fresh = {0};
    if (!layout_build_context(
            &fresh, session->budget, document,
            &stream->blocking_stylesheet, stream->fonts, stream->images,
            &stream->viewport)) {
        layout_destroy(&fresh);
        return false;
    }
    stream->cssom_layout = fresh;
    stream->cssom_layout_ready = true;
    script_runtime_set_stylesheet(
        stream->runtime, &stream->blocking_stylesheet);
    script_runtime_set_layout(
        stream->runtime, &stream->cssom_layout,
        stream->viewport.css_height);
    (void) script_runtime_consume_relayout(stream->runtime);
    return true;
}
