/* The mutation journal: how a script DOM mutation is classified (resource
   and SVG-raster effects, :has() reach, form-owner moves), recorded for
   layout and style invalidation, and summarized when the journal
   overflows. bridge_mutated*() are the entry points the DOM natives call.
   Included by js_dom_bindings.c, in its translation unit, so those static
   entry points stay inlinable into their callers. */

static bool bridge_mutation_name_equal(const char *name, size_t length,
                                       const char *wanted)
{
    size_t wanted_length = strlen(wanted);
    if (name == NULL || length != wanted_length) return false;
    for (size_t i = 0; i < length; i++) {
        if (tolower((unsigned char) name[i])
            != tolower((unsigned char) wanted[i])) return false;
    }
    return true;
}

static bool bridge_mutation_node_name_is(lxb_dom_node_t *node,
                                         const char *wanted)
{
    size_t length = 0;
    const char *name = document_element_name(node, &length);
    return bridge_mutation_name_equal(name, length, wanted);
}

static bool bridge_mutation_inside_svg(lxb_dom_node_t *node)
{
    size_t depth = 0;
    for (lxb_dom_node_t *at = node; at != NULL && depth++ < 64;
         at = at->parent) {
        if (bridge_mutation_node_name_is(at, "svg")) return true;
    }
    return false;
}

static bool bridge_mutation_subtree_contains_image(
    const DomBridge *bridge, lxb_dom_node_t *root)
{
    enum { MAXIMUM_WORK = 256, MAXIMUM_DEPTH = 48 };
    if (root == NULL) return false;
    lxb_dom_node_t *node = root;
    size_t work = 0, depth = 0;
    for (;;) {
        /* Bounded-out is resource-sensitive: a conservative refresh is
           preferable to retaining a stale raster below a large subtree. */
        if (++work > MAXIMUM_WORK) return true;
        if (bridge_mutation_node_name_is(node, "svg")
            || (bridge_mutation_node_name_is(node, "img")
                && bridge->images != NULL
                && images_find_node(bridge->images, node) == NULL
                /* A formerly visible missing image is not a reveal. Its
                   explicit src mutation/retry owns discovery. Internal:
                   script observes none of it, so a truncated (bounded
                   focus) layout's box is evidence too. */
                && layout_box_for_node(bridge->layout, node) == NULL))
            return true;
        if (node->first_child != NULL && depth < MAXIMUM_DEPTH) {
            node = node->first_child;
            depth++;
            continue;
        }
        while (node != root && node->next == NULL) {
            node = node->parent;
            if (depth != 0) depth--;
        }
        if (node == root) break;
        node = node->next;
    }
    return false;
}

/* Only a document holding inline SVG rasters has a presentation-only raster
   to go stale; without one, colour and custom-property churn refreshes
   nothing and must not pay for an image refresh. */
static bool bridge_mutation_has_inline_svg_rasters(const DomBridge *bridge)
{
    return bridge->images != NULL
        && (bridge->images->stats.inline_svg_rasterized != 0
            || bridge->images->stats.inline_svg_refresh_reused != 0);
}

/* Inline-style properties that can change what an SVG beneath resolves for
   its raster (currentColor, em sizes, inherited presentation values, a
   custom property one of those reads). */
static bool bridge_mutation_style_property_affects_svg_raster(
    const char *name, size_t length)
{
    if (name == NULL) return false;
    if (length > 2 && name[0] == '-' && name[1] == '-') return true;
    return bridge_mutation_name_equal(name, length, "color")
        || bridge_mutation_name_equal(name, length, "font")
        || bridge_mutation_name_equal(name, length, "font-size")
        || (length >= 4 && strncasecmp(name, "fill", 4) == 0)
        || (length >= 6 && strncasecmp(name, "stroke", 6) == 0);
}

static bool bridge_mutation_inside_style(lxb_dom_node_t *node)
{
    for (lxb_dom_node_t *at = node; at != NULL; at = at->parent) {
        if (bridge_mutation_node_name_is(at, "style")) return true;
    }
    return false;
}

typedef enum {
    BRIDGE_MUTATION_RESOURCE_NONE = 0,
    BRIDGE_MUTATION_RESOURCE_IMAGE = 1u << 0,
    BRIDGE_MUTATION_RESOURCE_STYLESHEET = 1u << 1,
    BRIDGE_MUTATION_RESOURCE_BOUNDED_OUT = 1u << 2,
    /* Not a fetched resource: a <meta http-equiv=refresh>, which navigation
       must consider for the document's declarative refresh. */
    BRIDGE_MUTATION_RESOURCE_REFRESH_META = 1u << 3
} BridgeMutationResourceFlags;

/* An HTML <meta> whose http-equiv is "refresh" (ASCII case-insensitive,
   untrimmed, as the enumerated attribute is matched). */
static bool bridge_mutation_link_is_style_resource(lxb_dom_node_t *node)
{
    size_t length = 0;
    const char *rel = document_attribute(node, "rel", &length);
    if (rel == NULL) return false;
    bool preload = false;
    size_t at = 0;
    while (at < length) {
        while (at < length && isspace((unsigned char) rel[at])) at++;
        size_t start = at;
        while (at < length && !isspace((unsigned char) rel[at])) at++;
        if (at - start == sizeof("stylesheet") - 1
            && strncasecmp(rel + start, "stylesheet",
                           sizeof("stylesheet") - 1) == 0) return true;
        if (at - start == sizeof("preload") - 1
            && strncasecmp(rel + start, "preload",
                           sizeof("preload") - 1) == 0) preload = true;
    }
    const char *as = document_attribute(node, "as", &length);
    return preload && as != NULL && length == 5u
        && strncasecmp(as, "style", 5u) == 0;
}

static BridgeMutationResourceFlags bridge_mutation_resource_attribute(
    lxb_dom_node_t *node, const char *name, size_t length)
{
    if (node == NULL || name == NULL) return BRIDGE_MUTATION_RESOURCE_NONE;
    if (bridge_mutation_node_name_is(node, "img")) {
        return (bridge_mutation_name_equal(name, length, "src")
            || bridge_mutation_name_equal(name, length, "srcset")
            || bridge_mutation_name_equal(name, length, "sizes")
            || bridge_mutation_name_equal(name, length, "data-src")
            || bridge_mutation_name_equal(name, length, "data-srcset"))
            ? BRIDGE_MUTATION_RESOURCE_IMAGE
            : BRIDGE_MUTATION_RESOURCE_NONE;
    }
    if (bridge_mutation_node_name_is(node, "source")) {
        return (bridge_mutation_name_equal(name, length, "srcset")
            || bridge_mutation_name_equal(name, length, "data-srcset")
            || bridge_mutation_name_equal(name, length, "media")
            || bridge_mutation_name_equal(name, length, "type"))
            ? BRIDGE_MUTATION_RESOURCE_IMAGE
            : BRIDGE_MUTATION_RESOURCE_NONE;
    }
    if (bridge_mutation_node_name_is(node, "video")) {
        return bridge_mutation_name_equal(name, length, "poster")
            ? BRIDGE_MUTATION_RESOURCE_IMAGE
            : BRIDGE_MUTATION_RESOURCE_NONE;
    }
    if (bridge_mutation_node_name_is(node, "link")) {
        /* A rel transition may remove the sheet which was active before the
           mutation, so it cannot be classified from the new value alone. */
        if (bridge_mutation_name_equal(name, length, "rel")
            || bridge_mutation_name_equal(name, length, "as")) {
            return BRIDGE_MUTATION_RESOURCE_STYLESHEET;
        }
        return bridge_mutation_link_is_style_resource(node)
               && (bridge_mutation_name_equal(name, length, "href")
                   || bridge_mutation_name_equal(name, length, "media")
                   || bridge_mutation_name_equal(name, length, "crossorigin")
                   || bridge_mutation_name_equal(name, length, "integrity")
                   || bridge_mutation_name_equal(name, length, "disabled"))
            ? BRIDGE_MUTATION_RESOURCE_STYLESHEET
            : BRIDGE_MUTATION_RESOURCE_NONE;
    }
    if (bridge_mutation_node_name_is(node, "base")) {
        return bridge_mutation_name_equal(name, length, "href")
            ? BRIDGE_MUTATION_RESOURCE_STYLESHEET
            : BRIDGE_MUTATION_RESOURCE_NONE;
    }
    if (bridge_mutation_node_name_is(node, "use")) {
        return (bridge_mutation_name_equal(name, length, "href")
                || bridge_mutation_name_equal(name, length, "xlink:href"))
            ? BRIDGE_MUTATION_RESOURCE_IMAGE
            : BRIDGE_MUTATION_RESOURCE_NONE;
    }
    return BRIDGE_MUTATION_RESOURCE_NONE;
}

static bool bridge_mutation_inline_style_has_resource(lxb_dom_node_t *node)
{
    size_t length = 0;
    const char *style = document_attribute(node, "style", &length);
    if (style == NULL || length < 4) return false;
    /* The renderer's only externally fetched inline-style value is a CSS
       image URL.  A var() can resolve to one through either the declaration
       itself or a custom property consumed by an image declaration, so it is
       conservatively resource-sensitive too.  Match function names folded
       and tolerate whitespace before the opening parenthesis.  Removing the
       last resource value does not need a rebuild: the next layout reads the
       live declaration and any now-unused resource remains safely owned until
       teardown. */
    for (size_t at = 0; at + 3 < length; at++) {
        bool url = tolower((unsigned char) style[at]) == 'u'
            && tolower((unsigned char) style[at + 1]) == 'r'
            && tolower((unsigned char) style[at + 2]) == 'l';
        bool variable = tolower((unsigned char) style[at]) == 'v'
            && tolower((unsigned char) style[at + 1]) == 'a'
            && tolower((unsigned char) style[at + 2]) == 'r';
        if (!url && !variable) continue;
        size_t after = at + 3;
        while (after < length
               && isspace((unsigned char) style[after])) after++;
        if (after < length && style[after] == '(') return true;
    }
    return false;
}

static bool bridge_mutation_style_property_is_resource(
    const char *name, size_t length)
{
    if (name == NULL) return true;
    return (length >= 2 && name[0] == '-' && name[1] == '-')
        || bridge_mutation_name_equal(name, length, "background")
        || bridge_mutation_name_equal(name, length, "background-image")
        || bridge_mutation_name_equal(name, length, "mask")
        || bridge_mutation_name_equal(name, length, "mask-image");
}

static BridgeMutationResourceFlags bridge_mutation_resource_subtree(
    const PocDocument *document, lxb_dom_node_t *root)
{
    /* DOM parent links let this be a stackless depth-first walk.  The caps
       prevent hostile author trees from turning one bridge call into an
       unbounded PSP stack/work boundary; bounded-out means "unknown", never
       "clean".  The root's siblings are intentionally outside the subtree. */
    enum { MAXIMUM_SUBTREE_WORK = 256, MAXIMUM_SUBTREE_DEPTH = 48 };
    if (root == NULL) return BRIDGE_MUTATION_RESOURCE_NONE;
    bool adopters = document_adoption_shadow_roots_present(document);
    lxb_dom_node_t *node = root;
    size_t work = 0, depth = 0;
    BridgeMutationResourceFlags result = BRIDGE_MUTATION_RESOURCE_NONE;
    for (;;) {
        if (++work > MAXIMUM_SUBTREE_WORK) {
            return result | BRIDGE_MUTATION_RESOURCE_BOUNDED_OUT;
        }
        size_t tag_length = 0;
        const char *tag = document_element_name(node, &tag_length);
        if (bridge_mutation_name_equal(tag, tag_length, "style")) {
            result |= BRIDGE_MUTATION_RESOURCE_STYLESHEET;
        }
        if (document_is_refresh_meta(node)) {
            result |= BRIDGE_MUTATION_RESOURCE_REFRESH_META;
        }
        /* A shadow root's adopted sheets enter or leave the cascade with
           its carrier. */
        if (adopters && node->type == LXB_DOM_NODE_TYPE_ELEMENT
            && document_adoption_root_has_sheets(document, node)) {
            result |= BRIDGE_MUTATION_RESOURCE_STYLESHEET;
        }
        if (node->type == LXB_DOM_NODE_TYPE_ELEMENT) {
            bool link = bridge_mutation_name_equal(tag, tag_length, "link");
            bool stylesheet_link =
                link && bridge_mutation_link_is_style_resource(node);
            if (stylesheet_link) {
                result |= BRIDGE_MUTATION_RESOURCE_STYLESHEET;
            }
            /* bridge_mutation_resource_attribute() classifies attributes of
               these elements only; on any other element every lookup below
               would be answered "none". */
            bool resource_element = link
                || bridge_mutation_name_equal(tag, tag_length, "img")
                || bridge_mutation_name_equal(tag, tag_length, "source")
                || bridge_mutation_name_equal(tag, tag_length, "video")
                || bridge_mutation_name_equal(tag, tag_length, "base")
                || bridge_mutation_name_equal(tag, tag_length, "use");
            static const char *const attributes[] = {
                "src", "srcset", "data-src", "data-srcset", "href",
                "xlink:href", "rel", "media", "type", "sizes", "poster"
            };
            for (size_t i = 0; resource_element
                 && i < sizeof(attributes) / sizeof(attributes[0]); i++) {
                /* A connected rel mutation is destructive because its old
                   value may have removed a stylesheet. For an inserted
                   subtree the final rel is fully known; only stylesheets
                   and style preloads belong to this resource lane. */
                if (!stylesheet_link && link
                    && bridge_mutation_name_equal(
                        attributes[i], strlen(attributes[i]), "rel")) {
                    continue;
                }
                size_t length = 0;
                (void) document_attribute(node, attributes[i], &length);
                if (length != 0) {
                    result |= bridge_mutation_resource_attribute(
                        node, attributes[i], strlen(attributes[i]));
                }
            }
            if (bridge_mutation_inline_style_has_resource(node)) {
                result |= BRIDGE_MUTATION_RESOURCE_IMAGE;
            }
        }
        if (node->first_child != NULL) {
            if (++depth > MAXIMUM_SUBTREE_DEPTH) {
                return result | BRIDGE_MUTATION_RESOURCE_BOUNDED_OUT;
            }
            node = node->first_child;
            continue;
        }
        while (node != root && node->next == NULL) {
            node = node->parent;
            depth--;
        }
        if (node == root) break;
        node = node->next;
    }
    return result;
}

static bool bridge_mutation_record_equal(
    const ScriptMutationRecord *record, ScriptMutationKind kind,
    const lxb_dom_node_t *node, const char *attribute,
    size_t attribute_length)
{
    if (record == NULL || record->kind != kind || record->node != node) {
        return false;
    }
    size_t stored_length = strlen(record->attribute);
    if (attribute == NULL) return stored_length == 0;
    /* Renderer records retain a conservative dependency prefix, not the
       author-visible attribute name. Compare the same bounded prefix we
       store: otherwise every write to a long name occupies another slot.
       Resource flags are still accumulated from every full-name mutation. */
    if (attribute_length >= sizeof(record->attribute))
        attribute_length = sizeof(record->attribute) - 1u;
    if (stored_length != attribute_length) return false;
    for (size_t i = 0; i < attribute_length; i++) {
        if (record->attribute[i]
            != (char) tolower((unsigned char) attribute[i])) return false;
    }
    return true;
}

/* Adds `root` to the overflow roots, merged past the limit into the
   deepest ancestor it shares with one of them. */
static void bridge_mutation_overflow_add(ScriptMutationJournal *journal,
                                         lxb_dom_node_t *root)
{
    for (unsigned merges = 0; merges <= SCRIPT_MUTATION_OVERFLOW_ROOT_LIMIT;
         merges++) {
        for (size_t i = 0; i < journal->overflow_root_count; i++)
            if (js_rt_node_within(root, journal->overflow_roots[i])) return;
        /* Roots the new one contains go. */
        size_t kept = 0;
        for (size_t i = 0; i < journal->overflow_root_count; i++)
            if (!js_rt_node_within(journal->overflow_roots[i], root))
                journal->overflow_roots[kept++] = journal->overflow_roots[i];
        journal->overflow_root_count = (uint8_t) kept;
        if (kept < SCRIPT_MUTATION_OVERFLOW_ROOT_LIMIT) {
            journal->overflow_roots[journal->overflow_root_count++] = root;
            return;
        }
        size_t best = 0, best_depth = 0;
        lxb_dom_node_t *best_ancestor = NULL;
        for (size_t i = 0; i < kept; i++) {
            lxb_dom_node_t *common = journal->overflow_roots[i];
            while (common != NULL && !js_rt_node_within(root, common))
                common = common->parent;
            size_t depth = 0;
            for (lxb_dom_node_t *at = common; at != NULL; at = at->parent)
                depth++;
            if (common != NULL && common->type == LXB_DOM_NODE_TYPE_ELEMENT
                && (best_ancestor == NULL || depth > best_depth)) {
                best = i;
                best_depth = depth;
                best_ancestor = common;
            }
        }
        if (best_ancestor == NULL) break;
        journal->overflow_roots[best] =
            journal->overflow_roots[--journal->overflow_root_count];
        root = best_ancestor;
    }
    journal->overflow_roots_lost = true;
}

/* Where a change the full journal cannot record is contained: the
   subtree of the element's parent (sibling and :has() reach included),
   or for a tree or text change of its parent's parent (the parent's own
   emptiness and position tests too). */
static void bridge_mutation_overflow_root(ScriptMutationJournal *journal,
                                          ScriptMutationKind kind,
                                          lxb_dom_node_t *node)
{
    if (journal->overflow_roots_lost) return;
    lxb_dom_node_t *root = node == NULL ? NULL : node->parent;
    if (root != NULL && (kind == SCRIPT_MUTATION_CHILD_LIST
                         || kind == SCRIPT_MUTATION_HEAD_SCRIPT
                         || kind == SCRIPT_MUTATION_TEXT))
        root = root->parent;
    if (root != NULL && root->type != LXB_DOM_NODE_TYPE_ELEMENT) {
        /* The change is at the top: the document element's subtree. */
        root = NULL;
        for (lxb_dom_node_t *at = node; at != NULL; at = at->parent)
            if (at->type == LXB_DOM_NODE_TYPE_ELEMENT
                && at->parent != NULL
                && at->parent->type == LXB_DOM_NODE_TYPE_DOCUMENT) root = at;
    }
    if (root == NULL || !bridge_node_is_connected(root)) {
        journal->overflow_roots_lost = true;
        return;
    }
    bridge_mutation_overflow_add(journal, root);
}

static void bridge_mutation_journal_append(
    DomBridge *bridge, ScriptMutationKind kind, lxb_dom_node_t *node,
    const char *attribute, size_t attribute_length,
    const uint32_t *changed_tokens, size_t changed_token_count,
    bool relational, uint64_t has_entries, uint32_t has_serial)
{
    ScriptMutationJournal *journal = &bridge->mutations;
    if (has_serial != 0) {
        if (journal->has_serial == 0) journal->has_serial = has_serial;
        else if (journal->has_serial != has_serial)
            journal->has_serial_mixed = true;
    }
    for (size_t reverse = journal->count; reverse != 0; reverse--) {
        ScriptMutationRecord *record = &journal->records[reverse - 1];
        if (bridge_mutation_record_equal(
                record, kind, node, attribute, attribute_length)) {
            record->relational |= relational;
            record->has_entries |= has_serial == 0 ? UINT64_MAX : has_entries;
            /* Coalescing the node/attribute must retain every dependency
               changed during this turn, not just the first write's tokens. */
            if (changed_tokens == NULL
                || changed_token_count > SCRIPT_MUTATION_TOKEN_LIMIT) {
                record->changed_tokens_exact = false;
            } else if (record->changed_tokens_exact) {
                size_t count = record->changed_token_count;
                const uint32_t *run = journal->tokens
                    + record->changed_token_offset;
                size_t added = 0;
                uint32_t fresh[SCRIPT_MUTATION_TOKEN_LIMIT];
                for (size_t i = 0; i < changed_token_count; i++) {
                    bool present = false;
                    for (size_t at = 0; at < count && !present; at++)
                        present = run[at] == changed_tokens[i];
                    for (size_t at = 0; at < added && !present; at++)
                        present = fresh[at] == changed_tokens[i];
                    if (!present) fresh[added++] = changed_tokens[i];
                }
                if (added != 0) {
                    bool at_end = (size_t) record->changed_token_offset
                        + count == journal->token_count;
                    size_t start = at_end ? record->changed_token_offset
                                          : journal->token_count;
                    if (count + added > SCRIPT_MUTATION_TOKEN_LIMIT
                        || start + count + added
                               > SCRIPT_MUTATION_TOKEN_POOL) {
                        record->changed_tokens_exact = false;
                    } else {
                        /* A run left behind by relocation stays unused
                           until the journal is consumed. */
                        if (!at_end)
                            memmove(journal->tokens + start, run,
                                    count * sizeof(*run));
                        memcpy(journal->tokens + start + count, fresh,
                               added * sizeof(*fresh));
                        record->changed_token_offset = (uint16_t) start;
                        record->changed_token_count =
                            (uint8_t) (count + added);
                        journal->token_count = start + count + added;
                    }
                }
            }
            if (!record->changed_tokens_exact) record->changed_token_count = 0;
            return;
        }
    }
    if (journal->count >= SCRIPT_MUTATION_JOURNAL_LIMIT) {
        journal->overflowed = true;
        bridge_mutation_overflow_root(journal, kind, node);
        /* Records beyond the fixed journal cannot be independently audited,
           so retain the old whole-document fingerprint oracle. */
        journal->conservative_resource_scan = true;
        return;
    }
    ScriptMutationRecord *record = &journal->records[journal->count++];
    record->kind = kind;
    record->node = node;
    record->relational = relational;
    record->has_entries = has_serial == 0 ? UINT64_MAX : has_entries;
    record->inserted_from_detached = false;
    record->scope = NULL;
    record->removed_last = false;
    size_t copy_length = attribute == NULL ? 0 : attribute_length;
    if (copy_length >= sizeof(record->attribute)) {
        copy_length = sizeof(record->attribute) - 1;
    }
    for (size_t i = 0; i < copy_length; i++) {
        record->attribute[i] = (char) tolower(
            (unsigned char) attribute[i]);
    }
    record->attribute[copy_length] = '\0';
    record->changed_tokens_exact = changed_tokens != NULL
        && changed_token_count <= SCRIPT_MUTATION_TOKEN_LIMIT
        && journal->token_count + changed_token_count
               <= SCRIPT_MUTATION_TOKEN_POOL;
    record->changed_token_count = 0;
    record->changed_token_offset = (uint16_t) journal->token_count;
    if (record->changed_tokens_exact) {
        record->changed_token_count = (uint8_t) changed_token_count;
        memcpy(journal->tokens + journal->token_count, changed_tokens,
               changed_token_count * sizeof(*changed_tokens));
        journal->token_count += changed_token_count;
    }
}

/* Whether a mutation can change which form owns a control, or whether an
   element is a listed control at all (form.elements): tree changes, and the
   id, form and type attributes; an unnamed or internal (custom-element
   state) attribute stays conservative. Class, style and the rest cannot. */
static bool bridge_mutation_moves_form_controls(ScriptMutationKind kind,
                                                const char *attribute,
                                                size_t length)
{
    if (kind == SCRIPT_MUTATION_INLINE_STYLE
        || kind == SCRIPT_MUTATION_CANVAS) return false;
    if (kind != SCRIPT_MUTATION_ATTRIBUTE || attribute == NULL) return true;
    return (length == 2 && strncasecmp(attribute, "id", 2) == 0)
        || (length == 4 && (strncasecmp(attribute, "form", 4) == 0
                            || strncasecmp(attribute, "type", 4) == 0))
        || (length > 14 && strncasecmp(attribute, "data-tilefinch", 14) == 0);
}

static void bridge_mutated_summarized(
    DomBridge *bridge, ScriptMutationKind kind, lxb_dom_node_t *node,
    const char *attribute, size_t attribute_length,
    bool relational_selector_sensitive, const uint32_t *changed_tokens,
    size_t changed_token_count, uint64_t has_entries, uint32_t has_serial);

static void bridge_mutated_with_relational(
    DomBridge *bridge, ScriptMutationKind kind, lxb_dom_node_t *node,
    const char *attribute, size_t attribute_length,
    bool relational_selector_sensitive, const uint32_t *changed_tokens,
    size_t changed_token_count)
{
    bridge_mutated_summarized(bridge, kind, node, attribute,
                              attribute_length, relational_selector_sensitive,
                              changed_tokens, changed_token_count,
                              UINT64_MAX, 0);
}

/* has_entries/has_serial: the :has() entries a tree or text change can
   move (stylesheet_tree_change_has_entries), kept on its record. */
static void bridge_mutated_summarized(
    DomBridge *bridge, ScriptMutationKind kind, lxb_dom_node_t *node,
    const char *attribute, size_t attribute_length,
    bool relational_selector_sensitive, const uint32_t *changed_tokens,
    size_t changed_token_count, uint64_t has_entries, uint32_t has_serial)
{
    if (bridge == NULL) return;
    if (kind == SCRIPT_MUTATION_TEXT || kind == SCRIPT_MUTATION_INNER_HTML)
        document_cssom_sheet_reset(node);
    /* Every script mutation, connected or not, invalidates DOM-derived
       caches kept in the realm (attribute lists). */
    bridge->dom_version++;
    if (bridge_mutation_moves_form_controls(kind, attribute,
                                            attribute_length))
        bridge->dom_structure_version++;
    /* Mutating a detached construction tree cannot affect layout or the
       connected resource graph. Its eventual insertion is represented by
       one child-list record over the complete subtree. Besides avoiding
       wasted work, this keeps a framework assembling one card from
       overflowing the PSP's fixed mutation journal before it is appended. */
    if (node != NULL && !bridge_node_is_connected(node)) return;
    /* A declaration miss is valid only for the DOM generation that was
       scanned. Author mutation may hydrate metadata or a playable element;
       native card insertion does not pass through this bridge and therefore
       keeps its already-bound selection. */
    media_declared_video_cache_destroy(bridge->document);
    document_note_connected_mutation(bridge->document);
    js_rt_bridge_computed_style_note_mutation(
        bridge, kind, node, attribute, attribute_length,
        relational_selector_sensitive, changed_tokens, changed_token_count);
    /* Attribute and tree mutations can change which connected <base href> is
       first. Invalidating conservatively keeps the mutation fast and avoids a
       second subtree walk on the PSP hot path. */
    bridge->document_base_dirty = true;
    if (bridge->result != NULL) {
        bridge->result->dom_mutations++;
        bridge->result->relayout_required = true;
    }
    /* An attribute, inline-style or canvas change leaves every document
       statistic but the attribute totals intact, so it need not cost a
       whole-document refresh. While the parser is still inserting nodes,
       though, that refresh is also what brings the parser's growth into the
       statistics the streaming checkpoints compare, so keep it until the
       tree is complete. */
    bool attribute_only = kind == SCRIPT_MUTATION_ATTRIBUTE
        || kind == SCRIPT_MUTATION_INLINE_STYLE
        || kind == SCRIPT_MUTATION_CANVAS;
    if (attribute_only)
        document_note_attribute_mutation(
            bridge->document, attribute, attribute_length);
    if (!attribute_only || bridge->document == NULL
        || bridge->document->html == NULL
        || bridge->document->html->ready_state
               != LXB_HTML_DOCUMENT_READY_STATE_COMPLETE)
        bridge->stats_mutations++;
    if (bridge->relayout_dirty != NULL) *bridge->relayout_dirty = true;

    ScriptMutationJournal *journal = &bridge->mutations;
    bridge_mutation_journal_append(
        bridge, kind, node, attribute, attribute_length, changed_tokens,
        changed_token_count, relational_selector_sensitive, has_entries,
        has_serial);

    bool resource_rebuild = false;
    /* resource_rebuild's causes: a stylesheet source and/or an image. */
    bool stylesheet_cause = false;
    bool image_rebuild = false;
    bool image_resource_scan = false;
    bool image_resource_refresh = false;
    bool conservative_scan = false;
    /* A connected <meta http-equiv=refresh> may have arrived (or a subtree
       too large to inspect did). */
    bool refresh_meta = false;
    switch (kind) {
    case SCRIPT_MUTATION_TEXT:
        if (node == NULL) conservative_scan = true;
        else resource_rebuild = bridge_mutation_inside_style(node);
        stylesheet_cause = resource_rebuild;
        break;
    case SCRIPT_MUTATION_INLINE_STYLE:
        /* The prior declaration may have owned an image even when the new
           one does not. Keep only image-bearing properties destructive;
           ordinary geometry/color declarations are layout-only. */
        resource_rebuild = bridge_mutation_style_property_is_resource(
            attribute, attribute_length);
        image_rebuild = resource_rebuild;
        /* A custom property never changes the stylesheet: at most it lets
           a var() image declaration resolve to a new URL. Discover that
           additively, and only when such a declaration can exist (the
           sheet uses var() in one, or this element's own inline style may);
           a no-longer-used image stays owned until teardown. */
        if (resource_rebuild && attribute != NULL && attribute_length >= 2u
            && attribute[0] == '-' && attribute[1] == '-') {
            resource_rebuild = false;
            image_resource_scan = (bridge->stylesheet != NULL
                    && bridge->stylesheet->image_declarations_use_variables)
                || bridge_mutation_inline_style_has_resource(node);
            break;
        }
        /* Visibility changes can expose a background URL which the initial
           image walk correctly skipped under display:none/hidden.  Re-scan
           only when this element's final inline declaration actually owns a
           resource, keeping ordinary animation/style churn off the resource
           path while ensuring a script-revealed hero is discovered. */
        image_resource_scan = !resource_rebuild
            && bridge_mutation_inline_style_has_resource(node)
            && (bridge_mutation_name_equal(
                    attribute, attribute_length, "display")
                || bridge_mutation_name_equal(
                    attribute, attribute_length, "visibility")
                || bridge_mutation_name_equal(
                    attribute, attribute_length, "opacity"));
        break;
    case SCRIPT_MUTATION_ATTRIBUTE:
    {
        BridgeMutationResourceFlags flags =
            bridge_mutation_resource_attribute(
                node, attribute, attribute_length);
        /* Setting content or http-equiv on a connected meta processes it
           again, as browsers do for an element already in the document. */
        refresh_meta = node != NULL && node->local_name == LXB_TAG_META
            && node->ns == LXB_NS_HTML
            && (bridge_mutation_name_equal(
                    attribute, attribute_length, "content")
                || bridge_mutation_name_equal(
                    attribute, attribute_length, "http-equiv"));
        /* A style attribute is an inline declaration block, not a sheet:
           a URL (or var() that may resolve to one) in the new value needs
           additive image discovery, never a stylesheet rebuild. */
        bool style_attribute = bridge_mutation_name_equal(
            attribute, attribute_length, "style")
            && !bridge_mutation_node_name_is(node, "style");
        if (style_attribute)
            image_resource_scan =
                bridge_mutation_inline_style_has_resource(node);
        resource_rebuild = bridge_mutation_node_name_is(node, "style")
            || (flags & BRIDGE_MUTATION_RESOURCE_STYLESHEET) != 0;
        stylesheet_cause = resource_rebuild;
        if ((flags & BRIDGE_MUTATION_RESOURCE_IMAGE) != 0) {
            /* An icon swap (<use href> inside an inline <svg>) changes only
               that SVG's raster, which the refresh lane re-resolves from its
               root, sprite fetch included; outside an <svg> a <use> renders
               nothing. Neither needs the sheet or other images rebuilt. */
            if (bridge_mutation_node_name_is(node, "img")
                || bridge_mutation_node_name_is(node, "use")) {
                image_resource_refresh = true;
            } else {
                resource_rebuild = true;
                image_rebuild = true;
            }
        }
        break;
    }
    case SCRIPT_MUTATION_INNER_HTML:
    {
        BridgeMutationResourceFlags subtree =
            bridge_mutation_resource_subtree(bridge->document, node);
        resource_rebuild = node != NULL
            && (bridge_mutation_inside_style(node)
                || (subtree & (BRIDGE_MUTATION_RESOURCE_IMAGE
                               | BRIDGE_MUTATION_RESOURCE_STYLESHEET)) != 0);
        image_rebuild = node != NULL
            && (subtree & BRIDGE_MUTATION_RESOURCE_IMAGE) != 0;
        stylesheet_cause = node != NULL
            && (bridge_mutation_inside_style(node)
                || (subtree & BRIDGE_MUTATION_RESOURCE_STYLESHEET) != 0);
        /* innerHTML may have removed the last resource, which cannot be
           inferred by walking only the replacement subtree. */
        conservative_scan = !resource_rebuild
            || (subtree & BRIDGE_MUTATION_RESOURCE_BOUNDED_OUT) != 0;
        refresh_meta = (subtree & (BRIDGE_MUTATION_RESOURCE_REFRESH_META
                                   | BRIDGE_MUTATION_RESOURCE_BOUNDED_OUT))
            != 0;
        break;
    }
    case SCRIPT_MUTATION_CHILD_LIST:
    case SCRIPT_MUTATION_HEAD_SCRIPT:
    {
        BridgeMutationResourceFlags subtree =
            bridge_mutation_resource_subtree(bridge->document, node);
        bool connected = node != NULL && bridge_node_is_connected(node);
        if (!connected) {
            /* A removal can invalidate retained decoded surfaces and source
               order, so it remains destructive when resources are present. */
            resource_rebuild =
                (subtree & (BRIDGE_MUTATION_RESOURCE_IMAGE
                            | BRIDGE_MUTATION_RESOURCE_STYLESHEET)) != 0;
            image_rebuild = (subtree & BRIDGE_MUTATION_RESOURCE_IMAGE) != 0;
            stylesheet_cause =
                (subtree & BRIDGE_MUTATION_RESOURCE_STYLESHEET) != 0;
        } else {
            resource_rebuild =
                (subtree & BRIDGE_MUTATION_RESOURCE_STYLESHEET) != 0;
            stylesheet_cause = resource_rebuild;
            image_resource_scan =
                (subtree & BRIDGE_MUTATION_RESOURCE_IMAGE) != 0;
        }
        conservative_scan =
            (subtree & BRIDGE_MUTATION_RESOURCE_BOUNDED_OUT) != 0;
        refresh_meta = connected
            && (subtree & (BRIDGE_MUTATION_RESOURCE_REFRESH_META
                           | BRIDGE_MUTATION_RESOURCE_BOUNDED_OUT)) != 0;
        break;
    }
    case SCRIPT_MUTATION_CANVAS:
        /* The resource already exists by the time this record is emitted.
           Creation/resizing still takes the ordinary fast-layout path;
           same-geometry publication is eligible for paint-only damage. */
        break;
    case SCRIPT_MUTATION_UNKNOWN:
    default:
        conservative_scan = true;
        break;
    }
    /* A class/id change whose tokens no rule able to affect display,
       visibility or an image depends on cannot reveal or hide an image.
       With inline SVG rasters present it must also leave what those SVGs
       resolve alone: a change inside one always refreshes it, one above
       one only when a colour, font-size, size, presentation or
       custom-property rule depends on its tokens (checked last, after the
       bounded subtree walk has found an SVG to refresh). */
    bool svg_rasters = node != NULL
        && bridge_mutation_has_inline_svg_rasters(bridge);
    bool identity_bounded = kind == SCRIPT_MUTATION_ATTRIBUTE
        && changed_tokens != NULL && bridge->stylesheet != NULL
#ifndef TILEFINCH_NO_TRACE
        && getenv("TILEFINCH_DISABLE_DISCOVERY_GATE") == NULL
#endif
        && (bridge_mutation_name_equal(attribute, attribute_length, "class")
            || bridge_mutation_name_equal(attribute, attribute_length, "id"))
        && !stylesheet_tokens_may_affect_discovery(
               bridge->stylesheet, changed_tokens, changed_token_count)
        && !(svg_rasters
             && (bridge_mutation_inside_svg(node)
                 || (bridge_mutation_subtree_contains_image(bridge, node)
                     && stylesheet_tokens_may_affect_svg_raster(
                            bridge->stylesheet, changed_tokens,
                            changed_token_count))));
    bool descendant_image_sensitive =
        node != NULL && !identity_bounded
        && (bridge_mutation_inside_svg(node)
            || ((kind == SCRIPT_MUTATION_ATTRIBUTE
                 || kind == SCRIPT_MUTATION_INLINE_STYLE)
                && (bridge_mutation_name_equal(
                        attribute, attribute_length, "class")
                    || bridge_mutation_name_equal(
                        attribute, attribute_length, "id")
                    || bridge_mutation_name_equal(
                        attribute, attribute_length, "style")
                    || (kind == SCRIPT_MUTATION_ATTRIBUTE
                        && bridge_mutation_name_equal(
                               attribute, attribute_length, "color"))
                    || bridge_mutation_name_equal(
                        attribute, attribute_length, "hidden")
                    || (kind == SCRIPT_MUTATION_INLINE_STYLE
                        && (bridge_mutation_name_equal(attribute, attribute_length, "display")
                            || bridge_mutation_name_equal(attribute, attribute_length, "visibility")
                            || bridge_mutation_name_equal(attribute, attribute_length, "opacity")
                            || (svg_rasters
                                && bridge_mutation_style_property_affects_svg_raster(
                                       attribute, attribute_length)))))
               && bridge_mutation_subtree_contains_image(bridge, node)));
    if (refresh_meta) bridge->refresh_meta_mutated = true;
    journal->resource_rebuild_required |= resource_rebuild;
    journal->image_rebuild_required |= resource_rebuild && image_rebuild;
    journal->stylesheet_rebuild_required |= resource_rebuild && stylesheet_cause;
    journal->image_resource_scan_required |= image_resource_scan;
    journal->image_resource_refresh_required |=
        image_resource_refresh || descendant_image_sensitive;
    journal->conservative_resource_scan |= conservative_scan;
    bool focus_marker = kind == SCRIPT_MUTATION_ATTRIBUTE
        && attribute != NULL
        && attribute_length == sizeof("data-tilefinch-focus") - 1
        && strncasecmp(
            attribute, "data-tilefinch-focus",
            sizeof("data-tilefinch-focus") - 1) == 0;
    /* Focus has a dedicated ancestor-aware invalidator. Keep this aggregate
       for unrelated records in the same author turn. */
    if (!focus_marker) {
        journal->relational_selector_sensitive |=
            relational_selector_sensitive;
    }
    if ((resource_rebuild || image_resource_scan || image_resource_refresh
         || conservative_scan)
        && tilefinch_trace_mutation_policy()) {
        size_t name_length = 0;
        const char *name = document_element_name(node, &name_length);
        fprintf(stderr,
                "tilefinch: mutation-policy resource kind=%d class=%s "
                "node=%.*s "
                "attribute=%.*s rebuild=%d image-scan=%d "
                "image-refresh=%d conservative=%d\n",
                (int) kind,
                resource_rebuild ? "rebuild"
                : (image_resource_scan ? "image-add" : "image-refresh"),
                (int) name_length, name == NULL ? "" : name,
                (int) attribute_length,
                attribute == NULL ? "" : attribute,
                resource_rebuild, image_resource_scan,
                image_resource_refresh, conservative_scan);
    }
}

void js_rt_bridge_note_canvas_mutation(DomBridge *bridge,
                                       lxb_dom_node_t *node,
                                       bool paint_only)
{
    static const char paint[] = "paint";
    static const char structure[] = "surface";
    if (paint_only) {
        /* Replacing pixels in an already-sized canvas changes neither the
           connected DOM nor document-derived title/body/statistics. Publish
           bounded paint damage without making js_rt_runtime_refresh() walk
           and re-summarize the complete document on every animation frame. */
        if (bridge == NULL || node == NULL
            || !bridge_node_is_connected(node)) return;
        if (bridge->result != NULL) bridge->result->relayout_required = true;
        if (bridge->relayout_dirty != NULL) *bridge->relayout_dirty = true;
        bridge_mutation_journal_append(
            bridge, SCRIPT_MUTATION_CANVAS, node,
            paint, sizeof(paint) - 1u, NULL, 0, false, UINT64_MAX, 0);
        return;
    }
    bridge_mutated_with_relational(
        bridge, SCRIPT_MUTATION_CANVAS, node,
        structure, sizeof(structure) - 1u,
        false, NULL, 0);
}

static void bridge_mutated(DomBridge *bridge, ScriptMutationKind kind,
                           lxb_dom_node_t *node,
                           const char *attribute,
                           size_t attribute_length)
{
    /* Callers without exact pre-mutation state remain conservative. A child
       inserted (after the fact) or about to be removed, and a text node's
       data, are classified against the sheet's :has() rules instead; a
       text write that replaced an element's children cannot be. */
    bool relational = true;
    uint64_t has_entries = UINT64_MAX;
    uint32_t has_serial = 0;
    if (bridge != NULL && node != NULL && node->parent != NULL
        && (kind == SCRIPT_MUTATION_CHILD_LIST
            || kind == SCRIPT_MUTATION_HEAD_SCRIPT
            || (kind == SCRIPT_MUTATION_TEXT
                && node->type != LXB_DOM_NODE_TYPE_ELEMENT)))
        relational = stylesheet_tree_change_has_entries(
            bridge->stylesheet, node, node->parent, &has_entries,
            &has_serial);
    bridge_mutated_summarized(
        bridge, kind, node, attribute, attribute_length, relational, NULL, 0,
        has_entries, has_serial);
}
