static void navigation_stream_mark_parser_refusal(
    NavigationDocumentStream *stream, NavigationParserRefusalReason reason);

static void navigation_stream_drop_previews(
    NavigationDocumentStream *stream);

/* Drops an unfinished stylesheet preparation (style_prepare.inc). Its
   deferred parse borrows the blocking sheet, so this precedes any release
   of that sheet; the partly built sheet itself is the caller's to drop. */
static void navigation_stream_style_prepare_cancel(
    NavigationDocumentStream *stream)
{
    NavigationStylePrepare *prepare = &stream->style_prepare;
    if (prepare->phase == NAVIGATION_STYLE_PREPARE_IDLE
        && prepare->inputs == NULL && prepare->deferral.parse == NULL)
        return;
    stylesheet_parse_deferral_release(&prepare->deferral);
    if (stream->session != NULL)
        budget_free(stream->session->budget, prepare->inputs);
    prepare->inputs = NULL;
    prepare->input_count = 0;
    prepare->next_input = 0;
    prepare->phase = NAVIGATION_STYLE_PREPARE_IDLE;
}

static void navigation_stream_release_blocking_stylesheet(
    NavigationDocumentStream *stream)
{
    if (stream == NULL) return;
    navigation_stream_style_prepare_cancel(stream);
    stream->style_prepare.fresh = false;
    /* Retained previews borrow the sheet's web fonts (and, through stream
       teardown, which releases the sheet first, the DOM). */
    navigation_stream_drop_previews(stream);
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

/* The loader-visible attributes of a <link rel=stylesheet>
   (stylesheet_inputs.h) and its nonce. `resolved_href`, when given, stands
   for the href. */
static uint64_t navigation_stylesheet_link_hash(
    uint64_t hash, lxb_dom_node_t *node, const char *resolved_href)
{
    static const char *const attributes[] = {
#define NAVIGATION_STYLESHEET_INPUT_NAME(name) name,
        STYLESHEET_LINK_INPUTS(NAVIGATION_STYLESHEET_INPUT_NAME)
#undef NAVIGATION_STYLESHEET_INPUT_NAME
    };
    for (size_t i = 0; i < sizeof(attributes) / sizeof(attributes[0]); i++) {
        size_t length = 0;
        const char *value = document_attribute(node, attributes[i], &length);
        if (resolved_href != NULL && value != NULL
            && strcmp(attributes[i], STYLESHEET_INPUT_HREF) == 0) {
            value = resolved_href;
            length = strlen(resolved_href);
        }
        hash = navigation_stylesheet_fingerprint_field(
            hash, attributes[i], value, length, value != NULL);
    }
    size_t nonce_length = 0;
    const char *nonce = document_element_nonce(node, &nonce_length);
    return navigation_stylesheet_fingerprint_field(
        hash, "nonce", nonce, nonce_length, nonce != NULL);
}

/* The root element's loader-visible attributes (its theme selection). */
static uint64_t navigation_stylesheet_root_hash(
    uint64_t hash, const PocDocument *document)
{
    lxb_dom_element_t *element = lxb_dom_document_element(
        &document->html->dom_document);
    lxb_dom_node_t *root = element == NULL
        ? NULL : lxb_dom_interface_node(element);
    static const char *const attributes[] = {
#define NAVIGATION_STYLESHEET_INPUT_NAME(name) name,
        STYLESHEET_ROOT_INPUTS(NAVIGATION_STYLESHEET_INPUT_NAME)
#undef NAVIGATION_STYLESHEET_INPUT_NAME
    };
    for (size_t i = 0; i < sizeof(attributes) / sizeof(attributes[0]); i++) {
        size_t length = 0;
        const char *value = root == NULL
            ? NULL : document_attribute(root, attributes[i], &length);
        hash = navigation_stylesheet_fingerprint_field(
            hash, attributes[i], value, length, value != NULL);
    }
    return hash;
}

/* One walk over the document's stylesheet sources, <style> and
   <link rel=stylesheet>, in document order (the order the loader takes
   them). The streamed fingerprint, the head key, the static key and the
   continuation's input capture all fold this walk with the folds below, so
   they cannot disagree on what a source is or what of it counts. */
typedef struct {
    lxb_dom_node_t *next;
    lxb_dom_node_t *boundary_parent;
    size_t visited;
} NavigationStylesheetWalk;

typedef struct {
    lxb_dom_node_t *node;
    const char *name;
    size_t name_length;
    bool style; /* otherwise a <link rel=stylesheet> */
} NavigationStylesheetSource;

typedef enum {
    NAVIGATION_STYLESHEET_WALK_END,
    NAVIGATION_STYLESHEET_WALK_SOURCE,
    /* NAVIGATION_DOM_TRAVERSAL_NODE_LIMIT nodes visited. */
    NAVIGATION_STYLESHEET_WALK_LIMIT
} NavigationStylesheetWalkStep;

static void navigation_stylesheet_walk_begin(
    NavigationStylesheetWalk *walk, const PocDocument *document)
{
    walk->next = lxb_dom_interface_node(document->html);
    walk->boundary_parent = walk->next->parent;
    walk->visited = 0;
}

static NavigationStylesheetWalkStep navigation_stylesheet_walk_next(
    NavigationStylesheetWalk *walk, NavigationStylesheetSource *source)
{
    while (walk->next != NULL && walk->next != walk->boundary_parent) {
        if (++walk->visited > NAVIGATION_DOM_TRAVERSAL_NODE_LIMIT)
            return NAVIGATION_STYLESHEET_WALK_LIMIT;
        lxb_dom_node_t *node = walk->next;
        if (node->first_child != NULL) {
            walk->next = node->first_child;
        } else {
            lxb_dom_node_t *up = node;
            while (up != NULL && up != walk->boundary_parent
                   && up->next == NULL) up = up->parent;
            walk->next = up != NULL && up != walk->boundary_parent
                ? up->next : NULL;
        }
        size_t name_length = 0;
        const char *name = document_element_name(node, &name_length);
        bool style = name != NULL && name_length == 5
            && memcmp(name, "style", 5) == 0;
        bool link = name != NULL && name_length == 4
            && memcmp(name, "link", 4) == 0;
        if (link) {
            size_t rel_length = 0;
            const char *rel = document_attribute(
                node, STYLESHEET_INPUT_REL, &rel_length);
            link = rel != NULL
                && navigation_token_contains(rel, rel_length, "stylesheet");
        }
        if (style || link) {
            *source = (NavigationStylesheetSource) {
                .node = node, .name = name, .name_length = name_length,
                .style = style
            };
            return NAVIGATION_STYLESHEET_WALK_SOURCE;
        }
    }
    return NAVIGATION_STYLESHEET_WALK_END;
}

/* A <style>'s nonce and text (the CSSOM's text once script replaced it). */
static uint64_t navigation_stylesheet_style_hash(
    uint64_t hash, lxb_dom_node_t *node)
{
    size_t nonce_length = 0;
    const char *nonce = document_element_nonce(node, &nonce_length);
    hash = navigation_stylesheet_fingerprint_field(
        hash, "nonce", nonce, nonce_length, nonce != NULL);
    size_t override_length = 0;
    const char *override = document_cssom_sheet_text(node, &override_length);
    if (override != NULL)
        return navigation_stylesheet_fingerprint_field(
            hash, "cssom", override, override_length, true);
    for (lxb_dom_node_t *child = node->first_child; child != NULL;
         child = child->next) {
        size_t length = 0;
        const char *text = document_text_data(child, &length);
        hash = navigation_stylesheet_fingerprint_field(
            hash, "css", text, length, text != NULL);
    }
    return hash;
}

/* A source as every key sees it: its element, then its text or its
   loader-visible attributes. `resolved_href` as for
   navigation_stylesheet_link_hash. */
static uint64_t navigation_stylesheet_source_hash(
    uint64_t hash, const NavigationStylesheetSource *source,
    const char *resolved_href)
{
    hash = navigation_stylesheet_fingerprint_field(
        hash, "element", source->name, source->name_length, true);
    return source->style
        ? navigation_stylesheet_style_hash(hash, source->node)
        : navigation_stylesheet_link_hash(hash, source->node, resolved_href);
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
    /* What a link applies depends on the root's theme selection too. */
    const uint64_t root_inputs = navigation_stylesheet_root_hash(
        UINT64_C(1469598103934665603), document);
    NavigationStylesheetWalk walk;
    NavigationStylesheetSource source;
    NavigationStylesheetWalkStep step;
    navigation_stylesheet_walk_begin(&walk, document);
    while ((step = navigation_stylesheet_walk_next(&walk, &source))
           == NAVIGATION_STYLESHEET_WALK_SOURCE) {
        /* The raw preload scanner discovers links before Lexbor creates
           their nodes. A provisional stylesheet checkpoint can precede the
           broader metadata walk, so bind the already-walked node here as
           well. Otherwise settle ignores the speculative request and the
           authoritative loader starts a duplicate fetch. */
        if (!source.style)
            navigation_stream_confirm_stylesheet_preload(stream, source.node);
        if (*count == NAVIGATION_STYLESHEET_INPUT_LIMIT) {
            *overflow = true;
            continue;
        }
        if (*count == capacity) {
            size_t next = capacity == 0 ? 8u : capacity * 2u;
            if (next > NAVIGATION_STYLESHEET_INPUT_LIMIT) {
                next = NAVIGATION_STYLESHEET_INPUT_LIMIT;
            }
            NavigationStylesheetInput *grown = budget_realloc(
                stream->session->budget, collected, next * sizeof(*grown));
            if (grown == NULL) {
                budget_free(stream->session->budget, collected);
                return false;
            }
            collected = grown;
            capacity = next;
        }
        uint64_t hash = navigation_stylesheet_source_hash(
            UINT64_C(1469598103934665603), &source, NULL);
        if (!source.style)
            hash = fingerprint_bytes(hash, (const char *) &root_inputs,
                                     sizeof(root_inputs));
        collected[*count] = (NavigationStylesheetInput) {
            .node = source.node, .hash = hash, .style = source.style
        };
        (*count)++;
    }
    if (step == NAVIGATION_STYLESHEET_WALK_LIMIT) {
        budget_free(stream->session->budget, collected);
        return false;
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

/* The session's user stylesheet, as a blocking sheet built or taken now
   carries it. */
static uint64_t navigation_stylesheet_user_css_hash(
    const NavigationSession *session)
{
    return navigation_stylesheet_fingerprint_field(
        UINT64_C(1469598103934665603), "user-css", session->user_css,
        session->user_css_length, session->user_css_length != 0);
}

/* Appends the stylesheet inputs that follow an unchanged, already compiled
   prefix. `document` supplies both the inputs and the Content-Security-Policy
   that admits them: at commit the parser's document has already been moved
   into the page, so the stream's own copy is empty. `require_inline` limits
   the suffix to <style> elements, whose append needs no stream fetch state. */
static bool navigation_stream_continue_stylesheet_suffix(
    NavigationDocumentStream *stream, PocDocument *document,
    uint64_t fingerprint, bool require_inline)
{
#ifndef TILEFINCH_NO_TRACE
    if (getenv("TILEFINCH_DISABLE_STYLESHEET_CONTINUATION") != NULL) {
        return false;
    }
#endif
    if (stream == NULL || stream->session == NULL || document == NULL
        || !stream->blocking_stylesheet_ready
        || stream->blocking_stylesheet.user_css_changes_context
        || stream->stylesheet_input_overflow) return false;
    /* A suffix appends at the end of the sheet, but adopted sheets must
       stay after every document source: rebuild instead. */
    if (stream->blocking_stylesheet.scopes.adopted_source_count != 0
        || document_adopted_sheets_signature(document) != 0) return false;
    /* Media queries are evaluated at parse against the sheet's viewport. A
       later metadata scan (a <meta name=viewport> after the first blocking
       stylesheet, or after a parser-blocking script) can move the layout
       viewport; the compiled prefix then answers the wrong width and no
       suffix append can repair it.  Rebuild instead of continuing. */
    if (stream->blocking_stylesheet.viewport_width
            != stream->viewport.css_width
        || stream->blocking_stylesheet.viewport_height
            != stream->viewport.css_height) {
        stream->session->performance
            .blocking_stylesheet_viewport_rebuilds++;
        return false;
    }
    /* Likewise for prefers-color-scheme: night mode changed after the
       prefix compiled. */
    if (stream->blocking_stylesheet.prefers_dark
        != stylesheet_prefers_dark_color_scheme()) return false;
    /* And for the user stylesheet: one set after the prefix compiled (the
       PSP's presentation sheet arriving while the page loads) is not in
       it. */
    if (stream->blocking_stylesheet_user_css
        != navigation_stylesheet_user_css_hash(stream->session))
        return false;
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
    if (require_inline && !inline_only) {
        budget_free(stream->session->budget, current);
        return false;
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
            &document->content_security_policy)
        : stylesheets_append_ordered_suffix_with_context(
            &stream->blocking_stylesheet, session->budget,
            suffix, suffix_count, stream->resource_base_url,
            stream->document_url, stream->referrer_policy,
            &document->content_security_policy,
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

/* The parse context every compiled sheet depends on beyond its sources:
   referrer policy, viewport, the prefers-color-scheme answer (night mode;
   media queries are resolved at compile), resource switches and bounds,
   user CSS, the Content-Security-Policy, adopted sheets and registered
   properties. */
static bool navigation_stylesheet_context_hash(
    const NavigationDocumentStream *stream, const PocDocument *document,
    uint64_t *io_hash)
{
    const NavigationSession *session = stream->session;
    uint64_t hash = *io_hash;
    hash = navigation_stylesheet_fingerprint_field(
        hash, "referrer-policy", stream->referrer_policy,
        strlen(stream->referrer_policy), true);
    hash = fingerprint_bytes(
        hash, (const char *) &stream->viewport.css_width,
        sizeof(stream->viewport.css_width));
    hash = fingerprint_bytes(
        hash, (const char *) &stream->viewport.css_height,
        sizeof(stream->viewport.css_height));
    const bool prefers_dark = stylesheet_prefers_dark_color_scheme();
    hash = fingerprint_bytes(
        hash, (const char *) &prefers_dark, sizeof(prefers_dark));
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
    /* Adopted constructed sheets are sources outside the tree. */
    uint64_t adopted = document_adopted_sheets_signature(document);
    hash = fingerprint_bytes(hash, (const char *) &adopted, sizeof(adopted));
    hash = fingerprint_bytes(hash,
        (const char *) &document->registered_property_count,
        sizeof(document->registered_property_count));
    *io_hash = hash;
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
    if (!navigation_stylesheet_context_hash(stream, document, &hash))
        return false;
    hash = navigation_stylesheet_root_hash(hash, document);
    NavigationStylesheetWalk walk;
    NavigationStylesheetSource source;
    NavigationStylesheetWalkStep step;
    navigation_stylesheet_walk_begin(&walk, document);
    while ((step = navigation_stylesheet_walk_next(&walk, &source))
           == NAVIGATION_STYLESHEET_WALK_SOURCE)
        hash = navigation_stylesheet_source_hash(hash, &source, NULL);
    if (step == NAVIGATION_STYLESHEET_WALK_LIMIT) return false;
    *fingerprint = hash;
    return true;
}

/* Whether a stylesheet input is inside <head>, as stylesheet_note_style_source
   classifies the source of a head mark. */
static bool navigation_stylesheet_input_in_head(const lxb_dom_node_t *node)
{
    for (const lxb_dom_node_t *parent = node->parent; parent != NULL;
         parent = parent->parent) {
        if (parent->type != LXB_DOM_NODE_TYPE_ELEMENT) return false;
        if (parent->local_name == LXB_TAG_HEAD) return true;
        if (parent->local_name == LXB_TAG_BODY
            || parent->local_name == LXB_TAG_HTML) return false;
    }
    return false;
}

/* The key of the sheet compiled from the document's <head> sources alone
   (the part stylesheet_truncate_to_head keeps): the fingerprint's parse
   context and <head> inputs, hashed as the fingerprint hashes them, except
   that a link's href is resolved and the base URL counts only when a
   <head> <style> is present, so the pages of one site that share a <head>
   share the key. The document's origin and the root's theme selection
   (alternate theme links) count too. Also returns how many inputs follow
   <head> and whether they are all <style> elements. False when the <head>
   inputs are not a prefix of the document's inputs, when there are more
   inputs than the continuation tracks, or when an href cannot be
   resolved. */
static bool navigation_stream_stylesheet_head_key(
    NavigationDocumentStream *stream, const PocDocument *document,
    uint64_t *key, size_t *head_count, size_t *body_count,
    bool *body_inline)
{
    if (stream == NULL || stream->session == NULL || document == NULL
        || document->html == NULL || key == NULL || head_count == NULL
        || body_count == NULL || body_inline == NULL) return false;
    const NavigationSession *session = stream->session;
    if (session->user_css_length != 0 && session->user_css == NULL)
        return false;
    *head_count = 0;
    *body_count = 0;
    *body_inline = true;
    bool head_style = false;
    uint64_t inputs = UINT64_C(1469598103934665603);
    inputs = navigation_stylesheet_root_hash(inputs, document);
    NavigationStylesheetWalk walk;
    NavigationStylesheetSource source;
    NavigationStylesheetWalkStep step;
    navigation_stylesheet_walk_begin(&walk, document);
    while ((step = navigation_stylesheet_walk_next(&walk, &source))
           == NAVIGATION_STYLESHEET_WALK_SOURCE) {
        if (*head_count + *body_count == NAVIGATION_STYLESHEET_INPUT_LIMIT)
            return false;
        if (!navigation_stylesheet_input_in_head(source.node)) {
            (*body_count)++;
            if (!source.style) *body_inline = false;
            continue;
        }
        if (*body_count != 0) return false;
        (*head_count)++;
        if (source.style) head_style = true;
        size_t length = 0;
        const char *href = source.style ? NULL : document_attribute(
            source.node, STYLESHEET_INPUT_HREF, &length);
        char resolved[NAVIGATION_URL_LIMIT * 2u];
        if (href != NULL) {
            char reference[NAVIGATION_URL_LIMIT];
            if (length >= sizeof(reference)) return false;
            memcpy(reference, href, length);
            reference[length] = '\0';
            if (!fetch_resolve_url(stream->resource_base_url, reference,
                                   resolved, sizeof(resolved)))
                return false;
        }
        inputs = navigation_stylesheet_source_hash(
            inputs, &source, href != NULL ? resolved : NULL);
    }
    if (step == NAVIGATION_STYLESHEET_WALK_LIMIT) return false;
    char origin[NAVIGATION_URL_LIMIT];
    if (!tilefinch_url_origin(stream->document_url, origin, sizeof(origin)))
        return false;
    uint64_t hash = UINT64_C(1469598103934665603);
    hash = navigation_stylesheet_fingerprint_field(
        hash, "head-key", NULL, 0, false);
    hash = navigation_stylesheet_fingerprint_field(
        hash, "base-url", stream->resource_base_url,
        strlen(stream->resource_base_url), head_style);
    hash = navigation_stylesheet_fingerprint_field(
        hash, "origin", origin, strlen(origin), true);
    if (!navigation_stylesheet_context_hash(stream, document, &hash))
        return false;
    *key = fingerprint_bytes(hash, (const char *) &inputs, sizeof(inputs));
    return true;
}

static bool navigation_stream_try_continue_stylesheet(
    NavigationDocumentStream *stream, PocDocument *document,
    uint64_t fingerprint)
{
    return navigation_stream_continue_stylesheet_suffix(
        stream, document, fingerprint, false);
}

static bool navigation_stream_build_blocking_stylesheet_with_timeout(
    NavigationDocumentStream *stream, PocDocument *document,
    uint64_t fingerprint, long timeout_ms)
{
    if (stream == NULL || stream->session == NULL || document == NULL) {
        return false;
    }
    NavigationSession *session = stream->session;
    tilefinch_platform_trace_step("style-release");
    navigation_stream_release_blocking_stylesheet(stream);
    /* Speculative fetches never supply CSS directly. Settle only the style
       entries into the ordinary HTTP cache, then let the ordered stylesheet
       loader perform its normal provenance, media, parse, and cascade work. */
    tilefinch_platform_trace_step("style-preload-settle");
    if (session->external_resources_enabled) {
        navigation_stream_settle_style_preloads(stream);
    }
    tilefinch_platform_trace_step("style-compiled-cache");
    if (navigation_compiled_stylesheet_cache_take(
            session, fingerprint, &stream->blocking_stylesheet,
            &stream->blocking_stylesheets)) {
        stream->blocking_stylesheet_ready = true;
        stream->blocking_stylesheet_user_css =
            navigation_stylesheet_user_css_hash(session);
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
    tilefinch_platform_trace_step("style-base-build");
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
    if (session->user_css_length != 0) {
        if (!stylesheet_add_user_css(
                &stream->blocking_stylesheet, session->user_css,
                session->user_css_length)) {
            navigation_stream_release_blocking_stylesheet(stream);
            return false;
        }
    }
    stream->blocking_stylesheet_user_css =
        navigation_stylesheet_user_css_hash(session);
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
            stream, document, &fingerprint)) {
        navigation_stream_mark_parser_refusal(
            stream, NAVIGATION_PARSER_REFUSAL_CSSOM);
        return false;
    }
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
        if (!refreshed) {
            navigation_stream_mark_parser_refusal(
                stream, NAVIGATION_PARSER_REFUSAL_CSSOM);
            return false;
        }
    }

    script_runtime_set_stylesheet(
        stream->runtime, &stream->blocking_stylesheet);
    /* A parser-blocking head script can read computed style or geometry
       before the parser creates a body. There is no body layout to build
       yet, not a refused allocation or exhausted work bound. Publish the
       current cascade and retain the geometry fallback; a later read after
       body insertion still takes the authoritative layout path below. */
    if (document_body_node(document) == NULL) return true;

    LayoutDocument fresh = {0};
    if (!layout_build_context(
            &fresh, session->budget, document,
            &stream->blocking_stylesheet, stream->fonts, stream->images,
            &stream->viewport)) {
        layout_destroy(&fresh);
        navigation_stream_mark_parser_refusal(
            stream, NAVIGATION_PARSER_REFUSAL_CSSOM);
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
