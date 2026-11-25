/* How far a DOM change reaches into the layout reuse cache: each case
   applies one change and its invalidation as the navigation journal does,
   then requires the reused build to equal a fresh one and bounds what the
   change dropped (the second build's style misses). The differential
   verifier gate (tests/run_invalidation_verify.py) runs them again with
   TILEFINCH_TRACE_INVALIDATION_VERIFY=1. */

typedef void (*InvalidationStep)(LayoutReuseCache *reuse,
                                 const Stylesheet *sheet,
                                 lxb_dom_node_t *root);

/* The second build's style misses after `step`, or SIZE_MAX when the
   reused build differs from a fresh one. The cache keeps matched-rule
   lists (`keep_lists`), as the page's cache does; a streaming preview's
   cache (`preview`) does not. */
static size_t invalidation_misses_in(Budget *budget, const char *label,
                                     const char *html, InvalidationStep step,
                                     bool keep_lists, bool preview)
{
    PocDocument document = {0};
    Stylesheet sheet = {0};
    ViewportContext viewport;
    if (!document_parse(&document, budget, html, strlen(html), 23)
        || !viewport_context_init(&viewport, 480, 272, 480, 272)
        || !stylesheet_build_context(&sheet, budget, &document, &viewport)) {
        printf("invalidation case %s: setup failed\n", label);
        return SIZE_MAX;
    }
    LayoutReuseCache *reuse = layout_reuse_cache_create(budget);
    if (reuse == NULL) return SIZE_MAX;
    if (keep_lists) layout_reuse_cache_enable_retained_matches(reuse);
    if (preview) layout_reuse_cache_mark_streaming_preview(reuse);
    LayoutDocument retained = {0}, fresh = {0};
    bool built = layout_build_context_reuse(&retained, budget, &document,
                                            &sheet, NULL, NULL, &viewport,
                                            reuse);
    layout_destroy(&retained);
    lxb_dom_node_t *body = document_body_node(&document);
    size_t misses = SIZE_MAX;
    if (built && body != NULL && body->parent != NULL) {
        step(reuse, &sheet, body->parent);
        layout_reuse_cache_flush_invalidations(reuse);
        LayoutReuseStats before, after;
        layout_reuse_cache_stats(reuse, &before);
        built = layout_build_context_reuse(&retained, budget, &document,
                                           &sheet, NULL, NULL, &viewport,
                                           reuse)
            && layout_build_context(&fresh, budget, &document, &sheet, NULL,
                                    NULL, &viewport);
        layout_reuse_cache_stats(reuse, &after);
        if (built && commands_equal(&retained, &fresh))
            misses = after.style_misses - before.style_misses;
        else printf("invalidation case %s diverged from a fresh build\n",
                    label);
        layout_destroy(&retained);
        layout_destroy(&fresh);
    }
    layout_reuse_cache_destroy(reuse);
    stylesheet_destroy(&sheet);
    document_destroy(&document);
    return misses;
}

static size_t invalidation_misses(Budget *budget, const char *label,
                                  const char *html, InvalidationStep step)
{
    return invalidation_misses_in(budget, label, html, step, true, false);
}

/* The element the cases write: the first one carrying data-t (its id is
   free to change). */
static lxb_dom_node_t *invalidation_target(lxb_dom_node_t *node)
{
    for (; node != NULL; node = node->next) {
        size_t length = 0;
        if (node->type == LXB_DOM_NODE_TYPE_ELEMENT
            && document_attribute(node, "data-t", &length) != NULL)
            return node;
        lxb_dom_node_t *found = invalidation_target(node->first_child);
        if (found != NULL) return found;
    }
    return NULL;
}

/* Sets attribute `name` of the target to `value` (NULL removes it). */
static void invalidation_write(lxb_dom_node_t *root, const char *name,
                               const char *value)
{
    lxb_dom_node_t *node = invalidation_target(root);
    if (node == NULL) return;
    if (value == NULL)
        (void) lxb_dom_element_remove_attribute(
            lxb_dom_interface_element(node), (const lxb_char_t *) name,
            strlen(name));
    else
        (void) lxb_dom_element_set_attribute(
            lxb_dom_interface_element(node), (const lxb_char_t *) name,
            strlen(name), (const lxb_char_t *) value, strlen(value));
}

/* A class change nothing invalidates: the verifier must report the stale
   descendant (expect=miss in the gate). */
static void invalidation_planted_step(LayoutReuseCache *reuse,
                                      const Stylesheet *sheet,
                                      lxb_dom_node_t *root)
{
    (void) reuse;
    (void) sheet;
    invalidation_write(root, "class", "on");
}

#define INVALIDATION_PLANTED_HTML \
    "<!doctype html><style>.on .leaf{color:#700001}</style><body>" \
    "<div data-t><p class=leaf>leaf</p></div></body>"

static int test_invalidation_verifier_planted_miss(Budget *budget)
{
    (void) invalidation_misses(budget, "planted miss",
                               INVALIDATION_PLANTED_HTML,
                               invalidation_planted_step);
    return 0;
}

/* The same miss in a streaming preview's cache counts apart from the bar
   (expect=streaming in the gate). */
static int test_invalidation_verifier_streaming_miss(Budget *budget)
{
    (void) invalidation_misses_in(budget, "streaming miss",
                                  INVALIDATION_PLANTED_HTML,
                                  invalidation_planted_step, false, true);
    return 0;
}

/* In any other cache it counts against the bar, retained lists or not
   (expect=miss; fails while the exemption keyed on a cache keeping no
   lists). */
static int test_invalidation_verifier_listless_miss(Budget *budget)
{
    (void) invalidation_misses_in(budget, "listless miss",
                                  INVALIDATION_PLANTED_HTML,
                                  invalidation_planted_step, false, false);
    return 0;
}

/* The pending write of the identity cases below. */
static struct {
    const char *name;
    const char *value;  /* NULL removes the attribute */
} invalidation_pending;

/* An attribute write on the target, its tokens and :has() classification
   taken as the bridge takes them (js_dom_set_attribute) and applied as the
   journal applies them (navigation/layout_reuse.inc). */
static void invalidation_identity_step(LayoutReuseCache *reuse,
                                       const Stylesheet *sheet,
                                       lxb_dom_node_t *root)
{
    lxb_dom_node_t *node = invalidation_target(root);
    if (node == NULL) return;
    const char *name = invalidation_pending.name;
    const char *value = invalidation_pending.value;
    size_t name_length = strlen(name), old_length = 0;
    const lxb_char_t *old = lxb_dom_element_get_attribute(
        lxb_dom_interface_element(node), (const lxb_char_t *) name,
        name_length, &old_length);
    char before[512];
    snprintf(before, sizeof(before), "%.*s", (int) old_length,
             old == NULL ? "" : (const char *) old);
    const char *old_value = old == NULL ? NULL : before;
    uint32_t tokens[32];
    size_t count = 0;
    bool exact = stylesheet_attribute_change_tokens(
        sheet, name, name_length, old_value, old_length, value,
        value == NULL ? 0 : strlen(value), tokens, 32, &count);
    bool relational = stylesheet_attribute_change_may_affect_has(
        sheet, name, name_length, old_value, old_length, value,
        value == NULL ? 0 : strlen(value));
    invalidation_write(root, name, value);
    if (exact)
        layout_reuse_cache_invalidate_attribute(reuse, node, tokens, count,
                                                false, relational);
    else if (relational)
        layout_reuse_cache_invalidate_relational(reuse, node, name, false);
    else layout_reuse_cache_invalidate_own_attribute(reuse, node, name);
}

/* Eight leaves under the target (and a sibling after it) restyle only
   through `rules`. */
#define INVALIDATION_LEAVES \
    "<p class=leaf>1</p><p class=leaf>2</p><p class=leaf>3</p>" \
    "<p class=leaf>4</p><p class=leaf>5</p><p class=leaf>6</p>" \
    "<p class=leaf>7</p><p class=leaf>8</p>"

static size_t identity_case(Budget *budget, const char *label,
                            const char *rules, const char *start,
                            const char *name, const char *value)
{
    char html[16384];
    snprintf(html, sizeof(html),
             "<!doctype html><style>%s</style><body><div data-t %s>"
             INVALIDATION_LEAVES "</div><p class=sib>s</p></body>",
             rules, start);
    invalidation_pending.name = name;
    invalidation_pending.value = value;
    return invalidation_misses(budget, label, html,
                               invalidation_identity_step);
}

/* A partial [class]/[id] test off the subject reaches its rule's subjects
   only when its answer flips between the old and the new value. `reach`
   0: the change leaves the test's answer alone, so only the target
   restyles (its leaves re-key off its unchanged style); otherwise at least
   `reach` styles restyle, and the reused build still equals a fresh one
   either way. The reach-0 cases fail on the empty-name token alone. */
static int test_partial_identity_tests_flip(Budget *budget)
{
    static const struct {
        const char *label, *rules, *start, *name, *value;
        size_t reach;
    } cases[] = {
        {"id prefix kept", "[id^=pre] .leaf{color:#710001}", "id=t",
         "id", "other", 0},
        {"id prefix on", "[id^=pre] .leaf{color:#710002}", "id=t",
         "id", "prefixed", 8},
        {"id prefix off", "[id^=pre] .leaf{color:#710003}",
         "id=prefixed", "id", "t", 8},
        {"class substring kept", "[class*=dis] .leaf{color:#710004}",
         "id=t class=a", "class", "a b", 0},
        {"class substring on", "[class*=dis] .leaf{color:#710005}",
         "id=t class=a", "class", "a is-disabled", 8},
        {"class substring off", "[class*=dis] .leaf{color:#710006}",
         "id=t class='a disabled'", "class", "a", 8},
        {"class suffix", "[class$=end] .leaf{color:#710007}",
         "id=t class=a", "class", "a-end", 8},
        {"class suffix kept", "[class$=end] .leaf{color:#710008}",
         "id=t class=a", "class", "end-a", 0},
        {"class dash", "[class|=lang] .leaf{color:#710009}",
         "id=t class=a", "class", "lang-x", 8},
        {"class dash kept", "[class|=lang] .leaf{color:#71000a}",
         "id=t class=a", "class", "language", 0},
        {"class whole value", "[class='a b'] .leaf{color:#71000b}",
         "id=t class=a", "class", "a b", 8},
        {"class whole value kept", "[class='a b'] .leaf{color:#71000c}",
         "id=t class=a", "class", "b a", 0},
        {"class word flag", "[class~=ON i] .leaf{color:#71000d}",
         "id=t class=a", "class", "a on", 8},
        {"case flag", "[class*=DIS i] .leaf{color:#71000e}",
         "id=t class=a", "class", "xdisy", 8},
        {"case kept without flag", "[class*=DIS] .leaf{color:#71000f}",
         "id=t class=a", "class", "xdisy", 0},
        {"id dash", "[id|=nav] .leaf{color:#710010}", "id=t",
         "id", "nav-top", 8},
        {"id exact flag", "[id=T2 i] .leaf{color:#710011}", "id=t",
         "id", "t2", 8},
        {"id exact flag kept", "[id=T2 i] .leaf{color:#710012}", "id=t",
         "id", "t3", 0},
        {"sibling", "[class*=on] + .sib{color:#710013}", "id=t class=a",
         "class", "on", 2},
        {"sibling kept", "[class*=on] + .sib{color:#710014}",
         "id=t class=a", "class", "b", 0},
        {"removal", "[class^=a] .leaf{color:#710015}", "id=t class=a",
         "class", NULL, 8},
        {"quoted escape", "[class*='d\\69s'] .leaf{color:#710016}",
         "id=t class=a", "class", "dis", 8},
    };
    int failed = 0;
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        size_t misses = identity_case(budget, cases[i].label,
                                      cases[i].rules, cases[i].start,
                                      cases[i].name, cases[i].value);
        bool ok = misses != SIZE_MAX
            && (cases[i].reach == 0 ? misses <= 1u
                                    : misses >= cases[i].reach);
        if (!ok) {
            printf("partial identity %s: misses=%zu\n", cases[i].label,
                   misses == SIZE_MAX ? (size_t) 0 : misses);
            failed = 1;
        }
    }
    CHECK(failed == 0);
    return 0;
}

/* An empty value is not an absent one to [class]/[id], although it carries
   no token: adding class="" or id="" must restyle what reads them. */
static int test_identity_presence_reaches(Budget *budget)
{
    int failed = 0;
    size_t misses = identity_case(budget, "class presence",
        "[class] .leaf{color:#720001}", "id=t", "class", "");
    failed |= misses == SIZE_MAX || misses < 8u;
    misses = identity_case(budget, "class presence gone",
        ":not([class]) > .leaf{color:#720002}", "id=t class=''", "class",
        NULL);
    failed |= misses == SIZE_MAX || misses < 8u;
    misses = identity_case(budget, "id presence",
        "[id] .leaf{color:#720003}", "class=x", "id", "");
    failed |= misses == SIZE_MAX || misses < 8u;
    misses = identity_case(budget, "empty id value",
        "[id=\"\"] .leaf{color:#720004}", "class=x", "id", "");
    failed |= misses == SIZE_MAX || misses < 8u;
    CHECK(failed == 0);
    return 0;
}

/* Past the bounds (more partial tests than the table holds) and inside a
   :has() argument the empty-name token keeps standing for every test: the
   change is still exact, only wider. */
static int test_partial_identity_fallbacks(Budget *budget)
{
    char rules[16384];
    size_t used = 0;
    for (unsigned i = 0; i < 300 && used + 64 < sizeof(rules); i++)
        used += (size_t) snprintf(rules + used, sizeof(rules) - used,
                                  "[class*=k%u] .leaf{order:%u}", i, i);
    snprintf(rules + used, sizeof(rules) - used,
             "[class*=dis] .leaf{color:#730001}");
    size_t misses = identity_case(budget, "past the row bound", rules,
                                  "id=t class=a", "class", "a b");
    CHECK(misses != SIZE_MAX && misses >= 8u);
    misses = identity_case(budget, "past the row bound flips", rules,
                           "id=t class=a", "class", "dis");
    CHECK(misses != SIZE_MAX && misses >= 8u);
    /* Without the sheet a change cannot name its tests' tokens. */
    uint32_t tokens[8];
    size_t count = 0;
    CHECK(!stylesheet_attribute_change_tokens(NULL, "class", 5, "a", 1,
                                              "b", 1, tokens, 8, &count));
    /* An escaped name the matcher decodes to class (fails while the
       scan read only the unescaped run and registered it under neither
       kind). */
    misses = identity_case(budget, "escaped name",
        "[cl\\61ss*=dis] .leaf{color:#730004}", "id=t class=a", "class",
        "dis");
    CHECK(misses != SIZE_MAX && misses >= 8u);
    misses = identity_case(budget, "escaped id name",
        "[\\69 d^=on] .leaf{color:#730005}", "class=x", "id", "on1");
    CHECK(misses != SIZE_MAX && misses >= 8u);
    misses = identity_case(budget, "inside :has()",
        "body:has([class*=dis]) .leaf{color:#730002}", "id=t class=a",
        "class", "dis");
    CHECK(misses != SIZE_MAX);
    misses = identity_case(budget, "inside :has() kept",
        "body:has([class*=dis]) .leaf{color:#730003}", "id=t class=a",
        "class", "b");
    CHECK(misses != SIZE_MAX);
    return 0;
}

static size_t attribute_case(Budget *budget, const char *label,
                             const char *html, const char *name,
                             const char *value)
{
    invalidation_pending.name = name;
    invalidation_pending.value = value;
    return invalidation_misses(budget, label, html,
                               invalidation_identity_step);
}

/* The attributes the resolver or a pseudo-class reads on other elements
   reach past the element only on the element kinds that read them: on a
   div, `name`, `value`, `type`, `href`, `checked` and `colspan` restyle the
   div alone (fails on the 50-name list, which dropped the subtree). */
static int test_attribute_reach_by_kind_narrows(Budget *budget)
{
    static const char *const names[] = {
        "name", "value", "type", "href", "checked", "colspan", "min"
    };
    int failed = 0;
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
        size_t misses = attribute_case(budget, names[i],
            "<!doctype html><style>.leaf{color:#740001}</style><body>"
            "<div data-t>" INVALIDATION_LEAVES "</div></body>",
            names[i], "x");
        if (misses == SIZE_MAX || misses > 1u) {
            printf("attribute reach %s on a div: misses=%zu\n", names[i],
                   misses == SIZE_MAX ? (size_t) 0 : misses);
            failed = 1;
        }
    }
    CHECK(failed == 0);
    return 0;
}

/* Where they are read on other elements the reach stays, and the reused
   build equals a fresh one: list numbering, table spans, details and
   :open, fieldset and select state, a radio's name, :link. */
static int test_attribute_reach_by_kind_keeps(Budget *budget)
{
    static const struct {
        const char *label, *html, *name, *value;
    } cases[] = {
        {"li value", "<ol><li data-t>a<li>b<li>c</ol>", "value", "7"},
        {"ol start", "<ol data-t><li>a<li>b</ol>", "start", "4"},
        {"td colspan", "<table border=1><tr><td data-t>a<td>b<tr><td>c"
         "<td>d</table>", "colspan", "2"},
        {"col span", "<table><colgroup><col data-t style='width:40px'>"
         "<col style='width:90px'></colgroup><tr><td>a<td>b</table>",
         "span", "2"},
        {"details open", "<details data-t><summary>s</summary><p>body"
         "</details>", "open", ""},
        {"open on a div", "<style>:open + p{color:#740002}</style>"
         "<div data-t>d</div><p>after</p>", "open", ""},
        {"fieldset disabled", "<style>input:disabled{width:30px}</style>"
         "<fieldset data-t><input><input></fieldset>", "disabled", ""},
        {"select multiple", "<select data-t><option>a<option>b</select>",
         "multiple", ""},
        {"input checked", "<style>:checked + span{color:#740003}</style>"
         "<input type=checkbox data-t><span>s</span>", "checked", ""},
        {"radio name", "<input type=radio name=g checked>"
         "<input type=radio data-t>", "name", "g"},
        {"a href", "<style>:link + span{color:#740004}</style>"
         "<a data-t>a</a><span>s</span>", "href", "#x"},
    };
    int failed = 0;
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        char html[1024];
        snprintf(html, sizeof(html), "<!doctype html><body>%s</body>",
                 cases[i].html);
        if (attribute_case(budget, cases[i].label, html, cases[i].name,
                           cases[i].value) == SIZE_MAX)
            failed = 1;
    }
    CHECK(failed == 0);
    return 0;
}

/* The innerHTML replacement of the cases below: the target's children
   give way to `count` chains `depth` elements deep, the innermost with
   class `leaf_class` (NULL: none). */
static struct {
    const char *leaf_class;
    unsigned depth, count;
} invalidation_inner;

static lxb_dom_node_t *invalidation_element(lxb_dom_node_t *root,
                                            const char *class_name)
{
    lxb_dom_element_t *element = lxb_dom_document_create_element(
        root->owner_document, (const lxb_char_t *) "span", 4, NULL);
    if (element == NULL) return NULL;
    if (class_name != NULL)
        (void) lxb_dom_element_set_attribute(
            element, (const lxb_char_t *) "class", 5,
            (const lxb_char_t *) class_name, strlen(class_name));
    return lxb_dom_interface_node(element);
}

/* As bridge_replace_inner_html probes and journals it, and as the
   navigation journal applies it (node retirement on). */
static void invalidation_inner_html_step(LayoutReuseCache *reuse,
                                         const Stylesheet *sheet,
                                         lxb_dom_node_t *root)
{
    lxb_dom_node_t *node = invalidation_target(root);
    if (node == NULL) return;
    uint64_t entries = UINT64_MAX;
    uint32_t serial = 0;
    bool relational = stylesheet_children_change_has_entries(
        sheet, node, &entries, &serial);
    while (node->first_child != NULL) {
        lxb_dom_node_t *child = node->first_child;
        layout_reuse_cache_retire_subtree(reuse, child);
        lxb_dom_node_remove(child);
        lxb_dom_node_destroy_deep(child);
    }
    for (unsigned c = 0; c < invalidation_inner.count; c++) {
        lxb_dom_node_t *parent = node;
        for (unsigned d = 0; d < invalidation_inner.depth; d++) {
            lxb_dom_node_t *element = invalidation_element(
                root, d + 1 == invalidation_inner.depth
                          ? invalidation_inner.leaf_class : NULL);
            if (element == NULL
                || lxb_dom_node_append_child(parent, element)
                       != LXB_DOM_EXCEPTION_OK) return;
            parent = element;
        }
    }
    if (serial != 0)
        relational = stylesheet_children_change_has_entries(
            sheet, node, &entries, &serial) || relational;
    if (serial == 0) {
        relational = true;
        entries = UINT64_MAX;
    }
    layout_reuse_cache_set_structure_filter(reuse, entries, serial);
    layout_reuse_cache_invalidate_children(reuse, node, NULL, relational);
    layout_reuse_cache_set_structure_filter(reuse, UINT64_MAX, 0);
}

static size_t inner_html_case(Budget *budget, const char *label,
                              const char *old_content,
                              const char *leaf_class, unsigned depth,
                              unsigned count)
{
    char html[2048];
    snprintf(html, sizeof(html),
             "<!doctype html><style>.a:has(.x) .leaf{color:#750001}"
             ".a:has(> .y) .leaf{color:#750002}</style><body>"
             "<div class=a><div data-t>%s</div>" INVALIDATION_LEAVES
             "</div></body>", old_content);
    invalidation_inner.leaf_class = leaf_class;
    invalidation_inner.depth = depth;
    invalidation_inner.count = count;
    return invalidation_misses(budget, label, html,
                               invalidation_inner_html_step);
}

/* An innerHTML write reaches the :has() rules whose arguments its old or
   new children can match, not every rule anchored above it: content no
   argument matches leaves the anchor's subjects (the eight leaves) alone
   (fails while innerHTML records named every entry). Past the probe's
   element bound it stays conservative. */
static int test_inner_html_has_probe(Budget *budget)
{
    int failed = 0;
    size_t misses = inner_html_case(budget, "unrelated content",
                                    "<span>old</span>", NULL, 2, 2);
    if (misses == SIZE_MAX || misses > 6u) {
        printf("innerHTML unrelated content: misses=%zu\n",
               misses == SIZE_MAX ? (size_t) 0 : misses);
        failed = 1;
    }
    static const struct {
        const char *label, *old_content, *leaf_class;
        unsigned depth, count;
    } reached[] = {
        {"adds an argument", "<span>old</span>", "x", 1, 1},
        {"adds it nested", "<span>old</span>", "x", 4, 1},
        {"removes an argument", "<span class=x>old</span>", NULL, 1, 1},
        {"removes it nested", "<i><b><span class=x>old</span></b></i>",
         NULL, 1, 1},
    };
    for (size_t i = 0; i < sizeof(reached) / sizeof(reached[0]); i++) {
        misses = inner_html_case(budget, reached[i].label,
                                 reached[i].old_content,
                                 reached[i].leaf_class, reached[i].depth,
                                 reached[i].count);
        if (misses == SIZE_MAX || misses < 8u) {
            printf("innerHTML %s: misses=%zu\n", reached[i].label,
                   misses == SIZE_MAX ? (size_t) 0 : misses);
            failed = 1;
        }
    }
    /* A child argument (> .y) one level down is not the anchor's child. */
    misses = inner_html_case(budget, "child argument below the anchor",
                             "<span>old</span>", "y", 1, 1);
    failed |= misses == SIZE_MAX;
    /* 600 new elements: past the bound, every entry is noted. */
    misses = inner_html_case(budget, "past the bound", "<span>old</span>",
                             NULL, 1, 600);
    if (misses == SIZE_MAX || misses < 8u) {
        printf("innerHTML past the bound: misses=%zu\n",
               misses == SIZE_MAX ? (size_t) 0 : misses);
        failed = 1;
    }
    CHECK(failed == 0);
    return 0;
}
