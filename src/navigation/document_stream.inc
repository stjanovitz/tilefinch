#define NAVIGATION_SPECULATIVE_RESOURCE_TIMEOUT_MS 5000L

static void navigation_stream_adopt_stylesheet_event_baseline(
    NavigationDocumentStream *stream)
{
    if (stream == NULL || stream->stylesheet_event_baseline_adopted) return;
    navigation_adopt_stylesheet_events(
        &stream->parser->document, stream->resource_base_url,
        stream->stylesheet_events, &stream->stylesheet_event_count);
    stream->stylesheet_event_baseline_adopted = true;
}

static void navigation_stream_try_pre_eof_preview(
    NavigationDocumentStream *stream);

static bool navigation_span_equal(const char *value, size_t length,
                                  const char *wanted)
{
    size_t wanted_length = strlen(wanted);
    return value != NULL && length == wanted_length
        && strncasecmp(value, wanted, length) == 0;
}

static bool navigation_token_contains(const char *value, size_t length,
                                      const char *wanted)
{
    size_t wanted_length = strlen(wanted);
    for (size_t at = 0; value != NULL && at < length;) {
        while (at < length
               && (value[at] == ' ' || value[at] == '\t'
                   || value[at] == '\r' || value[at] == '\n'
                   || value[at] == '\f')) at++;
        size_t end = at;
        while (end < length && value[end] != ' ' && value[end] != '\t'
               && value[end] != '\r' && value[end] != '\n'
               && value[end] != '\f') end++;
        if (end - at == wanted_length
            && strncasecmp(value + at, wanted, wanted_length) == 0) {
            return true;
        }
        at = end;
    }
    return false;
}

static bool navigation_stylesheet_mime_matches(const char *content_type)
{
    if (content_type == NULL) return false;
    while (isspace((unsigned char) *content_type)) content_type++;
    static const char expected[] = "text/css";
    if (strncasecmp(content_type, expected, sizeof(expected) - 1u) != 0) {
        return false;
    }
    char after = content_type[sizeof(expected) - 1u];
    return after == '\0' || after == ';'
        || isspace((unsigned char) after);
}

static TilefinchCredentialsMode navigation_module_credentials(
    lxb_dom_node_t *node)
{
    size_t length = 0;
    const char *value = document_attribute(node, "crossorigin", &length);
    static const char use_credentials[] = "use-credentials";
    return value != NULL && length == sizeof(use_credentials) - 1
        && strncasecmp(value, use_credentials, length) == 0
            ? TILEFINCH_CREDENTIALS_INCLUDE
            : TILEFINCH_CREDENTIALS_SAME_ORIGIN;
}

static bool navigation_stream_preload_seen(
    const NavigationDocumentStream *stream, const lxb_dom_node_t *node)
{
    for (size_t i = 0; i < stream->preload_count; i++) {
        if (node != NULL && stream->preloads[i].node == node) return true;
    }
    return false;
}

static bool navigation_stream_preload_url_seen(
    const NavigationDocumentStream *stream, const char *url)
{
    for (size_t i = 0; stream != NULL && url != NULL
         && i < stream->preload_count; i++) {
        if (stream->preloads[i].url != NULL
            && strcmp(stream->preloads[i].url, url) == 0) return true;
    }
    return false;
}

static void navigation_stream_add_preload_url(
    NavigationDocumentStream *stream, lxb_dom_node_t *node,
    const char *reference, bool script, bool module, bool cors,
    TilefinchCredentialsMode credentials, bool count_discovery,
    bool render_blocking_style)
{
    if (stream == NULL || stream->session == NULL || reference == NULL
        || reference[0] == '\0') return;
    NavigationSession *session = stream->session;
    char resolved[NAVIGATION_URL_LIMIT];
    if (!fetch_resolve_url(stream->resource_base_url, reference, resolved,
                           sizeof(resolved))) {
        session->preloads_failed++;
        return;
    }
    if (navigation_stream_preload_url_seen(stream, resolved)) return;
    if (count_discovery) session->preloads_discovered++;
    if (stream->preload_count == NAVIGATION_PRELOAD_LIMIT) {
        session->preloads_deferred++;
        return;
    }
    NavigationPreload *preload = &stream->preloads[stream->preload_count++];
    memset(preload, 0, sizeof(*preload));
    preload->node = node;
    preload->script = script;
    preload->module = module;
    preload->cors = cors;
    preload->render_blocking_style = render_blocking_style;
    preload->credentials = credentials;
    size_t length = strlen(resolved);
    preload->url = budget_malloc(session->budget, length + 1);
    if (preload->url == NULL) {
        preload->complete = true;
        session->preloads_failed++;
        return;
    }
    memcpy(preload->url, resolved, length + 1);
    preload->maximum_bytes = script
        ? session->maximum_script_file_bytes
        : session->maximum_stylesheet_file_bytes;
    if (preload->maximum_bytes == 0) {
        preload->complete = true;
        session->preloads_deferred++;
    }
}

static void navigation_stream_discover_preload(
    NavigationDocumentStream *stream, lxb_dom_node_t *node)
{
    if (stream == NULL || node == NULL
        || navigation_stream_preload_seen(stream, node)) return;
    size_t rel_length = 0;
    const char *rel = document_attribute(node, "rel", &rel_length);
    bool module = navigation_token_contains(rel, rel_length,
                                            "modulepreload");
    if (!module && !navigation_token_contains(rel, rel_length, "preload")) {
        return;
    }
    stream->session->preloads_discovered++;
    size_t crossorigin_length = 0;
    const char *crossorigin = document_attribute(
        node, "crossorigin", &crossorigin_length);
    bool cors = module || crossorigin != NULL;
    TilefinchCredentialsMode credentials = cors
        ? navigation_module_credentials(node) : TILEFINCH_CREDENTIALS_INCLUDE;
    size_t as_length = 0;
    const char *as = document_attribute(node, "as", &as_length);
    bool style = !module && navigation_span_equal(as, as_length, "style");
    bool script = module
        || navigation_span_equal(as, as_length, "script");
    if (!style && !script) {
        if (stream->preload_count < NAVIGATION_PRELOAD_LIMIT) {
            NavigationPreload *preload =
                &stream->preloads[stream->preload_count++];
            memset(preload, 0, sizeof(*preload));
            preload->node = node;
            preload->complete = true;
        }
        stream->session->preloads_deferred++;
        return;
    }
    size_t href_length = 0;
    const char *href = document_attribute(node, "href", &href_length);
    if (href == NULL || href_length == 0 || href_length >= 2048) {
        if (stream->preload_count < NAVIGATION_PRELOAD_LIMIT) {
            NavigationPreload *preload =
                &stream->preloads[stream->preload_count++];
            memset(preload, 0, sizeof(*preload));
            preload->node = node;
            preload->complete = true;
        }
        stream->session->preloads_failed++;
        return;
    }
    char reference[2048];
    memcpy(reference, href, href_length);
    reference[href_length] = '\0';
    navigation_stream_add_preload_url(
        stream, node, reference, script, module, cors, credentials, false,
        false);
}

static void navigation_stream_confirm_stylesheet_preload(
    NavigationDocumentStream *stream, lxb_dom_node_t *node)
{
    if (stream == NULL || node == NULL) return;
    size_t rel_length = 0, href_length = 0;
    const char *rel = document_attribute(node, "rel", &rel_length);
    const char *href = document_attribute(node, "href", &href_length);
    if (!navigation_token_contains(rel, rel_length, "stylesheet")
        || href == NULL || href_length == 0 || href_length >= 2048) return;
    char reference[2048], resolved[NAVIGATION_URL_LIMIT];
    memcpy(reference, href, href_length);
    reference[href_length] = '\0';
    if (!fetch_resolve_url(stream->resource_base_url, reference, resolved,
                           sizeof(resolved))) return;
    for (size_t i = 0; i < stream->preload_count; i++) {
        NavigationPreload *preload = &stream->preloads[i];
        if (preload->render_blocking_style && preload->url != NULL
            && strcmp(preload->url, resolved) == 0) {
            preload->node = node;
            return;
        }
    }
}

static bool navigation_ascii_whitespace(char character)
{
    return character == ' ' || character == '\t' || character == '\n'
        || character == '\r' || character == '\f';
}

static bool navigation_preload_scan_attribute(
    const char *tag, size_t length, const char *wanted,
    const char **value, size_t *value_length)
{
    if (value != NULL) *value = NULL;
    if (value_length != NULL) *value_length = 0;
    if (tag == NULL || length < 3 || wanted == NULL
        || value == NULL || value_length == NULL) return false;
    size_t at = 1;
    if (at < length && tag[at] == '/') at++;
    while (at < length && !navigation_ascii_whitespace(tag[at])
           && tag[at] != '>' && tag[at] != '/') at++;
    while (at < length) {
        while (at < length && navigation_ascii_whitespace(tag[at])) at++;
        if (at >= length || tag[at] == '>' || tag[at] == '/') break;
        size_t name_start = at;
        while (at < length && !navigation_ascii_whitespace(tag[at])
               && tag[at] != '=' && tag[at] != '>'
               && tag[at] != '/') at++;
        size_t name_length = at - name_start;
        while (at < length && navigation_ascii_whitespace(tag[at])) at++;
        const char *attribute_value = tag + at;
        size_t attribute_length = 0;
        if (at < length && tag[at] == '=') {
            at++;
            while (at < length && navigation_ascii_whitespace(tag[at])) at++;
            if (at < length && (tag[at] == '"' || tag[at] == '\'')) {
                char quote = tag[at++];
                size_t start = at;
                while (at < length && tag[at] != quote) at++;
                attribute_value = tag + start;
                attribute_length = at - start;
                if (at < length) at++;
            } else {
                size_t start = at;
                while (at < length && !navigation_ascii_whitespace(tag[at])
                       && tag[at] != '>') at++;
                attribute_value = tag + start;
                attribute_length = at - start;
            }
        }
        if (name_length == strlen(wanted)
            && strncasecmp(tag + name_start, wanted, name_length) == 0) {
            *value = attribute_value;
            *value_length = attribute_length;
            return true;
        }
    }
    return false;
}

static bool navigation_preload_decode_reference(
    const char *source, size_t length, char output[2048])
{
    if (source == NULL || length == 0 || output == NULL) return false;
    size_t written = 0;
    for (size_t at = 0; at < length;) {
        unsigned char character = (unsigned char) source[at++];
        if (character != '&') {
            if (written == 2047) return false;
            output[written++] = (char) character;
            continue;
        }
        size_t entity = at;
        while (at < length && at - entity <= 12 && source[at] != ';') at++;
        if (at >= length || source[at] != ';') return false;
        size_t entity_length = at - entity;
        at++;
        if (entity_length == 3
            && memcmp(source + entity, "amp", 3) == 0) character = '&';
        else if (entity_length == 4
                 && memcmp(source + entity, "quot", 4) == 0) character = '"';
        else if (entity_length == 4
                 && memcmp(source + entity, "apos", 4) == 0) character = '\'';
        else if (entity_length == 2
                 && memcmp(source + entity, "lt", 2) == 0) character = '<';
        else if (entity_length == 2
                 && memcmp(source + entity, "gt", 2) == 0) character = '>';
        else if (entity_length >= 2 && source[entity] == '#') {
            size_t digit = entity + 1;
            unsigned base = 10;
            if (digit < entity + entity_length
                && (source[digit] == 'x' || source[digit] == 'X')) {
                base = 16;
                digit++;
            }
            unsigned value = 0;
            if (digit == entity + entity_length) return false;
            for (; digit < entity + entity_length; digit++) {
                unsigned next;
                if (source[digit] >= '0' && source[digit] <= '9') {
                    next = (unsigned) (source[digit] - '0');
                } else if (base == 16 && source[digit] >= 'a'
                           && source[digit] <= 'f') {
                    next = 10u + (unsigned) (source[digit] - 'a');
                } else if (base == 16 && source[digit] >= 'A'
                           && source[digit] <= 'F') {
                    next = 10u + (unsigned) (source[digit] - 'A');
                } else {
                    return false;
                }
                if (value > (127u - next) / base) return false;
                value = value * base + next;
            }
            character = (unsigned char) value;
        } else {
            return false;
        }
        if (written == 2047) return false;
        output[written++] = (char) character;
    }
    output[written] = '\0';
    return true;
}

static bool navigation_preload_tag_name(
    const char *tag, size_t length, bool *closing,
    const char **name, size_t *name_length)
{
    if (closing != NULL) *closing = false;
    if (name != NULL) *name = NULL;
    if (name_length != NULL) *name_length = 0;
    if (tag == NULL || length < 3 || closing == NULL
        || name == NULL || name_length == NULL || tag[0] != '<') {
        return false;
    }
    size_t at = 1;
    if (tag[at] == '/') {
        *closing = true;
        at++;
    }
    size_t start = at;
    while (at < length && !navigation_ascii_whitespace(tag[at])
           && tag[at] != '>' && tag[at] != '/') at++;
    if (at == start) return false;
    *name = tag + start;
    *name_length = at - start;
    return true;
}

static void navigation_stream_preload_scan_tag(
    NavigationDocumentStream *stream)
{
    bool closing = false;
    const char *name = NULL;
    size_t name_length = 0;
    const char *tag = stream->preload_scan_tag;
    size_t length = stream->preload_scan_tag_length;
    if (!navigation_preload_tag_name(
            tag, length, &closing, &name, &name_length)) return;
    if (stream->preload_scan_raw_text != 0) {
        const char *wanted =
            stream->preload_scan_raw_text == 1 ? "script" : "style";
        if (closing && navigation_span_equal(name, name_length, wanted)) {
            stream->preload_scan_raw_text = 0;
        }
        return;
    }
    if (closing && navigation_span_equal(name, name_length, "head")) {
        stream->preload_scan_done = true;
        return;
    }
    if (closing) return;
    if (navigation_span_equal(name, name_length, "script")) {
        const char *src = NULL, *type = NULL, *crossorigin = NULL;
        size_t src_length = 0, type_length = 0, crossorigin_length = 0;
        bool has_src = navigation_preload_scan_attribute(
            tag, length, "src", &src, &src_length);
        bool has_type = navigation_preload_scan_attribute(
            tag, length, "type", &type, &type_length);
        bool module = has_type
            && navigation_span_equal(type, type_length, "module");
        bool classic = !has_type || type_length == 0
            || navigation_span_equal(type, type_length, "text/javascript")
            || navigation_span_equal(
                type, type_length, "application/javascript");
        const char *nomodule_value = NULL;
        size_t nomodule_length = 0;
        bool nomodule = navigation_preload_scan_attribute(
            tag, length, "nomodule", &nomodule_value, &nomodule_length);
        if (has_src && (module || (classic && !nomodule))) {
            char reference[2048];
            if (navigation_preload_decode_reference(
                    src, src_length, reference)) {
                TilefinchCredentialsMode credentials =
                    TILEFINCH_CREDENTIALS_INCLUDE;
                bool has_crossorigin = navigation_preload_scan_attribute(
                    tag, length, "crossorigin", &crossorigin,
                    &crossorigin_length);
                bool cors = module || has_crossorigin;
                if (cors) {
                    credentials = has_crossorigin
                            && navigation_span_equal(
                                crossorigin, crossorigin_length,
                                "use-credentials")
                        ? TILEFINCH_CREDENTIALS_INCLUDE
                        : TILEFINCH_CREDENTIALS_SAME_ORIGIN;
                }
                navigation_stream_add_preload_url(
                    stream, NULL, reference, true, module, cors, credentials,
                    true, false);
            }
        }
        stream->preload_scan_raw_text = 1;
        return;
    }
    if (navigation_span_equal(name, name_length, "style")) {
        stream->preload_scan_raw_text = 2;
        return;
    }
    /* A base element can be inserted or rewritten by an earlier blocking
       script. Stop rather than speculating against an origin the real parser
       may never use. */
    if (navigation_span_equal(name, name_length, "base")) {
        stream->preload_scan_disabled = true;
        return;
    }
    if (!navigation_span_equal(name, name_length, "link")) return;
    const char *disabled = NULL, *href = NULL, *rel = NULL;
    const char *crossorigin = NULL;
    size_t disabled_length = 0, href_length = 0, rel_length = 0;
    size_t crossorigin_length = 0;
    if (navigation_preload_scan_attribute(
            tag, length, "disabled", &disabled, &disabled_length)
        || !navigation_preload_scan_attribute(
               tag, length, "rel", &rel, &rel_length)
        || !navigation_token_contains(rel, rel_length, "stylesheet")
        || !navigation_preload_scan_attribute(
               tag, length, "href", &href, &href_length)) {
        return;
    }
    char reference[2048];
    if (!navigation_preload_decode_reference(
            href, href_length, reference)) return;
    bool cors = navigation_preload_scan_attribute(
        tag, length, "crossorigin", &crossorigin, &crossorigin_length);
    TilefinchCredentialsMode credentials = cors
        && navigation_span_equal(crossorigin, crossorigin_length,
                                 "use-credentials")
            ? TILEFINCH_CREDENTIALS_INCLUDE
            : cors ? TILEFINCH_CREDENTIALS_SAME_ORIGIN
                   : TILEFINCH_CREDENTIALS_INCLUDE;
    navigation_stream_add_preload_url(
        stream, NULL, reference, false, false, cors, credentials, true, true);
}

static void navigation_stream_scan_style_preloads(
    NavigationDocumentStream *stream, const unsigned char *data,
    size_t length)
{
    if (stream == NULL || data == NULL || stream->preload_scan_done
        || stream->preload_scan_disabled) return;
    for (size_t i = 0; i < length && !stream->preload_scan_done
         && !stream->preload_scan_disabled; i++) {
        char character = (char) data[i];
        if (stream->preload_scan_in_comment) {
            if (character == '-') {
                if (stream->preload_scan_tag_length < 2) {
                    stream->preload_scan_tag_length++;
                }
            } else if (character == '>'
                       && stream->preload_scan_tag_length == 2) {
                stream->preload_scan_in_comment = false;
                stream->preload_scan_tag_length = 0;
            } else {
                stream->preload_scan_tag_length = 0;
            }
            continue;
        }
        if (!stream->preload_scan_in_tag) {
            if (character == '<') {
                stream->preload_scan_in_tag = true;
                stream->preload_scan_tag_length = 1;
                stream->preload_scan_tag[0] = '<';
            }
            continue;
        }
        if (stream->preload_scan_tag_length
            == NAVIGATION_PRELOAD_SCAN_TAG_LIMIT) {
            stream->preload_scan_tag_overflow = true;
        } else {
            stream->preload_scan_tag[
                stream->preload_scan_tag_length++] = character;
        }
        if (!stream->preload_scan_tag_overflow
            && stream->preload_scan_tag_length == 4
            && memcmp(stream->preload_scan_tag, "<!--", 4) == 0) {
            stream->preload_scan_in_comment = true;
            stream->preload_scan_in_tag = false;
            stream->preload_scan_tag_length = 0;
            continue;
        }
        if (stream->preload_scan_raw_text != 0
            && stream->preload_scan_tag_length == 2
            && character != '/') {
            stream->preload_scan_in_tag = false;
            stream->preload_scan_tag_length = 0;
            continue;
        }
        if (stream->preload_scan_quote != '\0') {
            if (character == stream->preload_scan_quote) {
                stream->preload_scan_quote = '\0';
            }
            continue;
        }
        if (character == '"' || character == '\'') {
            stream->preload_scan_quote = character;
            continue;
        }
        if (character != '>') continue;
        if (stream->preload_scan_tag_overflow) {
            stream->preload_scan_disabled = true;
        } else {
            navigation_stream_preload_scan_tag(stream);
        }
        stream->preload_scan_in_tag = false;
        stream->preload_scan_tag_overflow = false;
        stream->preload_scan_tag_length = 0;
    }
}

static const char *navigation_normalized_referrer_policy(
    const char *value, size_t length)
{
    if (value == NULL) return NULL;
    while (length != 0 && navigation_ascii_whitespace(*value)) {
        value++;
        length--;
    }
    while (length != 0
           && navigation_ascii_whitespace(value[length - 1])) length--;
    static const char *known[] = {
        "no-referrer", "no-referrer-when-downgrade", "origin",
        "origin-when-cross-origin", "same-origin", "strict-origin",
        "strict-origin-when-cross-origin", "unsafe-url"
    };
    for (size_t i = 0; i < sizeof(known) / sizeof(known[0]); i++) {
        if (strlen(known[i]) == length
            && strncasecmp(value, known[i], length) == 0) return known[i];
    }
    return NULL;
}

static bool navigation_scan_document_referrer_policy(
    const PocDocument *document, char policy[128])
{
    return document_referrer_policy(document, policy, policy, 128);
}

static void navigation_apply_declared_viewport(
    NavigationSession *session, int device_width, int device_height)
{
    if (session == NULL || session->declared_css_width <= 0
        || session->declared_css_height <= 0) return;
    ViewportContext declared;
    if (viewport_context_init(&declared, session->declared_css_width,
                              session->declared_css_height,
                              device_width, device_height)) {
        session->viewport = declared;
    }
}

static bool navigation_stream_scan_metadata_node(
    NavigationDocumentStream *stream, lxb_dom_node_t *node)
{
    if (node == NULL) return true;
    lxb_dom_node_t *boundary_parent = node->parent;
    size_t visited = 0;
    while (node != NULL && node != boundary_parent) {
        if (++visited > NAVIGATION_DOM_TRAVERSAL_NODE_LIMIT) {
            snprintf(stream->session->last_error,
                     sizeof(stream->session->last_error),
                     "stream metadata traversal exceeded %u nodes",
                     NAVIGATION_DOM_TRAVERSAL_NODE_LIMIT);
            return false;
        }
        size_t name_length = 0;
        const char *name = document_element_name(node, &name_length);
        if (navigation_span_equal(name, name_length, "meta")) {
            size_t key_length = 0, content_length = 0;
            const char *key = document_attribute(node, "name", &key_length);
            const char *content = document_attribute(
                node, "content", &content_length);
            if (navigation_span_equal(key, key_length, "referrer")) {
                const char *normalized =
                    navigation_normalized_referrer_policy(
                        content, content_length);
                /* Invalid metadata is ignored, preserving the response policy
                   already installed by the streaming header callback. */
                if (normalized != NULL) {
                    snprintf(stream->referrer_policy,
                             sizeof(stream->referrer_policy), "%s",
                             normalized);
                }
            }
        }
        if (navigation_span_equal(name, name_length, "link")) {
            navigation_stream_confirm_stylesheet_preload(stream, node);
            navigation_stream_discover_preload(stream, node);
        }
        if (node->first_child != NULL) {
            node = node->first_child;
            continue;
        }
        while (node != NULL && node != boundary_parent
               && node->next == NULL) node = node->parent;
        if (node == NULL || node == boundary_parent) break;
        node = node->next;
    }
    return true;
}

static bool navigation_stream_scan_metadata(NavigationDocumentStream *stream)
{
    if (stream == NULL || stream->parser == NULL
        || stream->parser->document.html == NULL) return true;
    if (!document_base_url(&stream->parser->document, stream->document_url,
                           stream->resource_base_url,
                           sizeof(stream->resource_base_url))
        || !navigation_stream_scan_metadata_node(
            stream, lxb_dom_interface_node(stream->parser->document.html))
        || !viewport_context_resolve(
            &stream->session->viewport, &stream->parser->document,
            stream->viewport_width, 272, 980)) {
        if (stream->session->last_error[0] == '\0') {
            snprintf(stream->session->last_error,
                     sizeof(stream->session->last_error),
                     "stream viewport metadata traversal failed");
        }
        return false;
    }
    navigation_apply_declared_viewport(stream->session,
                                       stream->viewport_width, 272);
    /* Parser-blocking scripts execute from the streaming runtime before the
       final page commit.  Give that runtime the same resolved/declared
       layout viewport that style and layout will use; otherwise innerWidth,
       matchMedia(), and viewport units observe the physical display while
       the committed document is laid out at the declared CSS width. */
    stream->viewport = stream->session->viewport;
    return true;
}

static bool navigation_stream_cache_preload(
    NavigationDocumentStream *stream, NavigationPreload *preload,
    FetchResult *result)
{
    BrowserSession *browser = stream->session->browser_session;
    if (browser == NULL || preload->url == NULL || result == NULL) {
        return false;
    }
    TilefinchRequestContext request_context = {
        .target_url = preload->url,
        .initiator_url = stream->document_url,
        .top_level_url = stream->document_url,
        .method = "GET",
        .mode = preload->cors ? TILEFINCH_REQUEST_MODE_CORS
                              : TILEFINCH_REQUEST_MODE_NO_CORS,
        .credentials = preload->credentials,
        .destination = preload->script
            ? TILEFINCH_DESTINATION_SCRIPT
            : TILEFINCH_DESTINATION_STYLE
    };
    /* The generic HTTP cache has no CORS provenance key. A CORS stylesheet
       was retained in the document-local ledger before this call; a classic
       CORS script is deliberately left to its authoritative loader. */
    if (preload->cors && !preload->module) return true;
    char cache_control[256] = {0}, vary[128] = {0};
    (void) fetch_response_header_value(result, "cache-control",
                                       cache_control,
                                       sizeof(cache_control));
    (void) fetch_response_header_value(result, "vary", vary, sizeof(vary));
    char initiator_origin[TILEFINCH_ORIGIN_SERIALIZED_LIMIT] = {0};
    const BrowserCacheEntry *cached = NULL;
    if (preload->cors
        && !url_origin(stream->document_url, initiator_origin,
                       sizeof(initiator_origin))) {
        return false;
    }
    if (preload->module && result->status_code == 304
        && browser_session_cache_match_module(
            browser, preload->url, initiator_origin,
            stream->document_url, false, preload->credentials,
            tilefinch_platform_monotonic_time_ns(),
            &cached) == BROWSER_CACHE_MISS) return false;
    if (preload->script && !preload->module && result->status_code == 304
        && browser_session_cache_match_classic_script(
               browser, preload->url, &request_context,
               tilefinch_platform_monotonic_time_ns(), &cached)
               == BROWSER_CACHE_MISS) return false;
    if (!preload->script && result->status_code == 304
        && browser_session_cache_match_resource(
               browser, preload->url, &request_context,
               tilefinch_platform_monotonic_time_ns(), &cached)
               == BROWSER_CACHE_MISS) return false;
    const char *effective_url = result->effective_url[0] == '\0'
        ? (preload->module && cached != NULL
            ? cached->module_effective_url : preload->url)
        : result->effective_url;
    if (result->status_code == 304) {
        if (preload->module) {
            if (cached == NULL
                || !script_module_revalidated_mime_allowed(
                    cached->content_type,
                    result->content_type)) return false;
            BrowserModuleCacheProvenance provenance = {
                .effective_url = effective_url,
                .initiator_origin = initiator_origin,
                .top_level_url = stream->document_url,
                .credentials = preload->credentials,
                .cors_validated = true,
                .cors_redirect_origin_tainted =
                    result->redirect_origin_tainted,
                .javascript_mime_validated = true
            };
            return provenance.javascript_mime_validated
                && browser_session_cache_revalidate_module(
                    browser, preload->url, cache_control, vary,
                    tilefinch_platform_monotonic_time_ns(), &provenance);
        }
        if (preload->script) {
            return cached != NULL && cached->resource_grant_valid
                && browser_session_cache_revalidate_classic_script(
                       browser, preload->url, cache_control, vary,
                       tilefinch_platform_monotonic_time_ns(),
                       &request_context, &cached->resource_grant);
        }
        TilefinchResourceGrant revalidated = {0};
        bool mime_valid = result->content_type[0] == '\0'
            ? cached != NULL
              && navigation_stylesheet_mime_matches(cached->content_type)
            : navigation_stylesheet_mime_matches(result->content_type);
        bool cache_revalidated = cached != NULL
            && cached->resource_grant_valid
            && fetch_resource_grant_revalidate_304(
                   result, &request_context, &cached->resource_grant,
                   false, mime_valid, false, &revalidated, NULL)
            && browser_session_cache_revalidate_resource(
                   browser, preload->url, cache_control, vary,
                   tilefinch_platform_monotonic_time_ns(),
                   &request_context, &revalidated);
        bool policy_present = false;
        char response_policy[STYLESHEET_REFERRER_POLICY_LIMIT] = {0};
        if (!cache_revalidated
            || !fetch_response_referrer_policy(
                   result, &policy_present, response_policy)) return false;
        if (!policy_present) return true;
        const char *revalidated_url = cached->response_url_known
            ? cached->response_url : effective_url;
        return browser_session_cache_set_resource_response_provenance(
            browser, preload->url, &request_context, revalidated_url,
            response_policy);
    }
    if (preload->module
        && !script_module_mime_type_allowed(result->content_type)) {
        return false;
    }
    TilefinchResourceGrant resource_grant = {0};
    if (!preload->module
        && !fetch_resource_grant_create(
               result, &request_context, false,
               preload->script
                   ? script_module_mime_type_allowed(result->content_type)
                   : navigation_stylesheet_mime_matches(
                         result->content_type),
               false, &resource_grant, NULL)) return false;
    BrowserModuleCacheProvenance provenance = {
        .effective_url = effective_url,
        .initiator_origin = initiator_origin,
        .top_level_url = stream->document_url,
        .credentials = preload->credentials,
        .cors_validated = preload->module,
        .cors_redirect_origin_tainted =
            preload->module && result->redirect_origin_tainted,
        .javascript_mime_validated = preload->module
    };
    if (fetch_result_share_body(result)) {
        if (preload->module) {
            (void) browser_session_cache_put_http_shared_module(
                browser, preload->url, result->shared_body,
                result->etag, result->last_modified,
                result->content_type, cache_control, vary,
                tilefinch_platform_monotonic_time_ns(), &provenance);
        } else if (preload->script) {
            (void) browser_session_cache_put_http_shared_classic_script(
                browser, preload->url, result->shared_body,
                result->etag, result->last_modified,
                result->content_type, cache_control, vary,
                tilefinch_platform_monotonic_time_ns(), &request_context,
                &resource_grant);
        } else {
            bool stored = browser_session_cache_put_http_shared_resource(
                browser, preload->url, result->shared_body,
                result->etag, result->last_modified,
                result->content_type, cache_control, vary,
                tilefinch_platform_monotonic_time_ns(), &request_context,
                &resource_grant);
            if (!stored) return false;
            bool policy_present = false;
            char response_policy[STYLESHEET_REFERRER_POLICY_LIMIT] = {0};
            if (!fetch_response_referrer_policy(
                    result, &policy_present, response_policy)) return false;
            (void) policy_present;
            return browser_session_cache_set_resource_response_provenance(
                browser, preload->url, &request_context, effective_url,
                response_policy);
        }
    } else {
        if (preload->module) {
            (void) browser_session_cache_put_http_module(
                browser, preload->url,
                (const unsigned char *) result->data,
                result->length, result->etag, result->last_modified,
                result->content_type, cache_control, vary,
                tilefinch_platform_monotonic_time_ns(), &provenance);
        } else if (!preload->script) {
            return false;
        }
    }
    return true;
}

static size_t navigation_stream_active_preloads(
    const NavigationDocumentStream *stream)
{
    size_t active = 0;
    for (size_t i = 0; i < stream->preload_count; i++) {
        if (stream->preloads[i].request_id != 0
            && !stream->preloads[i].complete) active++;
    }
    return active;
}

static size_t navigation_stream_active_style_preloads(
    const NavigationDocumentStream *stream)
{
    size_t active = 0;
    for (size_t i = 0; stream != NULL && i < stream->preload_count; i++) {
        const NavigationPreload *preload = &stream->preloads[i];
        if (preload->render_blocking_style && preload->node != NULL
            && preload->request_id != 0 && !preload->complete) active++;
    }
    return active;
}

static bool navigation_stream_has_active_style_speculation(
    const NavigationDocumentStream *stream)
{
    for (size_t i = 0; stream != NULL && i < stream->preload_count; i++) {
        const NavigationPreload *preload = &stream->preloads[i];
        if (preload->render_blocking_style && preload->request_id != 0
            && !preload->complete) return true;
    }
    return false;
}

static void navigation_stream_finish_preload(
    NavigationDocumentStream *stream, NavigationPreload *preload,
    bool success, FetchResult *result)
{
    NavigationSession *session = stream->session;
    size_t failure_stage = 0;
    preload->request_id = 0;
    preload->complete = true;
    if (!success || result == NULL) {
        success = false;
        failure_stage = 1; /* Transport/scheduler failure. */
    } else if (result->status_code != 304
               && (result->status_code < 200
                   || result->status_code >= 300)) {
        success = false;
        failure_stage = 2; /* HTTP response was not usable. */
    }
    if (success && preload->module && result->status_code != 304) {
        success = script_module_mime_type_allowed(result->content_type);
        if (!success) failure_stage = 3;
    }
    if (success && !preload->script && result->status_code != 304) {
        bool header_present = false;
        char response_policy[STYLESHEET_REFERRER_POLICY_LIMIT] = {0};
        const char *response_url = result->effective_url[0] == '\0'
            ? preload->url : result->effective_url;
        if (!fetch_response_referrer_policy(
                result, &header_present, response_policy)) {
            success = false;
            failure_stage = 4;
        } else if (result->length != 0
                   && !fetch_result_share_body(result)) {
            success = false;
            failure_stage = 5;
        } else if (!stylesheet_document_resources_retain(
                       &stream->stylesheet_resources, preload->url,
                       response_url, response_policy, result->shared_body,
                       result->length, preload->cors,
                       preload->credentials)) {
            success = false;
            failure_stage = 6;
        }
        (void) header_present;
    }
    if (success) {
        success = navigation_stream_cache_preload(stream, preload, result);
        if (!success) failure_stage = 7;
    }
    if (!success) {
        if (preload->render_blocking_style) {
            session->performance.stylesheet_preload_failure_stage =
                failure_stage;
            session->performance.stylesheet_preload_failure_bytes =
                result == NULL ? 0 : result->length;
            session->performance.stylesheet_preload_failure_status =
                result == NULL ? 0 : result->status_code;
            snprintf(
                session->performance.stylesheet_preload_failure_error,
                sizeof(session->performance.stylesheet_preload_failure_error),
                "%.95s", result == NULL ? "" : result->error);
        }
        session->preloads_failed++;
        return;
    }
    TilefinchRequestContext request_context = {
        .target_url = preload->url,
        .initiator_url = stream->document_url,
        .top_level_url = stream->document_url,
        .method = "GET",
        .mode = preload->cors ? TILEFINCH_REQUEST_MODE_CORS
                              : TILEFINCH_REQUEST_MODE_NO_CORS,
        .credentials = preload->credentials,
        .destination = preload->script
            ? TILEFINCH_DESTINATION_SCRIPT
            : TILEFINCH_DESTINATION_STYLE
    };
    navigation_accept_response_cookies(
        session->browser_session, &request_context, result,
        result->effective_url[0] == '\0'
            ? preload->url : result->effective_url);
    if (preload->render_blocking_style) {
        if (result->length <=
                SIZE_MAX
                - session->performance.stylesheet_preload_completed_bytes) {
            session->performance.stylesheet_preload_completed_bytes +=
                result->length;
        } else {
            session->performance.stylesheet_preload_completed_bytes =
                SIZE_MAX;
        }
        uint64_t now_us = navigation_now_us();
        if (session->navigation_started_us != 0
            && now_us >= session->navigation_started_us) {
            session->performance.stylesheet_preload_last_completed_us =
                now_us - session->navigation_started_us;
        }
    }
    session->preloads_completed++;
}

static void navigation_stream_progress_preloads(
    NavigationDocumentStream *stream, bool wait)
{
    if (stream == NULL || stream->scheduler == NULL) return;
    NavigationSession *session = stream->session;
    for (;;) {
        FetchPumpQuota quota = {
            .maximum_body_callbacks = 2,
            .maximum_body_bytes = 32u * 1024u,
            .maximum_time_us = 2000
        };
        FetchPumpMetrics metrics = {0};
        (void) fetch_scheduler_pump_bounded(
            stream->scheduler, NAVIGATION_PRELOAD_CONCURRENCY,
            wait ? 10 : 0, &quota, &metrics);
        for (size_t i = 0; i < stream->preload_count; i++) {
            NavigationPreload *preload = &stream->preloads[i];
            if (preload->request_id == 0 || preload->complete) continue;
            FetchResult *result = fetch_result_create(session->budget);
            if (result == NULL) break;
            bool success = false;
            if (!fetch_scheduler_take(stream->scheduler,
                                      preload->request_id,
                                      &success, result)) {
                fetch_result_free(result);
                continue;
            }
            navigation_stream_finish_preload(
                stream, preload, success, result);
            fetch_result_free(result);
        }
        size_t active = navigation_stream_active_preloads(stream);
        if (active == 0 || !wait) return;
        if (navigation_now_us() < stream->preload_deadline_us) continue;
        for (size_t i = 0; i < stream->preload_count; i++) {
            NavigationPreload *preload = &stream->preloads[i];
            if (preload->request_id == 0 || preload->complete) continue;
            if (preload->render_blocking_style) {
                FetchRequestProgress progress = {0};
                if (fetch_scheduler_request_progress(
                        stream->scheduler, preload->request_id, &progress)) {
                    session->performance.stylesheet_preload_timeout_bytes +=
                        progress.received_body_bytes;
                    session->performance
                        .stylesheet_preload_timeout_requests++;
                    if (progress.status_code != 0) {
                        session->performance
                            .stylesheet_preload_timeout_responses++;
                    }
                }
            }
            (void) fetch_scheduler_cancel(stream->scheduler,
                                          preload->request_id,
                                          "preload stage deadline exceeded");
            FetchResult *result = fetch_result_create(session->budget);
            bool ignored = false;
            if (result != NULL) {
                (void) fetch_scheduler_take(stream->scheduler,
                                            preload->request_id,
                                            &ignored, result);
            }
            fetch_result_free(result);
            preload->request_id = 0;
            preload->complete = true;
            session->preloads_deferred++;
        }
        return;
    }
}

static void navigation_stream_settle_style_preloads(
    NavigationDocumentStream *stream)
{
    if (stream == NULL || stream->scheduler == NULL) return;
    while (navigation_stream_active_style_preloads(stream) != 0) {
        navigation_stream_progress_preloads(stream, false);
        if (navigation_stream_active_style_preloads(stream) == 0) return;
        if (stream->preload_deadline_us != 0
            && navigation_now_us() >= stream->preload_deadline_us) {
            for (size_t i = 0; i < stream->preload_count; i++) {
                NavigationPreload *preload = &stream->preloads[i];
                if (!preload->render_blocking_style
                    || preload->node == NULL || preload->request_id == 0
                    || preload->complete) continue;
                FetchRequestProgress progress = {0};
                if (fetch_scheduler_request_progress(
                        stream->scheduler, preload->request_id, &progress)) {
                    stream->session->performance
                        .stylesheet_preload_timeout_bytes +=
                            progress.received_body_bytes;
                    stream->session->performance
                        .stylesheet_preload_timeout_requests++;
                    if (progress.status_code != 0) {
                        stream->session->performance
                            .stylesheet_preload_timeout_responses++;
                    }
                }
                (void) fetch_scheduler_cancel(
                    stream->scheduler, preload->request_id,
                    "stylesheet speculation deadline exceeded");
                FetchResult *result =
                    fetch_result_create(stream->session->budget);
                bool ignored = false;
                if (result != NULL) {
                    (void) fetch_scheduler_take(
                        stream->scheduler, preload->request_id,
                        &ignored, result);
                }
                fetch_result_free(result);
                preload->request_id = 0;
                preload->complete = true;
                stream->session->preloads_deferred++;
            }
            return;
        }
        FetchPumpQuota quota = {
            .maximum_body_callbacks = 2,
            .maximum_body_bytes = 32u * 1024u,
            .maximum_time_us = 2000
        };
        FetchPumpMetrics metrics = {0};
        (void) fetch_scheduler_pump_bounded(
            stream->scheduler, NAVIGATION_PRELOAD_CONCURRENCY, 10,
            &quota, &metrics);
    }
}

static void navigation_stream_settle_script_preload(
    NavigationDocumentStream *stream, const char *reference)
{
    if (stream == NULL || stream->scheduler == NULL || reference == NULL
        || reference[0] == '\0') return;
    char resolved[NAVIGATION_URL_LIMIT];
    if (!fetch_resolve_url(stream->resource_base_url, reference, resolved,
                           sizeof(resolved))) return;
    NavigationPreload *target = NULL;
    for (size_t i = 0; i < stream->preload_count; i++) {
        NavigationPreload *candidate = &stream->preloads[i];
        if (candidate->script && candidate->url != NULL
            && strcmp(candidate->url, resolved) == 0) {
            target = candidate;
            break;
        }
    }
    while (target != NULL && target->request_id != 0
           && !target->complete) {
        navigation_stream_progress_preloads(stream, false);
        if (target->request_id == 0 || target->complete) return;
        if (stream->preload_deadline_us != 0
            && navigation_now_us() >= stream->preload_deadline_us) {
            (void) fetch_scheduler_cancel(
                stream->scheduler, target->request_id,
                "parser-blocking script preload deadline exceeded");
            FetchResult *result =
                fetch_result_create(stream->session->budget);
            bool ignored = false;
            if (result != NULL) {
                (void) fetch_scheduler_take(
                    stream->scheduler, target->request_id,
                    &ignored, result);
            }
            fetch_result_free(result);
            target->request_id = 0;
            target->complete = true;
            stream->session->preloads_deferred++;
            return;
        }
        FetchPumpQuota quota = {
            .maximum_body_callbacks = 2,
            .maximum_body_bytes = 32u * 1024u,
            .maximum_time_us = 2000
        };
        FetchPumpMetrics metrics = {0};
        (void) fetch_scheduler_pump_bounded(
            stream->scheduler, NAVIGATION_PRELOAD_CONCURRENCY, 10,
            &quota, &metrics);
    }
}

static void navigation_stream_settle_preloads(
    NavigationDocumentStream *stream)
{
    if (stream == NULL || stream->scheduler == NULL) return;

    /* Consume every response which became ready at EOF before cancelling the
       remaining speculation, so useful work still reaches the HTTP cache.
       The scheduler is transferred into the committed page; no preload ID
       may be forgotten while it still owns a scheduler slot. */
    navigation_stream_progress_preloads(stream, false);
    if (navigation_stream_active_preloads(stream) == 0) return;

    FetchResult result = {.budget = stream->session->budget};
    bool release_failed = false;
    for (size_t i = 0; i < stream->preload_count; i++) {
        NavigationPreload *preload = &stream->preloads[i];
        if (preload->request_id == 0 || preload->complete) continue;
        bool success = false;
        if (fetch_scheduler_take(stream->scheduler, preload->request_id,
                                 &success, &result)) {
            navigation_stream_finish_preload(
                stream, preload, success, &result);
            fetch_result_destroy(&result);
            continue;
        }
        bool cancelled = fetch_scheduler_cancel(
            stream->scheduler, preload->request_id,
            "speculative preload ended at navigation commit");
        if (!fetch_scheduler_take(stream->scheduler, preload->request_id,
                                  &success, &result)) {
            release_failed = true;
            break;
        }
        preload->request_id = 0;
        preload->complete = true;
        if (cancelled) {
            stream->session->preloads_deferred++;
        } else {
            navigation_stream_finish_preload(
                stream, preload, success, &result);
        }
        fetch_result_destroy(&result);
    }
    fetch_result_destroy(&result);

    if (!release_failed) return;
    /* Cancellation is synchronous, so this is only a defensive ownership
       fallback. Destroying the staging scheduler is safe here; candidate
       construction will create a fresh bounded scheduler when resources are
       enabled. */
    fetch_scheduler_destroy(stream->scheduler);
    stream->scheduler = NULL;
    for (size_t i = 0; i < stream->preload_count; i++) {
        NavigationPreload *preload = &stream->preloads[i];
        if (preload->request_id == 0 || preload->complete) continue;
        preload->request_id = 0;
        preload->complete = true;
        stream->session->preloads_deferred++;
    }
}

/* The largest single response any lane of the stream scheduler may reserve.
   Both the byte pool sizing and the speculative headroom gate are expressed
   in units of this value so the two cannot drift apart. */
static size_t navigation_stream_maximum_file_bytes(
    const NavigationSession *session)
{
    if (session == NULL) return 0;
    size_t maximum_file = session->maximum_script_file_bytes;
    if (session->maximum_stylesheet_file_bytes > maximum_file) {
        maximum_file = session->maximum_stylesheet_file_bytes;
    }
    if (session->maximum_image_file_bytes > maximum_file) {
        maximum_file = session->maximum_image_file_bytes;
    }
    if (session->maximum_font_file_bytes > maximum_file) {
        maximum_file = session->maximum_font_file_bytes;
    }
    return maximum_file;
}

/* Room the speculative lane must leave untouched in the byte pool: one file
   limit for the stylesheet checkpoint the parser is about to block on, and
   one for the parser-blocking script behind it. Reservations are held for the
   whole life of a request, so on a slow link preloads would otherwise still
   own the pool when those authoritative fetches are enqueued. */
static size_t navigation_stream_authoritative_reserve_bytes(
    const NavigationSession *session)
{
    size_t maximum_file = navigation_stream_maximum_file_bytes(session);
    return maximum_file <= SIZE_MAX / 2u ? maximum_file * 2u : SIZE_MAX;
}

static void navigation_stream_launch_preloads(
    NavigationDocumentStream *stream)
{
    NavigationSession *session = stream->session;
    if (stream->scheduler == NULL || session->browser_session == NULL) {
        return;
    }
    size_t authoritative_reserve =
        navigation_stream_authoritative_reserve_bytes(session);
    size_t active = navigation_stream_active_preloads(stream);
    /* Application order is preserved by the parser and ordered stylesheet
       loader, so speculative transport may prioritize render-blocking CSS.
       This prevents a large script discovered in the same HTML chunk from
       occupying the last lane while first paint waits for an already-known
       stylesheet. */
    size_t launch_order[NAVIGATION_PRELOAD_LIMIT];
    size_t launch_count = 0;
    for (unsigned priority = 0; priority < 2; priority++) {
        for (size_t i = 0; i < stream->preload_count; i++) {
            bool render_blocking_priority = priority == 0;
            if (stream->preloads[i].render_blocking_style
                    == render_blocking_priority) {
                launch_order[launch_count++] = i;
            }
        }
    }
    for (size_t order = 0;
         order < launch_count && active < NAVIGATION_PRELOAD_CONCURRENCY;
         order++) {
        size_t i = launch_order[order];
        NavigationPreload *preload = &stream->preloads[i];
        if (preload->complete || preload->launched || preload->url == NULL) {
            continue;
        }
        char initiator_origin[TILEFINCH_ORIGIN_SERIALIZED_LIMIT] = {0};
        if (preload->cors
            && !url_origin(stream->document_url, initiator_origin,
                           sizeof(initiator_origin))) {
            preload->complete = true;
            session->preloads_failed++;
            continue;
        }
        const BrowserCacheEntry *cached = NULL;
        const char *destination = preload->script ? "script" : "style";
        TilefinchRequestContext request_context = {
            .target_url = preload->url,
            .initiator_url = stream->document_url,
            .top_level_url = stream->document_url,
            .method = "GET",
            .mode = preload->cors ? TILEFINCH_REQUEST_MODE_CORS
                                  : TILEFINCH_REQUEST_MODE_NO_CORS,
            .credentials = preload->credentials,
            .destination = preload->script
                ? TILEFINCH_DESTINATION_SCRIPT : TILEFINCH_DESTINATION_STYLE
        };
        bool cache_blocked = content_blocker_would_block(
            session->browser_session->content_blocker, preload->url,
            stream->document_url, destination,
            preload->cors ? "cors" : "no-cors");
        BrowserCacheStatus status = cache_blocked
            ? BROWSER_CACHE_MISS : (preload->module
            ? browser_session_cache_match_module(
                session->browser_session, preload->url, initiator_origin,
                stream->document_url, false, preload->credentials,
                tilefinch_platform_monotonic_time_ns(),
                &cached)
            : preload->script
            ? browser_session_cache_match_classic_script(
                session->browser_session, preload->url, &request_context,
                tilefinch_platform_monotonic_time_ns(), &cached)
            : browser_session_cache_match_resource(
                session->browser_session, preload->url, &request_context,
                tilefinch_platform_monotonic_time_ns(), &cached));
        if (preload->module && cached != NULL
            && !script_module_mime_type_allowed(cached->content_type)) {
            cached = NULL;
            status = BROWSER_CACHE_MISS;
        }
        if (status == BROWSER_CACHE_FRESH && cached != NULL
            && cached->length <= preload->maximum_bytes) {
            preload->complete = true;
            session->preloads_cache_hits++;
            continue;
        }
        if (preload->maximum_bytes == 0
            || (cached != NULL
                && cached->length > preload->maximum_bytes)) {
            preload->complete = true;
            session->preloads_deferred++;
            continue;
        }
        FetchRequest transport = {
            .allow_http_errors = true,
            .accept = preload->script ? "*/*" : "text/css,*/*;q=0.1",
            .if_none_match = cached == NULL ? NULL : cached->etag,
            .if_modified_since = cached == NULL
                                  ? NULL : cached->last_modified,
            .cors_cached_response_validated = preload->module
                && cached != NULL
                && !cached->module_cors_redirect_origin_tainted
                && (cached->etag[0] != '\0'
                    || cached->last_modified[0] != '\0'),
        };
        FetchPreparedPageRequest prepared;
        if (!fetch_prepare_page_request_context(
                &request_context, stream->document_url,
                stream->referrer_policy, session->browser_session,
                &stream->parser->document.content_security_policy, NULL,
                &transport, &prepared, NULL)) continue;
        const FetchRequest *request = fetch_prepared_page_request(&prepared);
        if (request == NULL) continue;
        const long maximum_speculation_ms =
            NAVIGATION_SPECULATIVE_RESOURCE_TIMEOUT_MS;
        long preload_timeout_ms = session->resource_timeout_ms;
        if (preload_timeout_ms > maximum_speculation_ms) {
            preload_timeout_ms = maximum_speculation_ms;
        }
        if (!fetch_scheduler_reservation_available(
                stream->scheduler, preload->maximum_bytes,
                authoritative_reserve)) {
            /* Not a failure: the preload stays queued and unlaunched, and
               navigation_stream_progress_preloads calls back here once a
               completion releases enough of the pool. Stopping the whole
               sweep keeps a smaller preload from slipping into room the
               parser's own fetches are about to need. */
            session->preloads_headroom_skipped++;
            return;
        }
        preload->request_id = fetch_scheduler_enqueue(
            stream->scheduler, preload->url, request,
            preload->maximum_bytes, preload_timeout_ms);
        preload->launched = true;
        if (preload->request_id == 0) {
            preload->complete = true;
            session->preloads_deferred++;
            continue;
        }
        if (stream->preload_deadline_us == 0) {
            /* Speculation must never inherit the full resource timeout.
               A slow speculative request otherwise turns the parser's first
               stylesheet checkpoint into one irreducible wait (15 seconds
               with the normal frontend configuration).  The three-second
               window still lets a useful render-blocking response overlap
               the document transfer on PSP-class links; after that the
               ordered resource loader owns the request and its normal
               retry/error policy. */
            uint64_t duration_ms =
                (uint64_t) preload_timeout_ms;
            uint64_t duration = duration_ms * UINT64_C(1000);
            uint64_t now = navigation_now_us();
            stream->preload_deadline_us = duration > UINT64_MAX - now
                ? UINT64_MAX : now + duration;
        }
        session->preloads_launched++;
        active++;
    }
}

#include "stylesheet_checkpoint.inc"


static void navigation_stream_destroy_staging(
    NavigationDocumentStream *stream)
{
    if (stream == NULL) return;
    if (stream->runtime != NULL) {
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
    budget_free(stream->session == NULL ? NULL : stream->session->budget,
                stream->stylesheet_inputs);
    stream->stylesheet_inputs = NULL;
    stream->stylesheet_input_count = 0;
    stylesheet_document_resources_destroy(&stream->stylesheet_resources);
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
           memory: on the PSP profile max_file is 384 KiB, so the pool states
           a 2.25 MiB ceiling instead of 1.5 MiB. */
        const size_t preload_lanes = (size_t) NAVIGATION_PRELOAD_CONCURRENCY;
        const size_t authoritative_lanes = 4u;
        const size_t lanes = preload_lanes + authoritative_lanes;
        size_t reserved = maximum_file <= SIZE_MAX / lanes
            ? maximum_file * lanes : maximum_file;
        /* An SPA can hold several concurrent async chunk fetches while a
           synchronous module-loader fetch also needs a slot; capacity must
           cover the async bridge's native limit plus that headroom. */
        stream->scheduler = fetch_scheduler_create_in_domain(
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
        stream->referrer_policy, true, &stream->viewport,
        SCRIPT_DOCUMENT_SCOPE_TOP_LEVEL, false, stream->fetch_domain,
        NULL,
        &stream->script_result);
    if (stream->runtime != NULL) {
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

#include "parser_script_checkpoint.inc"



/* Candidate adoption is a distinct transactional boundary. */
#include "commit_transaction.inc"

#include "progressive_preview.inc"


static bool navigation_commit_html_policy(
    NavigationSession *session, uint64_t generation,
    const char *url, const char *html, size_t html_length,
    int viewport_width, const FontSet *fonts,
    const ImageResources *images, bool record_history,
    bool allow_runtime)
{
    if (session == NULL || session->budget == NULL || generation == 0
        || generation != session->generation || session->cancelled
        || url == NULL || html == NULL || viewport_width <= 0) return false;
    PocDocument document = {0};
    uint64_t phase_started = navigation_now_us();
    if (!document_parse(&document, session->budget, html, html_length, 512)) {
        snprintf(session->last_error, sizeof(session->last_error),
                 "HTML parsing failed within the page budget");
        return false;
    }
    char document_referrer_policy[
        sizeof(session->pending_response_referrer_policy)];
    snprintf(document_referrer_policy, sizeof(document_referrer_policy), "%s",
             session->pending_response_referrer_policy);
    if (!navigation_scan_document_referrer_policy(
            &document, document_referrer_policy)) {
        snprintf(session->last_error, sizeof(session->last_error),
                 "document referrer metadata traversal failed");
        document_destroy(&document);
        return false;
    }
    snprintf(session->pending_response_referrer_policy,
             sizeof(session->pending_response_referrer_policy), "%s",
             document_referrer_policy);
    session->performance.parse_us += navigation_record_slice(
        session, NAVIGATION_SLICE_PARSER, phase_started, html_length);
    bool committed = navigation_commit_document(
        session, generation, url, &document, viewport_width, fonts, images,
        record_history, NULL, NULL, allow_runtime, false, NULL, NULL);
    document_destroy(&document);
    return committed;
}

bool navigation_commit_html(NavigationSession *session, uint64_t generation,
                            const char *url, const char *html,
                            size_t html_length, int viewport_width,
                            const FontSet *fonts,
                            const ImageResources *images,
                            bool record_history)
{
    return navigation_commit_html_policy(
        session, generation, url, html, html_length, viewport_width,
        fonts, images, record_history, true);
}

bool navigation_commit_static_html(
    NavigationSession *session, uint64_t generation,
    const char *url, const char *html, size_t html_length,
    int viewport_width, const FontSet *fonts,
    const ImageResources *images, bool record_history)
{
    return navigation_commit_html_policy(
        session, generation, url, html, html_length, viewport_width,
        fonts, images, record_history, false);
}

bool navigation_commit_html_preserve_runtime(
    NavigationSession *session, uint64_t generation, const char *url,
    const char *html, size_t html_length, int viewport_width,
    const FontSet *fonts, const ImageResources *images, bool record_history)
{
    if (session == NULL || session->page.runtime == NULL) {
        return navigation_commit_html(
            session, generation, url, html, html_length, viewport_width,
            fonts, images, record_history);
    }
    if (generation == 0 || generation != session->generation
        || session->cancelled || url == NULL || html == NULL
        || viewport_width <= 0) return false;
    char retained_referrer_policy[sizeof(session->page.referrer_policy)];
    snprintf(retained_referrer_policy, sizeof(retained_referrer_policy), "%s",
             session->page.referrer_policy);
    NavigationDocumentStream *transfer = budget_calloc(
        session->budget, 1, sizeof(*transfer));
    if (transfer == NULL) {
        snprintf(session->last_error, sizeof(session->last_error),
                 "navigation swap scratch allocation failed");
        return false;
    }
    transfer->session = session;
    transfer->runtime = session->page.runtime;
    transfer->fetch_domain = session->page.fetch_domain;
    transfer->script_result = session->page.script_result;
    transfer->runtime_document_replacement = true;
    /* Section replacement is intentionally destructive: keeping the old DOM,
       layout, and resource graph alive while parsing its neighbor can double
       transient memory on the PSP. Preserve only the origin realm and make
       its old node handles inert when the replacement document is rebound. */
    script_runtime_detach_document(
        transfer->runtime, &session->page.document);
    session->page.runtime = NULL;
    session->page.fetch_domain = NULL;
    uint64_t teardown_started = navigation_now_us();
    page_destroy(session);
    session->performance.last_swap_teardown_us =
        navigation_now_us() - teardown_started;
    script_runtime_bound_allocator_cache(transfer->runtime, 16, 4);
    size_t parse_working = html_length > SIZE_MAX / 6u
        ? SIZE_MAX : html_length * 6u;
    bool collect_before = budget_pressure_required(
            session->budget, parse_working, 2u * 1024u * 1024u)
        || session->budget->categories[BUDGET_CATEGORY_JAVASCRIPT].current
             > session->js_memory_limit * 3u / 4u
        || session->performance.swaps_since_gc >= 7;
    session->performance.last_swap_gc_us = 0;
    if (collect_before) {
        uint64_t gc_started = navigation_now_us();
        session->performance.swap_gc_trimmed_bytes +=
            script_runtime_collect_and_trim(transfer->runtime);
        session->performance.last_swap_gc_us +=
            navigation_now_us() - gc_started;
        session->performance.swap_gc_runs++;
        session->performance.swaps_since_gc = 0;
    } else {
        session->performance.swap_gc_skips++;
        session->performance.swaps_since_gc++;
    }
    PocDocument document = {0};
    uint64_t phase_started = navigation_now_us();
    if (!document_parse(&document, session->budget, html, html_length, 512)) {
        snprintf(session->last_error, sizeof(session->last_error),
                 "HTML parsing failed within the page budget");
        navigation_stream_release(transfer);
        return false;
    }
    if (!navigation_scan_document_referrer_policy(
            &document, retained_referrer_policy)) {
        snprintf(session->last_error, sizeof(session->last_error),
                 "replacement referrer metadata traversal failed");
        document_destroy(&document);
        navigation_stream_release(transfer);
        return false;
    }
    snprintf(session->pending_response_referrer_policy,
             sizeof(session->pending_response_referrer_policy), "%s",
             retained_referrer_policy);
    snprintf(transfer->referrer_policy, sizeof(transfer->referrer_policy), "%s",
             retained_referrer_policy);
    session->performance.parse_us += navigation_record_slice(
        session, NAVIGATION_SLICE_PARSER, phase_started, html_length);
    session->performance.last_swap_parse_us =
        navigation_now_us() - phase_started;
    bool document_scripts_enabled = session->document_scripts_enabled;
    session->document_scripts_enabled = false;
    uint64_t commit_started = navigation_now_us();
    bool committed = navigation_commit_document(
        session, generation, url, &document, viewport_width, fonts, images,
        record_history, transfer, NULL, true, false, NULL, NULL);
    session->performance.last_swap_commit_us =
        navigation_now_us() - commit_started;
    session->document_scripts_enabled = document_scripts_enabled;
    if (committed
        && (budget_remaining(session->budget) < 2u * 1024u * 1024u
            || session->budget->categories[BUDGET_CATEGORY_JAVASCRIPT].current
                 > session->js_memory_limit * 9u / 10u)) {
        uint64_t gc_started = navigation_now_us();
        session->performance.swap_gc_trimmed_bytes +=
            script_runtime_collect_and_trim(session->page.runtime);
        session->performance.last_swap_gc_us +=
            navigation_now_us() - gc_started;
        session->performance.swap_gc_runs++;
        session->performance.swaps_since_gc = 0;
    }
    document_destroy(&document);
    navigation_stream_release(transfer);
    return committed;
}
