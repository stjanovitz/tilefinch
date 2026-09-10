#include "tilefinch/budget.h"
#include "tilefinch/document.h"
#include "tilefinch/layout.h"
#include "tilefinch/render.h"
#include "tilefinch/style.h"
#include "../src/style_cache_internal.h"
#include "../src/style_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MIB (1024u * 1024u)
#define CHECK(condition) do {                                                \
    if (!(condition)) {                                                      \
        fprintf(stderr, "style-index test failed at %s:%d: %s\n",          \
                __FILE__, __LINE__, #condition);                             \
        return 1;                                                            \
    }                                                                        \
} while (0)

static lxb_dom_node_t *find_id(lxb_dom_node_t *node, const char *wanted)
{
    if (node == NULL) return NULL;
    if (node->type == LXB_DOM_NODE_TYPE_ELEMENT) {
        size_t length = 0;
        const char *id = document_attribute(node, "id", &length);
        if (id != NULL && strlen(wanted) == length
            && memcmp(id, wanted, length) == 0) return node;
    }
    for (lxb_dom_node_t *child = node->first_child; child != NULL;
         child = child->next) {
        lxb_dom_node_t *found = find_id(child, wanted);
        if (found != NULL) return found;
    }
    return NULL;
}

static const StyleRule *find_rule(const Stylesheet *sheet,
                                  const char *selector)
{
    if (sheet == NULL || selector == NULL) return NULL;
    for (size_t i = 0; i < sheet->count; i++) {
        if (strcmp(sheet->rules[i].selector, selector) == 0) {
            return &sheet->rules[i];
        }
    }
    return NULL;
}

static const StyleCustomRule *find_custom_rule(
    const Stylesheet *sheet, const char *selector, const char *name)
{
    if (sheet == NULL || selector == NULL || name == NULL) return NULL;
    for (size_t i = 0; i < sheet->custom_rule_count; i++) {
        if (strcmp(sheet->custom_rules[i].selector, selector) == 0
            && strcmp(sheet->custom_rules[i].name, name) == 0) {
            return &sheet->custom_rules[i];
        }
    }
    return NULL;
}

static bool equivalent(const Stylesheet *left_sheet,
                       const ComputedStyle *left,
                       const Stylesheet *right_sheet,
                       const ComputedStyle *right)
{
    const char *left_image = left->background_image == NULL
        ? "" : left->background_image;
    const char *right_image = right->background_image == NULL
        ? "" : right->background_image;
    const StyleGradient *left_gradient = stylesheet_background_gradient(
        left_sheet, left);
    const StyleGradient *right_gradient = stylesheet_background_gradient(
        right_sheet, right);
    bool gradients_equal = left_gradient == NULL || right_gradient == NULL
        ? left_gradient == right_gradient
        : memcmp(left_gradient, right_gradient, sizeof(*left_gradient)) == 0;
    return left->display == right->display
        && left->color == right->color
        && left->letter_spacing == right->letter_spacing
        && left->word_spacing == right->word_spacing
        && left->padding.top == right->padding.top
        && left->padding.right == right->padding.right
        && left->padding.bottom == right->padding.bottom
        && left->padding.left == right->padding.left
        && strcmp(left_image, right_image) == 0
        /* The discriminant and shared gradient have to match too, not just
           the scalar URL slot. */
        && left->background_image_kind == right->background_image_kind
        && gradients_equal;
}

static uint16_t rgb565(uint32_t color)
{
    return (uint16_t) ((((color >> 16) & 0xffu) >> 3) << 11
                       | (((color >> 8) & 0xffu) >> 2) << 5
                       | ((color & 0xffu) >> 3));
}

static uint64_t frame_hash(const uint16_t *pixels, size_t count)
{
    uint64_t hash = UINT64_C(1469598103934665603);
    for (size_t i = 0; i < count; i++) {
        hash ^= pixels[i];
        hash *= UINT64_C(1099511628211);
    }
    return hash;
}

static bool cache_test_cooperate(void *opaque, lxb_dom_node_t *node, size_t visits)
{
    (void) opaque;
    (void) node;
    (void) visits;
    return true;
}

static int test_ancestor_filter_canonical_tokens(void)
{
    Budget budget;
    budget_init(&budget, 8u * MIB);
    CHECK(budget_install_lexbor(&budget));
    PocDocument document = {0};
    static const char html[] = "<div class=root><aside class=guard></aside>"
        "<section class=branch><i></i><span id=probe class=leaf></span>"
        "</section></div>";
    CHECK(document_parse(&document, &budget, html, sizeof(html) - 1u, 17));
    Stylesheet sheet = {0};
    CHECK(stylesheet_build(&sheet, &budget, &document, 480));
    static const char css[] = "section .leaf{color:#123456}"
        ".root .guard + .branch .leaf{padding-top:7px}"
        ".root .guard ~ .branch > i + .leaf{margin-top:9px}"
        ".missing .branch > i + .leaf{color:red}"
        ".root .absent + .branch .leaf{color:red}";
    CHECK(stylesheet_add_css(&sheet, css, sizeof(css) - 1u));
    lxb_dom_node_t *node = find_id(lxb_dom_interface_node(document.html), "probe");
    CHECK(node != NULL);
    ComputedStyle actual = style_for_node(&sheet, node, NULL);
    CHECK(actual.color == 0x123456 && actual.padding.top == 7
        && actual.margin.top == 9 && sheet.rule_filters != NULL);
    const StyleRule *tag = find_rule(&sheet, "section .leaf");
    const StyleRule *sibling = find_rule(&sheet, ".root .guard + .branch .leaf");
    const StyleRule *mixed = find_rule(&sheet, ".root .guard ~ .branch > i + .leaf");
    CHECK(tag != NULL && sibling != NULL && mixed != NULL);
    StyleTokenBloom expected_tag = style_compound_token_bloom(
        STYLE_SELECTOR_TAG, "section", 7);
    StyleTokenBloom expected_classes = style_compound_token_bloom(
        STYLE_SELECTOR_CLASS, "root", 4);
    style_token_bloom_merge(&expected_classes, style_compound_token_bloom(
        STYLE_SELECTOR_CLASS, "branch", 6));
    /* Numeric compiled tags and generic tag predicates must reserve the same
       bits. Sibling tokens are excluded, but their shared ancestors survive. */
    CHECK(memcmp(&sheet.rule_filters[tag - sheet.rules].ancestors,
        &expected_tag, sizeof(expected_tag)) == 0);
    CHECK(memcmp(&sheet.rule_filters[sibling - sheet.rules].ancestors,
        &expected_classes, sizeof(expected_classes)) == 0);
    CHECK(memcmp(&sheet.rule_filters[mixed - sheet.rules].ancestors,
        &expected_classes, sizeof(expected_classes)) == 0);
    uint64_t rejected = sheet.rule_ancestor_filter_rejections;
    CHECK(rejected != 0);
    /* Disabling only admission must leave every resolved value identical. */
    sheet.rule_ancestor_filter_active = false;
    ComputedStyle unfiltered = style_for_node(&sheet, node, NULL);
    CHECK(memcmp(&actual, &unfiltered, sizeof(actual)) == 0);
    CHECK(sheet.rule_ancestor_filter_rejections == rejected);
    stylesheet_destroy(&sheet);
    document_destroy(&document);
    CHECK(budget.current == 0);
    return 0;
}


/* Attribute tests and argument-less pseudo-classes compile to instructions
   that must agree with the string matcher on every node, and rules naming
   an unimplemented pseudo-element must not survive to matching. */
static int test_compiled_attribute_and_pseudo_instructions(void)
{
    Budget budget;
    budget_init(&budget, 8u * MIB);
    CHECK(budget_install_lexbor(&budget));
    PocDocument document = {0};
    static const char html[] = "<style>"
        "a[href]{color:#111111}"
        "a[rel~='mw:referencedBy']{color:#222222}"
        "[type=\"submit\"]{color:#333333}"
        "p:last-child{color:#444444}"
        "a.k:not(.other)[href]{color:#555555}"
        "a:hover{color:#666666}"
        "[data-kind^=note]{margin-top:3px}"
        "input:disabled{color:#777777}"
        "[title i]{color:#888888}"
        "[title=title i]{color:#999999}"
        "[lang|=en]{color:#aaaaaa}"
        "[type='submit']::-moz-focus-inner{color:#bbbbbb}"
        "a:first-child{padding-top:2px}"
        "p:first-child{padding-top:9px}"
        "</style><div id=root data-kind=\"note book\">"
        "<a id=a1 href=/x rel=\"nofollow mw:referencedBy\" class=k>a</a>"
        "<a id=a2 class=\"k other\" title=Title>b</a>"
        "<input id=i1 type=submit disabled><p id=p1 lang=en-US>t</p></div>";
    CHECK(document_parse(&document, &budget, html, sizeof(html) - 1u, 17));
    Stylesheet sheet = {0};
    CHECK(stylesheet_build(&sheet, &budget, &document, 480));
    size_t before = 0;
    (void) unsetenv("TILEFINCH_DISABLE_COMPILED_SELECTORS");
    (void) unsetenv("TILEFINCH_DISABLE_COMPILED_COMPLEX_SELECTORS");
    stylesheet_prepare_selector_program(&sheet);
    /* Thirteen matchable rules; the vendor pseudo-element rule is dropped. */
    CHECK(sheet.count == 13u && sheet.selector_program_ready
          && sheet.selector_program_offsets != NULL
          && find_rule(&sheet, "[type='submit']::-moz-focus-inner") == NULL);
    lxb_dom_node_t *root = lxb_dom_interface_node(document.html);
    lxb_dom_node_t *nodes[5] = {
        find_id(root, "root"), find_id(root, "a1"), find_id(root, "a2"),
        find_id(root, "i1"), find_id(root, "p1") };
    for (size_t i = 0; i < 5; i++) CHECK(nodes[i] != NULL);
    for (size_t r = before; r < sheet.count; r++) {
        const StyleRule *rule = &sheet.rules[r];
        for (size_t i = 0; i < 5; i++) {
            bool compiled = style_rule_selector_matches(&sheet, r, nodes[i]);
            bool textual = style_selector_matches_profiled(
                &sheet, nodes[i], rule->selector, rule->selector_length);
            if (compiled != textual)
                fprintf(stderr, "mismatch rule=%.*s node=%zu compiled=%d text=%d\n",
                        (int) rule->selector_length, rule->selector, i,
                        compiled, textual);
            CHECK(compiled == textual);
        }
    }
    ComputedStyle a1 = style_for_node(&sheet, nodes[1], NULL);
    ComputedStyle a2 = style_for_node(&sheet, nodes[2], NULL);
    ComputedStyle i1 = style_for_node(&sheet, nodes[3], NULL);
    ComputedStyle p1 = style_for_node(&sheet, nodes[4], NULL);
    ComputedStyle rootstyle = style_for_node(&sheet, nodes[0], NULL);
    CHECK(a1.color == 0x555555 && a1.padding.top == 2
          && a2.color == 0x999999 && a2.padding.top == 0
          && i1.color == 0x777777 && p1.color == 0x444444
          && p1.padding.top == 0 && rootstyle.margin.top == 3);
    /* The compiled forms: presence, name+word, tag+class then a text
       suffix, and a pseudo instruction carrying its kind. */
    const StyleRule *href = find_rule(&sheet, "a[href]");
    const StyleRule *word = find_rule(&sheet, "a[rel~='mw:referencedBy']");
    const StyleRule *suffix = find_rule(&sheet, "a.k:not(.other)[href]");
    const StyleRule *last = find_rule(&sheet, "p:last-child");
    CHECK(href != NULL && word != NULL && suffix != NULL && last != NULL);
    const StyleSelectorInstruction *ops = sheet.selector_program;
    uint16_t at = sheet.selector_program_offsets[href - sheet.rules];
    CHECK(ops[at].opcode == STYLE_SELECTOR_TAG_ID
          && ops[at + 1].opcode == STYLE_SELECTOR_ATTRIBUTE_PRESENT
          && ops[at + 1].text_length == 4
          && ops[at + 2].opcode == STYLE_SELECTOR_END);
    at = sheet.selector_program_offsets[word - sheet.rules];
    CHECK(ops[at + 1].opcode == STYLE_SELECTOR_ATTRIBUTE_NAME
          && ops[at + 2].opcode == STYLE_SELECTOR_ATTRIBUTE_WORD
          && ops[at + 2].text_length == sizeof("mw:referencedBy") - 1u
          && ops[at + 3].opcode == STYLE_SELECTOR_END);
    at = sheet.selector_program_offsets[suffix - sheet.rules];
    CHECK(ops[at].opcode == STYLE_SELECTOR_TAG_ID
          && ops[at + 1].opcode == STYLE_SELECTOR_CLASS
          && ops[at + 2].opcode == STYLE_SELECTOR_COMPOUND
          && memcmp(suffix->selector + ops[at + 2].text_offset,
                    ":not(.other)[href]", ops[at + 2].text_length) == 0
          && ops[at + 3].opcode == STYLE_SELECTOR_END);
    at = sheet.selector_program_offsets[last - sheet.rules];
    CHECK(ops[at + 1].opcode == STYLE_SELECTOR_PSEUDO
          && ops[at + 1].text_offset == STYLE_PSEUDO_LAST
          && ops[at + 2].opcode == STYLE_SELECTOR_END);
    stylesheet_destroy(&sheet);
    document_destroy(&document);
    CHECK(budget.current == 0);
    return 0;
}

static int test_pseudo_state_work(void)
{
    Budget budget;
    budget_init(&budget, 8u * MIB);
    CHECK(budget_install_lexbor(&budget));
    PocDocument document = {0};
    static const char html[] = "<fieldset disabled><legend><input id=exempt>"
        "</legend><input id=disabled required checked></fieldset>"
        "<input id=optional><dialog id=modal open data-tilefinch-modal></dialog>"
        "<div id=plain class='a:b hot' data-tilefinch-popover-open></div>";
    CHECK(document_parse(&document, &budget, html, sizeof(html) - 1u, 17));
    Stylesheet sheet = {0};
    CHECK(stylesheet_build(&sheet, &budget, &document, 480));
    static const struct { const char *id; const char *selector; bool matches; } cases[] = {
        {"disabled", ":disabled:required:checked", true},
        {"disabled", ":enabled", false},
        {"exempt", ":enabled:optional", true},
        {"optional", ":enabled:optional", true},
        {"optional", ":required", false},
        {"plain", ":enabled", false},
        {"plain", ":optional", false},
        {"plain", ":open:popover-open", true},
        {"plain", ":modal", false},
        {"modal", ":open:modal", true},
        {"modal", ":popover-open", false},
        {"plain", ".a\\:b:is(.hot):not(.cold):empty", true},
        {"plain", ".a\\00003ab:not(.cold)", true},
        {"plain", ":unknown", false},
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        lxb_dom_node_t *node = find_id(lxb_dom_interface_node(document.html), cases[i].id);
        CHECK(node != NULL);
        bool matched = style_selector_matches_profiled(&sheet, node,
            cases[i].selector, strlen(cases[i].selector));
        if (matched != cases[i].matches) {
            fprintf(stderr, "pseudo fixture %s %s: got %d expected %d\n",
                cases[i].id, cases[i].selector, matched, cases[i].matches);
        }
        CHECK(matched == cases[i].matches);
    }
    CHECK(sheet.selector_pseudo_state_checks != 0);
    uint64_t state_checks = sheet.selector_pseudo_state_checks;
    lxb_dom_node_t *node = find_id(lxb_dom_interface_node(document.html), "plain");
    static const char unrelated[] = ".hot:is(div):not(.cold):empty";
    for (unsigned i = 0; i < 100; i++) {
        CHECK(style_selector_matches_profiled(&sheet, node, unrelated,
            sizeof(unrelated) - 1u));
    }
    /* Structural/functional predicates must never inspect unrelated form or
       open state. This work assertion is deterministic, unlike a timer floor. */
    CHECK(sheet.selector_pseudo_state_checks == state_checks);
    stylesheet_destroy(&sheet);
    document_destroy(&document);
    CHECK(budget.current == 0);
    return 0;
}

static int test_layout_scoped_selector_work(void)
{
    Budget budget;
    budget_init(&budget, 16u * MIB);
    budget_install_lexbor(&budget);
    PocDocument document = {0};
    static const char html[] = "<section class=outer><div class=inner>"
        "<a id=probe>link</a></div></section>";
    CHECK(document_parse(&document, &budget, html, sizeof(html) - 1u, 17));
    lxb_dom_node_t *node = find_id(lxb_dom_interface_node(document.html), "probe");
    Stylesheet sheet = {0};
    CHECK(node != NULL && stylesheet_build(&sheet, &budget, &document, 480));
    for (unsigned i = 0; i < 80; i++) {
        char css[128];
        int length = snprintf(css, sizeof(css),
            ".outer .inner a{color:#%06x}", i + 1u);
        CHECK(length > 0 && stylesheet_add_css(&sheet, css, (size_t) length));
    }
    static const char pseudo_css[] = "*::before{content:'A';color:red}"
        "a::before{color:blue;font-size:2em}*::after{content:'Z';color:green}"
        "a:focus::before{color:white}"
        ".outer .inner a:focus{color:white}";
    CHECK(stylesheet_add_css(&sheet, pseudo_css, sizeof(pseudo_css) - 1u));
    ComputedStyle parent = style_for_node(&sheet, node->parent, NULL);
    uint64_t candidates = sheet.rule_index_candidates;
    ComputedStyle before = style_for_pseudo(&sheet, node, PSEUDO_BEFORE, &parent);
    ComputedStyle after = style_for_pseudo(&sheet, node, PSEUDO_AFTER, &parent);
    CHECK(before.color == 0x0000ff && after.color == 0x008000);
    CHECK(sheet.rule_index_candidates - candidates <= 4u);
    StyleAncestorBloomCache *cache = budget_calloc(&budget, 1, sizeof(*cache));
    size_t before_cache = budget.current;
    CHECK(cache != NULL && style_selector_cooperation_begin(
        &sheet, cache_test_cooperate, NULL, cache));
    size_t cache_bytes = budget.current - before_cache;
    CHECK(sizeof(StyleSelectorResultCacheEntry) <= 16u);
    CHECK(cache_bytes <= 64u * 1024u + 128u); /* Budget allocation header. */
    printf("selector cache: entries=%u bytes=%zu\n",
        STYLE_SELECTOR_RESULT_CACHE_CAPACITY, cache_bytes);
    before = style_for_pseudo(&sheet, node, PSEUDO_BEFORE, &parent);
    candidates = sheet.rule_index_candidates;
    ComputedStyle changed_parent = parent;
    changed_parent.font_size = 23;
    changed_parent.font_size_fraction = 0;
    ComputedStyle repeated = style_for_pseudo(
        &sheet, node, PSEUDO_BEFORE, &changed_parent);
    /* Reuse matching rules, not their values: font-relative values must be
       evaluated again, without another indexed candidate scan. */
    CHECK(repeated.color == before.color && repeated.font_size == 46);
    CHECK(sheet.rule_index_candidates == candidates);
    after = style_for_pseudo(&sheet, node, PSEUDO_AFTER, &parent);
    CHECK(after.color == 0x008000);
    ComputedStyle cold = style_for_node(&sheet, node, &parent);
    uint64_t visits = sheet.selector_compound_calls;
    ComputedStyle warm = style_for_node(&sheet, node, &parent);
    CHECK(cold.color == 80u && memcmp(&cold, &warm, sizeof(cold)) == 0);
    CHECK(visits != 0 && sheet.selector_compound_calls - visits < visits / 2u);
    /* Temporary focus edits must invalidate exact answers even if the sheet
       has no custom properties / variable-cache allocation. */
    CHECK(lxb_dom_element_set_attribute(lxb_dom_interface_element(node),
        (const lxb_char_t *) "data-tilefinch-focus", 20,
        (const lxb_char_t *) "", 0) != NULL);
    style_variable_cache_invalidate_node(&sheet, node);
    ComputedStyle focused = style_for_node(&sheet, node, &parent);
    CHECK(focused.color == 0xffffff);
    repeated = style_for_pseudo(&sheet, node, PSEUDO_BEFORE, &parent);
    CHECK(repeated.color == 0xffffff);
    CHECK(lxb_dom_element_remove_attribute(lxb_dom_interface_element(node),
        (const lxb_char_t *) "data-tilefinch-focus", 20) == LXB_STATUS_OK);
    style_variable_cache_invalidate_node(&sheet, node);
    warm = style_for_node(&sheet, node, &parent);
    CHECK(warm.color == cold.color);
    repeated = style_for_pseudo(&sheet, node, PSEUDO_BEFORE, &parent);
    CHECK(repeated.color == 0x0000ff);
    style_selector_cooperation_end(&sheet);
    CHECK(budget.current == before_cache);
    /* A fresh scope must not reuse answers after an ancestor class edit. */
    CHECK(lxb_dom_element_set_attribute(lxb_dom_interface_element(node->parent),
        (const lxb_char_t *) "class", 5,
        (const lxb_char_t *) "other", 5) != NULL);
    CHECK(style_selector_cooperation_begin(&sheet, cache_test_cooperate, NULL, cache));
    warm = style_for_node(&sheet, node, &parent);
    CHECK(warm.color != cold.color);
    style_selector_cooperation_end(&sheet);
    size_t limit = budget.limit;
    budget.limit = budget.current;
    CHECK(style_selector_cooperation_begin(&sheet, cache_test_cooperate, NULL, cache));
    CHECK(cache->results == NULL);
    budget.limit = limit;
    ComputedStyle refused = style_for_node(&sheet, node, &parent);
    CHECK(refused.color == warm.color);
    style_selector_cooperation_end(&sheet);
    /* Exceed the tiny matched-list bound: never retain a truncated cascade. */
    for (unsigned i = 1; i <= 9; i++) {
        char css[64];
        int length = snprintf(css, sizeof(css), "a::after{color:#%06x}", i);
        CHECK(length > 0 && stylesheet_add_css(&sheet, css, (size_t) length));
    }
    CHECK(style_selector_cooperation_begin(&sheet, cache_test_cooperate, NULL, cache));
    after = style_for_pseudo(&sheet, node, PSEUDO_AFTER, &parent);
    candidates = sheet.rule_index_candidates;
    repeated = style_for_pseudo(&sheet, node, PSEUDO_AFTER, &parent);
    CHECK(after.color == 9 && repeated.color == 9
        && sheet.rule_index_candidates > candidates);
    style_selector_cooperation_end(&sheet);
    budget_free(&budget, cache);
    stylesheet_destroy(&sheet);
    document_destroy(&document);
    CHECK(budget.current == 0);
    return 0;
}

static int test_pseudo_absence_proof(void)
{
    Budget budget;
    budget_init(&budget, 8u * MIB);
    budget_install_lexbor(&budget);
    PocDocument document = {0};
    static const char html[] = "<p id=absent>prose</p><span id=empty></span>"
        "<b id=none></b><i id=variable></i>";
    CHECK(document_parse(&document, &budget, html, sizeof(html) - 1u, 17));
    Stylesheet sheet = {0};
    CHECK(stylesheet_build(&sheet, &budget, &document, 480));
    static const char css[] = "p::before{color:red;background:blue}"
        "span::before{content:'';display:block;height:10px;background:red}"
        "b::before{content:none}i::before{--mark:'V';content:var(--mark)}"
        "p:focus::before{content:'F'}";
    CHECK(stylesheet_add_css(&sheet, css, sizeof(css) - 1u));
    lxb_dom_node_t *p = find_id(lxb_dom_interface_node(document.html), "absent");
    lxb_dom_node_t *span = find_id(lxb_dom_interface_node(document.html), "empty");
    CHECK(p != NULL && span != NULL && !style_pseudo_known_absent(&sheet, p, PSEUDO_BEFORE));
    StyleAncestorBloomCache *cache = budget_calloc(&budget, 1, sizeof(*cache));
    CHECK(cache != NULL && style_selector_cooperation_begin(
        &sheet, cache_test_cooperate, NULL, cache));
    ComputedStyle parent = style_for_node(&sheet, p, NULL);
    ComputedStyle absent = style_for_pseudo(&sheet, p, PSEUDO_BEFORE, &parent);
    CHECK(!absent.generated_content && style_pseudo_known_absent(&sheet, p, PSEUDO_BEFORE));
    ComputedStyle omitted = style_for_layout_pseudo(&sheet, p, PSEUDO_BEFORE, &parent);
    CHECK(!omitted.generated_content && omitted.font_size == 0);
    /* Public computed-style reads retain defaults/inheritance even when the
       layout-only path can omit an absent box, including on a cold lookup. */
    CHECK(absent.color == 0xff0000 && absent.font_size == parent.font_size);
    omitted = style_for_layout_pseudo(&sheet, span, PSEUDO_AFTER, &parent);
    CHECK(!omitted.generated_content && omitted.font_size == 0);
    ComputedStyle public_after = style_for_pseudo(&sheet, span, PSEUDO_AFTER, &parent);
    CHECK(!public_after.generated_content && public_after.font_size == parent.font_size);
    ComputedStyle empty = style_for_pseudo(&sheet, span, PSEUDO_BEFORE, &parent);
    ComputedStyle laid_out = style_for_layout_pseudo(&sheet, span, PSEUDO_BEFORE, &parent);
    CHECK(empty.generated_content && empty.height == 10
        && !style_pseudo_known_absent(&sheet, span, PSEUDO_BEFORE)
        && memcmp(&empty, &laid_out, sizeof(empty)) == 0);
    lxb_dom_node_t *none = find_id(lxb_dom_interface_node(document.html), "none");
    lxb_dom_node_t *variable = find_id(lxb_dom_interface_node(document.html), "variable");
    CHECK(none != NULL && variable != NULL);
    ComputedStyle no_content = style_for_pseudo(&sheet, none, PSEUDO_BEFORE, &parent);
    CHECK(!no_content.generated_content && style_pseudo_known_absent(&sheet, none, PSEUDO_BEFORE));
    ComputedStyle deferred = style_for_layout_pseudo(&sheet, variable, PSEUDO_BEFORE, &parent);
    CHECK(deferred.generated_content && !style_pseudo_known_absent(&sheet, variable, PSEUDO_BEFORE));
    CHECK(lxb_dom_element_set_attribute(lxb_dom_interface_element(p),
        (const lxb_char_t *) "data-tilefinch-focus", 20,
        (const lxb_char_t *) "", 0) != NULL);
    style_variable_cache_invalidate_node(&sheet, p);
    CHECK(!style_pseudo_known_absent(&sheet, p, PSEUDO_BEFORE));
    ComputedStyle focused = style_for_pseudo(&sheet, p, PSEUDO_BEFORE, &parent);
    CHECK(focused.generated_content && !style_pseudo_known_absent(&sheet, p, PSEUDO_BEFORE));
    style_selector_cooperation_end(&sheet);
    CHECK(!style_pseudo_known_absent(&sheet, p, PSEUDO_BEFORE));
    budget_free(&budget, cache);
    stylesheet_destroy(&sheet);
    document_destroy(&document);
    CHECK(budget.current == 0);
    return 0;
}

static int test_deferred_expansion_reuse(void)
{
    Budget budget;
    budget_init(&budget, 8u * MIB);
    CHECK(budget_install_lexbor(&budget));
    PocDocument document = {0};
    static const char html[] = "<style>:root{--gap:17px}"
        "#probe{margin-inline-start:var(--gap)}"
        "#axes{margin-inline-start:var(--gap);direction:rtl;margin-left:0}"
        "</style><div id=probe></div><div id=axes></div>";
    CHECK(document_parse(&document, &budget, html, sizeof(html) - 1u, 17));
    Stylesheet sheet = {0};
    CHECK(stylesheet_build(&sheet, &budget, &document, 480));
    lxb_dom_node_t *root = lxb_dom_interface_node(document.html);
    lxb_dom_node_t *probe = find_id(root, "probe");
    lxb_dom_node_t *axes = find_id(root, "axes");
    CHECK(probe != NULL && axes != NULL);
    ComputedStyle parent = layout_initial_root_style();
    uint64_t lookups = sheet.variable_lookup_calls;
    ComputedStyle result = style_for_node(&sheet, probe, &parent);
    CHECK(result.margin.left == 17 && result.margin.right == 0);
    printf("logical declaration: variable lookups=%llu\n",
        (unsigned long long) (sheet.variable_lookup_calls - lookups));
    /* One expansion in each of the existing logical correction passes,
       not three identical ancestor lookups per pass. */
    CHECK(sheet.variable_lookup_calls - lookups == 2u);
    /* Axes-changing blocks still correct earlier logical mappings. */
    result = style_for_node(&sheet, axes, &parent);
    CHECK(computed_style_direction_rtl(&result)
        && result.margin.right == 17 && result.margin.left == 0);
    stylesheet_destroy(&sheet);
    document_destroy(&document);
    CHECK(budget.current == 0);
    return 0;
}

static int test_head_script_dependency_cache(void)
{
    Budget budget;
    budget_init(&budget, 16u * MIB);
    CHECK(budget_install_lexbor(&budget));
    PocDocument document = {0};
    static const char html[] = "<!doctype html><title>Cache</title><p>body</p>";
    CHECK(document_parse(&document, &budget, html, sizeof(html) - 1u, 17));
    Stylesheet sheet = {0};
    CHECK(stylesheet_build(&sheet, &budget, &document, 480));
    static const char css[] = ".observer:has(script) p{color:red}";
    CHECK(stylesheet_add_css(&sheet, css, sizeof(css) - 1u));
    lxb_dom_node_t *head = lxb_dom_interface_node(
        lxb_html_document_head_element(document.html));
    CHECK(!stylesheet_head_scripts_affect_ancestors(&sheet, head));
    uint64_t scans = sheet.head_script_selector_scans;
    CHECK(scans != 0);
    for (unsigned i = 0; i < 100; i++)
        CHECK(!stylesheet_head_scripts_affect_ancestors(&sheet, head));
    CHECK(sheet.head_script_selector_scans == scans);
    /* A DOM-only change must still recheck the cached :has subject. */
    CHECK(lxb_dom_element_set_attribute(lxb_dom_interface_element(head),
        (const lxb_char_t *) "class", 5,
        (const lxb_char_t *) "observer", 8) != NULL);
    CHECK(stylesheet_head_scripts_affect_ancestors(&sheet, head));
    CHECK(sheet.head_script_selector_scans == scans);
    CHECK(lxb_dom_element_remove_attribute(lxb_dom_interface_element(head),
        (const lxb_char_t *) "class", 5) == LXB_STATUS_OK);
    CHECK(!stylesheet_head_scripts_affect_ancestors(&sheet, head));
    /* New stylesheet contents invalidate the lexical summary. */
    static const char changed[] = "html:has(script){color:blue}";
    CHECK(stylesheet_add_css(&sheet, changed, sizeof(changed) - 1u));
    CHECK(stylesheet_head_scripts_affect_ancestors(&sheet, head));
    CHECK(sheet.head_script_selector_scans > scans);
    scans = sheet.head_script_selector_scans;
    CHECK(stylesheet_head_scripts_affect_ancestors(&sheet, head));
    CHECK(sheet.head_script_selector_scans == scans);
    printf("head selector cache: repeated scans=0 across 100 checks\n");
    stylesheet_destroy(&sheet);
    document_destroy(&document);
    CHECK(budget.current == 0);
    return 0;
}

static int test_quoted_declaration_boundaries(void)
{
    Budget budget;
    budget_init(&budget, 8u * MIB);
    budget_install_lexbor(&budget);
    PocDocument document = {0};
    static const char html[] = "<style>"
        "#probe{--label:'a;b'; /* ; ignored */ --size:37px;"
        "width:var(--size);font-family:'A;B',serif}"
        "</style><div id=probe>text</div>";
    CHECK(document_parse(&document, &budget, html, sizeof(html) - 1u, 17));
    Stylesheet sheet = {0};
    CHECK(stylesheet_build(&sheet, &budget, &document, 480));
    const StyleCustomRule *label = find_custom_rule(&sheet, "#probe", "--label");
    CHECK(label != NULL && strcmp(label->value, "'a;b'") == 0);
    lxb_dom_node_t *node = find_id(lxb_dom_interface_node(document.html), "probe");
    ComputedStyle parent = layout_initial_root_style();
    ComputedStyle computed = style_for_node(&sheet, node, &parent);
    CHECK(computed.width == 37);
    const StyleCustomRule *size = find_custom_rule(&sheet, "#probe", "--size");
    CHECK(size != NULL && strcmp(size->value, "37px") == 0);
    stylesheet_destroy(&sheet);
    document_destroy(&document);
    CHECK(budget.current == 0);
    return 0;
}

static int test_retained_range_cache_handoff(void)
{
    Budget budget;
    budget_init(&budget, 8u * MIB);
    CHECK(budget_install_lexbor(&budget));
    PocDocument document = {0};
    Stylesheet sheet = {0};
    static const char html[] = "<style>p{color:#123456}"
        "p::before{content:'x';color:#654321}</style><p id=p>Text</p>";
    CHECK(document_parse(&document, &budget, html, sizeof(html)-1u, 17)
          && stylesheet_build(&sheet, &budget, &document, 480));
    lxb_dom_node_t *node = find_id(lxb_dom_interface_node(document.html), "p");
    StyleAncestorBloomCache *local = budget_calloc(&budget, 1, sizeof(*local));
    StyleRetainedMatches *retained = style_retained_matches_create(&budget);
    CHECK(node != NULL && local != NULL && retained != NULL
          && style_selector_cooperation_begin(&sheet, cache_test_cooperate, NULL, local));
    ComputedStyle warm = style_for_node(&sheet, node, NULL);
    ComputedStyle warm_pseudo = style_for_pseudo(&sheet, node, PSEUDO_BEFORE, &warm);
    (void) style_retained_matches_attach(&sheet, retained);
    (void) style_for_node(&sheet, node, NULL);
    (void) style_for_pseudo(&sheet, node, PSEUDO_BEFORE, &warm);
    ComputedStyle replay = style_for_node(&sheet, node, NULL);
    ComputedStyle replay_pseudo = style_for_pseudo(&sheet, node, PSEUDO_BEFORE, &warm);
    CHECK(retained->hits >= 2 && replay.color == warm.color
          && replay_pseudo.color == warm_pseudo.color
          && replay_pseudo.generated_content == warm_pseudo.generated_content);
    (void) style_retained_matches_attach(&sheet, NULL);
    style_selector_cooperation_end(&sheet);
    style_retained_matches_destroy(retained);
    budget_free(&budget, local);
    stylesheet_destroy(&sheet);
    document_destroy(&document);
    CHECK(budget.current == 0 && budget_uninstall_lexbor(&budget));
    return 0;
}

static int test_retained_retirement_probe_holes(void)
{
    Budget budget;
    budget_init(&budget, 8u * MIB);
    CHECK(budget_install_lexbor(&budget));
    PocDocument document = {0};
    Stylesheet sheet = {0};
    static const char html[] = "<style>p{color:red}"
        "p::before{content:'x'}</style><div id=group><p id=p>Text</p></div>";
    CHECK(document_parse(&document, &budget, html, sizeof(html)-1u, 17)
          && stylesheet_build(&sheet, &budget, &document, 480));
    lxb_dom_node_t *root = lxb_dom_interface_node(document.html);
    lxb_dom_node_t *node = find_id(root, "p"), *group = find_id(root, "group");
    StyleRetainedMatches *retained = style_retained_matches_create(&budget);
    CHECK(node != NULL && group != NULL && retained != NULL);
    (void) style_retained_matches_attach(&sheet, retained);
    const PseudoElement pseudos[] = {PSEUDO_NONE, PSEUDO_BEFORE, PSEUDO_AFTER};
    for (size_t p = 0; p < sizeof(pseudos) / sizeof(pseudos[0]); p++) {
        style_retained_matches_clear(retained);
        ComputedStyle computed = style_for_node(&sheet, node, NULL);
        if (pseudos[p] != PSEUDO_NONE)
            (void) style_for_pseudo(&sheet, node, pseudos[p], &computed);
        size_t home = 0;
        while (home < STYLE_RETAINED_MATCH_CAPACITY
               && !(retained->entries[home].node == node
                    && retained->entries[home].pseudo == (uint8_t) pseudos[p])) home++;
        CHECK(home < STYLE_RETAINED_MATCH_CAPACITY);
        StyleRetainedMatchEntry entry = retained->entries[home];
        style_retained_matches_clear(retained);
        /* Token invalidation leaves holes; a subsequent insertion can also
           duplicate a key that survives farther along its bounded probe. */
        retained->entries[(home + 1u) & (STYLE_RETAINED_MATCH_CAPACITY - 1u)] = entry;
        retained->entries[(home + 3u) & (STYLE_RETAINED_MATCH_CAPACITY - 1u)] = entry;
        retained->occupied = 2;
        style_retained_matches_forget_subtree(retained, group);
        CHECK(retained->occupied == 0);
        for (size_t i = 0; i < STYLE_RETAINED_MATCH_CAPACITY; i++)
            CHECK(retained->entries[i].node == NULL);
    }
    (void) style_retained_matches_attach(&sheet, NULL);
    style_retained_matches_destroy(retained);
    stylesheet_destroy(&sheet);
    document_destroy(&document);
    CHECK(budget.current == 0 && budget_uninstall_lexbor(&budget));
    return 0;
}

static int test_retained_nested_selector_invalidation(void)
{
    Budget budget;
    budget_init(&budget, 8u * MIB);
    CHECK(budget_install_lexbor(&budget));
    PocDocument document = {0};
    Stylesheet sheet = {0};
    static const char html[] = "<style>p{color:#123456}"
        "p:is(.active p){color:#654321}</style><div id=group><p id=p>Text</p></div>";
    CHECK(document_parse(&document, &budget, html, sizeof(html)-1u, 17)
          && stylesheet_build(&sheet, &budget, &document, 480));
    lxb_dom_node_t *root = lxb_dom_interface_node(document.html);
    lxb_dom_node_t *node = find_id(root, "p"), *group = find_id(root, "group");
    StyleRetainedMatches *retained = style_retained_matches_create(&budget);
    CHECK(node != NULL && group != NULL && retained != NULL);
    (void) style_retained_matches_attach(&sheet, retained);
    CHECK(style_for_node(&sheet, node, NULL).color == 0x123456);
    BudgetAllocationOwner owner = document_allocation_owner_enter(&document);
    CHECK(lxb_dom_element_set_attribute(lxb_dom_interface_element(group),
        (const lxb_char_t *) "class", 5, (const lxb_char_t *) "active", 6) != NULL);
    document_allocation_owner_leave(&document, owner);
    uint32_t token = stylesheet_identity_token_hash(false, "active", 6);
    const lxb_dom_node_t *nodes[] = {group};
    style_retained_matches_invalidate_tokens(retained, &sheet, nodes, 1, &token, 1, false);
    ComputedStyle replay = style_for_node(&sheet, node, NULL);
    (void) style_retained_matches_attach(&sheet, NULL);
    ComputedStyle exact = style_for_node(&sheet, node, NULL);
    CHECK(exact.color == 0x654321 && replay.color == exact.color);
    style_retained_matches_destroy(retained);
    stylesheet_destroy(&sheet);
    document_destroy(&document);
    CHECK(budget.current == 0 && budget_uninstall_lexbor(&budget));
    return 0;
}

static int test_retained_focus_descendants(void)
{
    Budget budget;
    budget_init(&budget, 8u * MIB);
    CHECK(budget_install_lexbor(&budget));
    PocDocument document = {0};
    Stylesheet sheet = {0};
    static const char html[] = "<style>.group{color:#123456}"
        ".group:focus-within{color:#654321}.group:focus-within p{padding-left:7px}"
        "</style><div class=group id=group><input id=input><p id=p>Text</p></div>";
    CHECK(document_parse(&document, &budget, html, sizeof(html)-1u, 17)
          && stylesheet_build(&sheet, &budget, &document, 480));
    for (unsigned i = 0; i < 64; i++) {
        char css[48];
        int length = snprintf(css, sizeof(css), ".pad%u{color:red}", i);
        CHECK(length > 0 && stylesheet_add_css(&sheet, css, (size_t) length));
    }
    lxb_dom_node_t *root = lxb_dom_interface_node(document.html);
    lxb_dom_node_t *group = find_id(root, "group"), *node = find_id(root, "p");
    lxb_dom_node_t *input = find_id(root, "input");
    LayoutReuseCache *reuse = layout_reuse_cache_create(&budget);
    CHECK(group != NULL && node != NULL && input != NULL && reuse != NULL);
    layout_reuse_cache_enable_retained_matches(reuse);
    layout_reuse_cache_prepare(reuse, &sheet, NULL, NULL, 480);
    ComputedStyle parent, child;
    (void) layout_reuse_cache_resolve_style(reuse, &sheet, NULL, group, NULL, &parent);
    (void) layout_reuse_cache_resolve_style(reuse, &sheet, NULL, node, &parent, &child);
    CHECK(child.padding.left == 0);
    BudgetAllocationOwner owner = document_allocation_owner_enter(&document);
    CHECK(lxb_dom_element_set_attribute(lxb_dom_interface_element(input),
        (const lxb_char_t *) "data-tilefinch-focus", 20, (const lxb_char_t *) "", 0) != NULL);
    document_allocation_owner_leave(&document, owner);
    layout_reuse_cache_invalidate_focus(reuse, input);
    layout_reuse_cache_prepare(reuse, &sheet, NULL, NULL, 480);
    (void) layout_reuse_cache_resolve_style(reuse, &sheet, NULL, group, NULL, &parent);
    (void) layout_reuse_cache_resolve_style(reuse, &sheet, NULL, node, &parent, &child);
    CHECK(parent.color == 0x654321 && child.padding.left == 7);
    layout_reuse_cache_destroy(reuse);
    stylesheet_destroy(&sheet);
    document_destroy(&document);
    CHECK(budget.current == 0 && budget_uninstall_lexbor(&budget));
    return 0;
}

static int test_retained_keyless_lost_match(void)
{
    Budget budget;
    budget_init(&budget, 8u * MIB);
    CHECK(budget_install_lexbor(&budget));
    PocDocument document = {0};
    Stylesheet sheet = {0};
    static const char html[] = "<style>p{color:#123456}"
        ".active > *{color:#654321}</style>"
        "<div id=group class=active><p id=p>Text</p></div>";
    CHECK(document_parse(&document, &budget, html, sizeof(html)-1u, 17)
          && stylesheet_build(&sheet, &budget, &document, 480));
    lxb_dom_node_t *root = lxb_dom_interface_node(document.html);
    lxb_dom_node_t *node = find_id(root, "p"), *group = find_id(root, "group");
    StyleRetainedMatches *retained = style_retained_matches_create(&budget);
    CHECK(node != NULL && group != NULL && retained != NULL);
    (void) style_retained_matches_attach(&sheet, retained);
    CHECK(style_for_node(&sheet, node, NULL).color == 0x654321);
    CHECK(style_for_node(&sheet, node, NULL).color == 0x654321
          && retained->hits != 0);
    BudgetAllocationOwner owner = document_allocation_owner_enter(&document);
    CHECK(lxb_dom_element_remove_attribute(lxb_dom_interface_element(group),
        (const lxb_char_t *) "class", 5) == LXB_STATUS_OK);
    document_allocation_owner_leave(&document, owner);
    uint32_t rules[2] = {0, 1};
    /* Only the keyless rule is invalidated: current matching is no longer
       sufficient to find the previously retained answer. */
    uint32_t keyless = sheet.rules[0].has_fast_key ? rules[1] : rules[0];
    CHECK(!sheet.rules[keyless].has_fast_key);
    style_retained_matches_invalidate_rules(retained, &sheet, &keyless, 1);
    CHECK(style_for_node(&sheet, node, NULL).color == 0x123456);
    (void) style_retained_matches_attach(&sheet, NULL);
    style_retained_matches_destroy(retained);
    stylesheet_destroy(&sheet);
    document_destroy(&document);
    CHECK(budget.current == 0 && budget_uninstall_lexbor(&budget));
    return 0;
}

static int test_quoted_pseudo_element_punctuation(void)
{
    Budget budget;
    budget_init(&budget, 8u * MIB);
    CHECK(budget_install_lexbor(&budget));
    PocDocument document = {0};
    Stylesheet sheet = {0};
    static const char html[] = "<style>[data-value='::']{color:#123456}"
        "p::unsupported{color:red}</style><p id=p data-value='::'>Text</p>";
    CHECK(document_parse(&document, &budget, html, sizeof(html)-1u, 17)
          && stylesheet_build(&sheet, &budget, &document, 480));
    lxb_dom_node_t *node = find_id(lxb_dom_interface_node(document.html), "p");
    CHECK(node != NULL && sheet.count == 1
          && style_for_node(&sheet, node, NULL).color == 0x123456);
    stylesheet_destroy(&sheet);
    document_destroy(&document);
    CHECK(budget.current == 0 && budget_uninstall_lexbor(&budget));
    return 0;
}

int main(int argc, char **argv)
{
    if (argc == 2 && strcmp(argv[1], "--retained-handoff-only") == 0)
        return test_retained_range_cache_handoff();
    if (argc == 2 && strcmp(argv[1], "--retained-nested-only") == 0)
        return test_retained_nested_selector_invalidation();
    if (argc == 2 && strcmp(argv[1], "--quoted-punctuation-only") == 0)
        return test_quoted_pseudo_element_punctuation();
    if (argc == 2 && strcmp(argv[1], "--retained-focus-only") == 0)
        return test_retained_focus_descendants();
    CHECK(test_retained_range_cache_handoff() == 0);
    CHECK(test_retained_retirement_probe_holes() == 0);
    CHECK(test_retained_nested_selector_invalidation() == 0);
    CHECK(test_quoted_pseudo_element_punctuation() == 0);
    CHECK(test_retained_focus_descendants() == 0);
    CHECK(test_retained_keyless_lost_match() == 0);
    CHECK(test_ancestor_filter_canonical_tokens() == 0);
    CHECK(test_compiled_attribute_and_pseudo_instructions() == 0);
    CHECK(test_quoted_declaration_boundaries() == 0);
    CHECK(test_pseudo_state_work() == 0);
    CHECK(test_layout_scoped_selector_work() == 0);
    CHECK(test_pseudo_absence_proof() == 0);
    CHECK(test_deferred_expansion_reuse() == 0);
    CHECK(test_head_script_dependency_cache() == 0);
    Budget budget;
    budget_init(&budget, 8u * MIB);
    budget_install_lexbor(&budget);
    static const char html[] =
        "<!doctype html><style>"
        "*{letter-spacing:1px}"
        "section div{word-spacing:2px}"
        ".card{color:#010203}"
        ".card{hyphens:none}.never:hover{hyphens:none}"
        "#target{color:#112233}"
        "section>.card.hot{display:flex}"
        "#primary+#secondary{word-spacing:5px}"
        "#primary~#target{letter-spacing:6px}"
        "[data-x]{padding:3px}"
        ".card:not(.cold){background-image:url('/shared.png')}"
        ".card::before{content:'indexed';color:#445566}"
        ".unused-a,.unused-b,.unused-c{margin:9px}"
        ".same-a{padding:7px}.between{height:11px}.same-b{padding:7px}"
        ".deferred-a{color:var(--tone)}.spacer{width:13px}"
        ".deferred-b{color:var(--tone)}"
        ":root{--button-bg:#563acc;--button-on:#fff;"
        "--button-text:#563acc}"
        "body{margin:0;background:#fff}"
        ".button{background-color:var(--button-bg);"
        "background-image:url('/button.png');color:var(--button-on);"
        "border:1px solid var(--button-text);border-radius:4px;"
        "box-sizing:border-box;display:block;width:120px;height:40px;"
        "padding:6px 17px}"
        ".button.button-secondary{color:var(--button-text);background:0 0}"
        "#gradient-pure{background:linear-gradient(110deg,#242424 30%,"
        "#606060 48%,#242424 66%)}"
        "#gradient-combined{background:#242424 linear-gradient(110deg,"
        "#242424 30%,#606060 48%,#242424 66%)}"
        "#gradient-longhand{background-color:#242424;background-image:"
        "linear-gradient(110deg,#242424 30%,#606060 48%,#242424 66%)}"
        ".special.button{margin-left:99px}"
        ".missing section .card{margin-right:99px}"
        "</style><section>"
        "<a id=primary class=button>Primary</a>"
        "<a id=secondary class='button button-secondary'>Login</a>"
        "<div id=target class='card hot' data-x=1></div>"
        "<p id=other class=cold></p>"
        "<div id=gradient-pure></div><div id=gradient-combined></div>"
        "<div id=gradient-longhand></div>"
        "</section>";
    PocDocument document = {0};
    CHECK(document_parse(&document, &budget, html, sizeof(html) - 1u, 17));
    lxb_dom_node_t *root = lxb_dom_interface_node(document.html);
    lxb_dom_node_t *target = find_id(root, "target");
    lxb_dom_node_t *other = find_id(root, "other");
    lxb_dom_node_t *primary = find_id(root, "primary");
    lxb_dom_node_t *secondary = find_id(root, "secondary");
    lxb_dom_node_t *gradient_pure = find_id(root, "gradient-pure");
    lxb_dom_node_t *gradient_combined = find_id(root, "gradient-combined");
    lxb_dom_node_t *gradient_longhand = find_id(root, "gradient-longhand");
    CHECK(target != NULL && other != NULL && primary != NULL
          && secondary != NULL && gradient_pure != NULL
          && gradient_combined != NULL && gradient_longhand != NULL);

    (void) unsetenv("TILEFINCH_DISABLE_STYLE_INDEX");
    (void) unsetenv("TILEFINCH_DISABLE_COMPILED_SELECTORS");
    (void) unsetenv("TILEFINCH_DISABLE_COMPILED_COMPLEX_SELECTORS");
    Stylesheet indexed = {0};
    CHECK(stylesheet_build(&indexed, &budget, &document, 480));
    const StyleRule *same_a = find_rule(&indexed, ".same-a");
    const StyleRule *same_b = find_rule(&indexed, ".same-b");
    const StyleRule *deferred_a = find_rule(&indexed, ".deferred-a");
    const StyleRule *deferred_b = find_rule(&indexed, ".deferred-b");
    const StyleRule *secondary_rule = find_rule(
        &indexed, ".button.button-secondary");
    const StyleRule *combinator_rule = find_rule(
        &indexed, "section>.card.hot");
    const StyleRule *adjacent_rule = find_rule(
        &indexed, "#primary+#secondary");
    const StyleRule *sibling_rule = find_rule(
        &indexed, "#primary~#target");
    const StyleRule *attribute_rule = find_rule(&indexed, "[data-x]");
    const StyleRule *functional_rule = find_rule(
        &indexed, ".card:not(.cold)");
    const StyleCustomRule *hyphens_rule = find_custom_rule(
        &indexed, ".card", "hyphens");
    CHECK(same_a != NULL && same_b != NULL && deferred_a != NULL
          && deferred_b != NULL && secondary_rule != NULL
          && combinator_rule != NULL && adjacent_rule != NULL
          && sibling_rule != NULL && attribute_rule != NULL
          && functional_rule != NULL && hyphens_rule != NULL
          && hyphens_rule->fast_key_offset != UINT8_MAX
          && strcmp(hyphens_rule->selector
                        + hyphens_rule->fast_key_offset,
                    "card") == 0
          && find_custom_rule(
                 &indexed, ".never:hover", "hyphens") == NULL
          && same_a->declaration_index == same_b->declaration_index
          && deferred_a->declaration_index == deferred_b->declaration_index
          && indexed.declaration_count < indexed.count
          && indexed.selector_chunks != NULL
          && indexed.selector_bytes != 0
          && indexed.selector_storage_bytes >= indexed.selector_bytes
          && sizeof(StyleRule) < sizeof(ComputedStyle));
    size_t secondary_rule_index = (size_t) (secondary_rule - indexed.rules);
    size_t combinator_rule_index = (size_t) (combinator_rule - indexed.rules);
    size_t adjacent_rule_index = (size_t) (adjacent_rule - indexed.rules);
    size_t sibling_rule_index = (size_t) (sibling_rule - indexed.rules);
    size_t attribute_rule_index = (size_t) (attribute_rule - indexed.rules);
    size_t functional_rule_index = (size_t) (functional_rule - indexed.rules);
    CHECK(secondary_rule->has_fast_key
          && secondary_rule->type == SELECTOR_CLASS
          && secondary_rule->fast_key_length
             == strlen("button-secondary")
          && memcmp(secondary_rule->selector
                        + secondary_rule->fast_key_offset,
                    "button-secondary", strlen("button-secondary")) == 0
          && combinator_rule->rightmost_compound_offset
             == strlen("section>")
          && strcmp(combinator_rule->selector
                        + combinator_rule->rightmost_compound_offset,
                    ".card.hot") == 0);
    const StyleDeclaration *deferred_declaration =
        stylesheet_rule_declaration(&indexed, deferred_a);
    CHECK(deferred_declaration != NULL
          && deferred_declaration->deferred_declarations != NULL
          && strcmp(deferred_declaration->deferred_declarations,
                    "color:var(--tone)") == 0
          && deferred_declaration->deferred_program_offset != UINT32_MAX
          && deferred_declaration->deferred_program_count == 1
          && indexed.deferred_instruction_count != 0);
    const char *stable_selector = same_a->selector;
    ComputedStyle indexed_target = style_for_node(&indexed, target, NULL);
    ComputedStyle indexed_other = style_for_node(&indexed, other, NULL);
    ComputedStyle indexed_secondary = style_for_node(&indexed, secondary,
                                                     NULL);
    ComputedStyle indexed_before = style_for_pseudo(
        &indexed, target, PSEUDO_BEFORE, &indexed_target);
    ComputedStyle indexed_gradient_pure = style_for_node(
        &indexed, gradient_pure, NULL);
    ComputedStyle indexed_gradient_combined = style_for_node(
        &indexed, gradient_combined, NULL);
    ComputedStyle indexed_gradient_longhand = style_for_node(
        &indexed, gradient_longhand, NULL);
    const StyleGradient *pure_gradient = stylesheet_background_gradient(
        &indexed, &indexed_gradient_pure);
    const StyleGradient *combined_gradient = stylesheet_background_gradient(
        &indexed, &indexed_gradient_combined);
    const StyleGradient *longhand_gradient = stylesheet_background_gradient(
        &indexed, &indexed_gradient_longhand);
    CHECK(computed_style_hyphens_none(&indexed_target)
          && !computed_style_hyphens_none(&indexed_other)
          && indexed.rule_index_ready && indexed.rule_index_bytes != 0
          && indexed.selector_program_ready
          && indexed.selector_program_rule_count == indexed.count
          && indexed.selector_program_bytes <= 256u * 1024u
          && indexed.selector_program_offsets != NULL
          && indexed.selector_program_offsets[secondary_rule_index]
               != UINT16_MAX
          && indexed.selector_program_offsets[combinator_rule_index]
               != UINT16_MAX
          && indexed.selector_program_offsets[adjacent_rule_index]
               != UINT16_MAX
          && indexed.selector_program_offsets[sibling_rule_index]
               != UINT16_MAX
          && indexed.selector_program_offsets[attribute_rule_index]
               != UINT16_MAX
          && indexed.selector_program_offsets[functional_rule_index]
               != UINT16_MAX
          && indexed.selector_tag_id_checks != 0
          && indexed.selector_subject_cache_hits != 0
          && indexed.custom_rule_index_ready
          && indexed.custom_rule_index_bytes != 0
          && indexed.variable_lookup_calls != 0
          && indexed.variable_rule_candidates != 0
          && indexed.deferred_program_executions != 0
          && indexed.deferred_program_fallbacks == 0
          && indexed.deferred_program_instructions
               >= indexed.deferred_program_executions
          && indexed.rule_index_queries >= 3
          && indexed.rule_index_candidates
               < indexed.rule_index_queries * indexed.count
          && pure_gradient != NULL && combined_gradient != NULL
          && longhand_gradient != NULL
          && memcmp(pure_gradient, combined_gradient,
                    sizeof(*pure_gradient)) == 0
          && memcmp(pure_gradient, longhand_gradient,
                    sizeof(*pure_gradient)) == 0
          && indexed_gradient_combined.has_background
          && indexed_gradient_combined.background == 0x242424
          && indexed_gradient_combined.background_alpha == 255
          && indexed.rule_filters != NULL
          && indexed.rule_compound_filter_rejections != 0
          && indexed.rule_ancestor_filter_rejections != 0);

    CHECK(setenv("TILEFINCH_DISABLE_STYLE_RULE_FILTER", "1", 1) == 0);
    Stylesheet unfiltered = {0};
    CHECK(stylesheet_build(&unfiltered, &budget, &document, 480));
    ComputedStyle unfiltered_target = style_for_node(
        &unfiltered, target, NULL);
    ComputedStyle unfiltered_other = style_for_node(
        &unfiltered, other, NULL);
    ComputedStyle unfiltered_secondary = style_for_node(
        &unfiltered, secondary, NULL);
    ComputedStyle unfiltered_before = style_for_pseudo(
        &unfiltered, target, PSEUDO_BEFORE, &unfiltered_target);
    CHECK(unfiltered.rule_index_ready
          && unfiltered.rule_filters == NULL
          && equivalent(&indexed, &indexed_target,
                        &unfiltered, &unfiltered_target)
          && equivalent(&indexed, &indexed_other,
                        &unfiltered, &unfiltered_other)
          && equivalent(&indexed, &indexed_secondary,
                        &unfiltered, &unfiltered_secondary)
          && indexed_before.color == unfiltered_before.color
          && indexed_before.generated_text_length
               == unfiltered_before.generated_text_length
          && memcmp(indexed_before.generated_text,
                    unfiltered_before.generated_text,
                    indexed_before.generated_text_length) == 0);
    stylesheet_destroy(&unfiltered);
    CHECK(unsetenv("TILEFINCH_DISABLE_STYLE_RULE_FILTER") == 0);

    CHECK(setenv("TILEFINCH_DISABLE_STYLE_INDEX", "1", 1) == 0);
    CHECK(setenv("TILEFINCH_DISABLE_COMPILED_SELECTORS", "1", 1) == 0);
    CHECK(setenv("TILEFINCH_DISABLE_COMPILED_DEFERRED", "1", 1) == 0);
    Stylesheet linear = {0};
    CHECK(stylesheet_build(&linear, &budget, &document, 480));
    ComputedStyle linear_target = style_for_node(&linear, target, NULL);
    ComputedStyle linear_other = style_for_node(&linear, other, NULL);
    ComputedStyle linear_secondary = style_for_node(&linear, secondary, NULL);
    ComputedStyle linear_before = style_for_pseudo(
        &linear, target, PSEUDO_BEFORE, &linear_target);
    CHECK(!linear.rule_index_ready && linear.rule_index_fallbacks >= 3
          && linear.deferred_instruction_count == 0
          && linear.deferred_program_executions == 0
          && linear.deferred_program_fallbacks != 0
          && equivalent(&indexed, &indexed_target, &linear, &linear_target)
          && equivalent(&indexed, &indexed_other, &linear, &linear_other)
          && equivalent(&indexed, &indexed_secondary,
                        &linear, &linear_secondary)
          && indexed_before.color == linear_before.color
          && indexed_before.generated_text_length
               == linear_before.generated_text_length
          && memcmp(indexed_before.generated_text,
                    linear_before.generated_text,
                    indexed_before.generated_text_length) == 0);
    CHECK(indexed_secondary.color == 0x563acc
          && indexed_secondary.word_spacing == 5
          && !indexed_secondary.has_background
          && indexed_secondary.background_alpha == 0
          && indexed_secondary.background_image == NULL
          && indexed_secondary.background_position_x == 0
          && indexed_secondary.background_position_y == 0);

    LayoutDocument layout = {0};
    CHECK(layout_build(&layout, &budget, &document, &indexed,
                       NULL, NULL, 480));
    const LayoutNodeBox *primary_box = layout_box_for_node(&layout, primary);
    const LayoutNodeBox *secondary_box = layout_box_for_node(
        &layout, secondary);
    CHECK(primary_box != NULL && secondary_box != NULL
          && primary_box->width == 120 && primary_box->height == 40
          && secondary_box->width == 120 && secondary_box->height == 40);
    bool primary_fill = false;
    bool secondary_stroke = false;
    bool secondary_brand_fill = false;
    for (size_t i = primary_box->command_start;
         i < primary_box->command_end; i++) {
        const DrawCommand *command = &layout.commands[i];
        if (command->type == DRAW_FILL_RECT
            && command->color == 0x563acc) primary_fill = true;
    }
    for (size_t i = secondary_box->command_start;
         i < secondary_box->command_end; i++) {
        const DrawCommand *command = &layout.commands[i];
        if (command->type == DRAW_STROKE_RECT
            && command->color == 0x563acc && command->scale == 1
            && command->radius == 4) secondary_stroke = true;
        if (command->type == DRAW_FILL_RECT
            && command->color == 0x563acc) secondary_brand_fill = true;
    }
    CHECK(primary_fill && secondary_stroke && !secondary_brand_fill);

    TileCache cache = {0};
    uint16_t *frame = budget_malloc(
        &budget, 480u * 272u * sizeof(*frame));
    CHECK(frame != NULL && tile_cache_init(&cache, &budget, &layout, 8)
          && tile_cache_set_frame(&cache, frame, 480u * 272u)
          && tile_cache_render_frame(&cache, 0, 480, 272, NULL));
    const LayoutNodeBox *visual_primary = layout_box_for_node(
        cache.layout, primary);
    const LayoutNodeBox *visual_secondary = layout_box_for_node(
        cache.layout, secondary);
    CHECK(visual_primary != NULL && visual_secondary != NULL);
    size_t primary_inner =
        (size_t) (visual_primary->y + visual_primary->height / 2) * 480u
        + (size_t) (visual_primary->x + visual_primary->width - 10);
    size_t secondary_border =
        (size_t) (visual_secondary->y + visual_secondary->height / 2) * 480u
        + (size_t) visual_secondary->x;
    size_t secondary_inner =
        (size_t) (visual_secondary->y + visual_secondary->height / 2) * 480u
        + (size_t) (visual_secondary->x + visual_secondary->width - 10);
    size_t secondary_corner = (size_t) visual_secondary->y * 480u
                              + (size_t) visual_secondary->x;
    CHECK(frame[primary_inner] == rgb565(0x563acc)
          && frame[secondary_border] == rgb565(0x563acc)
          && frame[secondary_inner] == rgb565(0xffffff)
          && frame[secondary_corner] == rgb565(0xffffff));
    uint64_t indexed_frame_hash = frame_hash(frame, 480u * 272u);
    int indexed_height = layout.height;
    tile_cache_destroy(&cache);
    layout_destroy(&layout);

    LayoutDocument linear_layout = {0};
    TileCache linear_cache = {0};
    CHECK(layout_build(&linear_layout, &budget, &document, &linear,
                       NULL, NULL, 480)
          && tile_cache_init(&linear_cache, &budget, &linear_layout, 8)
          && tile_cache_set_frame(&linear_cache, frame, 480u * 272u)
          && tile_cache_render_frame(
              &linear_cache, 0, 480, 272, NULL)
          && linear_layout.height == indexed_height
          && frame_hash(frame, 480u * 272u) == indexed_frame_hash);
    tile_cache_destroy(&linear_cache);
    layout_destroy(&linear_layout);
    budget_free(&budget, frame);
    CHECK(unsetenv("TILEFINCH_DISABLE_STYLE_INDEX") == 0);
    CHECK(unsetenv("TILEFINCH_DISABLE_COMPILED_SELECTORS") == 0);
    CHECK(unsetenv("TILEFINCH_DISABLE_COMPILED_DEFERRED") == 0);

    CHECK(indexed_target.display == DISPLAY_FLEX
          && indexed_target.color == 0x112233
          && indexed_target.letter_spacing == 6
          && indexed_target.word_spacing == 2
          && indexed_target.padding.top == 3
          && indexed_target.background_image != NULL
          && strcmp(indexed_target.background_image, "/shared.png") == 0);
    const char *override = "#target{color:#abcdef}";
    size_t indexed_rules_before_empty = indexed.count;
    uint64_t reused_before_append = indexed.selector_append_reused_rules;
    uint64_t compiled_before_append = indexed.selector_append_compiled_rules;
    CHECK(stylesheet_add_css(&indexed, NULL, 0)
          && stylesheet_add_user_css(&indexed, NULL, 0)
          && indexed.count == indexed_rules_before_empty
          && stylesheet_add_css(&indexed, override, strlen(override))
          && strcmp(stable_selector, ".same-a") == 0
          && indexed.rule_index_ready
          && indexed.selector_program_ready
          && indexed.selector_append_reused_rules
               >= reused_before_append + indexed_rules_before_empty
          && indexed.selector_append_compiled_rules
               == compiled_before_append + 1u);
    ComputedStyle updated = style_for_node(&indexed, target, NULL);
    CHECK(indexed.rule_index_ready && updated.color == 0xabcdef
          && updated.background_image != NULL
          && strcmp(updated.background_image, "/shared.png") == 0);

    puts("test: compiled selector edge cases converge with fallback");
    char long_identifier[151];
    memset(long_identifier, 'a', sizeof(long_identifier) - 1u);
    long_identifier[sizeof(long_identifier) - 1u] = '\0';
    char edge_html[2048];
    int edge_length = snprintf(
        edge_html, sizeof(edge_html),
        "<!doctype html><style>#%s{color:#135724}"
        "#escaped\\ identifier .edge{padding-top:11px}"
        ".subject:has(> .a + .b){margin-left:13px}"
        ".eligible:nth-child(odd of .eligible){margin-top:7px}"
        "[data-mode='loud' i]{padding-bottom:9px}"
        ":is(#edge,.absent){border-top-width:3px}</style>"
        "<body><div id='%s'>LONG</div>"
        "<div id='escaped identifier'><span id=edge class=edge>EDGE</span>"
        "</div><section id=subject class=subject data-mode=LOUD>"
        "<i class=a></i><i id=eligible class='b eligible'></i>"
        "<i class=eligible></i></section></body>",
        long_identifier, long_identifier);
    PocDocument edge_document = {0};
    Stylesheet edge_indexed = {0}, edge_linear = {0};
    CHECK(edge_length > 0 && (size_t) edge_length < sizeof(edge_html)
          && document_parse(
              &edge_document, &budget, edge_html, (size_t) edge_length, 17));
    lxb_dom_node_t *long_node = find_id(
        lxb_dom_interface_node(edge_document.html), long_identifier);
    lxb_dom_node_t *edge_node = find_id(
        lxb_dom_interface_node(edge_document.html), "edge");
    lxb_dom_node_t *subject_node = find_id(
        lxb_dom_interface_node(edge_document.html), "subject");
    lxb_dom_node_t *eligible_node = find_id(
        lxb_dom_interface_node(edge_document.html), "eligible");
    CHECK(long_node != NULL && edge_node != NULL
          && subject_node != NULL && eligible_node != NULL
          && stylesheet_build(
              &edge_indexed, &budget, &edge_document, 480));
    ComputedStyle edge_indexed_long = style_for_node(
        &edge_indexed, long_node, NULL);
    ComputedStyle edge_indexed_child = style_for_node(
        &edge_indexed, edge_node, NULL);
    ComputedStyle edge_indexed_subject = style_for_node(
        &edge_indexed, subject_node, NULL);
    ComputedStyle edge_indexed_eligible = style_for_node(
        &edge_indexed, eligible_node, NULL);
    CHECK(edge_indexed.selector_program_ready
          && setenv("TILEFINCH_DISABLE_COMPILED_SELECTORS", "1", 1) == 0
          && stylesheet_build(
              &edge_linear, &budget, &edge_document, 480));
    ComputedStyle edge_linear_long = style_for_node(
        &edge_linear, long_node, NULL);
    ComputedStyle edge_linear_child = style_for_node(
        &edge_linear, edge_node, NULL);
    ComputedStyle edge_linear_subject = style_for_node(
        &edge_linear, subject_node, NULL);
    ComputedStyle edge_linear_eligible = style_for_node(
        &edge_linear, eligible_node, NULL);
    CHECK(edge_indexed_long.color == 0x135724
          && edge_indexed_long.color == edge_linear_long.color
          && edge_indexed_child.padding.top == 11
          && edge_indexed_child.padding.top == edge_linear_child.padding.top
          && edge_indexed_child.border.top == 3
          && edge_indexed_child.border.top
                 == edge_linear_child.border.top
          && edge_indexed_subject.margin.left == 13
          && edge_indexed_subject.margin.left
                 == edge_linear_subject.margin.left
          && edge_indexed_subject.padding.bottom == 9
          && edge_indexed_subject.padding.bottom
                 == edge_linear_subject.padding.bottom
          && edge_indexed_eligible.margin.top == 7
          && edge_indexed_eligible.margin.top
                 == edge_linear_eligible.margin.top
          && unsetenv("TILEFINCH_DISABLE_COMPILED_SELECTORS") == 0);
    stylesheet_destroy(&edge_linear);
    stylesheet_destroy(&edge_indexed);
    document_destroy(&edge_document);

    stylesheet_destroy(&linear);
    stylesheet_destroy(&indexed);
    document_destroy(&document);
    CHECK(budget.current == 0);
    puts("style-index-tests status=PASS");
    return 0;
}
