/* Classifies a script mutation journal before relayout: transient,
   semantic-only and paint-only focus changes, plus node retirement binding. */

static bool navigation_selector_mentions_attribute(
    const Stylesheet *sheet, const char *attribute)
{
    if (sheet == NULL || attribute == NULL || attribute[0] == '\0') {
        return true;
    }
    size_t attribute_length = strlen(attribute);
    /* The renderer journal truncates names. An exact-match negative for
       such a prefix cannot justify skipping selector/generated-text work. */
    if (attribute_length >= SCRIPT_MUTATION_ATTRIBUTE_LIMIT - 1u)
        return true;
    size_t selector_count = sheet->count + sheet->custom_rule_count;
    for (size_t i = 0; i < selector_count; i++) {
        const char *selector = i < sheet->count
            ? sheet->rules[i].selector
            : sheet->custom_rules[i - sheet->count].selector;
        size_t length = selector == NULL ? 0 : strlen(selector);
        for (size_t at = 0; selector != NULL && at < length; at++) {
            if (selector[at] != '[') continue;
            size_t name = at + 1u;
            while (name < length
                   && isspace((unsigned char) selector[name])) name++;
            /* Namespace-qualified and escaped attribute names are rare and
               not worth approximating here. They remain conservative while
               ordinary unrelated [data-*] selectors no longer invalidate
               every ARIA-only mutation on a large page. */
            if (name < length && (selector[name] == '\\'
                                  || (selector[name] == '*'
                                      && name + 1u < length
                                      && selector[name + 1u] == '|'))) {
                return true;
            }
            size_t end = name;
            while (end < length) {
                unsigned char value = (unsigned char) selector[end];
                if (!(isalnum(value) || value == '-' || value == '_')) break;
                end++;
            }
            if (end < length && selector[end] == '|'
                && (end + 1u >= length || selector[end + 1u] != '=')) {
                return true;
            }
            if (end - name == attribute_length
                && strncasecmp(selector + name, attribute,
                               attribute_length) == 0) return true;
        }
    }
    for (size_t i = 0; i < sheet->declaration_count; i++) {
        const StyleDeclaration *declaration = &sheet->declarations[i];
        if ((declaration->values_flags
             & STYLE_DECLARATION_VALUES_GENERATED_ATTR) != 0) {
            ComputedStyle values;
            stylesheet_declaration_values(sheet, declaration, &values);
            if (values.generated_attr != NULL
                && values.generated_attr_length == attribute_length
                && memcmp(values.generated_attr, attribute,
                          attribute_length) == 0) {
                return true;
            }
        }
        /* A var()-dependent declaration is resolved against the element at
           match time, so its final attr() name is not necessarily present in
           values yet. Relayout rather than guessing about generated text. */
        if (declaration->deferred_declarations != NULL
            && strstr(declaration->deferred_declarations, "attr(") != NULL) {
            return true;
        }
    }
    return false;
}

static bool navigation_node_connected(const lxb_dom_node_t *node);

/* `relational`: some head-script write may move a :has() answer; when the
   bridge's probe proved none can, the sheet's :has() anchors need not be
   checked against the head and <html>. `changed`, when connected under the
   head, limits that check to the :has() arguments that can see it. */
static bool navigation_head_scripts_are_nonvisual(const NavigationPage *page,
                                                  bool relational,
                                                  lxb_dom_node_t *changed)
{
    if (page == NULL || page->document.html == NULL) return false;
    lxb_dom_node_t *head = lxb_dom_interface_node(
        lxb_html_document_head_element(page->document.html));
    if (head == NULL || head->parent == NULL
        || head->parent->local_name != LXB_TAG_HTML) return false;
    lxb_dom_node_t *previous = head->prev;
    for (unsigned i = 0; previous != NULL && i < 64;
         i++, previous = previous->prev) {
        if (previous->type == LXB_DOM_NODE_TYPE_ELEMENT) return false;
    }
    if (previous != NULL) return false;
    const Stylesheet *sheet = &page->stylesheet;
    /* The sheet caches only lexical dependencies, never live DOM matches.
       Only head/html can observe this hidden subtree from outside it. */
    if (relational && stylesheet_head_scripts_affect_ancestors(
            sheet, head, changed != NULL && changed->parent == head
                         && navigation_node_connected(changed)
                         ? changed : NULL))
        return false;
    ComputedStyle root = style_for_node(sheet, head->parent, NULL);
    ComputedStyle style = style_for_node(sheet, head, &root);
    return style.display == DISPLAY_NONE;
}

/* The stylesheet's recorded sources are compared by address during style
   append; one that is freed could be mistaken for a later element at its
   address, so its sheet stops vouching for any source. Until the next
   rebuild its adopted rules apply by document scope alone, and the next
   relayout rebuilds (navigation_relayout_body). */
static void navigation_retire_style_sources(Stylesheet *sheet,
                                            const lxb_dom_node_t *root)
{
    if (sheet->style_sources_bounded_out) return;
    for (size_t i = 0; i < sheet->style_source_count; i++) {
        size_t depth = 0;
        for (const lxb_dom_node_t *at = sheet->style_source_nodes[i];
             at != NULL; at = at->parent) {
            if (at == root || ++depth > 4096) {
                sheet->style_sources_bounded_out = true;
                return;
            }
        }
    }
}

static void navigation_retire_subtree(void *opaque,
                                      const lxb_dom_node_t *root)
{
    NavigationSession *session = opaque;
    if (session == NULL) return;
    navigation_retire_style_sources(&session->page.stylesheet, root);
    if (session->page.layout_reuse == NULL) return;
    layout_reuse_cache_retire_subtree(session->page.layout_reuse, root);
}

/* Idempotent: the page runtime reports every freed subtree to the page
   cache from the first journal consumption on; earlier frees stayed
   conservative through the journal's overflow signal. */
static void navigation_bind_node_retirement(NavigationSession *session)
{
    if (session == NULL || session->page.runtime == NULL
        || session->page.layout_reuse == NULL) return;
#ifndef TILEFINCH_NO_TRACE
    if (getenv("TILEFINCH_DISABLE_NODE_RETIREMENT") != NULL) return;
#endif
    script_runtime_set_node_retirement_callback(
        session->page.runtime, navigation_retire_subtree, session);
    layout_reuse_cache_enable_node_retirement(session->page.layout_reuse);
}

static bool navigation_mutation_is_transient(
    const ScriptMutationJournal *mutations, size_t index)
{
    const ScriptMutationRecord *record = &mutations->records[index];
    if (record->node == NULL) return false; /* retired/unknown, stay conservative */
    if ((record->kind == SCRIPT_MUTATION_CHILD_LIST
         || record->kind == SCRIPT_MUTATION_HEAD_SCRIPT)
        && !record->inserted_from_detached) return false;
    if (record->kind != SCRIPT_MUTATION_CHILD_LIST
        && record->kind != SCRIPT_MUTATION_HEAD_SCRIPT
        && record->kind != SCRIPT_MUTATION_ATTRIBUTE
        && record->kind != SCRIPT_MUTATION_INLINE_STYLE
        && record->kind != SCRIPT_MUTATION_TEXT
        && record->kind != SCRIPT_MUTATION_INNER_HTML) return false;
    const lxb_dom_node_t *ancestors[64];
    size_t depth = 0;
    for (const lxb_dom_node_t *at = record->node; at != NULL; at = at->parent) {
        if (at->type == LXB_DOM_NODE_TYPE_DOCUMENT || depth == 64) return false;
        ancestors[depth++] = at;
    }
    /* Every accepted record is inside a now-detached root whose *earlier*
       first child mutation inserted detached content. Connected-node moves
       and pre-existing removals remain observable changes even if their final
       parent happens to be a transient probe. No allocation or DOM rewrite. */
    for (size_t i = 0; i <= index; i++) {
        const ScriptMutationRecord *first = &mutations->records[i];
        if (!first->inserted_from_detached) continue;
        for (size_t ancestor = 0; ancestor < depth; ancestor++) {
            if (ancestors[ancestor] == first->node) return true;
        }
    }
    return false;
}

static bool navigation_mutations_are_semantic_only(
    const NavigationSession *session, const ScriptMutationJournal *mutations)
{
    /* A required rebuild is never skipped: the flag would stay set, and
       each later relayout would pause the image preloads again for it. */
    if (session == NULL || mutations == NULL || mutations->count == 0
        || session->style_rebuild_required
        || mutations->overflowed || mutations->resource_rebuild_required
        || mutations->image_resource_scan_required
        || mutations->image_resource_refresh_required) {
        return false;
    }
    bool head_checked = false;
    for (size_t i = 0; i < mutations->count; i++) {
        const ScriptMutationRecord *record = &mutations->records[i];
        if (navigation_mutation_is_transient(mutations, i)) continue;
        if (record->kind == SCRIPT_MUTATION_HEAD_SCRIPT) {
            /* A relational record is checked for its own node. */
            if ((!head_checked || record->relational)
                && !navigation_head_scripts_are_nonvisual(
                       &session->page, record->relational, record->node))
                return false;
            head_checked = true;
            continue;
        }
        if (record->kind != SCRIPT_MUTATION_ATTRIBUTE
            || strncmp(record->attribute, "aria-", 5) != 0
            || navigation_selector_mentions_attribute(
                   &session->page.stylesheet, record->attribute)) {
            return false;
        }
    }
    return true;
}

static bool navigation_focus_parent_style(
    const Stylesheet *sheet, lxb_dom_node_t *node, ComputedStyle *parent,
    bool *has_parent)
{
    if (sheet == NULL || node == NULL || parent == NULL
        || has_parent == NULL) return false;
    lxb_dom_node_t *ancestors[64];
    size_t count = 0;
    for (lxb_dom_node_t *at = node->parent; at != NULL; at = at->parent) {
        if (at->type != LXB_DOM_NODE_TYPE_ELEMENT) continue;
        if (count == sizeof(ancestors) / sizeof(ancestors[0])) return false;
        ancestors[count++] = at;
    }
    *parent = (ComputedStyle) {0};
    *has_parent = false;
    while (count != 0) {
        *parent = style_for_node(
            sheet, ancestors[--count], *has_parent ? parent : NULL);
        /* Retained colour writes must not undo ancestor compositing. A
           pure translation (the translate(-50%, -50%) centring idiom) is
           baked into every descendant command and box by layout, so it
           composites nothing and focus paint stays exact under it. */
        bool translation_only = parent->transform_scale_q14 == 16384
            && parent->individual_rotate_quadrants == 0
            && !parent->has_perspective;
        if (parent->opacity != 255
            || (parent->has_transform && !translation_only)
            || parent->has_filter)
            return false;
        *has_parent = true;
    }
    return true;
}

typedef struct {
    lxb_dom_node_t *node;
    ComputedStyle normal;
    ComputedStyle focused;
    StyleFocusChange change;
    StyleFocusPseudoTranslation translation;
    StyleFocusColorTexts texts;  /* COLOR_PAINT_ONLY */
    LayoutFocusColorRuns runs;   /* the dry run's owned text commands */
    bool focused_now;
    bool paint;
} NavigationFocusPaintCandidate;

static bool navigation_focus_paint_candidate(
    NavigationSession *session, NavigationFocusPaintCandidate *candidate,
    bool dry_run, int *left, int *top, int *right, int *bottom)
{
    const StyleFocusPseudoTranslation *t = &candidate->translation;
    bool translated = t->dx[0] || t->dx[1] || t->dy[0] || t->dy[1];
    bool valid = false;
    for (unsigned pass = 0; pass < 4; pass++) {
        int l, u, r, b;
        if (pass == 3) {
            if (candidate->change != STYLE_FOCUS_CHANGE_COLOR_PAINT_ONLY) continue;
            if (!layout_apply_focus_color_paint(&session->page.layout,
                    candidate->node,
                    candidate->focused_now ? &candidate->focused : &candidate->normal,
                    candidate->focused_now ? &candidate->normal : &candidate->focused,
                    &candidate->texts, &candidate->runs, dry_run,
                    &l, &u, &r, &b)) return false;
        } else if (pass == 0) {
            if (!translated) continue;
            if (!layout_apply_focus_pseudo_translation(&session->page.layout,
                    candidate->node, t, candidate->focused_now, dry_run,
                    &l, &u, &r, &b)) return false;
        } else if (pass == 1) {
            if (candidate->change != STYLE_FOCUS_CHANGE_BORDER_PAINT_ONLY
                && candidate->change != STYLE_FOCUS_CHANGE_INSET_PAINT_ONLY
                && candidate->change != STYLE_FOCUS_CHANGE_OUTSET_PAINT_ONLY) continue;
            if (!layout_apply_focus_border_paint(&session->page.layout,
                    &session->page.stylesheet, candidate->node,
                    candidate->focused_now ? &candidate->focused : &candidate->normal,
                    candidate->change, dry_run, &l, &u, &r, &b)) return false;
        } else {
            if (!t->visibility_changed) continue;
            if (!layout_apply_focus_pseudo_visibility(&session->page.layout,
                    candidate->node, t, candidate->focused_now, dry_run,
                    &l, &u, &r, &b)) return false;
        }
        if (!valid) { *left=l; *top=u; *right=r; *bottom=b; valid=true; }
        else {
            if (l < *left) *left = l;
            if (u < *top) *top = u;
            if (r > *right) *right = r;
            if (b > *bottom) *bottom = b;
        }
    }
    return valid;
}

enum { NAVIGATION_FOCUS_PAINT_CANDIDATE_LIMIT = 4 };
static bool navigation_layout_provisional(const NavigationSession *session);

/* Geometry reads need not rebuild paint. Even where a retained ring cannot
   be patched safely in z order, a local border/shadow/outline colour change
   leaves boxes and scrollable overflow intact. The publication path still
   sees the entire journal and performs its authoritative paint update. */
static bool navigation_focus_geometry_unchanged(
    NavigationSession *session, const ScriptMutationJournal *mutations)
{
    /* Unadmitted optional images have not changed the installed geometry.
       A pending publication or a journaled image change still blocks reuse. */
    if (session == NULL || !session->page.loaded || session->style_rebuild_required
        || session->page.visual_image_publication_pending
        || navigation_layout_provisional(session)
        || mutations == NULL || mutations->count == 0
        || mutations->count > NAVIGATION_FOCUS_PAINT_CANDIDATE_LIMIT
        || mutations->overflowed || mutations->retirement_unobserved
        || mutations->resource_rebuild_required
        || mutations->image_resource_scan_required
        || mutations->image_resource_refresh_required
        || mutations->conservative_resource_scan) return false;
    if (stylesheet_selectors_reference_attribute_prefix(
            &session->page.stylesheet, "data-tilefinch-focus",
            sizeof("data-tilefinch-focus") - 1u)) return false;
    /* Reject mixed author work before running any two-state style proof. */
    for (size_t i = 0; i < mutations->count; i++) {
        const ScriptMutationRecord *record = &mutations->records[i];
        if (record->kind != SCRIPT_MUTATION_ATTRIBUTE || record->node == NULL
            || strcmp(record->attribute, "data-tilefinch-focus") != 0)
            return false;
    }
    ComputedStyle parent = {0};
    lxb_dom_node_t *parent_of = NULL;
    bool has_parent = false;
    for (size_t i = 0; i < mutations->count; i++) {
        const ScriptMutationRecord *record = &mutations->records[i];
        if (parent_of == NULL || parent_of != record->node->parent) {
            parent_of = record->node->parent;
            if (!navigation_focus_parent_style(&session->page.stylesheet,
                    record->node, &parent, &has_parent)) return false;
        }
        ComputedStyle normal, focused;
        StyleFocusPseudoTranslation translation;
        StyleFocusChange change = style_focus_pseudo_change_classify(
            &session->page.stylesheet, record->node,
            has_parent ? &parent : NULL, &normal, &focused, &translation, NULL);
        if (change == STYLE_FOCUS_CHANGE_UNSAFE
            || change == STYLE_FOCUS_CHANGE_PSEUDO_TRANSLATION_ONLY
            || translation.visibility_changed
            || translation.dx[0] || translation.dx[1]
            || translation.dy[0] || translation.dy[1]) return false;
    }
    return true;
}

static bool navigation_mutations_are_paint_only_focus(
    NavigationSession *session, const ScriptMutationJournal *mutations,
    bool *border_paint, bool *local)
{
    if (border_paint != NULL) *border_paint = false;
    /* As for a semantic-only skip: a required rebuild is never skipped. */
    if (session == NULL || mutations == NULL || mutations->count == 0
        || session->style_rebuild_required
        || mutations->count > NAVIGATION_FOCUS_PAINT_CANDIDATE_LIMIT
        || mutations->overflowed || mutations->resource_rebuild_required
        || mutations->image_resource_scan_required
        || mutations->image_resource_refresh_required
        || mutations->conservative_resource_scan) return false;
    NavigationFocusPaintCandidate candidates[
        NAVIGATION_FOCUS_PAINT_CANDIDATE_LIMIT] = {0};
    lxb_dom_node_t *focused_node = NULL;
    ComputedStyle focused_style = {0};
    bool has_border_paint = false;
    /* The blurred and the focused control are usually siblings in one
       menu: resolve their shared ancestor chain once. */
    const lxb_dom_node_t *parent_of = NULL;
    ComputedStyle parent = {0};
    bool has_parent = false;
    for (size_t i = 0; i < mutations->count; i++) {
        const ScriptMutationRecord *record = &mutations->records[i];
        if (record->kind != SCRIPT_MUTATION_ATTRIBUTE
            || record->node == NULL
            || strcmp(record->attribute, "data-tilefinch-focus") != 0) {
            return false;
        }
        bool parent_known = parent_of != NULL
            && parent_of == record->node->parent;
        if (!parent_known) parent_of = record->node->parent;
        if (!parent_known
             && !navigation_focus_parent_style(
                    &session->page.stylesheet, record->node,
                    &parent, &has_parent)) return false;
        /* The pseudo-aware proof includes the ordinary paint proof. Do not
           repeat its two marker toggles, style queries and dependency scans
           after a non-pseudo refusal (geometry, inherited state, z-order). */
        candidates[i].change = style_focus_pseudo_change_classify(
                &session->page.stylesheet, record->node,
                has_parent ? &parent : NULL, &candidates[i].normal,
                &candidates[i].focused, &candidates[i].translation, &local[i]);
        if (candidates[i].change == STYLE_FOCUS_CHANGE_UNSAFE) return false;
        if (candidates[i].change == STYLE_FOCUS_CHANGE_COLOR_PAINT_ONLY
            && !style_focus_color_texts(&session->page.stylesheet,
                   record->node, &candidates[i].normal,
                   &candidates[i].focused, &candidates[i].texts))
            return false;
        candidates[i].node = record->node;
        size_t focus_length = 0;
        candidates[i].focused_now = document_attribute(
            record->node, "data-tilefinch-focus", &focus_length) != NULL;
        candidates[i].paint = candidates[i].change != STYLE_FOCUS_CHANGE_OUTLINE_ONLY
            || candidates[i].translation.visibility_changed
            || candidates[i].translation.dx[0] || candidates[i].translation.dx[1]
            || candidates[i].translation.dy[0] || candidates[i].translation.dy[1];
        if (candidates[i].paint) {
            int left, top, right, bottom;
            if (!navigation_focus_paint_candidate(session, &candidates[i], true,
                    &left, &top, &right, &bottom)) return false;
            has_border_paint = true;
        }
        if (candidates[i].focused_now) {
            focused_node = record->node;
            focused_style = candidates[i].focused;
        }
    }
    if (has_border_paint) {
        /* A pseudo translation moves text, which the colour apply would
           not see if it reused its dry run's runs: rescan then. */
        bool moves_text = false;
        for (size_t i = 0; i < mutations->count; i++) {
            const StyleFocusPseudoTranslation *t = &candidates[i].translation;
            moves_text = moves_text || (candidates[i].paint
                && (t->dx[0] || t->dx[1] || t->dy[0] || t->dy[1]));
        }
        for (size_t i = 0; moves_text && i < mutations->count; i++)
            candidates[i].runs.complete = false;
        bool damage_valid = false;
        int damage_left = 0, damage_top = 0, damage_right = 0, damage_bottom = 0;
        for (size_t i = 0; i < mutations->count; i++) {
            if (!candidates[i].paint) continue;
            int left = 0, top = 0, right = 0, bottom = 0;
            bool applied = navigation_focus_paint_candidate(session, &candidates[i], false,
                &left, &top, &right, &bottom);
            if (!applied) return false;
            if (!damage_valid) {
                damage_left = left;
                damage_top = top;
                damage_right = right;
                damage_bottom = bottom;
                damage_valid = true;
            } else {
                if (left < damage_left) damage_left = left;
                if (top < damage_top) damage_top = top;
                if (right > damage_right) damage_right = right;
                if (bottom > damage_bottom) damage_bottom = bottom;
            }
        }
        if (!damage_valid) return false;
        session->focus_paint_damage_valid = true;
        session->focus_paint_damage_left = damage_left;
        session->focus_paint_damage_top = damage_top;
        session->focus_paint_damage_right = damage_right;
        session->focus_paint_damage_bottom = damage_bottom;
        session->focus_paint_generation++;
        if (border_paint != NULL) *border_paint = true;
    }
    /* The retained styles and match lists of every element on either end
       of the move were resolved in the other state; a later relayout must
       not reuse them. An outline-only element's style is kept focus-neutral
       but its match lists are not. A recoloured element's descendants
       inherit the colour. */
    for (size_t i = 0; i < mutations->count; i++) {
        if (candidates[i].change == STYLE_FOCUS_CHANGE_COLOR_PAINT_ONLY)
            layout_reuse_cache_invalidate_local_focus(
                session->page.layout_reuse, candidates[i].node);
        else
            layout_reuse_cache_forget_focus_state(
                session->page.layout_reuse, candidates[i].node);
    }
    unsigned width = computed_style_outline_width(&focused_style);
    unsigned line_style = computed_style_outline_style(&focused_style);
    if (focused_node != NULL && width <= UINT8_MAX && line_style <= UINT8_MAX) {
        bool current_color =
            (focused_style.outline_state & STYLE_OUTLINE_CURRENT_COLOR) != 0;
        session->focus_outline_cache_node = focused_node;
        session->focus_outline_cache_stylesheet_generation =
            session->page.stylesheet.build_generation;
        session->focus_outline_cache_relayout_generation =
            session->incremental_relayouts;
        session->focus_outline_cache_color =
            current_color ? focused_style.color : focused_style.outline_color;
        session->focus_outline_cache_offset =
            computed_style_outline_offset(&focused_style);
        session->focus_outline_cache_alpha =
            current_color
              ? focused_style.color_alpha : focused_style.outline_alpha;
        session->focus_outline_cache_width = (uint8_t) width;
        session->focus_outline_cache_style = (uint8_t) line_style;
        /* Absence is a proved result too. Otherwise the controller repeats
           both marker states and their dependency scans merely to learn
           again that this control has no authored outline. */
        session->focus_outline_cache_valid = true;
    }
    return true;
}
