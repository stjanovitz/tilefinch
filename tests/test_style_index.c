#include "tilefinch/budget.h"
#include "tilefinch/document.h"
#include "tilefinch/layout.h"
#include "tilefinch/platform.h"
#include "tilefinch/render.h"
#include "tilefinch/style.h"
#include "../src/style_cache_internal.h"
#include "../src/layout_internal.h"
#undef budget_malloc
#undef budget_calloc
#undef budget_realloc
#include "../src/style_internal.h"

#include <pthread.h>
#include <stdio.h>
#include <ctype.h>
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
        && left->letter_spacing_fixed == right->letter_spacing_fixed
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


/* HTML type selectors match without ASCII case in an HTML document, so the
   rule prefilter, the rule index and the ancestor filter must reach the same
   answer as the matcher for legacy uppercase author CSS. Foreign elements
   keep exact names. */
static int uppercase_type_selector_case(bool indexed)
{
    Budget budget;
    budget_init(&budget, 8u * MIB);
    CHECK(budget_install_lexbor(&budget));
    PocDocument document = {0};
    static const char html[] = "<!doctype html><style>"
        "TD{color:#123456}TD SPAN{padding-top:7px}Td>B{padding-right:5px}"
        "td{padding-bottom:2px}"
        "FOREIGNOBJECT{padding-left:9px}"
        "</style><table><tr><td id=cell><span id=inner>x</span>"
        "<b id=bold>y</b></td></tr></table>"
        "<svg><foreignObject id=fo></foreignObject></svg>";
    CHECK(document_parse(&document, &budget, html, sizeof(html) - 1u, 17));
    if (!indexed) CHECK(setenv("TILEFINCH_DISABLE_STYLE_INDEX", "1", 1) == 0);
    Stylesheet sheet = {0};
    bool built = stylesheet_build(&sheet, &budget, &document, 480);
    CHECK(built);
    lxb_dom_node_t *root = lxb_dom_interface_node(document.html);
    lxb_dom_node_t *cell = find_id(root, "cell");
    lxb_dom_node_t *inner = find_id(root, "inner");
    lxb_dom_node_t *bold = find_id(root, "bold");
    lxb_dom_node_t *foreign = find_id(root, "fo");
    CHECK(cell != NULL && inner != NULL && bold != NULL && foreign != NULL);
    ComputedStyle cell_style = style_for_node(&sheet, cell, NULL);
    ComputedStyle inner_style = style_for_node(&sheet, inner, NULL);
    ComputedStyle bold_style = style_for_node(&sheet, bold, NULL);
    ComputedStyle foreign_style = style_for_node(&sheet, foreign, NULL);
    if (!indexed) CHECK(unsetenv("TILEFINCH_DISABLE_STYLE_INDEX") == 0);
    CHECK(cell_style.color == 0x123456 && cell_style.padding.bottom == 2);
    CHECK(inner_style.padding.top == 7);
    CHECK(bold_style.padding.right == 5);
    CHECK(foreign_style.padding.left == 0);
    CHECK(sheet.rule_index_ready == indexed);
    stylesheet_destroy(&sheet);
    document_destroy(&document);
    CHECK(budget.current == 0);
    return 0;
}

static int test_uppercase_type_selectors(void)
{
    CHECK(uppercase_type_selector_case(true) == 0);
    CHECK(uppercase_type_selector_case(false) == 0);
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

/* Atomic-CSS bundles ship thousands of single-property rules of which a
   page matches about a fifth (chatgpt.com: 712 of 3,432 in this engine).
   A parsed declaration used to carry a whole ComputedStyle, so each
   unmatched rule retained more than 400 bytes of values it never applied.
   Bound the retained and peak cost per distinct unmatched rule, and prove
   the compact values still resolve exactly for the rules that do match. */
static int test_unmatched_declaration_footprint(void)
{
    enum { RULES = 512 };
    static const char base_css[] =
        ".hit{color:#102030}#probe{padding-left:3px}";
    size_t css_capacity = sizeof(base_css) + RULES * 64u;
    char *css = malloc(css_capacity);
    CHECK(css != NULL);
    size_t used = (size_t) snprintf(css, css_capacity, "%s", base_css);
    for (unsigned i = 0; i < RULES; i++) {
        /* Distinct values defeat declaration interning, as they do in a
           real atomic bundle; four property families vary the layout. */
        int written;
        switch (i & 3u) {
        case 0:
            written = snprintf(css + used, css_capacity - used,
                               ".u%u{margin-top:%upx}", i, i + 1u);
            break;
        case 1:
            written = snprintf(css + used, css_capacity - used,
                               ".u%u{color:#%06x}", i, 0x7919u * i);
            break;
        case 2:
            written = snprintf(css + used, css_capacity - used,
                               ".u%u{width:%upx}", i, i + 1u);
            break;
        default:
            written = snprintf(css + used, css_capacity - used,
                               ".u%u{border-radius:%upx}", i, i + 1u);
            break;
        }
        CHECK(written > 0 && (size_t) written < css_capacity - used);
        used += (size_t) written;
    }
    size_t retained[2] = {0}, peak[2] = {0}, rules[2] = {0};
    for (int variant = 0; variant < 2; variant++) {
        Budget budget;
        budget_init(&budget, 16u * MIB);
        CHECK(budget_install_lexbor(&budget));
        PocDocument document = {0};
        static const char html[] = "<div class='hit u5 u6 u7'>"
            "<span id=probe class=u4>x</span></div>";
        CHECK(document_parse(&document, &budget, html,
                             sizeof(html) - 1u, 17));
        size_t before = budget.current;
        budget.peak = budget.current;
        Stylesheet sheet = {0};
        CHECK(stylesheet_build(&sheet, &budget, &document, 480));
        CHECK(stylesheet_add_css(&sheet, variant == 0 ? base_css : css,
                                 variant == 0 ? strlen(base_css) : used));
        lxb_dom_node_t *probe = find_id(
            lxb_dom_interface_node(document.html), "probe");
        CHECK(probe != NULL && probe->parent != NULL);
        ComputedStyle outer = style_for_node(&sheet, probe->parent, NULL);
        ComputedStyle inner = style_for_node(&sheet, probe, &outer);
        CHECK(sheet.rule_index_ready);
        CHECK(inner.padding.left == 3);
        if (variant == 0) {
            CHECK(outer.color == 0x102030);
        } else {
            /* .u5 (color #025d7d) follows .hit in source order; .u6 sets
               width 7px, .u7 radius 8px and .u4 margin-top 5px. */
            CHECK(sheet.count == rules[0] + RULES);
            CHECK(outer.color == 0x025d7d && outer.width == 7
                  && outer.border_radius == 8);
            CHECK(inner.margin.top == 5 && inner.color == 0x025d7d);
        }
        rules[variant] = sheet.count;
        retained[variant] = budget.current - before;
        peak[variant] = budget.peak - before;
        stylesheet_destroy(&sheet);
        document_destroy(&document);
        CHECK(budget.current == 0);
    }
    free(css);
    CHECK(retained[1] > retained[0] && peak[1] > peak[0]);
    size_t retained_per_rule = (retained[1] - retained[0]) / RULES;
    size_t peak_per_rule = (peak[1] - peak[0]) / RULES;
    fprintf(stderr, "style-index unmatched-rule bytes retained=%zu "
            "peak=%zu declaration=%zu\n", retained_per_rule,
            peak_per_rule, sizeof(StyleDeclaration));
    CHECK(retained_per_rule < 320u);
    CHECK(peak_per_rule < 480u);
    return 0;
}

/* Scoped custom properties (theme classes, atomic `--x:` rules) and the
   retained sparse properties are kept as StyleCustomRule records. Those
   carried fixed 192/48/96-byte selector, name and value buffers whatever
   the text length: 360 bytes per record, 1,006 records (362,160 bytes, 61%
   of them unmatched) on the chatgpt.com capture. Bound the per-record cost
   and check that values, names and selectors still resolve exactly. */
static int test_custom_rule_footprint(void)
{
    enum { RULES = 512 };
    static const char base_css[] =
        ".probe{color:var(--tone-7, #000000)}";
    size_t css_capacity = sizeof(base_css) + RULES * 48u;
    char *css = malloc(css_capacity);
    CHECK(css != NULL);
    size_t used = (size_t) snprintf(css, css_capacity, "%s", base_css);
    for (unsigned i = 0; i < RULES; i++) {
        int written = snprintf(css + used, css_capacity - used,
                               ".v%u{--tone-%u:#%06x}", i, i,
                               0x7919u * i);
        CHECK(written > 0 && (size_t) written < css_capacity - used);
        used += (size_t) written;
    }
    size_t retained[2] = {0}, customs[2] = {0};
    for (int variant = 0; variant < 2; variant++) {
        Budget budget;
        budget_init(&budget, 16u * MIB);
        CHECK(budget_install_lexbor(&budget));
        PocDocument document = {0};
        static const char html[] =
            "<div class='v7'><span id=probe class=probe>x</span></div>";
        CHECK(document_parse(&document, &budget, html,
                             sizeof(html) - 1u, 17));
        size_t before = budget.current;
        Stylesheet sheet = {0};
        CHECK(stylesheet_build(&sheet, &budget, &document, 480));
        CHECK(stylesheet_add_css(&sheet, variant == 0 ? base_css : css,
                                 variant == 0 ? strlen(base_css) : used));
        lxb_dom_node_t *probe = find_id(
            lxb_dom_interface_node(document.html), "probe");
        CHECK(probe != NULL && probe->parent != NULL);
        ComputedStyle outer = style_for_node(&sheet, probe->parent, NULL);
        ComputedStyle inner = style_for_node(&sheet, probe, &outer);
        if (variant == 0) {
            CHECK(inner.color == 0x000000);
        } else {
            CHECK(inner.color == 0x7919u * 7u);
            const StyleCustomRule *last = find_custom_rule(
                &sheet, ".v511", "--tone-511");
            CHECK(last != NULL && last->name_length == strlen("--tone-511")
                  && last->selector_length == strlen(".v511")
                  && strcmp(last->value, "#f1b8e7") == 0);
        }
        customs[variant] = sheet.custom_rule_count;
        retained[variant] = budget.current - before;
        stylesheet_destroy(&sheet);
        document_destroy(&document);
        CHECK(budget.current == 0);
    }
    free(css);
    CHECK(customs[1] == customs[0] + RULES && retained[1] > retained[0]);
    size_t per_rule = (retained[1] - retained[0]) / RULES;
    fprintf(stderr, "style-index custom-rule bytes retained=%zu "
            "record=%zu\n", per_rule, sizeof(StyleCustomRule));
    CHECK(per_rule < 160u);
    return 0;
}

static size_t custom_property_text(const Stylesheet *sheet,
                                   lxb_dom_node_t *node, const char *name,
                                   char *output, size_t output_size)
{
    output[0] = '\0';
    if (!style_custom_property_value(sheet, node, PSEUDO_NONE, name,
                                     strlen(name), output, output_size))
        return 0;
    return strlen(output);
}

/* Tailwind v4 composes custom properties longer than the former 96-byte
   value cap (--tw-gradient-stops is 160 bytes, shadow stacks 100 and
   more), and they were dropped. Values are kept in the sheet's text arena
   up to STYLE_CUSTOM_VALUE_CAPACITY, long ones under a per-sheet total;
   anything longer is still ignored, so an earlier declaration wins. */
static int test_long_custom_property_values(void)
{
#define SHADE_LAYER(blur) "0 0 " blur " 0 var(--shade,#000000)"
#define STACK SHADE_LAYER("8px") "," SHADE_LAYER("4px") "," \
    SHADE_LAYER("2px") "," SHADE_LAYER("1px")
    static const char css[] =
        ".outer{--shade:#ff0000;--stack:" STACK ";"
        "--fallback:var(--missing,0 0 8px 0 #102030,0 0 4px 0 #102030,"
        "0 0 2px 0 #102030,0 0 1px 0 #102030,0 0 0 1px #102030,"
        "0 0 0 2px #102030)}"
        ".leaf{--shade:#00ff00}"
        /* GitHub's 170-byte --fontStack-sansSerif, used by font-family. */
        "#long-name{--font-stack:\"Mona Sans VF\", -apple-system, "
        "BlinkMacSystemFont, \"Segoe UI\", \"Noto Sans Backtick Fix\", "
        "\"Noto Sans\", Helvetica, Arial, sans-serif, \"Apple Color Emoji\", "
        "\"Segoe UI Emoji\";font-family:var(--font-stack)}"
        /* A seven-item transition list (328 bytes) and a composed filter
           chain (150 bytes) through var(): each property's parser takes
           a resolved value up to the substitution capacity. */
        "#long-transition{--motion:"
        "color 150ms cubic-bezier(0.4, 0, 0.2, 1),"
        "background-color 150ms cubic-bezier(0.4, 0, 0.2, 1),"
        "border-color 150ms cubic-bezier(0.4, 0, 0.2, 1),"
        "outline-color 150ms cubic-bezier(0.4, 0, 0.2, 1),"
        "text-decoration-color 150ms cubic-bezier(0.4, 0, 0.2, 1),"
        "fill 150ms cubic-bezier(0.4, 0, 0.2, 1),"
        "stroke 150ms cubic-bezier(0.4, 0, 0.2, 1);"
        "transition:var(--motion)}"
        "#long-filter{--fx:blur(0px) brightness(100%) contrast(100%) "
        "grayscale(100%) hue-rotate(0deg) invert(0%) saturate(100%) "
        "sepia(0%) drop-shadow(0 1px 2px rgb(0 0 0 / 0.1));"
        "filter:var(--fx)}"
        "#over{--big:short}#over{--big:";
#undef STACK
#undef SHADE_LAYER
    static const char resolved_stack[] =
        "0 0 8px 0 #ff0000,0 0 4px 0 #ff0000,"
        "0 0 2px 0 #ff0000,0 0 1px 0 #ff0000";
    Budget budget;
    budget_init(&budget, 16u * MIB);
    CHECK(budget_install_lexbor(&budget));
    PocDocument document = {0};
    static const char html[] =
        "<div id=outer class=outer><span id=inner class=leaf>x</span></div>"
        "<p id=over></p><p id=long-name></p><p id=long-transition></p>"
        "<p id=long-filter></p>";
    CHECK(document_parse(&document, &budget, html, sizeof(html) - 1u, 17));
    lxb_dom_node_t *root = lxb_dom_interface_node(document.html);
    lxb_dom_node_t *outer = find_id(root, "outer");
    lxb_dom_node_t *inner = find_id(root, "inner");
    lxb_dom_node_t *over = find_id(root, "over");
    lxb_dom_node_t *long_name = find_id(root, "long-name");
    lxb_dom_node_t *long_transition = find_id(root, "long-transition");
    lxb_dom_node_t *long_filter = find_id(root, "long-filter");
    CHECK(outer != NULL && inner != NULL && over != NULL
          && long_name != NULL && long_transition != NULL
          && long_filter != NULL);

    /* The prefix plus one value past the bound, then a 600-byte value
       whose var() names resolve to well under the substitution capacity. */
    size_t over_length = STYLE_CUSTOM_VALUE_CAPACITY + 64u;
    size_t text_capacity = sizeof(css) + over_length + 2048u;
    char *text = malloc(text_capacity);
    CHECK(text != NULL);
    size_t used = (size_t) snprintf(text, text_capacity, "%s", css);
    memset(text + used, 'x', over_length);
    used += over_length;
    used += (size_t) snprintf(text + used, text_capacity - used,
                              "}#long-name{--shrinks:");
    size_t shrinks_start = used;
    while (used - shrinks_start < 600u) {
        used += (size_t) snprintf(text + used, text_capacity - used,
                                  "var(--an-unset-but-very-descriptive-name,"
                                  "1px) ");
    }
    used += (size_t) snprintf(text + used, text_capacity - used, "}");
    CHECK(used < text_capacity);

    Stylesheet sheet = {0};
    CHECK(stylesheet_build(&sheet, &budget, &document, 480));
    uint64_t drops = sheet.diagnostic_custom_property_drops;
    CHECK(stylesheet_add_css(&sheet, text, used));
    free(text);

    char value[640];
    /* Nested var() with fallbacks inside a 150-byte value resolves on the
       declaring element; the child inherits that result, so its own
       --shade override does not leak in. */
    CHECK(custom_property_text(&sheet, outer, "--stack", value,
                               sizeof(value)) == strlen(resolved_stack)
          && strcmp(value, resolved_stack) == 0);
    CHECK(custom_property_text(&sheet, inner, "--stack", value,
                               sizeof(value)) == strlen(resolved_stack)
          && strcmp(value, resolved_stack) == 0);
    CHECK(custom_property_text(&sheet, inner, "--fallback", value,
                               sizeof(value)) > 96u
          && strncmp(value, "0 0 8px 0 #102030,", 18) == 0);
    /* A value past the bound is ignored: the earlier declaration wins. */
    CHECK(custom_property_text(&sheet, over, "--big", value,
                               sizeof(value)) == 5
          && strcmp(value, "short") == 0);
    CHECK(sheet.diagnostic_custom_property_drops == drops + 1u);
    const StyleCustomRule *shrinks = NULL;
    for (size_t i = 0; i < sheet.custom_rule_count; i++) {
        if (strcmp(sheet.custom_rules[i].name, "--shrinks") == 0)
            shrinks = &sheet.custom_rules[i];
    }
    CHECK(shrinks != NULL && strlen(shrinks->value) >= 600u);
    size_t shrunk = custom_property_text(&sheet, long_name, "--shrinks",
                                         value, sizeof(value));
    CHECK(shrunk > 0 && shrunk < 200u && strncmp(value, "1px 1px", 7) == 0);
    ComputedStyle stacked = style_for_node(&sheet, long_name, NULL);
    CHECK(stacked.font_family == FONT_SYSTEM_SANS);
    StyleTransitionComputed motion;
    CHECK(style_transition_computed(&sheet, long_transition, PSEUDO_NONE,
                                    &motion));
    CHECK(motion.property_count == 7 && motion.duration_count == 7
          && strcmp(motion.properties[0], "color") == 0
          && strcmp(motion.properties[6], "stroke") == 0
          && motion.duration_ms[6] == 150.0);
    ComputedStyle filtered = style_for_node(&sheet, long_filter, NULL);
    CHECK(filtered.has_filter
          && (filtered.filter_code & STYLE_FILTER_CODE_MASK)
             == STYLE_FILTER_GRAYSCALE);

    /* The layout variable cache keeps long resolved values too (in its
       bounded spill), so inherited lookups hit instead of re-walking. */
    CHECK(style_variable_cache_begin(&sheet, &budget));
    size_t cache_bytes = style_variable_cache_bytes(&sheet);
    CHECK(custom_property_text(&sheet, inner, "--stack", value,
                               sizeof(value)) == strlen(resolved_stack));
    uint64_t hits = sheet.variable_cache_hits;
    CHECK(custom_property_text(&sheet, inner, "--stack", value,
                               sizeof(value)) == strlen(resolved_stack)
          && strcmp(value, resolved_stack) == 0
          && sheet.variable_cache_hits > hits);
    CHECK(style_variable_cache_bytes(&sheet)
          <= cache_bytes + STYLE_VARIABLE_CACHE_SPILL_BYTES);
    style_variable_cache_end(&sheet);
    stylesheet_destroy(&sheet);

    /* Long values share one per-sheet total: once it is spent, further
       long declarations are dropped and the sheet's growth stays bounded. */
    enum { LONG_RULES = 96 };
    size_t long_value = STYLE_CUSTOM_VALUE_CAPACITY - 1u;
    size_t many_capacity = LONG_RULES * (long_value + 32u);
    char *many = malloc(many_capacity);
    CHECK(many != NULL);
    size_t many_used = 0;
    for (unsigned i = 0; i < LONG_RULES; i++) {
        many_used += (size_t) snprintf(many + many_used,
                                       many_capacity - many_used,
                                       "#r%u{--long:", i);
        memset(many + many_used, 'a' + (char) (i % 26u), long_value);
        many_used += long_value;
        many[many_used++] = '}';
    }
    CHECK(many_used < many_capacity);
    Stylesheet bounded = {0};
    CHECK(stylesheet_build(&bounded, &budget, &document, 480));
    size_t before = budget.current;
    drops = bounded.diagnostic_custom_property_drops;
    CHECK(stylesheet_add_css(&bounded, many, many_used));
    free(many);
    size_t kept = 0;
    for (size_t i = 0; i < bounded.custom_rule_count; i++) {
        if (strcmp(bounded.custom_rules[i].name, "--long") == 0) kept++;
    }
    size_t limit_rules = STYLE_CUSTOM_LONG_VALUE_BYTES / (long_value + 1u);
    fprintf(stderr, "style-index long custom values kept=%zu dropped=%llu "
            "growth=%zu\n", kept,
            (unsigned long long) (bounded.diagnostic_custom_property_drops
                                  - drops),
            budget.current - before);
    CHECK(kept == limit_rules && kept < LONG_RULES
          && bounded.diagnostic_custom_property_drops
             == drops + (LONG_RULES - kept));
    CHECK(budget.current - before
          < STYLE_CUSTOM_LONG_VALUE_BYTES + 64u * 1024u);
    stylesheet_destroy(&bounded);
    document_destroy(&document);
    CHECK(budget.current == 0);
    return 0;
}

/* @property registrations (CSS Properties and Values API): Tailwind v4
   registers its --tw-* properties and only falls back to a universal
   reset block behind an @supports query Chromium (and this engine)
   answers false, so without them --tw-gradient-from-position and
   friends are undefined and every gradient is invalid. A non-inheriting
   registration sees only the element's own declaration, else its
   initial value; an inheriting one inherits and starts from it. */
static int test_registered_custom_properties(void)
{
    static const char css[] =
        "@property --reg-color{syntax:\"<color>\";inherits:false;"
        "initial-value:#0000}"
        "@property --reg-length{syntax:'<length>';inherits:true;"
        "initial-value:5px}"
        "@property --reg-any{syntax:\"*\";inherits:false}"
        "@property --reg-unit{syntax:'<length>';inherits:true;initial-value:1px}"
        /* No initial-value for a typed syntax: invalid, ignored. */
        "@property --reg-bad{syntax:\"<length>\";inherits:false}"
        "@layer properties{@supports (-webkit-hyphens:none){"
        "*{--reg-color:#123456}}}"
        ".outer{--reg-color:#ff0000;--reg-any:abc;--reg-bad:outer}"
        ".reset{--reg-length:initial}"
        ".outer{font-size:16px;--reg-unit:calc(1em / 16)}"
        "#inner{font-size:10px;width:calc(40 * var(--reg-unit))}";
    Budget budget;
    budget_init(&budget, 16u * MIB);
    CHECK(budget_install_lexbor(&budget));
    PocDocument document = {0};
    static const char html[] =
        "<div id=outer class=outer><span id=inner>x</span>"
        "<span id=reset class=reset>y</span></div>";
    CHECK(document_parse(&document, &budget, html, sizeof(html) - 1u, 17));
    lxb_dom_node_t *root = lxb_dom_interface_node(document.html);
    lxb_dom_node_t *outer = find_id(root, "outer");
    lxb_dom_node_t *inner = find_id(root, "inner");
    lxb_dom_node_t *reset = find_id(root, "reset");
    CHECK(outer != NULL && inner != NULL && reset != NULL);
    Stylesheet sheet = {0};
    CHECK(stylesheet_build(&sheet, &budget, &document, 480));
    uint64_t signature = stylesheet_parse_context_signature(&sheet);
    CHECK(stylesheet_add_css(&sheet, css, sizeof(css) - 1u));
    CHECK(sheet.registered_property_count == 4
          && stylesheet_parse_context_signature(&sheet) != signature);
    char value[64];
    CHECK(custom_property_text(&sheet, outer, "--reg-color", value,
                               sizeof(value)) == 7
          && strcmp(value, "#ff0000") == 0);
    CHECK(custom_property_text(&sheet, inner, "--reg-color", value,
                               sizeof(value)) == 5
          && strcmp(value, "#0000") == 0);
    CHECK(custom_property_text(&sheet, inner, "--reg-length", value,
                               sizeof(value)) == 3
          && strcmp(value, "5px") == 0);
    CHECK(custom_property_text(&sheet, inner, "--reg-unit", value,
                               sizeof(value)) == 3
          && strcmp(value, "1px") == 0);
    ComputedStyle outer_style = style_for_node(&sheet, outer, NULL);
    ComputedStyle inner_style = style_for_node(&sheet, inner, &outer_style);
    CHECK(inner_style.width == 40);
    CHECK(custom_property_text(&sheet, reset, "--reg-length", value,
                               sizeof(value)) == 3
          && strcmp(value, "5px") == 0);
    CHECK(custom_property_text(&sheet, outer, "--reg-any", value,
                               sizeof(value)) == 3);
    CHECK(custom_property_text(&sheet, inner, "--reg-any", value,
                               sizeof(value)) == 0);
    CHECK(custom_property_text(&sheet, inner, "--reg-bad", value,
                               sizeof(value)) == 5
          && strcmp(value, "outer") == 0);
    /* Through the layout variable cache as well. */
    CHECK(style_variable_cache_begin(&sheet, &budget));
    for (int pass = 0; pass < 2; pass++) {
        CHECK(custom_property_text(&sheet, inner, "--reg-color", value,
                                   sizeof(value)) == 5
              && custom_property_text(&sheet, outer, "--reg-color", value,
                                      sizeof(value)) == 7
              && custom_property_text(&sheet, inner, "--reg-any", value,
                                      sizeof(value)) == 0);
    }
    style_variable_cache_end(&sheet);
    stylesheet_destroy(&sheet);
    /* Registration storage refusal must not install a partial entry. */
    char registration_initial[256];
    memset(registration_initial, 'a', sizeof(registration_initial) - 1u);
    registration_initial[sizeof(registration_initial) - 1u] = '\0';
    for (size_t failure = 0; failure < 8; failure++) {
        CHECK(stylesheet_build(&sheet, &budget, &document, 480));
        DocumentRegisteredProperty registration = {
            .name = "--allocation-test", .initial_value = registration_initial,
            .inherits = true
        };
        budget_inject_failure_after(&budget, failure);
        bool registered = stylesheet_register_api_property(&sheet, &registration);
        budget_clear_failure_injection(&budget);
        CHECK(sheet.registered_property_count == (registered ? 1u : 0u));
        CHECK(sheet.custom_value_long_bytes
              == (registered ? sizeof(registration_initial) : 0u));
        CHECK(stylesheet_register_api_property(&sheet, &registration));
        CHECK(sheet.registered_property_count == 1);
        stylesheet_destroy(&sheet);
    }
    document_destroy(&document);
    CHECK(budget.current == 0);
    return 0;
}

/* A registered <length> in em converts on the declaring element, which
   needs that element's font. Sibling components must not each re-cascade
   the whole shared ancestor chain to find it, and the values must equal
   the uncached resolution's. */
static int test_registered_length_ancestor_reuse(void)
{
    enum { DEPTH = 15, CARDS = 100 };
    static const char css[] =
        "@property --gap{syntax:'<length>';inherits:true;initial-value:0px}"
        ".deep{font-size:12px}.card{--gap:1em;font-size:20px}"
        ".card i{font-size:10px;width:var(--gap)}"
        ".card.big{font-size:30px}";
    char html[8192];
    size_t used = 0;
    for (int i = 0; i < DEPTH; i++)
        used += (size_t) snprintf(html + used, sizeof(html) - used,
                                  "<div class=deep>");
    for (int i = 0; i < CARDS; i++)
        used += (size_t) snprintf(html + used, sizeof(html) - used,
            "<p class='card%s'><i id=c%d>x</i></p>",
            i % 7 == 3 ? " big" : "", i);
    CHECK(used < sizeof(html));
    Budget budget;
    budget_init(&budget, 16u * MIB);
    CHECK(budget_install_lexbor(&budget));
    PocDocument document = {0};
    CHECK(document_parse(&document, &budget, html, used, 17));
    Stylesheet sheet = {0};
    CHECK(stylesheet_build(&sheet, &budget, &document, 480));
    CHECK(stylesheet_add_css(&sheet, css, sizeof(css) - 1u));
    lxb_dom_node_t *root = lxb_dom_interface_node(document.html);
    int expected[CARDS];
    /* Uncached: every lookup converts afresh. */
    for (int i = 0; i < CARDS; i++) {
        char id[8];
        snprintf(id, sizeof(id), "c%d", i);
        lxb_dom_node_t *leaf = find_id(root, id);
        CHECK(leaf != NULL);
        char value[64];
        CHECK(custom_property_text(&sheet, leaf, "--gap", value,
                                   sizeof(value)) > 0);
        expected[i] = atoi(value);
        CHECK(expected[i] == (i % 7 == 3 ? 30 : 20));
    }
    /* A layout-like pass: one variable cache, top-down styles. */
    CHECK(style_variable_cache_begin(&sheet, &budget));
    uint64_t queries = sheet.rule_index_queries;
    for (int i = 0; i < CARDS; i++) {
        char id[8];
        snprintf(id, sizeof(id), "c%d", i);
        lxb_dom_node_t *leaf = find_id(root, id);
        char value[64];
        CHECK(custom_property_text(&sheet, leaf, "--gap", value,
                                   sizeof(value)) > 0
              && atoi(value) == expected[i]);
    }
    uint64_t spent = sheet.rule_index_queries - queries;
    style_variable_cache_end(&sheet);
    fprintf(stderr, "registered length reuse: %llu rule queries for %d "
            "cards at depth %d\n", (unsigned long long) spent, CARDS, DEPTH);
    /* Each card's own cascade, not its whole chain (DEPTH + 3 each). */
    CHECK(spent < (uint64_t) CARDS * 4u * 3u);
    stylesheet_destroy(&sheet);
    document_destroy(&document);
    CHECK(budget.current == 0);
    return 0;
}

/* Design-token names past the cache slot's inline bytes are still cached:
   a sibling's lookup is answered by its parent's entry instead of a winner
   search on every ancestor. */
static int test_long_variable_names_cached(void)
{
    enum { ITEMS = 40 };
    static const char css[] =
        ":root{--wp--preset--gradient--vivid-cyan-blue-to-vivid-purple-x:"
        "#123456;--short:var(--wp--preset--gradient--vivid-cyan-blue-to-"
        "vivid-purple-x)}"
        ".item{color:var(--wp--preset--gradient--vivid-cyan-blue-to-vivid-"
        "purple-x)}.item b{color:var(--short)}";
    char html[4096];
    size_t used = (size_t) snprintf(html, sizeof(html), "<div><div>");
    for (int i = 0; i < ITEMS; i++)
        used += (size_t) snprintf(html + used, sizeof(html) - used,
                                  "<p class=item id=p%d><b>x</b></p>", i);
    CHECK(used < sizeof(html));
    Budget budget;
    budget_init(&budget, 16u * MIB);
    CHECK(budget_install_lexbor(&budget));
    PocDocument document = {0};
    CHECK(document_parse(&document, &budget, html, used, 17));
    Stylesheet sheet = {0};
    CHECK(stylesheet_build(&sheet, &budget, &document, 480));
    CHECK(stylesheet_add_css(&sheet, css, sizeof(css) - 1u));
    lxb_dom_node_t *root = lxb_dom_interface_node(document.html);
    CHECK(style_variable_cache_begin(&sheet, &budget));
    uint64_t hits = sheet.variable_cache_hits;
    for (int i = 0; i < ITEMS; i++) {
        char id[8];
        snprintf(id, sizeof(id), "p%d", i);
        lxb_dom_node_t *item = find_id(root, id);
        CHECK(item != NULL && item->first_child != NULL);
        ComputedStyle style = style_for_node(&sheet, item, NULL);
        ComputedStyle bold = style_for_node(&sheet, item->first_child,
                                            &style);
        CHECK(style.color == 0x123456 && bold.color == 0x123456);
    }
    uint64_t gained = sheet.variable_cache_hits - hits;
    style_variable_cache_end(&sheet);
    fprintf(stderr, "long variable names: %llu cache hits for %d items\n",
            (unsigned long long) gained, ITEMS);
    CHECK(gained >= (uint64_t) ITEMS * 2u - 2u);
    stylesheet_destroy(&sheet);
    document_destroy(&document);
    CHECK(budget.current == 0);
    return 0;
}

/* Motion metadata keeps exactly the animation declarations of each rule,
   in any ASCII case, and nothing from rules without one. */
static int test_motion_source_declarations(void)
{
    static const char css[] =
        ".plain{color:red;transition:opacity 1s}"
        ".named{color:blue;Animation-Name:spin;margin:0}"
        ".vendor{-WEBKIT-animation:spin 1s}"
        ".shorthand{animation:spin 2s linear}"
        ".quoted{content:'animation:none'}"
        "@keyframes spin{from{opacity:0}to{opacity:1}}";
    Budget budget;
    budget_init(&budget, 8u * MIB);
    CHECK(budget_install_lexbor(&budget));
    PocDocument document = {0};
    static const char html[] = "<p>x</p>";
    CHECK(document_parse(&document, &budget, html, sizeof(html) - 1u, 17));
    Stylesheet sheet = {0};
    CHECK(stylesheet_build(&sheet, &budget, &document, 480));
    CHECK(stylesheet_add_css(&sheet, css, sizeof(css) - 1u));
    CHECK(sheet.motion_css != NULL);
    CHECK(strstr(sheet.motion_css, ".named{Animation-Name:spin;}") != NULL);
    CHECK(strstr(sheet.motion_css, ".vendor{-WEBKIT-animation:spin 1s;}")
          != NULL);
    CHECK(strstr(sheet.motion_css, ".shorthand{animation:spin 2s linear;}")
          != NULL);
    CHECK(strstr(sheet.motion_css, ".plain") == NULL
          && strstr(sheet.motion_css, ".quoted") == NULL);
    CHECK(sheet.motion_css_rules == 3 && sheet.motion_css_keyframes == 1);
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
    /* Keyed anchors nothing in the head's path carries, an unkeyed anchor
       that needs an ancestor <html> lacks, and an escaped selector without
       :has() (which the old lexical scan refused outright). */
    static const char css[] =
        ".observer:has(script) p{color:red}"
        ".wrap :is(.a,.b):has(+:not(.c)){color:red}"
        ".esc\\:wide{width:1px}";
    CHECK(stylesheet_add_css(&sheet, css, sizeof(css) - 1u));
    lxb_dom_node_t *head = lxb_dom_interface_node(
        lxb_html_document_head_element(document.html));
    CHECK(!stylesheet_head_scripts_affect_ancestors(&sheet, head, NULL));
    uint64_t scans = sheet.head_script_selector_scans;
    CHECK(scans != 0);
    /* The keyed anchor is checked against the live head. */
    CHECK(lxb_dom_element_set_attribute(lxb_dom_interface_element(head),
        (const lxb_char_t *) "class", 5,
        (const lxb_char_t *) "observer", 8) != NULL);
    CHECK(stylesheet_head_scripts_affect_ancestors(&sheet, head, NULL));
    CHECK(lxb_dom_element_remove_attribute(lxb_dom_interface_element(head),
        (const lxb_char_t *) "class", 5) == LXB_STATUS_OK);
    CHECK(!stylesheet_head_scripts_affect_ancestors(&sheet, head, NULL));
    /* So is the unkeyed one: <html> class=a matches `:is(.a,.b)` but not
       `.wrap :is(.a,.b)`; the head under a .wrap <html> does. */
    lxb_dom_node_t *root = head->parent;
    CHECK(lxb_dom_element_set_attribute(lxb_dom_interface_element(root),
        (const lxb_char_t *) "class", 5, (const lxb_char_t *) "a", 1) != NULL);
    CHECK(!stylesheet_head_scripts_affect_ancestors(&sheet, head, NULL));
    CHECK(lxb_dom_element_set_attribute(lxb_dom_interface_element(root),
        (const lxb_char_t *) "class", 5,
        (const lxb_char_t *) "wrap", 4) != NULL);
    CHECK(lxb_dom_element_set_attribute(lxb_dom_interface_element(head),
        (const lxb_char_t *) "class", 5, (const lxb_char_t *) "b", 1) != NULL);
    CHECK(stylesheet_head_scripts_affect_ancestors(&sheet, head, NULL));
    CHECK(lxb_dom_element_remove_attribute(lxb_dom_interface_element(head),
        (const lxb_char_t *) "class", 5) == LXB_STATUS_OK);
    CHECK(lxb_dom_element_remove_attribute(lxb_dom_interface_element(root),
        (const lxb_char_t *) "class", 5) == LXB_STATUS_OK);
    CHECK(!stylesheet_head_scripts_affect_ancestors(&sheet, head, NULL));
    /* An anchor <html> carries, and a universal one, are refused. */
    static const char tagged[] = "html:has(script){color:blue}";
    CHECK(stylesheet_add_css(&sheet, tagged, sizeof(tagged) - 1u));
    CHECK(stylesheet_head_scripts_affect_ancestors(&sheet, head, NULL));
    Stylesheet universal = {0};
    CHECK(stylesheet_build(&universal, &budget, &document, 480));
    static const char any[] = ".x>p,:has(> script) p{color:blue}";
    CHECK(stylesheet_add_css(&universal, any, sizeof(any) - 1u));
    CHECK(stylesheet_head_scripts_affect_ancestors(&universal, head, NULL));
    /* A change at the <title> cannot be what `> script` finds. */
    lxb_dom_node_t *title = head->first_child;
    while (title != NULL && title->type != LXB_DOM_NODE_TYPE_ELEMENT)
        title = title->next;
    CHECK(title != NULL
          && !stylesheet_head_scripts_affect_ancestors(&universal, head,
                                                       title));
    stylesheet_destroy(&universal);
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

static int test_computed_style_payload_storage(void)
{
    Budget budget;
    budget_init(&budget, 8u * MIB);
    CHECK(budget_install_lexbor(&budget));
    char html[12000];
    size_t used = 0;
    for (unsigned i = 0; i < 260; i++) {
        int n = snprintf(html + used, sizeof(html) - used,
                         "<p id=n%u>payload</p>", i);
        CHECK(n > 0 && (size_t) n < sizeof(html) - used);
        used += (size_t) n;
    }
    PocDocument document = {0};
    Stylesheet sheet = {0};
    CHECK(document_parse(&document, &budget, html, used, 17)
          && stylesheet_build(&sheet, &budget, &document, 480));
    static const char css[] = "p{color:#123456;width:37px}";
    CHECK(stylesheet_add_css(&sheet, css, sizeof(css) - 1u));
    lxb_dom_node_t *nodes[260];
    for (unsigned i = 0; i < 260; i++) {
        char id[16];
        snprintf(id, sizeof(id), "n%u", i);
        nodes[i] = find_id(lxb_dom_interface_node(document.html), id);
        CHECK(nodes[i] != NULL);
    }
    LayoutReuseCache *reuse = layout_reuse_cache_create(&budget);
    CHECK(reuse != NULL);
    layout_reuse_cache_expect_elements(reuse, 3000);
    layout_reuse_cache_prepare(reuse, &sheet, NULL, NULL, 480);
    CHECK(reuse->style_mask + 1u == 4096u);
    LayoutReuseStats stats;
    layout_reuse_cache_stats(reuse, &stats);
    printf("computed-style storage: slots=%zu slot_bytes=%zu style_bytes=%zu "
           "empty_bytes=%zu\n", reuse->style_mask + 1u,
           sizeof(LayoutReuseStyleEntry), sizeof(ComputedStyle),
           stats.retained_bytes);
    CHECK(stats.retained_bytes < 512u * 1024u);
    ComputedStyle parent = layout_initial_root_style(), result;
    /* Warm independent resolver scratch before refusing optional payloads. */
    ComputedStyle expected = style_for_node(&sheet, nodes[0], &parent);
    size_t limit = budget.limit, retained = budget.current;
    size_t failures = budget.failure_count;
    budget.limit = budget.current;
    for (unsigned i = 0; i < 20; i++) {
        CHECK(!layout_reuse_cache_resolve_style(reuse, &sheet, NULL,
                                               nodes[0], &parent, &result));
        CHECK(result.color == expected.color && result.width == expected.width);
        CHECK(budget.current == retained);
    }
    CHECK(budget.failure_count == failures);
    budget.limit = limit;
    /* Pressure preflight avoids an allocator refusal. Still exercise the
       real allocation rollback separately while there is ordinary room. */
    budget_inject_failure_after(&budget, 0);
    CHECK(!layout_reuse_cache_resolve_style(reuse, &sheet, NULL,
                                           nodes[0], &parent, &result));
    budget_clear_failure_injection(&budget);
    CHECK(budget.failure_count == failures + 1u && budget.current == retained
        && result.color == expected.color && result.width == expected.width);
    layout_reuse_cache_reset(reuse);
    layout_reuse_cache_prepare(reuse, &sheet, NULL, NULL, 480);
    for (unsigned i = 0; i < 128; i++) {
        CHECK(!layout_reuse_cache_resolve_style(reuse, &sheet, NULL,
                                               nodes[i], &parent, &result));
        CHECK(result.color == 0x123456 && result.width == 37);
    }
    retained = budget.current;
    failures = budget.failure_count;
    budget.limit = retained;
    for (unsigned i = 128; i < 148; i++) {
        CHECK(!layout_reuse_cache_resolve_style(reuse, &sheet, NULL,
                                               nodes[i], &parent, &result));
        CHECK(result.color == 0x123456 && budget.current == retained);
    }
    CHECK(budget.failure_count == failures);
    for (unsigned i = 0; i < 128; i++)
        CHECK(layout_reuse_cache_resolve_style(reuse, &sheet, NULL,
                                              nodes[i], &parent, &result));
    /* A scoped drop recycles storage even after growth was refused. */
    layout_reuse_cache_invalidate_inline_style(reuse, nodes[0], "color", false);
    CHECK(!layout_reuse_cache_resolve_style(reuse, &sheet, NULL,
                                           nodes[128], &parent, &result));
    CHECK(layout_reuse_cache_resolve_style(reuse, &sheet, NULL,
                                          nodes[128], &parent, &result));
    CHECK(budget.current == retained);
    budget.limit = limit;
    layout_reuse_cache_reset(reuse);
    layout_reuse_cache_prepare(reuse, &sheet, NULL, NULL, 480);
    for (unsigned i = 0; i < 260; i++)
        (void) layout_reuse_cache_resolve_style(reuse, &sheet, NULL,
                                               nodes[i], &parent, &result);
    for (unsigned i = 0; i < 260; i++) {
        CHECK(layout_reuse_cache_resolve_style(reuse, &sheet, NULL,
                                              nodes[i], &parent, &result));
        CHECK(result.color == 0x123456 && result.width == 37);
    }
    layout_reuse_cache_stats(reuse, &stats);
    /* Capacity remains 4096, but full styles are owned only for the walk. */
    CHECK(stats.retained_bytes < 512u * 1024u);
    ComputedStyle absent = {0};
    layout_reuse_note_pseudo(reuse, &sheet, nodes[0], PSEUDO_BEFORE,
                             &result, &absent);
    CHECK(layout_reuse_pseudo_absent(reuse, &sheet, nodes[0], PSEUDO_BEFORE,
                                     &result));
    /* Font publication only drops metric-dependent records, preserving all
       other payloads and the pseudo proof. Mark one cached dependency as a
       deterministic unit seam; real font publication is covered separately. */
    for (size_t i = 0; i <= reuse->style_mask; i++)
        if (reuse->styles[i].node == nodes[1])
            reuse->styles[i].dependent |= LAYOUT_REUSE_FONT_DEPENDENT;
    layout_reuse_cache_begin_font_publication(reuse);
    CHECK(layout_reuse_cache_resolve_style(reuse, &sheet, NULL,
                                          nodes[0], &parent, &result));
    CHECK(!layout_reuse_cache_resolve_style(reuse, &sheet, NULL,
                                           nodes[1], &parent, &result));
    layout_reuse_cache_end_font_publication(reuse);
    CHECK(layout_reuse_pseudo_absent(reuse, &sheet, nodes[0], PSEUDO_BEFORE,
                                     &result));
    static const char changed[] = "p{color:#654321}";
    CHECK(stylesheet_add_css(&sheet, changed, sizeof(changed) - 1u));
    layout_reuse_cache_prepare(reuse, &sheet, NULL, NULL, 480);
    CHECK(!layout_reuse_cache_resolve_style(reuse, &sheet, NULL,
                                           nodes[0], &parent, &result));
    CHECK(result.color == 0x654321);
    CHECK(!layout_reuse_pseudo_absent(reuse, &sheet, nodes[0], PSEUDO_BEFORE,
                                      &result));
    layout_reuse_cache_destroy(reuse);
    stylesheet_destroy(&sheet);
    document_destroy(&document);
    CHECK(budget.current == 0 && budget_uninstall_lexbor(&budget));
    return 0;
}

static int test_retained_cache_eligibility(void)
{
    Budget budget;
    budget_init(&budget, 8u * MIB);
    CHECK(budget_install_lexbor(&budget));
    PocDocument document = {0};
    static const char html[] = "<p>cache eligibility</p>";
    CHECK(document_parse(&document, &budget, html, sizeof(html) - 1u, 17));
    Stylesheet ordinary = {0}, containers = {0};
    CHECK(stylesheet_build(&ordinary, &budget, &document, 480));
    CHECK(stylesheet_build(&containers, &budget, &document, 480));
    for (unsigned i = 0; i < 64; i++) {
        char css[64];
        int length = snprintf(css, sizeof(css), ".pad%u{color:red}", i);
        CHECK(length > 0
              && stylesheet_add_css(&ordinary, css, (size_t) length)
              && stylesheet_add_css(&containers, css, (size_t) length));
    }
    static const char query[] =
        "@container (min-width: 100px){.pad0{color:blue}}";
    CHECK(stylesheet_add_css(&containers, query, sizeof(query) - 1u));
    CHECK(stylesheet_has_container_queries(&containers));
    LayoutReuseCache *reuse = layout_reuse_cache_create(&budget);
    CHECK(reuse != NULL);
    layout_reuse_cache_enable_retained_matches(reuse);
    layout_reuse_cache_prepare(reuse, &containers, NULL, NULL, 480);
    CHECK(reuse->matches != NULL && reuse->matches_attempted);
    layout_reuse_cache_prepare(reuse, &ordinary, NULL, NULL, 480);
    CHECK(reuse->matches != NULL);
    LayoutReuseStats eligible, conditional;
    layout_reuse_cache_stats(reuse, &eligible);
    size_t match_bytes = style_retained_matches_bytes(reuse->matches);
    layout_reuse_cache_prepare(reuse, &containers, NULL, NULL, 480);
    layout_reuse_cache_stats(reuse, &conditional);
    CHECK(reuse->matches != NULL && reuse->matches_attempted
          && style_retained_matches_bytes(reuse->matches) == match_bytes
          && conditional.retained_bytes == eligible.retained_bytes);
    Stylesheet too_large = { .count = (size_t) UINT16_MAX + 1u };
    CHECK(!style_retained_matches_supported(&too_large));
    layout_reuse_cache_prepare(reuse, &ordinary, NULL, NULL, 480);
    CHECK(reuse->matches != NULL);
    layout_reuse_cache_destroy(reuse);
    reuse = layout_reuse_cache_create(&budget);
    CHECK(reuse != NULL);
    layout_reuse_cache_enable_retained_matches(reuse);
    size_t limit = budget.limit;
    budget.limit = budget.current;
    layout_reuse_cache_prepare(reuse, &containers, NULL, NULL, 480);
    CHECK(reuse->matches == NULL && reuse->matches_attempted);
    budget.limit = limit;
    layout_reuse_cache_destroy(reuse);
    stylesheet_destroy(&ordinary);
    stylesheet_destroy(&containers);
    document_destroy(&document);
    CHECK(budget.current == 0 && budget_uninstall_lexbor(&budget));
    return 0;
}

static int test_unrelated_container_custom_rules(void)
{
    Budget budget;
    budget_init(&budget, 8u * MIB);
    CHECK(budget_install_lexbor(&budget));
    PocDocument document = {0};
    Stylesheet sheet = {0};
    static const char page[] =
        "<style>.target{color:var(--tone,red)}"
        "@container (width > 100px){.missing{--tone:blue}}"
        "</style><p id=target class=target>Text</p>";
    CHECK(document_parse(&document, &budget, page, sizeof(page) - 1u, 17)
          && stylesheet_build(&sheet, &budget, &document, 480));
    lxb_dom_node_t *node = find_id(lxb_dom_interface_node(document.html), "target");
    CHECK(node != NULL);
    uint32_t before = style_container_consults(&sheet);
    ComputedStyle result = style_for_node(&sheet, node, NULL);
    CHECK(result.color == 0xff0000u);
    CHECK(style_container_consults(&sheet) == before);
    stylesheet_destroy(&sheet);
    document_destroy(&document);
    CHECK(budget.current == 0 && budget_uninstall_lexbor(&budget));
    return 0;
}

/* Container conditions use the media grammar: calc() and every length
   unit, inline-size/block-size, vw against the viewport, and media-only
   features are unknown (false). */
static int test_container_condition_grammar(void)
{
    Budget budget;
    budget_init(&budget, 8u * MIB);
    CHECK(budget_install_lexbor(&budget));
    PocDocument document = {0};
    Stylesheet sheet = {0};
    static const char page[] =
        "<!doctype html><style>.box{container-type:inline-size}"
        "p{color:#000000}"
        "@container (min-width: calc(20rem + 1px)){#calc{color:#ff0000}}"
        "@container (inline-size > 50vw){#vw{color:#ff0000}}"
        "@container (width >= 3in){#inch{color:#ff0000}}"
        "@container (hover: none){#media{color:#ff0000}}"
        "</style><div id=box class=box><p id=calc>A</p><p id=vw>B</p>"
        "<p id=inch>C</p><p id=media>D</p></div>";
    CHECK(document_parse(&document, &budget, page, sizeof(page) - 1u, 17)
          && stylesheet_build(&sheet, &budget, &document, 480));
    lxb_dom_node_t *root = lxb_dom_interface_node(document.html);
    lxb_dom_node_t *box = find_id(root, "box");
    static const char *const ids[] = {"calc", "vw", "inch", "media"};
    /* 321 px is the calc() threshold, 240 px half the viewport, 288 px
       three inches. Hover is a media feature, never a container one. */
    static const struct { int width; uint32_t colors[4]; } turns[] = {
        {320, {0x000000, 0xff0000, 0xff0000, 0x000000}},
        {321, {0xff0000, 0xff0000, 0xff0000, 0x000000}},
        {240, {0x000000, 0x000000, 0x000000, 0x000000}},
    };
    for (size_t turn = 0; turn < sizeof(turns) / sizeof(turns[0]); turn++) {
        CHECK(style_container_layout_state_begin(&sheet, &budget, 16)
              && style_container_layout_state_add(
                  &sheet, box, turns[turn].width, 100, 0, 0));
        for (size_t i = 0; i < 4; i++) {
            ComputedStyle style = style_for_node(
                &sheet, find_id(root, ids[i]), NULL);
            if (style.color != turns[turn].colors[i])
                fprintf(stderr, "container %d px #%s=%06x, want %06x\n",
                        turns[turn].width, ids[i], (unsigned) style.color,
                        (unsigned) turns[turn].colors[i]);
            CHECK(style.color == turns[turn].colors[i]);
        }
    }
    stylesheet_destroy(&sheet);
    document_destroy(&document);
    CHECK(budget.current == 0 && budget_uninstall_lexbor(&budget));
    return 0;
}

/* Utility CSS repeats one @container prelude per class. Identical preludes
   (name, condition and enclosing query) share one of the 63 query ids, so
   a block past the 63rd still applies; a different enclosing query or name
   keeps its own id. */
static int test_container_query_registration_dedup(void)
{
    Budget budget;
    budget_init(&budget, 8u * MIB);
    CHECK(budget_install_lexbor(&budget));
    PocDocument document = {0};
    Stylesheet sheet = {0};
    enum { BLOCKS = 80 };
    static char page[16384];
    size_t length = 0;
    int wrote = snprintf(page, sizeof(page),
        "<!doctype html><style>.box{container:card / inline-size}"
        "p{color:#000000}");
    CHECK(wrote > 0);
    length = (size_t) wrote;
    for (unsigned i = 0; i < BLOCKS; i++) {
        wrote = snprintf(page + length, sizeof(page) - length,
                         "@container (width > 100px){#t%u{color:#ff0000}}", i);
        CHECK(wrote > 0 && (size_t) wrote < sizeof(page) - length);
        length += (size_t) wrote;
    }
    wrote = snprintf(page + length, sizeof(page) - length,
        "@container (width > 100px){@container (width > 400px)"
        "{#nested{color:#ff0000}}}"
        "@container card (width > 100px){#named{color:#ff0000}}"
        "@container other (width > 100px){#other{color:#ff0000}}"
        "</style><div id=box class=box><p id=nested>N</p><p id=named>M</p>"
        "<p id=other>O</p>");
    CHECK(wrote > 0 && (size_t) wrote < sizeof(page) - length);
    length += (size_t) wrote;
    for (unsigned i = 0; i < BLOCKS; i++) {
        wrote = snprintf(page + length, sizeof(page) - length,
                         "<p id=t%u>T</p>", i);
        CHECK(wrote > 0 && (size_t) wrote < sizeof(page) - length);
        length += (size_t) wrote;
    }
    CHECK(document_parse(&document, &budget, page, length, 17)
          && stylesheet_build(&sheet, &budget, &document, 480));
    /* (width > 100px); nested (width > 400px); card; other. */
    CHECK(sheet.conditional_queries != NULL
          && sheet.conditional_queries->query_count == 4);
    lxb_dom_node_t *root = lxb_dom_interface_node(document.html);
    lxb_dom_node_t *box = find_id(root, "box");
    CHECK(box != NULL);
    for (int turn = 0; turn < 2; turn++) {
        int width = turn == 0 ? 300 : 50;
        CHECK(style_container_layout_state_begin(&sheet, &budget, 16)
              && style_container_layout_state_add(&sheet, box, width, 100,
                                                  0, 0));
        uint32_t want = turn == 0 ? 0xff0000u : 0x000000u;
        for (unsigned i = 0; i < BLOCKS; i++) {
            char id[8];
            snprintf(id, sizeof(id), "t%u", i);
            ComputedStyle style = style_for_node(&sheet, find_id(root, id),
                                                 NULL);
            if (style.color != want)
                fprintf(stderr, "container %d px #%s=%06x, want %06x\n",
                        width, id, (unsigned) style.color, (unsigned) want);
            CHECK(style.color == want);
        }
        CHECK(style_for_node(&sheet, find_id(root, "nested"), NULL).color
              == 0x000000u);
        CHECK(style_for_node(&sheet, find_id(root, "named"), NULL).color
              == want);
        /* No ancestor is named `other`. */
        CHECK(style_for_node(&sheet, find_id(root, "other"), NULL).color
              == 0x000000u);
    }
    stylesheet_destroy(&sheet);
    document_destroy(&document);
    CHECK(budget.current == 0 && budget_uninstall_lexbor(&budget));
    return 0;
}

/* Condition answers are kept per container state: siblings under one
   container share them, and a store that changes the container's size
   (a live update, or the next measured pass) answers afresh. */
static int test_container_condition_cache_follows_state(void)
{
    Budget budget;
    budget_init(&budget, 8u * MIB);
    CHECK(budget_install_lexbor(&budget));
    PocDocument document = {0};
    Stylesheet sheet = {0};
    static const char page[] =
        "<!doctype html><style>.box{container-type:inline-size}"
        "p{color:#000000}@container (width > 200px){p{color:#ff0000}}"
        "</style><div id=box class=box><p id=a>A</p><p id=b>B</p>"
        "<div id=inner class=box><p id=c>C</p></div></div>";
    CHECK(document_parse(&document, &budget, page, sizeof(page) - 1u, 17)
          && stylesheet_build(&sheet, &budget, &document, 480));
    lxb_dom_node_t *root = lxb_dom_interface_node(document.html);
    lxb_dom_node_t *box = find_id(root, "box");
    lxb_dom_node_t *inner = find_id(root, "inner");
    CHECK(box != NULL && inner != NULL);
    CHECK(style_container_layout_state_begin(&sheet, &budget, 16)
          && style_container_layout_state_add(&sheet, box, 300, 100, 0, 0)
          && style_container_layout_state_add(&sheet, inner, 100, 50, 0, 0));
    CHECK(style_for_node(&sheet, find_id(root, "a"), NULL).color == 0xff0000u);
    CHECK(style_for_node(&sheet, find_id(root, "b"), NULL).color == 0xff0000u);
    /* The nearest container answers, not the shared outer result. */
    CHECK(style_for_node(&sheet, find_id(root, "c"), NULL).color == 0x000000u);
    /* Same geometry: the stored answers stay. Changed: they start over. */
    CHECK(style_container_layout_state_add(&sheet, box, 300, 100, 0, 0)
          && style_container_layout_state_add(&sheet, inner, 250, 50, 0, 0));
    CHECK(style_for_node(&sheet, find_id(root, "c"), NULL).color == 0xff0000u);
    CHECK(style_container_layout_state_add(&sheet, box, 150, 100, 0, 0));
    CHECK(style_for_node(&sheet, find_id(root, "a"), NULL).color == 0x000000u);
    CHECK(style_for_node(&sheet, find_id(root, "b"), NULL).color == 0x000000u);
    CHECK(style_for_node(&sheet, find_id(root, "c"), NULL).color == 0xff0000u);
    stylesheet_destroy(&sheet);
    document_destroy(&document);
    CHECK(budget.current == 0 && budget_uninstall_lexbor(&budget));
    return 0;
}

/* A container prelude's `not`, `and` and `or` are condition keywords, not
   container names (CSS Containment 3): `not` negates, and the reserved
   words, `none` and functions cannot name a container. */
static int test_container_condition_keywords(void)
{
    Budget budget;
    budget_init(&budget, 8u * MIB);
    CHECK(budget_install_lexbor(&budget));
    PocDocument document = {0};
    Stylesheet sheet = {0};
    static const char page[] =
        "<!doctype html><style>.box{container:card / inline-size}"
        "p{color:#000000}"
        "@container not (width > 400px){#not{color:#ff0000}}"
        "@container NOT (width < 200px){#upper{color:#ff0000}}"
        "@container card not (width < 200px){#named{color:#ff0000}}"
        "@container (width > 100px) and (width < 400px){#and{color:#ff0000}}"
        "@container (width > 400px) or (width < 350px){#or{color:#ff0000}}"
        "@container not ((width > 100px) and (width < 200px)){#nested{color:#ff0000}}"
        "@container and (width > 1px){#and_name{color:#ff0000}}"
        "@container or (width > 1px){#or_name{color:#ff0000}}"
        "@container none (width > 1px){#none_name{color:#ff0000}}"
        "@container card foo (width > 1px){#junk{color:#ff0000}}"
        "@container not(width > 1px){#function{color:#ff0000}}"
        "</style><div id=box class=box>"
        "<p id=not>A</p><p id=upper>B</p><p id=named>C</p><p id=and>D</p>"
        "<p id=or>E</p><p id=nested>F</p><p id=and_name>G</p>"
        "<p id=or_name>H</p><p id=none_name>I</p><p id=junk>J</p>"
        "<p id=function>K</p></div>";
    CHECK(document_parse(&document, &budget, page, sizeof(page) - 1u, 17)
          && stylesheet_build(&sheet, &budget, &document, 480));
    lxb_dom_node_t *root = lxb_dom_interface_node(document.html);
    lxb_dom_node_t *box = find_id(root, "box");
    static const char *const ids[] = {
        "not", "upper", "named", "and", "or", "nested",
        "and_name", "or_name", "none_name", "junk", "function"};
    enum { ID_COUNT = sizeof(ids) / sizeof(ids[0]) };
    static const struct { int width; bool matched[ID_COUNT]; } turns[] = {
        {300, {true, true, true, true, true, true,
               false, false, false, false, false}},
        {150, {true, false, false, true, true, false,
               false, false, false, false, false}},
        {500, {false, true, true, false, true, true,
               false, false, false, false, false}},
    };
    for (size_t turn = 0; turn < sizeof(turns) / sizeof(turns[0]); turn++) {
        CHECK(style_container_layout_state_begin(&sheet, &budget, 16)
              && style_container_layout_state_add(
                  &sheet, box, turns[turn].width, 100, 0, 0));
        for (size_t i = 0; i < ID_COUNT; i++) {
            ComputedStyle style = style_for_node(
                &sheet, find_id(root, ids[i]), NULL);
            uint32_t want = turns[turn].matched[i] ? 0xff0000u : 0x000000u;
            if (style.color != want)
                fprintf(stderr, "container %d px #%s=%06x, want %06x\n",
                        turns[turn].width, ids[i], (unsigned) style.color,
                        (unsigned) want);
            CHECK(style.color == want);
        }
    }
    stylesheet_destroy(&sheet);
    document_destroy(&document);
    CHECK(budget.current == 0 && budget_uninstall_lexbor(&budget));
    return 0;
}

/* Size container features beyond width and height (CSS Containment 3):
   orientation and aspect-ratio, in plain, min-/max- and range forms, are
   retained and answered against the container's content box. Both relate
   the two axes, so an inline-size container never answers them; a style()
   query stays unsupported and is never retained. */
static int test_container_ratio_features(void)
{
    Budget budget;
    budget_init(&budget, 8u * MIB);
    CHECK(budget_install_lexbor(&budget));
    PocDocument document = {0};
    Stylesheet sheet = {0};
    static const char page[] =
        "<!doctype html><style>.box{container-type:size}"
        ".row{container-type:inline-size}p{color:#000000}"
        "@container (orientation: landscape){#land{color:#ff0000}}"
        "@container (orientation: portrait){#port{color:#ff0000}}"
        "@container (aspect-ratio > 1){#wide{color:#ff0000}}"
        "@container (min-aspect-ratio: 16/9){#cinema{color:#ff0000}}"
        "@container (max-aspect-ratio: 1 / 2){#tall{color:#ff0000}}"
        "@container (aspect-ratio: 2/1){#exact{color:#ff0000}}"
        "@container (1/2 <= aspect-ratio < 3/2){#band{color:#ff0000}}"
        "@container (MIN-ASPECT-RATIO: 16/9) and (width < 400px)"
        "{#mixed{color:#ff0000}}"
        "@container (orientation: landscape) or style(--x: 1)"
        "{#style{color:#ff0000}}"
        "@container (aspect-ratio > 1){#inline{color:#ff0000}}"
        "@container (orientation: landscape){#lone{color:#ff0000}}"
        "</style><div id=lone-row class=row><p id=lone>K</p></div>"
        "<div id=box class=box>"
        "<p id=land>A</p><p id=port>B</p><p id=wide>C</p>"
        "<p id=cinema>D</p><p id=tall>E</p><p id=exact>F</p>"
        "<p id=band>G</p><p id=mixed>H</p><p id=style>I</p>"
        "<div id=row class=row><p id=inline>J</p></div></div>";
    CHECK(document_parse(&document, &budget, page, sizeof(page) - 1u, 17)
          && stylesheet_build(&sheet, &budget, &document, 480));
    lxb_dom_node_t *root = lxb_dom_interface_node(document.html);
    lxb_dom_node_t *box = find_id(root, "box");
    lxb_dom_node_t *row = find_id(root, "row");
    lxb_dom_node_t *lone_row = find_id(root, "lone-row");
    static const char *const ids[] = {
        "land", "port", "wide", "cinema", "tall", "exact", "band", "mixed",
        "style", "inline", "lone"};
    enum { RATIO_ID_COUNT = sizeof(ids) / sizeof(ids[0]) };
    /* #inline sits in an inline-size container inside the size one: the
       ratio query skips it and answers against #box, which is why it
       follows #wide. #lone has only an inline-size container, which cannot
       answer orientation, so it stays false; #style is never retained.
       Turns three and four differ only in the container's height. */
    static const struct {
        int width, height;
        bool matched[RATIO_ID_COUNT];
    } turns[] = {
        {320, 180, {true, false, true, true, false, false, false, true,
                    false, true, false}},
        {200, 100, {true, false, true, true, false, true, false, true,
                    false, true, false}},
        {100, 100, {false, true, false, false, false, false, true, false,
                    false, false, false}},
        {100, 200, {false, true, false, false, true, false, true, false,
                    false, false, false}},
        {640, 360, {true, false, true, true, false, false, false, false,
                    false, true, false}},
    };
    for (size_t turn = 0; turn < sizeof(turns) / sizeof(turns[0]); turn++) {
        CHECK(style_container_layout_state_begin(&sheet, &budget, 16)
              && style_container_layout_state_add(
                  &sheet, box, turns[turn].width, turns[turn].height, 0, 0)
              && style_container_layout_state_add(
                  &sheet, row, turns[turn].width, 20, 0, 0)
              && style_container_layout_state_add(
                  &sheet, lone_row, turns[turn].width, 20, 0, 0));
        for (size_t i = 0; i < RATIO_ID_COUNT; i++) {
            ComputedStyle style = style_for_node(
                &sheet, find_id(root, ids[i]), NULL);
            uint32_t want = turns[turn].matched[i] ? 0xff0000u : 0x000000u;
            if (style.color != want)
                fprintf(stderr, "container %dx%d #%s=%06x, want %06x\n",
                        turns[turn].width, turns[turn].height, ids[i],
                        (unsigned) style.color, (unsigned) want);
            CHECK(style.color == want);
        }
    }
    /* Height alone flips orientation for an unchanged width: the measured
       geometry, and so the relayout signature, must carry it. */
    CHECK(style_container_layout_state_begin(&sheet, &budget, 16)
          && style_container_layout_state_add(&sheet, box, 300, 100, 0, 0));
    uint64_t landscape = style_container_layout_state_signature(&sheet);
    CHECK(style_for_node(&sheet, find_id(root, "land"), NULL).color
          == 0xff0000u);
    CHECK(style_container_layout_state_begin(&sheet, &budget, 16)
          && style_container_layout_state_add(&sheet, box, 300, 400, 0, 0));
    CHECK(style_container_layout_state_signature(&sheet) != landscape);
    CHECK(style_for_node(&sheet, find_id(root, "land"), NULL).color
          == 0x000000u
          && style_for_node(&sheet, find_id(root, "port"), NULL).color
             == 0xff0000u);
    stylesheet_destroy(&sheet);
    document_destroy(&document);
    CHECK(budget.current == 0 && budget_uninstall_lexbor(&budget));
    return 0;
}

/* Shorthand components split at the top level only: a space inside rgb(),
   calc() or color-mix() never ends a component, whether the value is
   written inline or substituted through var(). Each spelling must compute
   what its comma or plain-length sibling computes. */
static int test_shorthand_top_level_components(void)
{
    Budget budget;
    budget_init(&budget, 8u * MIB);
    CHECK(budget_install_lexbor(&budget));
    PocDocument document = {0};
    Stylesheet sheet = {0};
    static const char page[] =
        "<!doctype html><style>"
        "#o-rgb{outline:2px solid rgb(0 0 0 / 50%)}"
        "#o-rgb-var{--c:rgb(0 0 0 / 50%);outline:2px solid var(--c)}"
        "#o-rgb-ref{outline:2px solid rgba(0,0,0,0.5)}"
        "#o-calc{outline:calc(1px + 1px) solid red}"
        "#o-calc-var{--o:calc(1px + 1px) solid red;outline:var(--o)}"
        "#o-calc-ref{outline:2px solid red}"
        "#o-mix{outline:1px dashed color-mix(in srgb, red 50%, blue)}"
        "#o-mix-ref{outline:1px dashed red;"
        "outline-color:color-mix(in srgb, red 50%, blue)}"
        "#r-calc{border-radius:calc(4px + 2px)}"
        "#r-calc-ref{border-radius:6px}"
        "#r-var{--r:calc(4px + 2px) 3px;border-radius:var(--r)}"
        "#r-var-ref{border-radius:6px 3px}"
        "</style><p id=plain>x</p>"
        "<p id=o-rgb>x</p><p id=o-rgb-var>x</p><p id=o-rgb-ref>x</p>"
        "<p id=o-calc>x</p><p id=o-calc-var>x</p><p id=o-calc-ref>x</p>"
        "<p id=o-mix>x</p><p id=o-mix-ref>x</p>"
        "<p id=r-calc>x</p><p id=r-calc-ref>x</p>"
        "<p id=r-var>x</p><p id=r-var-ref>x</p>";
    CHECK(document_parse(&document, &budget, page, sizeof(page) - 1u, 17)
          && stylesheet_build(&sheet, &budget, &document, 480));
    lxb_dom_node_t *root = lxb_dom_interface_node(document.html);
    ComputedStyle plain = style_for_node(&sheet, find_id(root, "plain"), NULL);
    static const struct { const char *id, *reference; } outlines[] = {
        {"o-rgb", "o-rgb-ref"}, {"o-rgb-var", "o-rgb-ref"},
        {"o-calc", "o-calc-ref"}, {"o-calc-var", "o-calc-ref"},
        {"o-mix", "o-mix-ref"},
    };
    for (size_t i = 0; i < sizeof(outlines) / sizeof(outlines[0]); i++) {
        ComputedStyle a = style_for_node(
            &sheet, find_id(root, outlines[i].id), NULL);
        ComputedStyle b = style_for_node(
            &sheet, find_id(root, outlines[i].reference), NULL);
        if (a.outline_state != b.outline_state
            || a.outline_color != b.outline_color
            || a.outline_alpha != b.outline_alpha)
            fprintf(stderr, "#%s outline %04x %06x/%u, want %04x %06x/%u\n",
                    outlines[i].id, a.outline_state,
                    (unsigned) a.outline_color, (unsigned) a.outline_alpha,
                    b.outline_state, (unsigned) b.outline_color,
                    (unsigned) b.outline_alpha);
        CHECK(b.outline_state != plain.outline_state
              && a.outline_state == b.outline_state
              && a.outline_color == b.outline_color
              && a.outline_alpha == b.outline_alpha);
    }
    static const struct { const char *id, *reference; } radii[] = {
        {"r-calc", "r-calc-ref"}, {"r-var", "r-var-ref"},
    };
    for (size_t i = 0; i < sizeof(radii) / sizeof(radii[0]); i++) {
        ComputedStyle a = style_for_node(
            &sheet, find_id(root, radii[i].id), NULL);
        ComputedStyle b = style_for_node(
            &sheet, find_id(root, radii[i].reference), NULL);
        if (a.border_radius != b.border_radius)
            fprintf(stderr, "#%s border-radius %d, want %d\n",
                    radii[i].id, a.border_radius, b.border_radius);
        CHECK(b.border_radius != plain.border_radius
              && a.border_radius == b.border_radius);
    }
    stylesheet_destroy(&sheet);
    document_destroy(&document);
    CHECK(budget.current == 0 && budget_uninstall_lexbor(&budget));
    return 0;
}

/* The four-edge shorthands take one to four components (border-radius
   takes one to four radii, gap one or two lengths). One more makes the
   whole declaration invalid, so the earlier declaration in the same rule
   survives, exactly as if the invalid one were absent. */
static int test_box_shorthand_component_limits(void)
{
    Budget budget;
    budget_init(&budget, 8u * MIB);
    CHECK(budget_install_lexbor(&budget));
    PocDocument document = {0};
    Stylesheet sheet = {0};
    static const char page[] =
        "<!doctype html><style>"
        "p{margin:7px;padding:7px;inset:7px;border-width:7px;"
        "border-color:red;border-style:dashed;border-radius:7px;gap:7px}"
        "#margin{margin:1px 2px 3px 4px 5px}"
        "#margin-auto{margin:auto auto auto auto auto}"
        "#margin-var{--m:1px 2px 3px 4px 5px;margin:var(--m)}"
        "#padding{padding:1px 2px 3px 4px 5px}"
        "#inset{inset:1px 2px 3px 4px 5px}"
        "#border-width{border-width:1px 2px 3px 4px 5px}"
        "#border-color{border-color:blue blue blue blue blue}"
        "#border-style{border-style:solid solid solid solid solid}"
        "#border-radius{border-radius:1px 2px 3px 4px 5px}"
        "#gap{gap:1px 2px 3px}"
        "#margin-four{margin:1px 2px 3px 4px}"
        "</style><p id=plain>x</p><p id=margin>x</p><p id=margin-auto>x</p>"
        "<p id=margin-var>x</p><p id=padding>x</p><p id=inset>x</p>"
        "<p id=border-width>x</p><p id=border-color>x</p>"
        "<p id=border-style>x</p><p id=border-radius>x</p><p id=gap>x</p>"
        "<p id=margin-four>x</p>";
    CHECK(document_parse(&document, &budget, page, sizeof(page) - 1u, 17)
          && stylesheet_build(&sheet, &budget, &document, 480));
    lxb_dom_node_t *root = lxb_dom_interface_node(document.html);
    ComputedStyle plain = style_for_node(&sheet, find_id(root, "plain"), NULL);
    CHECK(plain.margin.top == 7 && plain.margin.left == 7
          && !plain.margin_left_auto && plain.padding.right == 7
          && plain.top == 7 && plain.left == 7 && plain.border.bottom == 7
          && plain.border_radius == 7 && plain.gap == 7);
    static const char *const invalid[] = {
        "margin", "margin-auto", "margin-var", "padding", "inset",
        "border-width", "border-color", "border-style", "border-radius",
        "gap",
    };
    for (size_t i = 0; i < sizeof(invalid) / sizeof(invalid[0]); i++) {
        ComputedStyle a = style_for_node(
            &sheet, find_id(root, invalid[i]), NULL);
        bool same_lines = true;
        for (unsigned side = 0; side < STYLE_BORDER_SIDE_COUNT; side++) {
            same_lines &= computed_style_border_line(
                &a, (StyleBorderSide) side) == computed_style_border_line(
                &plain, (StyleBorderSide) side);
        }
        bool same = memcmp(&a.margin, &plain.margin, sizeof(a.margin)) == 0
            && a.margin_top_auto == plain.margin_top_auto
            && a.margin_left_auto == plain.margin_left_auto
            && memcmp(&a.padding, &plain.padding, sizeof(a.padding)) == 0
            && a.top == plain.top && a.right == plain.right
            && a.bottom == plain.bottom && a.left == plain.left
            && memcmp(&a.border, &plain.border, sizeof(a.border)) == 0
            && a.border_color == plain.border_color
            && a.border_color_set == plain.border_color_set
            && same_lines && a.border_radius == plain.border_radius
            && a.gap == plain.gap && a.row_gap == plain.row_gap;
        if (!same)
            fprintf(stderr, "#%s: margin %d %d %d %d (auto %d), border %d "
                    "%d %d %d, radius %d, gap %d/%d: declaration with too "
                    "many components was not dropped\n", invalid[i],
                    a.margin.top, a.margin.right, a.margin.bottom,
                    a.margin.left, a.margin_left_auto, a.border.top,
                    a.border.right, a.border.bottom, a.border.left,
                    a.border_radius, a.row_gap, a.gap);
        CHECK(same);
    }
    ComputedStyle four = style_for_node(
        &sheet, find_id(root, "margin-four"), NULL);
    CHECK(four.margin.top == 1 && four.margin.right == 2
          && four.margin.bottom == 3 && four.margin.left == 4);
    stylesheet_destroy(&sheet);
    document_destroy(&document);
    CHECK(budget.current == 0 && budget_uninstall_lexbor(&budget));
    return 0;
}

/* transition-timing-function and the transition shorthand take only a
   valid <easing-function>: cubic-bezier() with four numbers and both x
   values in [0, 1], steps() with a positive integer (two or more for
   jump-none) and an optional step position, and linear() with two or more
   stops. An invalid one drops the declaration, so the earlier ease-out in
   the same rule stays; a valid one serializes canonically. */
static int test_transition_timing_function_grammar(void)
{
    static const struct { const char *value, *computed; } cases[] = {
        {"EASE-IN", "ease-in"},
        {"cubic-bezier(.2,.8,.2,1)", "cubic-bezier(0.2, 0.8, 0.2, 1)"},
        {"cubic-bezier(0, -2, 1, 3)", "cubic-bezier(0, -2, 1, 3)"},
        {"cubic-bezier(calc(0.5), 0, 1, 1)", "cubic-bezier(0.5, 0, 1, 1)"},
        /* Math arguments take the range their literal forms take. */
        {"steps(calc(12), end)", "steps(12, end)"},
        {"steps(calc(6 * 2.2))", "steps(13)"},
        {"cubic-bezier(0.3, calc(-12), 0.7, 1)",
         "cubic-bezier(0.3, -12, 0.7, 1)"},
        {"cubic-bezier(0.3, -12, 0.7, max(1, 25))",
         "cubic-bezier(0.3, -12, 0.7, 25)"},
        {"linear(0, calc(20 * 0.6) 50%, 1)", "linear(0, 12 50%, 1)"},
        {"steps(calc(0))", NULL},
        {"cubic-bezier(calc(1.5), 0, 0, 1)", NULL},
        {"steps(4)", "steps(4)"},
        {"steps(4, END)", "steps(4, end)"},
        {"steps(+3,jump-both)", "steps(3, jump-both)"},
        {"steps(2, jump-none)", "steps(2, jump-none)"},
        {"linear(0, .25 50%, 1)", "linear(0, 0.25 50%, 1)"},
        {"linear(0, 25% 75% .5, 1)", "linear(0, 25% 75% 0.5, 1)"},
        {"steps(foo)", NULL},
        {"steps(\")\" 1s)", NULL},
        {"cubic-bezier(a,b,c,d)", NULL},
        {"cubic-bezier(1.5, 0, 0, 1)", NULL},
        {"cubic-bezier(0, 0, -0.1, 1)", NULL},
        {"cubic-bezier(0, 0, 1)", NULL},
        {"cubic-bezier(0, 0, 1, 1, 1)", NULL},
        {"cubic-bezier(0, 0, 1, 0x1)", NULL},
        {"cubic-bezier(0, 0, 1, 1.)", NULL},
        {"steps()", NULL},
        {"steps(0)", NULL},
        {"steps(-1)", NULL},
        {"steps(1.5)", NULL},
        {"steps(1, jump-none)", NULL},
        {"steps(2, middle)", NULL},
        {"steps(2, end, end)", NULL},
        {"steps(2,)", NULL},
        {"steps(1)x)", NULL},
        {"linear()", NULL},
        {"linear(1)", NULL},
        {"linear(0, x, 1)", NULL},
        {"linear(0, 50% 1 25%, 1)", NULL},
        {"linear(0, 1 2, 1)", NULL},
        {"bounce", NULL},
    };
    enum { CASES = sizeof(cases) / sizeof(cases[0]) };
    Budget budget;
    budget_init(&budget, 8u * MIB);
    CHECK(budget_install_lexbor(&budget));
    char page[16384];
    size_t used = (size_t) snprintf(page, sizeof(page),
        "<!doctype html><style>"
        "#var{--e:steps(foo);transition-timing-function:var(--e)}");
    for (size_t i = 0; i < CASES; i++) {
        int written = snprintf(page + used, sizeof(page) - used,
            "#l%zu{transition-timing-function:ease-out;"
            "transition-timing-function:%s}"
            "#s%zu{transition:opacity 1s ease-out;"
            "transition:opacity 1s %s}", i, cases[i].value, i,
            cases[i].value);
        CHECK(written > 0 && (size_t) written < sizeof(page) - used);
        used += (size_t) written;
    }
    used += (size_t) snprintf(page + used, sizeof(page) - used,
                              "</style><p id=var>x</p>");
    for (size_t i = 0; i < CASES; i++) {
        int written = snprintf(page + used, sizeof(page) - used,
                               "<p id=l%zu>x</p><p id=s%zu>x</p>", i, i);
        CHECK(written > 0 && (size_t) written < sizeof(page) - used);
        used += (size_t) written;
    }
    PocDocument document = {0};
    Stylesheet sheet = {0};
    CHECK(document_parse(&document, &budget, page, used, 17)
          && stylesheet_build(&sheet, &budget, &document, 480));
    lxb_dom_node_t *root = lxb_dom_interface_node(document.html);
    StyleTransitionComputed computed;
    CHECK(style_transition_computed(&sheet, find_id(root, "var"),
                                    PSEUDO_NONE, &computed)
          && strcmp(style_transition_timing(&computed, 0), "ease") == 0);
    for (size_t i = 0; i < CASES; i++) {
        const char *expected = cases[i].computed != NULL
            ? cases[i].computed : "ease-out";
        for (unsigned shorthand = 0; shorthand < 2u; shorthand++) {
            char id[16];
            snprintf(id, sizeof(id), "%c%zu", shorthand ? 's' : 'l', i);
            CHECK(style_transition_computed(&sheet, find_id(root, id),
                                            PSEUDO_NONE, &computed));
            const char *timing = style_transition_timing(&computed, 0);
            if (strcmp(timing, expected) != 0)
                fprintf(stderr, "%s: %s computed \"%s\", want \"%s\"\n",
                        shorthand ? "transition" : "transition-timing-function",
                        cases[i].value, timing, expected);
            CHECK(strcmp(timing, expected) == 0);
        }
        char direct[STYLE_TIMING_FUNCTION_CAPACITY];
        CHECK(style_timing_function(cases[i].value, strlen(cases[i].value),
                                    direct, sizeof(direct))
              == (cases[i].computed != NULL));
    }
    stylesheet_destroy(&sheet);
    document_destroy(&document);
    CHECK(budget.current == 0 && budget_uninstall_lexbor(&budget));
    return 0;
}

/* Writes linear(0, ..., 1) with `stops` stops in all into `output`,
   serialized as the computed value is or, when `compact`, in the shortest
   authored form (",.0169 1.69%"); its length. */
static size_t long_linear_easing(char *output, size_t size, unsigned stops,
                                 double step, bool compact)
{
    size_t used = (size_t) snprintf(output, size, "linear(0");
    for (unsigned i = 1; i + 1u < stops && used < size; i++) {
        char number[32];
        snprintf(number, sizeof(number), "%g", i * step);
        used += (size_t) snprintf(output + used, size - used, "%s%s %g%%",
                                  compact ? "," : ", ",
                                  compact && number[0] == '0'
                                      ? number + 1 : number,
                                  i * step * 100.0);
    }
    if (used < size)
        used += (size_t) snprintf(output + used, size - used, ", 1)");
    return used;
}

/* A linear() of many stops is a valid timing item: up to
   STYLE_TIMING_FUNCTION_CAPACITY bytes serialized in one item, and up to
   STYLE_TIMING_LIST_TEXT_CAPACITY across a list, in the longhand, the
   shorthand and the style attribute. A longer one, or a list past the
   shared text (three such items), is rejected like an invalid value, so
   the earlier declaration stays. */
static int test_transition_long_linear_easing(void)
{
    static char easing[1024], over[2048], third[1024], compact[1024];
    static char page[16384];
    size_t easing_length = long_linear_easing(easing, sizeof(easing), 60,
                                              0.0169, false);
    size_t over_length = long_linear_easing(over, sizeof(over), 100,
                                            0.0101, false);
    /* Three of these overflow the list's shared text, while their
       compact authored form still fits the retained value limit. */
    size_t third_length = long_linear_easing(third, sizeof(third), 50,
                                             0.0199, false);
    size_t compact_length = long_linear_easing(compact, sizeof(compact),
                                               50, 0.0199, true);
    CHECK(easing_length > 512u
          && easing_length < STYLE_TIMING_FUNCTION_CAPACITY
          && over_length >= STYLE_TIMING_FUNCTION_CAPACITY
          && over_length < sizeof(over) - 1u
          && 3u * (third_length + 1u) > STYLE_TIMING_LIST_TEXT_CAPACITY
          && 3u * compact_length + 40u < STYLE_TRANSITION_VALUE_CAPACITY);
    char direct[STYLE_TIMING_FUNCTION_CAPACITY];
    CHECK(style_timing_function(easing, easing_length, direct,
                                sizeof(direct))
          && strcmp(direct, easing) == 0
          && !style_timing_function(over, over_length, direct,
                                    sizeof(direct)));
    int written = snprintf(page, sizeof(page),
        "<!doctype html><style>"
        "#l{transition-timing-function:ease-out;"
        "transition-timing-function:%s}"
        "#s{transition:color 3s ease-in;transition:opacity 1s %s}"
        "#two{transition:opacity 1s %s,top 2s %s}"
        "#three{transition:color 3s ease-in;"
        "transition:opacity 1s %s,top 2s %s,left 3s %s}"
        "#over{transition-timing-function:ease-out;"
        "transition-timing-function:%s}"
        "</style><p id=l>x</p><p id=s>x</p><p id=two>x</p>"
        "<p id=three>x</p><p id=over>x</p>"
        "<p id=inline style='transition:opacity 1s %s'>x</p>",
        easing, easing, easing, easing, compact, compact, compact, over,
        easing);
    CHECK(written > 0 && (size_t) written < sizeof(page));
    Budget budget;
    budget_init(&budget, 8u * MIB);
    CHECK(budget_install_lexbor(&budget));
    PocDocument document = {0};
    Stylesheet sheet = {0};
    CHECK(document_parse(&document, &budget, page, (size_t) written, 17)
          && stylesheet_build(&sheet, &budget, &document, 480));
    lxb_dom_node_t *root = lxb_dom_interface_node(document.html);
    static const struct {
        const char *id;
        unsigned count;
        const char *timing; /* NULL: the long easing */
        double duration_ms;
    } cases[] = {
        {"l", 1, NULL, 0.0}, {"s", 1, NULL, 1000.0},
        {"two", 2, NULL, 1000.0}, {"three", 1, "ease-in", 3000.0},
        {"over", 1, "ease-out", 0.0}, {"inline", 1, NULL, 1000.0},
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        StyleTransitionComputed computed;
        CHECK(style_transition_computed(&sheet, find_id(root, cases[i].id),
                                        PSEUDO_NONE, &computed));
        bool ok = computed.timing_count == cases[i].count
            && computed.duration_ms[0] == cases[i].duration_ms;
        for (unsigned item = 0; ok && item < cases[i].count; item++)
            ok = strcmp(style_transition_timing(&computed, item),
                        cases[i].timing != NULL ? cases[i].timing : easing)
                == 0;
        if (!ok)
            fprintf(stderr, "#%s: %u timings, %g ms, first \"%.40s\"\n",
                    cases[i].id, computed.timing_count,
                    computed.duration_ms[0],
                    style_transition_timing(&computed, 0));
        CHECK(ok);
    }
    stylesheet_destroy(&sheet);
    document_destroy(&document);
    CHECK(budget.current == 0 && budget_uninstall_lexbor(&budget));
    return 0;
}

/* style_transition_computed_untimed() gives the property, duration and
   delay lists style_transition_computed() gives, whichever rules win which
   longhands, without the easings: validated sheet easings are only
   recognized, while style-attribute and var() values still validate theirs
   (an invalid one drops the whole declaration in both). */
static int test_transition_untimed_matches_full(void)
{
    static char easing[1024];
    static char page[8192];
    (void) long_linear_easing(easing, sizeof(easing), 60, 0.0169, false);
    int written = snprintf(page, sizeof(page),
        "<!doctype html><style>"
        "#long{transition:transform .4s %s}"
        "#split{transition:opacity 1s %s .2s;transition-duration:3s}"
        "#late{transition-property:top;transition:color 2s ease-in 1s}"
        "#word{transition:linear 1s}"
        "#list{transition:opacity 1s linear(0, .5, 1),"
        "transform 2s steps(2, jump-none) .5s, top 3s allow-discrete}"
        "#timing{transition-timing-function:%s;transition-delay:4s}"
        "#varbad{--e:steps(foo);transition:opacity 2s var(--e)}"
        "#vargood{--e:ease-in;transition:top 3s var(--e) 1s}"
        "#wide{transition:inherit}"
        "</style>"
        "<p id=long>x</p><p id=split>x</p><p id=late>x</p><p id=word>x</p>"
        "<p id=list>x</p><p id=timing>x</p><p id=varbad>x</p>"
        "<p id=vargood>x</p><p id=wide>x</p>"
        "<p id=attrbad style='transition:opacity 5s steps(foo)'>x</p>"
        "<p id=attrgood style='transition:opacity 5s cubic-bezier(0,0,1,9)'>"
        "x</p><p id=none>x</p>",
        easing, easing, easing);
    CHECK(written > 0 && (size_t) written < sizeof(page));
    Budget budget;
    budget_init(&budget, 8u * MIB);
    CHECK(budget_install_lexbor(&budget));
    PocDocument document = {0};
    Stylesheet sheet = {0};
    CHECK(document_parse(&document, &budget, page, (size_t) written, 17)
          && stylesheet_build(&sheet, &budget, &document, 480));
    lxb_dom_node_t *root = lxb_dom_interface_node(document.html);
    static const struct { const char *id; double duration_ms; } cases[] = {
        {"long", 400.0}, {"split", 3000.0}, {"late", 2000.0},
        {"word", 1000.0}, {"list", 1000.0}, {"timing", 0.0},
        {"varbad", 0.0}, {"vargood", 3000.0}, {"wide", 0.0},
        {"attrbad", 0.0}, {"attrgood", 5000.0}, {"none", 0.0},
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        lxb_dom_node_t *node = find_id(root, cases[i].id);
        StyleTransitionComputed full, untimed;
        CHECK(style_transition_computed(&sheet, node, PSEUDO_NONE, &full)
              && style_transition_computed_untimed(&sheet, node, PSEUDO_NONE,
                                                   &untimed));
        bool ok = full.property_count == untimed.property_count
            && full.duration_count == untimed.duration_count
            && full.delay_count == untimed.delay_count
            && full.duration_ms[0] == cases[i].duration_ms
            && untimed.timing_count == 1
            && strcmp(style_transition_timing(&untimed, 0), "ease") == 0;
        for (unsigned item = 0; ok && item < full.property_count; item++)
            ok = strcmp(full.properties[item], untimed.properties[item]) == 0;
        for (unsigned item = 0; ok && item < full.duration_count; item++)
            ok = full.duration_ms[item] == untimed.duration_ms[item];
        for (unsigned item = 0; ok && item < full.delay_count; item++)
            ok = full.delay_ms[item] == untimed.delay_ms[item];
        if (!ok)
            fprintf(stderr, "#%s: full %u/%s/%g ms, untimed %u/%s/%g ms\n",
                    cases[i].id, full.property_count, full.properties[0],
                    full.duration_ms[0], untimed.property_count,
                    untimed.properties[0], untimed.duration_ms[0]);
        CHECK(ok);
    }
    /* The full form still serializes the winners' easings. */
    StyleTransitionComputed list;
    CHECK(style_transition_computed(&sheet, find_id(root, "list"),
                                    PSEUDO_NONE, &list)
          && list.timing_count == 3 && list.property_count == 3
          && strcmp(list.properties[2], "top") == 0
          && strcmp(style_transition_timing(&list, 0), "linear(0, 0.5, 1)")
              == 0
          && strcmp(style_transition_timing(&list, 1),
                    "steps(2, jump-none)") == 0
          && strcmp(style_transition_timing(&list, 2), "ease") == 0
          && list.delay_ms[1] == 500.0);
    StyleTransitionComputed split;
    CHECK(style_transition_computed(&sheet, find_id(root, "split"),
                                    PSEUDO_NONE, &split)
          && strcmp(style_transition_timing(&split, 0), easing) == 0
          && split.delay_ms[0] == 200.0 && split.duration_ms[0] == 3000.0);
    StyleTransitionComputed word;
    CHECK(style_transition_computed(&sheet, find_id(root, "word"),
                                    PSEUDO_NONE, &word)
          && strcmp(word.properties[0], "all") == 0
          && strcmp(style_transition_timing(&word, 0), "linear") == 0);
    stylesheet_destroy(&sheet);
    document_destroy(&document);
    CHECK(budget.current == 0 && budget_uninstall_lexbor(&budget));
    return 0;
}

/* Two maximal linear() items (each STYLE_TIMING_FUNCTION_CAPACITY bytes
   serialized with its NUL) fit one timing list beside the initial "ease"
   it always keeps. */
static int test_transition_two_maximal_linear_easings(void)
{
    /* Serialized: "linear(0" + 109 x ", 0.5 50%" + 3 x ", 0.25 50%"
       + ", 1)" = 1023 bytes. Authored compactly to stay well inside the
       retained value limit. */
    static char authored[1024], expected[1024], page[4096];
    size_t authored_used = (size_t) snprintf(authored, sizeof(authored),
                                             "linear(0");
    size_t expected_used = (size_t) snprintf(expected, sizeof(expected),
                                             "linear(0");
    for (unsigned i = 0; i < 112u; i++) {
        bool quarter = i < 3u;
        authored_used += (size_t) snprintf(
            authored + authored_used, sizeof(authored) - authored_used,
            quarter ? ",.25 50%%" : ",.5 50%%");
        expected_used += (size_t) snprintf(
            expected + expected_used, sizeof(expected) - expected_used,
            quarter ? ", 0.25 50%%" : ", 0.5 50%%");
    }
    authored_used += (size_t) snprintf(authored + authored_used,
                                       sizeof(authored) - authored_used,
                                       ",1)");
    expected_used += (size_t) snprintf(expected + expected_used,
                                       sizeof(expected) - expected_used,
                                       ", 1)");
    CHECK(expected_used + 1u == STYLE_TIMING_FUNCTION_CAPACITY);
    char direct[STYLE_TIMING_FUNCTION_CAPACITY];
    CHECK(style_timing_function(authored, authored_used, direct,
                                sizeof(direct))
          && strcmp(direct, expected) == 0);
    int written = snprintf(page, sizeof(page),
        "<!doctype html><style>"
        "#two{transition-timing-function:ease-out;"
        "transition-timing-function:%s,%s}"
        "</style><p id=two>x</p>", authored, authored);
    CHECK(written > 0 && (size_t) written < sizeof(page));
    Budget budget;
    budget_init(&budget, 8u * MIB);
    CHECK(budget_install_lexbor(&budget));
    PocDocument document = {0};
    Stylesheet sheet = {0};
    CHECK(document_parse(&document, &budget, page, (size_t) written, 17)
          && stylesheet_build(&sheet, &budget, &document, 480));
    lxb_dom_node_t *root = lxb_dom_interface_node(document.html);
    StyleTransitionComputed computed;
    CHECK(style_transition_computed(&sheet, find_id(root, "two"),
                                    PSEUDO_NONE, &computed));
    bool ok = computed.timing_count == 2
        && strcmp(style_transition_timing(&computed, 0), expected) == 0
        && strcmp(style_transition_timing(&computed, 1), expected) == 0;
    if (!ok)
        fprintf(stderr, "two maximal easings: %u timings, first \"%.20s\"\n",
                computed.timing_count, style_transition_timing(&computed, 0));
    CHECK(ok);
    stylesheet_destroy(&sheet);
    document_destroy(&document);
    CHECK(budget.current == 0 && budget_uninstall_lexbor(&budget));
    return 0;
}

/* minmax(<inflexible-breadth>, <track-breadth>) rejects a floor or a
   ceiling outside its grammar instead of reading a garbage floor as 0:
   the track is invalid, and so is the declaration holding it. */
static int test_grid_minmax_arguments(void)
{
    Budget budget;
    budget_init(&budget, 8u * MIB);
    CHECK(budget_install_lexbor(&budget));
    PocDocument document = {0};
    Stylesheet sheet = {0};
    static const char page[] =
        "<!doctype html><style>"
        "#bad{display:grid;grid-template-columns:100px 100px;"
        "grid-template-columns:minmax(foo, 1fr) 1fr}"
        "#good{display:grid;grid-template-columns:100px 100px;"
        "grid-template-columns:minmax(40px, 1fr) 1fr}"
        "#mixed{display:grid;grid-template-columns:100px 100px;"
        "grid-template-columns:repeat(2, minmax(calc(50% - 1rem), 1fr))}"
        "#pair{display:grid;grid-template-columns:100px 100px;"
        "grid-template-columns:minmax(min(100%, 20rem), 1fr) 1fr}"
        "#fill{display:grid;grid-template-columns:100px 100px;"
        "grid-template-columns:"
        "repeat(auto-fill, minmax(min(100%, 20rem), 1fr))}"
        "#upper{display:grid;grid-template-columns:100px 100px;"
        "grid-template-columns:minmax(AUTO, 1fr) 1fr}"
        "</style><div id=bad>x</div><div id=good>x</div>"
        "<div id=mixed>x</div><div id=pair>x</div><div id=fill>x</div>"
        "<div id=upper>x</div>";
    CHECK(document_parse(&document, &budget, page, sizeof(page) - 1u, 17)
          && stylesheet_build(&sheet, &budget, &document, 480));
    static const struct {
        const char *text;
        bool valid;
        uint8_t type;
        unsigned value, minimum;
    } tracks[] = {
        {"minmax(100px, 1fr)", true, GRID_TRACK_FLEX, 1000, 100},
        {"minmax( auto ,1fr )", true, GRID_TRACK_FLEX, 1000, 0},
        {"minmax(0, 1fr)", true, GRID_TRACK_FLEX, 1000, 0},
        {"minmax(min-content, 200px)", true, GRID_TRACK_FIXED, 200, 0},
        {"minmax(10%, 2fr)", true, GRID_TRACK_FLEX, 2000, 0},
        {"minmax(max(10px, 5px), 1fr)", true, GRID_TRACK_FLEX, 1000, 10},
        /* A floor mixing a percentage with a length is a valid
           <length-percentage>; like a percentage it leaves the pixel
           minimum 0. */
        {"minmax(calc(50% - 1rem), 1fr)", true, GRID_TRACK_FLEX, 1000, 0},
        {"minmax(min(100%, 20rem), 1fr)", true, GRID_TRACK_FLEX, 1000, 0},
        {"minmax(max(100px, 20%), 1fr)", true, GRID_TRACK_FLEX, 1000, 0},
        {"minmax(clamp(10px, 5%, 2rem), 3fr)", true, GRID_TRACK_FLEX, 3000,
         0},
        {"minmax(AUTO, 1fr)", true, GRID_TRACK_FLEX, 1000, 0},
        {"minmax(Min-Content, 200px)", true, GRID_TRACK_FIXED, 200, 0},
        {"minmax(MAX-CONTENT, 2fr)", true, GRID_TRACK_FLEX, 2000, 0},
        {"minmax(calc(50% - foo), 1fr)", false, 0, 0, 0},
        {"minmax(min(100%, 2fr), 1fr)", false, 0, 0, 0},
        {"minmax(foo, 1fr)", false, 0, 0, 0},
        {"minmax(1fr, 100px)", false, 0, 0, 0},
        {"minmax(-5px, 1fr)", false, 0, 0, 0},
        {"minmax(, 1fr)", false, 0, 0, 0},
        {"minmax(10px)", false, 0, 0, 0},
        {"minmax(1px, 2px, 3px)", false, 0, 0, 0},
        {"minmax(10px, bar)", false, 0, 0, 0},
        {"minmax(10px, minmax(1px, 2px))", false, 0, 0, 0},
        {"minmax(10px, fit-content(5px))", false, 0, 0, 0},
    };
    for (size_t i = 0; i < sizeof(tracks) / sizeof(tracks[0]); i++) {
        uint8_t type = 0;
        unsigned value = 0, minimum = 0;
        bool valid = style_parse_grid_track(
            &sheet, tracks[i].text, strlen(tracks[i].text), &type, &value,
            &minimum);
        bool ok = valid == tracks[i].valid
            && (!valid || (type == tracks[i].type
                           && value == tracks[i].value
                           && minimum == tracks[i].minimum));
        if (!ok)
            fprintf(stderr, "%s: %s type %u value %u minimum %u\n",
                    tracks[i].text, valid ? "accepted" : "rejected",
                    type, value, minimum);
        CHECK(ok);
    }
    lxb_dom_node_t *root = lxb_dom_interface_node(document.html);
    ComputedStyle bad = style_for_node(&sheet, find_id(root, "bad"), NULL);
    ComputedStyle good = style_for_node(&sheet, find_id(root, "good"), NULL);
    CHECK(stylesheet_grid_track_type(&sheet, &bad, false, 0)
              == GRID_TRACK_FIXED
          && stylesheet_grid_track_value(&sheet, &bad, false, 0) == 100
          && stylesheet_grid_track_type(&sheet, &good, false, 0)
              == GRID_TRACK_FLEX
          && stylesheet_grid_track_minimum(&sheet, &good, false, 0) == 40);
    /* Mixed floors keep the declaration: the 100px template is replaced. */
    static const char *const replaced[] = {"mixed", "pair", "upper"};
    for (size_t i = 0; i < sizeof(replaced) / sizeof(replaced[0]); i++) {
        ComputedStyle style =
            style_for_node(&sheet, find_id(root, replaced[i]), NULL);
        bool ok = stylesheet_grid_track_count(&sheet, &style, false) == 2
            && stylesheet_grid_track_type(&sheet, &style, false, 0)
                == GRID_TRACK_FLEX
            && stylesheet_grid_track_value(&sheet, &style, false, 0) == 1000
            && stylesheet_grid_track_minimum(&sheet, &style, false, 0) == 0;
        if (!ok) fprintf(stderr, "#%s lost its grid template\n", replaced[i]);
        CHECK(ok);
    }
    ComputedStyle upper = style_for_node(&sheet, find_id(root, "upper"), NULL);
    CHECK(stylesheet_grid_track_automatic_minimum(&sheet, &upper, false, 0));
    /* repeat(auto-fill, ...) leaves no explicit tracks: the 100px pair is
       gone, and the auto-fill column count (0) is what survives. */
    ComputedStyle fill = style_for_node(&sheet, find_id(root, "fill"), NULL);
    CHECK(stylesheet_grid_track_count(&sheet, &fill, false) == 0
          && computed_style_grid_column_count(&fill) == 0
          && computed_style_grid_column_template_id(&fill) == 0);
    stylesheet_destroy(&sheet);
    document_destroy(&document);
    CHECK(budget.current == 0 && budget_uninstall_lexbor(&budget));
    return 0;
}

/* grid-auto-rows/columns take a <track-size>+ list. Two tracks fit the
   computed style; a third, or any invalid track, drops the declaration
   instead of being ignored, and a list starting with whitespace (an empty
   var() before it) still parses. A second track is always stored non-zero
   (an auto one included), and one the packed field cannot hold drops the
   declaration rather than being clamped. */
static int test_grid_auto_track_lists(void)
{
    Budget budget;
    budget_init(&budget, 8u * MIB);
    CHECK(budget_install_lexbor(&budget));
    PocDocument document = {0};
    Stylesheet sheet = {0};
    static const char page[] =
        "<!doctype html><style>"
        "#three{grid-auto-rows:7px;grid-auto-rows:10px 20px 30px;"
        "grid-auto-columns:7px;grid-auto-columns:10px 20px 30px}"
        "#bad{grid-auto-rows:7px;grid-auto-rows:10px foo;"
        "grid-auto-columns:7px;grid-auto-columns:10px foo}"
        "#two{grid-auto-rows:10px 20px;grid-auto-columns:1fr 30px}"
        "#lead{--empty: ;grid-auto-rows:var(--empty) 10px 20px;"
        "grid-auto-columns:var(--empty) 15px}"
        "#auto{grid-auto-rows:100px auto;grid-auto-columns:1fr min-content}"
        "#zero{grid-auto-rows:10px 0px;grid-auto-columns:10px 0fr}"
        "#fit{grid-auto-rows:7px;grid-auto-rows:auto fit-content(200px);"
        "grid-auto-columns:7px;grid-auto-columns:10px fit-content(5px)}"
        "#wide{grid-auto-rows:7px;grid-auto-rows:10px 17fr;"
        "grid-auto-columns:7px;grid-auto-columns:10px 164%}"
        "#edge{grid-auto-rows:10px 16383px;grid-auto-columns:10px 163.83%}"
        "#over{grid-auto-rows:7px;grid-auto-rows:10px 16384px;"
        "grid-auto-columns:7px;grid-auto-columns:10px 16.384fr}"
        "#alt{display:grid;grid-auto-flow:column;grid-auto-columns:100px auto;"
        "justify-content:start}"
        "#alt>i{display:block;width:20px;height:5px}"
        "</style><div id=three>x</div><div id=bad>x</div>"
        "<div id=two>x</div><div id=lead>x</div><div id=auto>x</div>"
        "<div id=zero>x</div><div id=fit>x</div><div id=wide>x</div>"
        "<div id=edge>x</div><div id=over>x</div>"
        "<div id=alt><i id=c0></i><i id=c1></i><i id=c2></i></div>";
    CHECK(document_parse(&document, &budget, page, sizeof(page) - 1u, 17)
          && stylesheet_build(&sheet, &budget, &document, 480));
    lxb_dom_node_t *root = lxb_dom_interface_node(document.html);
    static const struct {
        const char *id;
        uint16_t row_type, row_value, row_second;
        uint16_t column_type, column_value, column_second;
    } expected[] = {
        {"three", GRID_TRACK_FIXED, 7, 0, GRID_TRACK_FIXED, 7, 0},
        {"bad", GRID_TRACK_FIXED, 7, 0, GRID_TRACK_FIXED, 7, 0},
        {"two", GRID_TRACK_FIXED, 10, (GRID_TRACK_FIXED << 14) | 20,
         GRID_TRACK_FLEX, 1000, (GRID_TRACK_FIXED << 14) | 30},
        {"lead", GRID_TRACK_FIXED, 10, (GRID_TRACK_FIXED << 14) | 20,
         GRID_TRACK_FIXED, 15, 0},
        /* An auto second track is present (non-zero) and decodes back to
           auto; so does a zero-sized fixed or flexible one. */
        {"auto", GRID_TRACK_FIXED, 100, GRID_TRACK_AUTO_VALUE + 1u,
         GRID_TRACK_FLEX, 1000, GRID_TRACK_MIN_CONTENT_VALUE + 1u},
        {"zero", GRID_TRACK_FIXED, 10, GRID_TRACK_FIXED << 14,
         GRID_TRACK_FIXED, 10, GRID_TRACK_FLEX << 14},
        /* A second track the 14-bit packing cannot hold drops the list. */
        {"fit", GRID_TRACK_FIXED, 7, 0, GRID_TRACK_FIXED, 7, 0},
        {"wide", GRID_TRACK_FIXED, 7, 0, GRID_TRACK_FIXED, 7, 0},
        {"edge", GRID_TRACK_FIXED, 10, (GRID_TRACK_FIXED << 14) | 16383,
         GRID_TRACK_FIXED, 10, (GRID_TRACK_PERCENT << 14) | 16383},
        {"over", GRID_TRACK_FIXED, 7, 0, GRID_TRACK_FIXED, 7, 0},
    };
    for (size_t i = 0; i < sizeof(expected) / sizeof(expected[0]); i++) {
        ComputedStyle style = style_for_node(
            &sheet, find_id(root, expected[i].id), NULL);
        bool ok = style.grid_auto_row_type == expected[i].row_type
            && style.grid_auto_row_value == expected[i].row_value
            && style.grid_auto_row_second == expected[i].row_second
            && style.grid_auto_column_type == expected[i].column_type
            && style.grid_auto_column_value == expected[i].column_value
            && style.grid_auto_column_second == expected[i].column_second;
        if (!ok)
            fprintf(stderr, "#%s rows %u/%u/%04x columns %u/%u/%04x\n",
                    expected[i].id, style.grid_auto_row_type,
                    style.grid_auto_row_value, style.grid_auto_row_second,
                    style.grid_auto_column_type, style.grid_auto_column_value,
                    style.grid_auto_column_second);
        CHECK(ok);
    }
    ComputedStyle autos = style_for_node(&sheet, find_id(root, "auto"), NULL);
    CHECK(grid_auto_second_type(autos.grid_auto_row_second) == GRID_TRACK_AUTO
          && grid_auto_second_value(autos.grid_auto_row_second)
              == GRID_TRACK_AUTO_VALUE
          && grid_auto_second_type(autos.grid_auto_column_second)
              == GRID_TRACK_AUTO
          && grid_auto_second_value(autos.grid_auto_column_second)
              == GRID_TRACK_MIN_CONTENT_VALUE);
    /* Implicit columns alternate 100px and auto: the auto column fits its
       20px item instead of repeating 100px. */
    LayoutDocument layout = {0};
    CHECK(layout_build(&layout, &budget, &document, &sheet, NULL, NULL, 480));
    const LayoutNodeBox *grid = layout_box_for_node(
        &layout, find_id(root, "alt"));
    const LayoutNodeBox *c1 = layout_box_for_node(&layout, find_id(root, "c1"));
    const LayoutNodeBox *c2 = layout_box_for_node(&layout, find_id(root, "c2"));
    if (grid == NULL || c1 == NULL || c2 == NULL
        || c1->x != grid->x + 100 || c2->x != grid->x + 120)
        fprintf(stderr, "alternating columns: c1 %d c2 %d, grid %d\n",
                c1 != NULL ? c1->x : -1, c2 != NULL ? c2->x : -1,
                grid != NULL ? grid->x : -1);
    CHECK(grid != NULL && c1 != NULL && c2 != NULL
          && c1->x == grid->x + 100 && c2->x == grid->x + 120);
    layout_destroy(&layout);
    stylesheet_destroy(&sheet);
    document_destroy(&document);
    CHECK(budget.current == 0 && budget_uninstall_lexbor(&budget));
    return 0;
}

/* The two length parsers (parse_style_length, which compiles math into a
   StyleLength, and style_parse_length, which resolves to pixels at once)
   agree on what a length is: a bare parenthesized value or a top-level
   expression is not one, a parenthesized term inside a math function is,
   and env() resolves (the safe-area insets to 0, any other name to its
   fallback). Declarations that use either parser drop such a value. */
static int test_length_parsers_agree(void)
{
    Budget budget;
    budget_init(&budget, 8u * MIB);
    CHECK(budget_install_lexbor(&budget));
    PocDocument document = {0};
    Stylesheet sheet = {0};
    static const char page[] =
        "<!doctype html><style>"
        "#paren{margin-top:7px;margin-top:(10px);"
        "padding-top:7px;padding-top:(10px)}"
        "#env{margin-top:env(unknown, 9px);padding-top:env(unknown, 9px)}"
        "</style><p id=paren>x</p><p id=env>x</p>";
    CHECK(document_parse(&document, &budget, page, sizeof(page) - 1u, 17)
          && stylesheet_build(&sheet, &budget, &document, 480));
    static const struct { const char *text; int pixels; } cases[] = {
        {"1px", 1},
        {"calc((1px + 2px) * 2)", 6},
        {"calc(1px + (2px))", 3},
        {"min((1px), 2px)", 1},
        {"env(x, 1px)", 1},
        {"ENV(x,4px)", 4},
        {"calc(env(x, 3px) + 1px)", 4},
        {"env(safe-area-inset-top)", 0},
        {"env(safe-area-inset-left, 5px)", 0},
        {"(1px)", INT_MIN},
        {"(10px)", INT_MIN},
        {"((1px))", INT_MIN},
        {"1px + 2px", INT_MIN},
        {"-(1px)", INT_MIN},
        {"env(x)", INT_MIN},
        {"env()", INT_MIN},
        {"env(x 1px)", INT_MIN},
        {"env(safe-area-inset-top, foo)", INT_MIN},
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        const char *text = cases[i].text;
        StyleLength compiled = 0;
        bool compiled_ok = parse_style_length(&sheet, text, strlen(text),
                                              &compiled, NULL);
        int resolved = style_parse_length(&sheet, text, strlen(text),
                                          INT_MIN, NULL);
        bool ok = cases[i].pixels == INT_MIN
            ? !compiled_ok && resolved == INT_MIN
            : compiled_ok && compiled == cases[i].pixels
              && resolved == cases[i].pixels;
        if (!ok)
            fprintf(stderr, "%s: parse_style_length %s %d, "
                    "style_parse_length %d, want %d\n", text,
                    compiled_ok ? "accepted" : "rejected", (int) compiled,
                    resolved, cases[i].pixels);
        CHECK(ok);
    }
    lxb_dom_node_t *root = lxb_dom_interface_node(document.html);
    ComputedStyle paren = style_for_node(&sheet, find_id(root, "paren"), NULL);
    ComputedStyle env = style_for_node(&sheet, find_id(root, "env"), NULL);
    if (paren.margin.top != 7 || paren.padding.top != 7
        || env.margin.top != 9 || env.padding.top != 9)
        fprintf(stderr, "(10px) margin %d padding %d; env() margin %d "
                "padding %d\n", paren.margin.top, paren.padding.top,
                env.margin.top, env.padding.top);
    CHECK(paren.margin.top == 7 && paren.padding.top == 7
          && env.margin.top == 9 && env.padding.top == 9);
    stylesheet_destroy(&sheet);
    document_destroy(&document);
    CHECK(budget.current == 0 && budget_uninstall_lexbor(&budget));
    return 0;
}

/* Mask and background layers share one component scanner, so a
   backslash-escaped quote inside url() is part of the URL in a mask as it
   already was in a background: the position and size after it still
   apply, as they do for the same layer without the escape. */
static int test_layer_components_skip_escapes(void)
{
    Budget budget;
    budget_init(&budget, 8u * MIB);
    CHECK(budget_install_lexbor(&budget));
    PocDocument document = {0};
    Stylesheet sheet = {0};
    static const char page[] =
        "<!doctype html><style>"
        "#escaped{mask:url(\"a\\\"b.png\") center / 5px no-repeat}"
        "#plain{mask:url(\"ab.png\") center / 5px no-repeat}"
        "#bare{-webkit-mask:url(a\\\"b.png) left 25% / 6px}"
        "#bare-ref{-webkit-mask:url(ab.png) left 25% / 6px}"
        "</style><p id=escaped>x</p><p id=plain>x</p>"
        "<p id=bare>x</p><p id=bare-ref>x</p>";
    CHECK(document_parse(&document, &budget, page, sizeof(page) - 1u, 17)
          && stylesheet_build(&sheet, &budget, &document, 480));
    lxb_dom_node_t *root = lxb_dom_interface_node(document.html);
    static const struct { const char *id, *reference; } pairs[] = {
        {"escaped", "plain"}, {"bare", "bare-ref"},
    };
    for (size_t i = 0; i < sizeof(pairs) / sizeof(pairs[0]); i++) {
        ComputedStyle a = style_for_node(
            &sheet, find_id(root, pairs[i].id), NULL);
        ComputedStyle b = style_for_node(
            &sheet, find_id(root, pairs[i].reference), NULL);
        StylePaintStack sa = style_paint_stack_copy(&sheet, &a);
        StylePaintStack sb = style_paint_stack_copy(&sheet, &b);
        CHECK(sa.mask_count == 1 && sb.mask_count == 1
              && sb.masks[0].width != 0);
        const StylePaintLayer *la = &sa.masks[0], *lb = &sb.masks[0];
        bool same = la->kind == lb->kind && la->width == lb->width
            && la->height == lb->height && la->position_x == lb->position_x
            && la->position_y == lb->position_y && la->fit == lb->fit
            && la->flags == lb->flags
            && la->position_edges == lb->position_edges;
        if (!same)
            fprintf(stderr, "#%s mask %ux%u at %d,%d flags %u, want %ux%u "
                    "at %d,%d flags %u\n", pairs[i].id, la->width,
                    la->height, la->position_x, la->position_y, la->flags,
                    lb->width, lb->height, lb->position_x, lb->position_y,
                    lb->flags);
        CHECK(same);
    }
    stylesheet_destroy(&sheet);
    document_destroy(&document);
    CHECK(budget.current == 0 && budget_uninstall_lexbor(&budget));
    return 0;
}

/* thin, medium and thick are <line-width> keywords wherever a line width
   is accepted: in the outline and border shorthands as well as in
   outline-width and border-width, inline or through var(), and in any
   ASCII case. Every spelling computes what its pixel sibling computes,
   and the longhands agree on the pixel value of each keyword. */
static int test_line_width_keywords(void)
{
    Budget budget;
    budget_init(&budget, 8u * MIB);
    CHECK(budget_install_lexbor(&budget));
    PocDocument document = {0};
    Stylesheet sheet = {0};
    static const char page[] =
        "<!doctype html><style>"
        "#o-thick{outline:thick solid red}"
        "#o-thick-ref{outline:5px solid red}"
        "#o-upper{outline:solid THICK red}"
        "#o-var{--o:thin dashed blue;outline:var(--o)}"
        "#o-var-ref{outline:1px dashed blue}"
        "#b-thick{border:THICK solid red}"
        "#b-thick-ref{border:5px solid red}"
        "#b-side{border-left:Thin SOLID red}"
        "#b-side-ref{border-left:1px solid red}"
        "#b-var{--b:thick dotted red;border-top:var(--b)}"
        "#b-var-ref{border-top:5px dotted red}"
        "#w-thin{outline-width:thin;border-width:thin}"
        "#w-medium{outline-width:medium;border-width:medium}"
        "#w-thick{outline-width:THICK;border-width:thick}"
        "#i-pair{border-inline-width:thick thin;border-block-width:medium}"
        "#i-var{--i:thick thin;border-inline-width:var(--i)}"
        "#i-pair-ref{border-width:3px 1px 3px 5px}"
        "#i-var-ref{border-left-width:5px;border-right-width:1px}"
        "#i-percent{border-width:2px;border-inline-width:10% 5px}"
        "#i-percent-ref{border-width:2px}"
        "</style><p id=plain>x</p>"
        "<p id=o-thick>x</p><p id=o-thick-ref>x</p><p id=o-upper>x</p>"
        "<p id=o-var>x</p><p id=o-var-ref>x</p>"
        "<p id=b-thick>x</p><p id=b-thick-ref>x</p>"
        "<p id=b-side>x</p><p id=b-side-ref>x</p>"
        "<p id=b-var>x</p><p id=b-var-ref>x</p>"
        "<p id=w-thin>x</p><p id=w-medium>x</p><p id=w-thick>x</p>"
        "<p id=i-pair>x</p><p id=i-var>x</p><p id=i-pair-ref>x</p>"
        "<p id=i-var-ref>x</p>"
        "<p id=i-percent>x</p><p id=i-percent-ref>x</p>";
    CHECK(document_parse(&document, &budget, page, sizeof(page) - 1u, 17)
          && stylesheet_build(&sheet, &budget, &document, 480));
    lxb_dom_node_t *root = lxb_dom_interface_node(document.html);
    ComputedStyle plain = style_for_node(&sheet, find_id(root, "plain"), NULL);
    static const struct { const char *id, *reference; } pairs[] = {
        {"o-thick", "o-thick-ref"}, {"o-upper", "o-thick-ref"},
        {"o-var", "o-var-ref"}, {"b-thick", "b-thick-ref"},
        {"b-side", "b-side-ref"}, {"b-var", "b-var-ref"},
        {"i-pair", "i-pair-ref"}, {"i-var", "i-var-ref"},
        {"i-percent", "i-percent-ref"},
    };
    for (size_t i = 0; i < sizeof(pairs) / sizeof(pairs[0]); i++) {
        ComputedStyle a = style_for_node(
            &sheet, find_id(root, pairs[i].id), NULL);
        ComputedStyle b = style_for_node(
            &sheet, find_id(root, pairs[i].reference), NULL);
        bool same_lines = true, plain_lines = true;
        for (unsigned side = 0; side < STYLE_BORDER_SIDE_COUNT; side++) {
            unsigned line = computed_style_border_line(
                &b, (StyleBorderSide) side);
            same_lines &= computed_style_border_line(
                &a, (StyleBorderSide) side) == line;
            plain_lines &= computed_style_border_line(
                &plain, (StyleBorderSide) side) == line;
        }
        bool same = a.outline_state == b.outline_state
            && a.outline_color == b.outline_color
            && memcmp(&a.border, &b.border, sizeof(a.border)) == 0
            && a.border_color == b.border_color && same_lines;
        if (!same)
            fprintf(stderr, "#%s outline %04x border %d %d %d %d, "
                    "want %04x border %d %d %d %d\n", pairs[i].id,
                    a.outline_state, a.border.top, a.border.right,
                    a.border.bottom, a.border.left, b.outline_state,
                    b.border.top, b.border.right, b.border.bottom,
                    b.border.left);
        CHECK(same);
        CHECK(b.outline_state != plain.outline_state || !plain_lines
              || memcmp(&b.border, &plain.border, sizeof(b.border)) != 0);
    }
    static const struct { const char *id; int width; } widths[] = {
        {"w-thin", 1}, {"w-medium", 3}, {"w-thick", 5},
    };
    for (size_t i = 0; i < sizeof(widths) / sizeof(widths[0]); i++) {
        ComputedStyle a = style_for_node(
            &sheet, find_id(root, widths[i].id), NULL);
        if ((int) computed_style_outline_width(&a) != widths[i].width
            || a.border.top != widths[i].width
            || a.border.left != widths[i].width)
            fprintf(stderr, "#%s outline-width %u border-width %d, want %d\n",
                    widths[i].id, computed_style_outline_width(&a),
                    a.border.top, widths[i].width);
        CHECK((int) computed_style_outline_width(&a) == widths[i].width
              && a.border.top == widths[i].width
              && a.border.left == widths[i].width);
    }
    stylesheet_destroy(&sheet);
    document_destroy(&document);
    CHECK(budget.current == 0 && budget_uninstall_lexbor(&budget));
    return 0;
}

/* The shared top-level scanner skips a backslash escape, as the layer
   component scanner does: an escaped quote inside a string neither closes
   nor reopens it, so the comma after it still separates layers. */
static int test_top_level_scanner_escapes(void)
{
    static const char layers[] = "url(\"a\\\"b.png\") center, red";
    size_t cursor = 0, start = 0, end = 0;
    size_t length = sizeof(layers) - 1u;
    CHECK(style_next_top_level(layers, length, ',', &cursor, &start, &end));
    CHECK(end - start == strlen("url(\"a\\\"b.png\") center")
          && memcmp(layers + start, "url(", 4) == 0);
    CHECK(style_next_top_level(layers, length, ',', &cursor, &start, &end));
    CHECK(end - start == 3 && memcmp(layers + start, "red", 3) == 0);
    CHECK(!style_next_top_level(layers, length, ',', &cursor, &start, &end));
    /* An escaped space is part of the token, not a separator. */
    static const char words[] = "a\\ b c";
    cursor = 0;
    CHECK(style_next_top_level_token(words, sizeof(words) - 1u, &cursor,
                                     &start, &end)
          && start == 0 && end == 4);
    CHECK(style_next_top_level_token(words, sizeof(words) - 1u, &cursor,
                                     &start, &end)
          && start == 5 && end == 6);
    CHECK(!style_next_top_level_token(words, sizeof(words) - 1u, &cursor,
                                      &start, &end));
    /* A trailing lone backslash is an ordinary byte. */
    static const char trailing[] = "a, b\\";
    cursor = 0;
    CHECK(style_next_top_level(trailing, sizeof(trailing) - 1u, ',',
                               &cursor, &start, &end)
          && end - start == 1);
    CHECK(style_next_top_level(trailing, sizeof(trailing) - 1u, ',',
                               &cursor, &start, &end)
          && end - start == 2);

    Budget budget;
    budget_init(&budget, 8u * MIB);
    CHECK(budget_install_lexbor(&budget));
    PocDocument document = {0};
    Stylesheet sheet = {0};
    static const char page[] =
        "<!doctype html><style>"
        "#escaped{background:url(\"a\\\"b.png\") center, red}"
        "#plain{background:url(\"ab.png\") center, red}"
        "</style><p id=escaped>x</p><p id=plain>x</p>";
    CHECK(document_parse(&document, &budget, page, sizeof(page) - 1u, 17)
          && stylesheet_build(&sheet, &budget, &document, 480));
    lxb_dom_node_t *root = lxb_dom_interface_node(document.html);
    ComputedStyle escaped =
        style_for_node(&sheet, find_id(root, "escaped"), NULL);
    ComputedStyle plain = style_for_node(&sheet, find_id(root, "plain"), NULL);
    if (!escaped.has_background || escaped.background != plain.background)
        fprintf(stderr, "escaped layer colour %d %06x, want %06x\n",
                (int) escaped.has_background, (unsigned) escaped.background,
                (unsigned) plain.background);
    CHECK(plain.has_background && escaped.has_background
          && escaped.background == plain.background);
    stylesheet_destroy(&sheet);
    document_destroy(&document);
    CHECK(budget.current == 0 && budget_uninstall_lexbor(&budget));
    return 0;
}

/* Space-separated components are separated by any run of CSS
   whitespace: a tab, a newline or two spaces between box-shadow,
   text-wrap, @supports, or retained scroll-padding components must
   compute what one space computes. */
static int test_whitespace_separated_components(void)
{
    Budget budget;
    budget_init(&budget, 8u * MIB);
    CHECK(budget_install_lexbor(&budget));
    PocDocument document = {0};
    Stylesheet sheet = {0};
    static const char page[] =
        "<!doctype html><style>"
        "#s-tab{box-shadow:0 0\t4px red}"
        "#s-lines{box-shadow:0 0\n4px  red,\n1px 1px\t0 blue}"
        "#s-ref{box-shadow:0 0 4px red}"
        "#w-double{white-space:pre;text-wrap:wrap  pretty}"
        "#w-ref{white-space:pre;text-wrap:wrap pretty}"
        "@supports (translate:10px\t20px){#q-translate{order:1}}"
        "@supports (color-scheme:light  dark){#q-scheme{order:1}}"
        "@supports (scroll-margin:1px\n2px){#q-margin{order:1}}"
        "@supports (contain-intrinsic-size:auto\t10px){#q-size{order:1}}"
        "#p-tab{scroll-padding:1px\t2px}"
        "#p-double{scroll-padding:1px  2px 3px}"
        "</style><p id=plain>x</p>"
        "<p id=s-tab>x</p><p id=s-lines>x</p><p id=s-ref>x</p>"
        "<p id=w-double>x</p><p id=w-ref>x</p>"
        "<p id=q-translate>x</p><p id=q-scheme>x</p><p id=q-margin>x</p>"
        "<p id=q-size>x</p><p id=p-tab>x</p><p id=p-double>x</p>";
    CHECK(document_parse(&document, &budget, page, sizeof(page) - 1u, 17)
          && stylesheet_build(&sheet, &budget, &document, 480));
    lxb_dom_node_t *root = lxb_dom_interface_node(document.html);
    ComputedStyle plain = style_for_node(&sheet, find_id(root, "plain"), NULL);
    ComputedStyle ref = style_for_node(&sheet, find_id(root, "s-ref"), NULL);
    CHECK(stylesheet_box_shadow_count(&sheet, &ref) == 1);
    const StyleBoxShadow *expected = stylesheet_box_shadow(&sheet, &ref, 0);
    static const char *const shadows[] = {"s-tab", "s-lines"};
    for (size_t i = 0; i < 2; i++) {
        ComputedStyle a = style_for_node(
            &sheet, find_id(root, shadows[i]), NULL);
        size_t count = stylesheet_box_shadow_count(&sheet, &a);
        if (count != i + 1u)
            fprintf(stderr, "#%s box-shadow layers %zu, want %zu\n",
                    shadows[i], count, i + 1u);
        CHECK(count == i + 1u
              && memcmp(stylesheet_box_shadow(&sheet, &a, 0), expected,
                        sizeof(*expected)) == 0);
    }
    ComputedStyle wrap = style_for_node(
        &sheet, find_id(root, "w-double"), NULL);
    ComputedStyle wrap_ref = style_for_node(
        &sheet, find_id(root, "w-ref"), NULL);
    if (wrap.white_space_mode != wrap_ref.white_space_mode)
        fprintf(stderr, "#w-double white-space %u, want %u\n",
                (unsigned) wrap.white_space_mode,
                (unsigned) wrap_ref.white_space_mode);
    CHECK(wrap_ref.white_space_mode != WHITE_SPACE_PRE
          && wrap.white_space_mode == wrap_ref.white_space_mode);
    static const char *const supported[] = {
        "q-translate", "q-scheme", "q-margin", "q-size",
    };
    for (size_t i = 0; i < 4; i++) {
        ComputedStyle a = style_for_node(
            &sheet, find_id(root, supported[i]), NULL);
        if (a.order == plain.order)
            fprintf(stderr, "#%s @supports rule did not apply\n",
                    supported[i]);
        CHECK(a.order != plain.order);
    }
    static const struct {
        const char *id;
        const char *edges[4];
    } paddings[] = {
        {"p-tab", {"1px", "2px", "1px", "2px"}},
        {"p-double", {"1px", "2px", "3px", "2px"}},
    };
    for (size_t i = 0; i < 2; i++) {
        StyleRetainedBoxValues box;
        CHECK(style_retained_box_values(
            &sheet, find_id(root, paddings[i].id), "scroll-padding", NULL,
            &box));
        bool same = box.present_mask == 15u;
        for (size_t side = 0; side < 4; side++)
            same &= strcmp(box.values[side], paddings[i].edges[side]) == 0;
        if (!same)
            fprintf(stderr, "#%s scroll-padding mask %u '%s' '%s' '%s' '%s'\n",
                    paddings[i].id, (unsigned) box.present_mask,
                    box.values[0], box.values[1], box.values[2],
                    box.values[3]);
        CHECK(same);
    }
    stylesheet_destroy(&sheet);
    document_destroy(&document);
    CHECK(budget.current == 0 && budget_uninstall_lexbor(&budget));
    return 0;
}

/* A long value written through var() reaches each property's parser
   whole: every resolve buffer is sized from a named class (keywords,
   one math value, or open-ended), and each long spelling below is past
   the buffer its property used to resolve into. Each one must compute
   what the short spelling on its sibling computes. */
static int test_long_var_values_per_property(void)
{
    char translations[512] = "";
    for (size_t i = 0; i < 14; i++)
        strcat(translations, "translate(1px, 2px) ");
    /* Few terms (the math compiler bounds its nodes), long spellings. */
    char sum24[256] = "calc(10.000000000000px", sum22[256];
    for (size_t i = 0; i < 6; i++) strcat(sum24, " + 2.000000000000px");
    memcpy(sum22, sum24, sizeof(sum22));
    strcat(sum24, " + 2.000000000000px)");
    strcat(sum22, ")");
    char icons[160] = "\"";
    for (size_t i = 0; i < 20; i++) {
        char escape[8];
        snprintf(escape, sizeof(escape), "\\e9%02zu", i);
        strcat(icons, escape);
    }
    strcat(icons, "\"");
    static const char image[] = "https://static.example.test/assets/"
        "icons/navigation/arrow-right-long-variant-v2-dark-theme.svg";
    static const char clip[] = "inset(5px calc(5px + 0px) calc(5px + 0px) "
        "calc(5px + 0px) round 4px calc(4px + 0px) calc(4px + 0px) "
        "calc(4px + 0px))";
    static const char outline[] = "3px solid light-dark(light-dark("
        "rgba(255.0,0.0,0.0,1.0),rgba(1.0,1.0,1.0,1.0)),light-dark("
        "rgba(2.0,2.0,2.0,1.0),rgba(3.0,3.0,3.0,1.0)))";
    /* Line names are bounded by STYLE_GRID_AREA_NAME_CAPACITY; a value
       formatted over several lines still runs past 192 bytes. */
    static const char area[] = "header-row-start-line-aaaaaa\n"
        "                                / sidebar-column-start-line-bb\n"
        "                                / header-row-end-line-cccccccc\n"
        "                                / sidebar-column-end-line-dddd";
    CHECK(strlen(translations) > 256u && strlen(sum24) > 128u
          && strlen(sum24) < STYLE_MATH_SOURCE_CAPACITY
          && strlen(sum22) > 128u && strlen(icons) > 96u
          && strlen(image) + 7u > 96u && strlen(clip) > 96u
          && strlen(outline) > 128u && strlen(area) > 192u);
    char css[4096];
    int written = snprintf(css, sizeof(css),
        "#clip-long{--v:%s;clip-path:var(--v)}"
        "#clip-short{clip-path:inset(5px round 4px)}"
        "#icon-long::before{--v:%s;content:var(--v)}"
        "#image-long::before{--v:url(%s);content:var(--v)}"
        "#transform-long{--v:%s;transform:var(--v)}"
        "#transform-short{transform:translate(14px, 28px)}"
        "#outline-long{--v:%s;outline:var(--v)}"
        "#outline-short{outline:3px solid #ff0000}"
        "#lh-long{--v:%s;line-height:var(--v)}"
        "#lh-short{line-height:24px}"
        "#fs-long{--v:%s;font-size:var(--v)}"
        "#fs-short{font-size:22px}"
        "#area-long{--v:%s;grid-area:var(--v)}"
        "#area-short{grid-area:header-row-start-line-aaaaaa / "
        "sidebar-column-start-line-bb / header-row-end-line-cccccccc / "
        "sidebar-column-end-line-dddd}",
        clip, icons, image, translations, outline, sum24, sum22, area);
    CHECK(written > 0 && (size_t) written < sizeof(css));
    Budget budget;
    budget_init(&budget, 8u * MIB);
    CHECK(budget_install_lexbor(&budget));
    PocDocument document = {0};
    static const char html[] =
        "<p id=plain>x</p><p id=clip-long>x</p><p id=clip-short>x</p>"
        "<p id=icon-long>x</p><p id=image-long>x</p>"
        "<p id=transform-long>x</p><p id=transform-short>x</p>"
        "<p id=outline-long>x</p><p id=outline-short>x</p>"
        "<p id=lh-long>x</p><p id=lh-short>x</p>"
        "<p id=fs-long>x</p><p id=fs-short>x</p>"
        "<p id=area-long>x</p><p id=area-short>x</p>";
    CHECK(document_parse(&document, &budget, html, sizeof(html) - 1u, 17));
    Stylesheet sheet = {0};
    CHECK(stylesheet_build(&sheet, &budget, &document, 480)
          && stylesheet_add_css(&sheet, css, (size_t) written));
    lxb_dom_node_t *root = lxb_dom_interface_node(document.html);
#define STYLE_OF(id) style_for_node(&sheet, find_id(root, id), NULL)
    ComputedStyle plain = STYLE_OF("plain");
    ComputedStyle a = STYLE_OF("clip-long"), b = STYLE_OF("clip-short");
    if (a.clip_path_state != b.clip_path_state)
        fprintf(stderr, "long clip-path %04x, want %04x\n",
                a.clip_path_state, b.clip_path_state);
    CHECK(b.clip_path_state != plain.clip_path_state
          && a.clip_path_state == b.clip_path_state);
    lxb_dom_node_t *icon_node = find_id(root, "icon-long");
    ComputedStyle icon_parent = style_for_node(&sheet, icon_node, NULL);
    ComputedStyle icon = style_for_pseudo(&sheet, icon_node, PSEUDO_BEFORE,
                                          &icon_parent);
    if (icon.generated_text_length != 60u)
        fprintf(stderr, "long content text %u bytes, want 60\n",
                (unsigned) icon.generated_text_length);
    CHECK(icon.generated_content && icon.generated_text_length == 60u);
    lxb_dom_node_t *image_node = find_id(root, "image-long");
    ComputedStyle image_parent = style_for_node(&sheet, image_node, NULL);
    ComputedStyle pictured = style_for_pseudo(&sheet, image_node,
                                              PSEUDO_BEFORE, &image_parent);
    CHECK(pictured.generated_content && pictured.generated_image
          && pictured.generated_text != NULL
          && strstr(pictured.generated_text, "arrow-right-long") != NULL);
    a = STYLE_OF("transform-long");
    b = STYLE_OF("transform-short");
    if (a.has_transform != b.has_transform || a.transform_x != b.transform_x
        || a.transform_y != b.transform_y)
        fprintf(stderr, "long transform %d %d,%d, want %d %d,%d\n",
                a.has_transform, a.transform_x, a.transform_y,
                b.has_transform, b.transform_x, b.transform_y);
    CHECK(b.has_transform && a.has_transform
          && a.transform_x == b.transform_x
          && a.transform_y == b.transform_y);
    a = STYLE_OF("outline-long");
    b = STYLE_OF("outline-short");
    if (a.outline_state != b.outline_state
        || a.outline_color != b.outline_color)
        fprintf(stderr, "long outline %04x %06x, want %04x %06x\n",
                a.outline_state, (unsigned) a.outline_color,
                b.outline_state, (unsigned) b.outline_color);
    CHECK(b.outline_state != plain.outline_state
          && a.outline_state == b.outline_state
          && a.outline_color == b.outline_color
          && a.outline_alpha == b.outline_alpha);
    a = STYLE_OF("lh-long");
    b = STYLE_OF("lh-short");
    if (a.line_height != b.line_height)
        fprintf(stderr, "long line-height %d, want %d\n",
                a.line_height, b.line_height);
    CHECK(b.line_height != plain.line_height
          && a.line_height == b.line_height);
    a = STYLE_OF("fs-long");
    b = STYLE_OF("fs-short");
    if (a.font_size != b.font_size)
        fprintf(stderr, "long font-size %d, want %d\n",
                a.font_size, b.font_size);
    CHECK(b.font_size != plain.font_size && a.font_size == b.font_size);
    a = STYLE_OF("area-long");
    b = STYLE_OF("area-short");
    if (memcmp(a.grid_placement, b.grid_placement,
               sizeof(a.grid_placement)) != 0)
        fprintf(stderr, "long grid-area placement differs\n");
    CHECK(memcmp(b.grid_placement, plain.grid_placement,
                 sizeof(b.grid_placement)) != 0
          && memcmp(a.grid_placement, b.grid_placement,
                    sizeof(a.grid_placement)) == 0);
#undef STYLE_OF
    stylesheet_destroy(&sheet);
    document_destroy(&document);
    CHECK(budget.current == 0 && budget_uninstall_lexbor(&budget));
    return 0;
}

static int test_retained_container_selectors(void)
{
    Budget budget;
    budget_init(&budget, 8u * MIB);
    CHECK(budget_install_lexbor(&budget));
    PocDocument document = {0};
    Stylesheet sheet = {0};
    static const char page[] =
        "<!doctype html><style>.outer{container:outer / inline-size}"
        ".inner{container:inner / inline-size}.target{color:red}"
        "@container inner (width > 100px){"
        " @container outer (width > 500px){.target{color:blue}"
        " .target::before{content:'wide'}}}</style>"
        "<div id=outer class=outer><div id=inner class=inner>"
        "<p id=target class=target>Text</p></div></div>";
    CHECK(document_parse(&document, &budget, page, sizeof(page) - 1u, 17)
          && stylesheet_build(&sheet, &budget, &document, 480));
    lxb_dom_node_t *root = lxb_dom_interface_node(document.html);
    lxb_dom_node_t *outer = find_id(root, "outer");
    lxb_dom_node_t *inner = find_id(root, "inner");
    lxb_dom_node_t *node = find_id(root, "target");
    StyleRetainedMatches *retained = style_retained_matches_create(&budget);
    CHECK(outer != NULL && inner != NULL && node != NULL && retained != NULL);
    (void) style_retained_matches_attach(&sheet, retained);
    static const int widths[][2] = {{600, 90}, {600, 200}, {600, 80}, {400, 200}, {600, 200}};
    for (size_t turn = 0; turn < sizeof(widths) / sizeof(widths[0]); turn++) {
        CHECK(style_container_layout_state_begin(&sheet, &budget, 16)
              && style_container_layout_state_add(&sheet, outer, widths[turn][0], 100, 0, 0)
              && style_container_layout_state_add(&sheet, inner, widths[turn][1], 50, 0, 0));
        uint64_t candidates = sheet.rule_index_candidates;
        ComputedStyle result = style_for_node(&sheet, node, NULL);
        ComputedStyle pseudo = style_for_pseudo(&sheet, node, PSEUDO_BEFORE, &result);
        bool active = widths[turn][0] > 500 && widths[turn][1] > 100;
        CHECK(result.color == (active ? 0x0000ffu : 0xff0000u)
              && pseudo.generated_content == active);
        /* Inactive conditional selectors must be retained too, so a later
           resize can activate them without repeating selector matching. */
        if (turn != 0) CHECK(sheet.rule_index_candidates == candidates);
        (void) style_retained_matches_attach(&sheet, NULL);
        ComputedStyle fresh = style_for_node(&sheet, node, NULL);
        ComputedStyle fresh_pseudo = style_for_pseudo(&sheet, node, PSEUDO_BEFORE, &fresh);
        CHECK(fresh.color == result.color
              && fresh_pseudo.generated_content == pseudo.generated_content);
        (void) style_retained_matches_attach(&sheet, retained);
        if (turn == 1) {
            CHECK(style_container_log_begin(&sheet, &budget, &sheet));
            uint64_t generation = sheet.container_state_generation;
            style_container_live_update(&sheet, inner, 200, 50);
            CHECK(sheet.container_state_generation == generation);
            style_container_live_update(&sheet, inner, 90, 50);
            CHECK(sheet.container_state_generation != generation);
            ComputedStyle narrow = style_for_node(&sheet, node, NULL);
            CHECK(narrow.color == 0xff0000u);
            style_container_live_update(&sheet, inner, 200, 50);
            ComputedStyle wide = style_for_node(&sheet, node, NULL);
            CHECK(wide.color == 0x0000ffu);
            style_container_log_end(&sheet, &sheet);
        }
    }
    CHECK(retained->hits >= 8u);
    (void) style_retained_matches_attach(&sheet, NULL);
    style_retained_matches_destroy(retained);
    stylesheet_destroy(&sheet);
    document_destroy(&document);
    CHECK(budget.current == 0 && budget_uninstall_lexbor(&budget));
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

static int test_retained_dense_storage(void)
{
    Budget budget;
    budget_init(&budget, 8u * MIB);
    CHECK(budget_install_lexbor(&budget));
    PocDocument document = {0};
    Stylesheet sheet = {0};
    char html[8192];
    size_t length = (size_t) snprintf(html, sizeof(html),
        "<style>p{color:#123456}p::before{content:'x'}</style>");
    for (unsigned i = 0; i < 600; i++) {
        int added = snprintf(html + length, sizeof(html) - length,
                             "<p>x</p>");
        CHECK(added > 0 && (size_t) added < sizeof(html) - length);
        length += (size_t) added;
    }
    CHECK(document_parse(&document, &budget, html, length, 17)
          && stylesheet_build(&sheet, &budget, &document, 480));
    StyleRetainedMatches *table = style_retained_matches_create(&budget);
    CHECK(table != NULL && table->entry_count == 0
          && style_retained_matches_bytes(table) < 70u * 1024u);
    lxb_dom_node_t *body = lxb_dom_interface_node(document.html->body);
    lxb_dom_node_t *node = body->first_child;
    CHECK(node != NULL);
    ComputedStyle reference = style_for_node(&sheet, node, NULL);
    (void) style_retained_matches_attach(&sheet, table);
    size_t limit = budget.limit;
    budget.limit = budget.current;
    ComputedStyle refused = style_for_node(&sheet, node, NULL);
    CHECK(refused.color == reference.color && table->occupied == 0
          && table->growth_refused && table->entry_capacity == 0);
    size_t failures = budget.failure_count;
    (void) style_for_node(&sheet, node, NULL);
    CHECK(budget.failure_count == failures); /* No allocation-refusal storm. */
    budget.limit = limit;
    style_retained_matches_clear(table);
    /* A later optional growth refusal must also preserve already-cached
       answers, their hash associations, and their allocation. */
    node = body->first_child;
    while (node != NULL && table->entry_count < 128u) {
        (void) style_for_node(&sheet, node, NULL);
        node = node->next;
    }
    CHECK(node != NULL && table->entry_count == 128u
          && table->entry_capacity == 128u);
    budget.limit = budget.current;
    while (node != NULL && !table->growth_refused) {
        ComputedStyle computed = style_for_node(&sheet, node, NULL);
        CHECK(computed.color == reference.color);
        node = node->next;
    }
    CHECK(table->growth_refused && table->entry_count == 128u);
    size_t growth_hits = table->hits;
    CHECK(style_for_node(&sheet, body->first_child, NULL).color == reference.color
          && table->hits == growth_hits + 1u);
    budget.limit = limit;
    style_retained_matches_clear(table);
    for (node = body->first_child; node != NULL; node = node->next) {
        ComputedStyle computed = style_for_node(&sheet, node, NULL);
        CHECK(computed.color == reference.color);
        (void) style_for_pseudo(&sheet, node, PSEUDO_BEFORE, &computed);
        (void) style_for_pseudo(&sheet, node, PSEUDO_AFTER, &computed);
    }
    CHECK(table->entry_count > 128 && table->entry_capacity <= 2048
          && style_retained_matches_bytes(table) < 150u * 1024u);
    size_t count = table->entry_count, capacity = table->entry_capacity;
    size_t before_hits = table->hits;
    for (node = body->first_child; node != NULL; node = node->next) {
        ComputedStyle computed = style_for_node(&sheet, node, NULL);
        CHECK(computed.color == reference.color);
        (void) style_for_pseudo(&sheet, node, PSEUDO_BEFORE, &computed);
        (void) style_for_pseudo(&sheet, node, PSEUDO_AFTER, &computed);
    }
    CHECK(table->hits > before_hits && table->entry_count == count);
    for (node = body->first_child; node != NULL; node = node->next)
        style_retained_matches_forget_node(table, node);
    CHECK(table->occupied == 0);
    for (node = body->first_child; node != NULL; node = node->next)
        (void) style_for_node(&sheet, node, NULL);
    CHECK(table->entry_count == count && table->entry_capacity == capacity);
    style_retained_matches_clear(table);
    CHECK(table->occupied == 0 && table->entry_count == 0);
    (void) style_for_node(&sheet, body->first_child, NULL);
    CHECK(table->entry_count == 1 && table->entry_capacity == capacity);
    (void) style_retained_matches_attach(&sheet, NULL);
    style_retained_matches_destroy(table);
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
        while (home < STYLE_RETAINED_MATCH_CAPACITY) {
            uint16_t index = retained->slots[home];
            if (index != 0 && retained->entries[index - 1u].node == node
                && retained->entries[index - 1u].pseudo == (uint8_t) pseudos[p]) break;
            home++;
        }
        CHECK(home < STYLE_RETAINED_MATCH_CAPACITY);
        StyleRetainedMatchEntry entry = retained->entries[retained->slots[home] - 1u];
        style_retained_matches_clear(retained);
        /* Token invalidation leaves holes; a subsequent insertion can also
           duplicate a key that survives farther along its bounded probe. */
        for (size_t i = 0; i < 2; i++) {
            size_t slot = (home + 1u + i * 2u) & (STYLE_RETAINED_MATCH_CAPACITY - 1u);
            retained->entries[i] = entry;
            retained->entries[i].slot = (uint16_t) slot;
            retained->slots[slot] = (uint16_t) (i + 1u);
        }
        retained->entry_count = 2;
        retained->occupied = 2;
        style_retained_matches_forget_subtree(retained, group);
        CHECK(retained->occupied == 0);
        for (size_t i = 0; i < STYLE_RETAINED_MATCH_CAPACITY; i++)
            CHECK(retained->slots[i] == 0);
    }
    (void) style_retained_matches_attach(&sheet, NULL);
    style_retained_matches_destroy(retained);
    stylesheet_destroy(&sheet);
    document_destroy(&document);
    CHECK(budget.current == 0 && budget_uninstall_lexbor(&budget));
    return 0;
}

static bool append_unrelated_style(Stylesheet *sheet, void *unused)
{
    (void) unused;
    static const char css[] = ".unrelated{color:#abcdef;z-index:9!important}";
    return stylesheet_add_css(sheet, css, sizeof(css) - 1u);
}

static bool append_no_style(Stylesheet *sheet, void *cancel)
{
    (void) sheet;
    return cancel == NULL;
}

static int test_append_identity_fallback(void)
{
    for (unsigned mode = 0; mode < 3u; mode++) {
        Budget budget;
        budget_init(&budget, 8u * MIB);
        CHECK(budget_install_lexbor(&budget));
        PocDocument document = {0};
        Stylesheet sheet = {0};
        static const char html[] = "<style>.one{color:red}.two{color:blue}</style>";
        CHECK(document_parse(&document, &budget, html, sizeof(html)-1u, 17)
              && stylesheet_build(&sheet, &budget, &document, 480));
        if (mode == 0) sheet.rules[1].order = sheet.rules[0].order;
        if (mode == 1) sheet.next_order = UINT_MAX;
        StylesheetAppendResult append = {0};
        bool ok = stylesheet_append_sources_tracked(&sheet, append_no_style,
            mode == 2 ? &sheet : NULL, sheet.style_source_count, &append);
        CHECK(mode == 2 ? !ok : ok && append.appended_bounded_out
              && append.remap == NULL);
        stylesheet_append_result_release(&sheet, &append);
        stylesheet_destroy(&sheet);
        document_destroy(&document);
        CHECK(budget.current == 0 && budget_uninstall_lexbor(&budget));
    }
    return 0;
}

static int retained_mixed_importance_append_case(const char *css, bool insert,
                                                size_t fail_after,
                                                bool *bounded_out)
{
    Budget budget;
    budget_init(&budget, 8u * MIB);
    CHECK(budget_install_lexbor(&budget));
    PocDocument document = {0};
    Stylesheet sheet = {0};
    char html[1024];
    int length = snprintf(html, sizeof(html),
        "<style>%s</style><div class=card id=card>Card</div>", css);
    CHECK(length > 0 && (size_t) length < sizeof(html));
    CHECK(document_parse(&document, &budget, html, (size_t) length, 17)
          && stylesheet_build(&sheet, &budget, &document, 480));
    lxb_dom_node_t *node = find_id(lxb_dom_interface_node(document.html), "card");
    StyleRetainedMatches *retained = style_retained_matches_create(&budget);
    CHECK(node != NULL && retained != NULL);
    (void) style_retained_matches_attach(&sheet, retained);
    ComputedStyle before = style_for_node(&sheet, node, NULL);
    CHECK(before.out_of_flow && before.background_image_kind == STYLE_BACKGROUND_IMAGE_GRADIENT
          && before.padding.left == 10 && before.z_index == 7);
    StylesheetAppendResult append = {0};
    if (fail_after != SIZE_MAX) budget_inject_failure_after(&budget, fail_after);
    bool appended = stylesheet_append_sources_tracked(&sheet, append_unrelated_style,
        NULL, insert ? 0 : sheet.style_source_count, &append);
    budget_clear_failure_injection(&budget);
    CHECK(appended || fail_after != SIZE_MAX);
    if (appended && append.appended_bounded_out) {
        if (bounded_out != NULL) *bounded_out = true;
        style_retained_matches_clear(retained);
    } else if (appended) {
        CHECK(append.remap != NULL && append.appended_count == 2u);
        CHECK(sheet.rules[append.appended[0]].important
              != sheet.rules[append.appended[1]].important);
        for (size_t i = 0; i < append.old_count; i++) {
            for (size_t j = i + 1u; j < append.old_count; j++) {
                CHECK(append.remap[i] != append.remap[j]);
            }
        }
        style_retained_matches_remap(retained, append.remap, append.old_count);
        style_retained_matches_invalidate_rules(retained, &sheet,
            append.appended, append.appended_count);
    }
    ComputedStyle replay = appended ? style_for_node(&sheet, node, NULL) : before;
    (void) style_retained_matches_attach(&sheet, NULL);
    ComputedStyle exact = style_for_node(&sheet, node, NULL);
    CHECK(!appended || (replay.out_of_flow == exact.out_of_flow
          && replay.background_image_kind == exact.background_image_kind
          && replay.padding.left == exact.padding.left
          && replay.z_index == exact.z_index));
    stylesheet_append_result_release(&sheet, &append);
    style_retained_matches_destroy(retained);
    stylesheet_destroy(&sheet);
    document_destroy(&document);
    CHECK(budget.current == 0 && budget_uninstall_lexbor(&budget));
    return 0;
}

static int test_retained_mixed_importance_append(void)
{
    static const char *const css[] = {
        ".card{position:absolute;background:linear-gradient(black,transparent);"
        "padding:10px;z-index:7!important}",
        ".card{position:relative;position:absolute;padding:1px;padding:10px;"
        "background:linear-gradient(black,transparent);"
        "z-index:2!important;z-index:7!important}",
        "@layer cards{.card,.other{position:absolute;"
        "background:linear-gradient(black,transparent);padding:10px;"
        "z-index:7!important}}"
    };
    for (size_t i = 0; i < sizeof(css) / sizeof(css[0]); i++) {
        CHECK(retained_mixed_importance_append_case(css[i], false, SIZE_MAX, NULL) == 0);
        CHECK(retained_mixed_importance_append_case(css[i], true, SIZE_MAX, NULL) == 0);
    }
    bool bounded_out = false;
    for (size_t fail_after = 0; fail_after < 64u; fail_after++)
        CHECK(retained_mixed_importance_append_case(
            css[0], false, fail_after, &bounded_out) == 0);
    CHECK(bounded_out);
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

/* A bounded-out sheet without adopted sources (restored from the compiled
   stylesheet cache, or past STYLE_SOURCE_NODE_LIMIT <style> sources) has
   no confinement to fail closed: a focus move with a sibling focus rule
   keeps the scoped invalidation instead of resetting the reuse cache. */
static int test_bounded_out_sheet_keeps_focus_fast_path(void)
{
    for (int variant = 0; variant < 2; variant++) {
        Budget budget;
        budget_init(&budget, 8u * MIB);
        CHECK(budget_install_lexbor(&budget));
        PocDocument document = {0};
        Stylesheet sheet = {0};
        char html[4096];
        size_t length = 0;
        if (variant == 1) {
            /* One more source than the sheet records. */
            for (unsigned i = 0; i <= STYLE_SOURCE_NODE_LIMIT; i++) {
                int wrote = snprintf(html + length, sizeof(html) - length,
                                     "<style>.s%u{color:red}</style>", i);
                CHECK(wrote > 0 && (size_t) wrote < sizeof(html) - length);
                length += (size_t) wrote;
            }
        }
        int wrote = snprintf(html + length, sizeof(html) - length,
            "<style>label{color:#123456}"
            "input:focus + label{color:#654321}</style>"
            "<input id=input><label id=label>L</label><p id=p>Text</p>");
        CHECK(wrote > 0 && (size_t) wrote < sizeof(html) - length);
        length += (size_t) wrote;
        CHECK(document_parse(&document, &budget, html, length, 17)
              && stylesheet_build(&sheet, &budget, &document, 480));
        if (variant == 0) {
            CHECK(!sheet.style_sources_bounded_out);
            /* What navigation_compiled_stylesheet_cache_store does before
               the next document adopts the sheet. */
            stylesheet_prepare_for_document_reuse(&sheet);
        }
        CHECK(sheet.style_sources_bounded_out
              && sheet.adopted_source_count == 0);
        lxb_dom_node_t *root = lxb_dom_interface_node(document.html);
        lxb_dom_node_t *input = find_id(root, "input");
        lxb_dom_node_t *label = find_id(root, "label");
        lxb_dom_node_t *p = find_id(root, "p");
        CHECK(input != NULL && label != NULL && p != NULL);
        uint32_t rules[128];
        CHECK(style_focus_nonlocal_rules(&sheet, input, rules, 128u)
              != SIZE_MAX);
        LayoutReuseCache *reuse = layout_reuse_cache_create(&budget);
        CHECK(reuse != NULL);
        layout_reuse_cache_enable_retained_matches(reuse);
        layout_reuse_cache_prepare(reuse, &sheet, NULL, NULL, 480);
        ComputedStyle style;
        (void) layout_reuse_cache_resolve_style(reuse, &sheet, NULL, label,
                                                NULL, &style);
        CHECK(style.color == 0x123456);
        (void) layout_reuse_cache_resolve_style(reuse, &sheet, NULL, p,
                                                NULL, &style);
        LayoutReuseStats before = {0}, after = {0};
        layout_reuse_cache_stats(reuse, &before);
        BudgetAllocationOwner owner = document_allocation_owner_enter(&document);
        CHECK(lxb_dom_element_set_attribute(lxb_dom_interface_element(input),
            (const lxb_char_t *) "data-tilefinch-focus", 20,
            (const lxb_char_t *) "", 0) != NULL);
        document_allocation_owner_leave(&document, owner);
        layout_reuse_cache_invalidate_focus(reuse, input);
        layout_reuse_cache_stats(reuse, &after);
        CHECK(after.full_resets == before.full_resets
              && after.scoped_invalidations == before.scoped_invalidations + 1u);
        layout_reuse_cache_prepare(reuse, &sheet, NULL, NULL, 480);
        (void) layout_reuse_cache_resolve_style(reuse, &sheet, NULL, label,
                                                NULL, &style);
        CHECK(style.color == 0x654321);
        layout_reuse_cache_destroy(reuse);
        stylesheet_destroy(&sheet);
        document_destroy(&document);
        CHECK(budget.current == 0 && budget_uninstall_lexbor(&budget));
    }
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

static bool retained_has_entry(const StyleRetainedMatches *table,
                               const lxb_dom_node_t *node)
{
    for (size_t i = 0; i < table->entry_count; i++)
        if (table->entries[i].node == node) return true;
    return false;
}

/* Scoped and ancestor invalidation drop exactly the entries of the scope's
   subtree, or of a node and its ancestors, and keep every other entry. */
static int test_retained_scoped_invalidation(void)
{
    Budget budget;
    budget_init(&budget, 8u * MIB);
    CHECK(budget_install_lexbor(&budget));
    PocDocument document = {0};
    Stylesheet sheet = {0};
    static const char html[] = "<style>p{color:#123456}div{margin:1px}"
        "</style><div id=top><div id=left><p id=a>a</p><p id=b>b</p></div>"
        "<div id=right><p id=c>c</p></div></div>";
    CHECK(document_parse(&document, &budget, html, sizeof(html)-1u, 17)
          && stylesheet_build(&sheet, &budget, &document, 480));
    lxb_dom_node_t *root = lxb_dom_interface_node(document.html);
    static const char *const ids[] = {"top", "left", "a", "b", "right", "c"};
    lxb_dom_node_t *nodes[6];
    for (size_t i = 0; i < 6; i++) {
        nodes[i] = find_id(root, ids[i]);
        CHECK(nodes[i] != NULL);
    }
    StyleRetainedMatches *retained = style_retained_matches_create(&budget);
    CHECK(retained != NULL);
    (void) style_retained_matches_attach(&sheet, retained);
    for (int round = 0; round < 2; round++) {
        for (size_t i = 0; i < 6; i++)
            (void) style_for_node(&sheet, nodes[i], NULL);
        for (size_t i = 0; i < 6; i++) CHECK(retained_has_entry(retained, nodes[i]));
        if (round == 0) {
            /* Within #left: #left, #a and #b go; #top, #right, #c stay. */
            style_retained_matches_invalidate_within(retained, nodes[1]);
            CHECK(!retained_has_entry(retained, nodes[1])
                  && !retained_has_entry(retained, nodes[2])
                  && !retained_has_entry(retained, nodes[3]));
            CHECK(retained_has_entry(retained, nodes[0])
                  && retained_has_entry(retained, nodes[4])
                  && retained_has_entry(retained, nodes[5]));
        } else {
            /* Ancestors of #c: #c, #right and #top go; #left, #a, #b stay. */
            style_retained_matches_invalidate_ancestors(retained, nodes[5]);
            CHECK(!retained_has_entry(retained, nodes[5])
                  && !retained_has_entry(retained, nodes[4])
                  && !retained_has_entry(retained, nodes[0]));
            CHECK(retained_has_entry(retained, nodes[1])
                  && retained_has_entry(retained, nodes[2])
                  && retained_has_entry(retained, nodes[3]));
        }
    }
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

/* A rule whose subject compound is :is()/:where() takes a fast key when
   every option requires it, and selector-list arguments are matched from
   a per-sheet prepared form. Both must agree with the plain text matcher
   for every rule and element, and a derived key must be a key every
   matched element carries. */
static bool element_has_key(lxb_dom_node_t *node, const StyleRule *rule)
{
    const char *key = style_rule_fast_key(rule);
    size_t key_length = rule->fast_key_length;
    size_t length = 0;
    if (rule->type == SELECTOR_ID) {
        const char *id = document_attribute(node, "id", &length);
        return id != NULL && length == key_length
            && memcmp(id, key, key_length) == 0;
    }
    if (rule->type == SELECTOR_CLASS) {
        const char *classes = document_attribute(node, "class", &length);
        return classes != NULL
            && class_contains_length(classes, length, key, key_length);
    }
    const char *tag = document_element_name(node, &length);
    return tag != NULL && length == key_length
        && memcmp(tag, key, key_length) == 0;
}

static int test_functional_selector_keys(void)
{
    Budget budget;
    budget_init(&budget, 8u * MIB);
    CHECK(budget_install_lexbor(&budget));
    PocDocument document = {0};
    static const char html[] =
        "<div id=host class='a outer'>"
        "<p class='btn active x'>one</p><p class='btn'>two</p>"
        "<span class='q y'>three</span><div class='q'><i class=y></i></div>"
        "<b id=only class=z></b><em class='w1 k l'></em>"
        "<em class='n'></em><em class='deep k'></em>"
        "<section class='x'><p class='y m'></p></section></div>";
    CHECK(document_parse(&document, &budget, html, sizeof(html) - 1u, 17));
    Stylesheet sheet = {0};
    CHECK(stylesheet_build(&sheet, &budget, &document, 480));
    static const char css[] =
        ":is(.btn:first-child,.btn.active){color:#000001}"
        ":where(.w1){color:#000002}"
        ":is(div.q,span.q){color:#000003}"
        ":is(.a,.b){color:#000004}"
        ":is(#only,#only.z){color:#000005}"
        ":not(.n){color:#000006}"
        "p:is(.x .y,.y){color:#000007}"
        ":is(:is(.deep.k),.k.l){color:#000008}"
        ":where(.outer :is(.y,.q)){color:#000009}"
        ":is(.x > .y.m, section .y){color:#00000a}"
        ":where(:root *){color:#00000b}";
    CHECK(stylesheet_add_css(&sheet, css, sizeof(css) - 1u));
    static const struct {
        const char *selector;
        const char *key;
        SelectorType type;
    } keys[] = {
        { ":is(.btn:first-child,.btn.active)", "btn", SELECTOR_CLASS },
        { ":where(.w1)", "w1", SELECTOR_CLASS },
        { ":is(div.q,span.q)", "q", SELECTOR_CLASS },
        { ":is(.a,.b)", NULL, SELECTOR_TAG },
        { ":is(#only,#only.z)", "only", SELECTOR_ID },
        { ":not(.n)", NULL, SELECTOR_TAG },
        { "p:is(.x .y,.y)", "p", SELECTOR_TAG },
        { ":is(:is(.deep.k),.k.l)", "k", SELECTOR_CLASS },
        { ":where(.outer :is(.y,.q))", NULL, SELECTOR_TAG },
        { ":is(.x > .y.m, section .y)", "y", SELECTOR_CLASS },
        { ":where(:root *)", NULL, SELECTOR_TAG },
    };
    for (size_t i = 0; i < sizeof(keys) / sizeof(keys[0]); i++) {
        const StyleRule *rule = find_rule(&sheet, keys[i].selector);
        CHECK(rule != NULL);
        if (keys[i].key == NULL) {
            if (rule->has_fast_key)
                fprintf(stderr, "unexpected key for %s\n", keys[i].selector);
            CHECK(!rule->has_fast_key);
            continue;
        }
        const char *key = style_rule_fast_key(rule);
        if (key == NULL || rule->type != keys[i].type
            || rule->fast_key_length != strlen(keys[i].key)
            || memcmp(key, keys[i].key, rule->fast_key_length) != 0)
            fprintf(stderr, "wrong key for %s\n", keys[i].selector);
        CHECK(key != NULL && rule->type == keys[i].type
              && rule->fast_key_length == strlen(keys[i].key)
              && memcmp(key, keys[i].key, rule->fast_key_length) == 0);
    }
    size_t matches = 0;
    for (int pass = 0; pass < 2; pass++) {
        for (size_t r = 0; r < sheet.count; r++) {
            const StyleRule *rule = &sheet.rules[r];
            for (lxb_dom_node_t *node = lxb_dom_interface_node(document.html);
                 node != NULL;) {
                if (node->type == LXB_DOM_NODE_TYPE_ELEMENT) {
                    bool sheet_match =
                        style_rule_selector_matches(&sheet, r, node);
                    bool text_match = style_selector_matches(
                        node, rule->selector, rule->selector_length);
                    if (sheet_match != text_match)
                        fprintf(stderr, "mismatch %s\n", rule->selector);
                    CHECK(sheet_match == text_match);
                    if (text_match && rule->has_fast_key)
                        CHECK(element_has_key(node, rule));
                    matches += text_match ? 1u : 0u;
                }
                if (node->first_child != NULL) {
                    node = node->first_child;
                    continue;
                }
                while (node != NULL && node->next == NULL)
                    node = node->parent;
                if (node != NULL) node = node->next;
            }
        }
    }
    CHECK(matches > 20u);
    stylesheet_destroy(&sheet);
    document_destroy(&document);
    CHECK(budget.current == 0 && budget_uninstall_lexbor(&budget));
    return 0;
}

/* A deferred (var()) declaration block resolves em lengths against an
   absolute font-size in the same block. Blocks proven to lack font-size
   skip that per-element scan; one that has it must still use it. */
static int test_deferred_font_basis(void)
{
    Budget budget;
    budget_init(&budget, 8u * MIB);
    CHECK(budget_install_lexbor(&budget));
    PocDocument document = {0};
    static const char html[] =
        "<div id=parent style='font-size:10px'>"
        "<div id=with class=f>a</div><div id=without class=g>b</div></div>";
    CHECK(document_parse(&document, &budget, html, sizeof(html) - 1u, 17));
    Stylesheet sheet = {0};
    CHECK(stylesheet_build(&sheet, &budget, &document, 480));
    static const char css[] =
        ":root{--m:2}"
        ".f{margin-top:calc(var(--m) * 1em);font-size:20px}"
        ".g{margin-top:calc(var(--m) * 1em)}";
    CHECK(stylesheet_add_css(&sheet, css, sizeof(css) - 1u));
    lxb_dom_node_t *root = lxb_dom_interface_node(document.html);
    lxb_dom_node_t *with = find_id(root, "with");
    lxb_dom_node_t *without = find_id(root, "without");
    CHECK(with != NULL && without != NULL);
    lxb_dom_node_t *parent = find_id(root, "parent");
    CHECK(parent != NULL);
    ComputedStyle parent_style = style_for_node(&sheet, parent, NULL);
    ComputedStyle with_style = style_for_node(&sheet, with, &parent_style);
    ComputedStyle without_style =
        style_for_node(&sheet, without, &parent_style);
    if (with_style.margin.top != 40 || without_style.margin.top != 20)
        fprintf(stderr, "deferred font basis: %d %d\n",
                with_style.margin.top, without_style.margin.top);
    CHECK(with_style.margin.top == 40 && without_style.margin.top == 20);
    stylesheet_destroy(&sheet);
    document_destroy(&document);
    CHECK(budget.current == 0 && budget_uninstall_lexbor(&budget));
    return 0;
}

/* Which class/id tokens can change what an inline SVG raster resolves:
   colour and font size from any matched element, width/height only through
   an ancestor, custom properties and presentation values (escaped spellings
   included), opacity only through an ancestor. */
static int test_svg_raster_token_gate(void)
{
    Budget budget;
    budget_init(&budget, 8u * MIB);
    CHECK(budget_install_lexbor(&budget));
    PocDocument document = {0};
    Stylesheet sheet = {0};
    static const char html[] = "<style>.tint{color:red}.pad{padding:1px}"
        ".big .sz{width:16px}.w{height:4px}.em{font-size:20px}"
        ".dark\\:ic{--ic:blue}.\\31 0x{fill:red}.fade{opacity:.5}"
        ".dim svg{opacity:.5}#hero{stroke:red}</style><p id=p>x</p>";
    CHECK(document_parse(&document, &budget, html, sizeof(html) - 1u, 17)
          && stylesheet_build(&sheet, &budget, &document, 480));
    (void) style_for_node(&sheet, find_id(
        lxb_dom_interface_node(document.html), "p"), NULL);
    static const struct {
        bool id;
        const char *token;
        bool affects;
    } cases[] = {
        {false, "tint", true}, {false, "pad", false}, {false, "big", true},
        {false, "sz", false}, {false, "w", false}, {false, "em", true},
        {false, "dark:ic", true}, {false, "10x", true},
        {false, "fade", false}, {false, "dim", true}, {true, "hero", true},
        {false, "hero", false}, {false, "missing", false},
    };
    /* The custom-rule token list is built once per sheet generation. */
    uint32_t missing = stylesheet_identity_token_hash(false, "missing", 7);
    CHECK(!stylesheet_tokens_may_affect_svg_raster(&sheet, &missing, 1));
    size_t before = budget.current;
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        uint32_t token = stylesheet_identity_token_hash(
            cases[i].id, cases[i].token, strlen(cases[i].token));
        bool affects = stylesheet_tokens_may_affect_svg_raster(
            &sheet, &token, 1);
        if (affects != cases[i].affects)
            fprintf(stderr, "svg raster token %s: %d\n", cases[i].token,
                    (int) affects);
        CHECK(affects == cases[i].affects);
        /* None of these rules can reveal an image. */
        CHECK(!stylesheet_tokens_may_affect_discovery(&sheet, &token, 1));
        CHECK(budget.current == before);
    }
    CHECK(sheet.svg_raster_tokens_ready && !sheet.svg_raster_tokens_opaque
          && sheet.svg_raster_token_count == 4u);
    /* An identity attribute selector cannot be listed. */
    CHECK(stylesheet_add_css(&sheet, "[class~=x]{--ic:red}", 20));
    (void) style_for_node(&sheet, find_id(
        lxb_dom_interface_node(document.html), "p"), NULL);
    uint32_t pad = stylesheet_identity_token_hash(false, "pad", 3);
    CHECK(stylesheet_tokens_may_affect_svg_raster(&sheet, &pad, 1)
          && sheet.svg_raster_tokens_opaque);
    stylesheet_destroy(&sheet);
    document_destroy(&document);
    CHECK(budget.current == 0 && budget_uninstall_lexbor(&budget));
    return 0;
}

static bool selector_references(const Stylesheet *sheet, const char *name)
{
    return stylesheet_selectors_reference_attribute_prefix(
        sheet, name, name == NULL ? 0 : strlen(name));
}

static int test_selector_attribute_names(void)
{
    Budget budget;
    budget_init(&budget, 8u * MIB);
    CHECK(budget_install_lexbor(&budget));
    PocDocument document = {0};
    Stylesheet sheet = {0};
    CHECK(!selector_references(&sheet, "data-empty-sheet"));
    static const char html[] = "<style>[data-exact]{color:red}"
        "div[DATA-Upper='x'] p{color:red}"
        ".x:is([data-in-is]){color:red}"
        ":where(.a [ data-in-where ~= v ]){color:red}"
        ":not([data-in-not]) .q{color:red}"
        ".p:has(> [data-in-has]){color:red}"
        "li:nth-child(2n of [data-in-nth]){color:red}"
        "[data-dash|=en]{color:red}"
        "[data-value-escape='a\\]b'] i{color:red}"
        "[data-long-attribute-name-well-beyond-the-journal]{color:red}"
        ".data-\\[state\\=open\\]\\:block{display:block}"
        "[title='[data-quoted]']{color:red}"
        "[data-theme]{--tone:blue}"
        "</style><p id=p>x</p>";
    CHECK(document_parse(&document, &budget, html, sizeof(html) - 1u, 17)
          && stylesheet_build(&sheet, &budget, &document, 480));
    static const struct {
        const char *name;
        bool referenced;
    } cases[] = {
        {"data-exact", true}, {"data-exac", true}, {"data-", true},
        {"data-exact-more", false}, {"data-upper", true},
        {"DATA-UPPER", true}, {"data-in-is", true}, {"data-in-where", true},
        {"data-in-not", true}, {"data-in-has", true}, {"data-in-nth", true},
        {"data-dash", true}, {"data-value-escape", true},
        /* A mutation journal keeps the first 31 bytes of a longer name. */
        {"data-long-attribute-name-well-", true}, {"data-theme", true},
        {"data-state", false}, {"data-quoted", false},
        {"data-missing", false}, {"datum", false}, {"", true},
    };
    size_t unbuilt = budget.current;
    CHECK(!sheet.selector_attribute_names_ready
          && sheet.selector_attribute_names == NULL);
    CHECK(!selector_references(&sheet, "data-missing"));
    /* Built once for this generation; later queries allocate nothing. */
    size_t built = budget.current;
    CHECK(sheet.selector_attribute_names_ready
          && !sheet.selector_attribute_names_opaque
          && sheet.selector_attribute_name_count == 12u
          && built - unbuilt >= sheet.selector_attribute_name_bytes
          && sheet.selector_attribute_name_bytes > 0);
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        bool referenced = selector_references(&sheet, cases[i].name);
        if (referenced != cases[i].referenced)
            fprintf(stderr, "selector attribute %s: %d\n", cases[i].name,
                    (int) referenced);
        CHECK(referenced == cases[i].referenced);
        CHECK(budget.current == built);
    }
    CHECK(stylesheet_selectors_reference_attribute_prefix(&sheet, NULL, 4));
    /* A new generation's rules are seen. */
    CHECK(!selector_references(&sheet, "data-late"));
    uint64_t generation = sheet.selector_attribute_generation;
    static const char late[] = "section [data-late]{color:red}";
    CHECK(stylesheet_add_css(&sheet, late, sizeof(late) - 1u)
          && selector_references(&sheet, "data-late")
          && !selector_references(&sheet, "data-missing")
          && sheet.selector_attribute_generation != generation
          && sheet.selector_attribute_generation == sheet.build_generation
          && sheet.selector_attribute_name_count == 13u);
    /* A rule-count change alone also rebuilds the list. */
    static const char custom[] = "[data-custom-only]{--x:1}";
    size_t custom_rules = sheet.custom_rule_count;
    CHECK(stylesheet_add_css(&sheet, custom, sizeof(custom) - 1u)
          && sheet.custom_rule_count > custom_rules);
    sheet.build_generation = sheet.selector_attribute_generation;
    CHECK(selector_references(&sheet, "data-custom-only"));
    stylesheet_destroy(&sheet);
    CHECK(sheet.selector_attribute_names == NULL);
    /* Scratch holds unique names only: many repeated, interleaved names
       still list, but more distinct names than the bound turn opaque. */
    for (unsigned distinct = 0; distinct < 2u; distinct++) {
        Stylesheet bounded = {0};
        CHECK(stylesheet_build(&bounded, &budget, &document, 480));
        size_t capacity = 256u * 1024u, used = 0;
        char *css = malloc(capacity);
        CHECK(css != NULL);
        for (unsigned i = 0; i < 3000u; i++) {
            int written = snprintf(css + used, capacity - used,
                ".r%u[data-%s%u]{color:red}", i,
                distinct ? "distinct-" : "repeat-", distinct ? i : i % 3u);
            CHECK(written > 0 && (size_t) written < capacity - used);
            used += (size_t) written;
        }
        CHECK(stylesheet_add_css(&bounded, css, used));
        free(css);
        CHECK(selector_references(&bounded, "data-missing") == (distinct != 0));
        CHECK(bounded.selector_attribute_names_opaque == (distinct != 0));
        CHECK(distinct || (bounded.selector_attribute_name_count == 12u + 3u
              && selector_references(&bounded, "data-repeat-2")));
        stylesheet_destroy(&bounded);
    }
    /* Names the scan cannot list make every answer conservative. */
    static const char *const opaque[] = {
        "[data\\2d esc]{color:red}", "[ns|data-ns]{color:red}",
        "[*|data-any]{color:red}", "[|data-none]{color:red}",
    };
    for (size_t i = 0; i < sizeof(opaque) / sizeof(opaque[0]); i++) {
        Stylesheet opaque_sheet = {0};
        CHECK(stylesheet_build(&opaque_sheet, &budget, &document, 480));
        CHECK(!selector_references(&opaque_sheet, "data-missing"));
        CHECK(stylesheet_add_css(&opaque_sheet, opaque[i],
                                 strlen(opaque[i])));
        bool listed = opaque_sheet.count != 0
            || opaque_sheet.custom_rule_count != 0;
        if (listed && (!selector_references(&opaque_sheet, "data-missing")
                       || !opaque_sheet.selector_attribute_names_opaque
                       || opaque_sheet.selector_attribute_names != NULL)) {
            fprintf(stderr, "opaque attribute selector %s answered no\n",
                    opaque[i]);
            return 1;
        }
        if (!listed) fprintf(stderr, "attribute selector %s not retained\n",
                             opaque[i]);
        stylesheet_destroy(&opaque_sheet);
    }
    document_destroy(&document);
    CHECK(budget.current == 0 && budget_uninstall_lexbor(&budget));
    return 0;
}

/* A ChatGPT-sized sheet: every unobserved data-* mutation record asks this
   question during relayout preparation. */
static int benchmark_selector_attribute_names(void)
{
    Budget budget;
    budget_init(&budget, 32u * MIB);
    CHECK(budget_install_lexbor(&budget));
    PocDocument document = {0};
    Stylesheet sheet = {0};
    static const char html[] = "<p id=p>x</p>";
    CHECK(document_parse(&document, &budget, html, sizeof(html) - 1u, 17)
          && stylesheet_build(&sheet, &budget, &document, 480));
    size_t capacity = 512u * 1024u, used = 0;
    char *css = malloc(capacity);
    CHECK(css != NULL);
    for (unsigned i = 0; i < 5000u; i++) {
        int written;
        if (i % 20u == 0)
            written = snprintf(css + used, capacity - used,
                ".group[data-state-%u=open] .item-%u{color:red}", i, i);
        else if (i % 20u == 1)
            written = snprintf(css + used, capacity - used,
                ".open-block-%u[data-state=open]{display:block}", i);
        else
            written = snprintf(css + used, capacity - used,
                ".card-%u .title-%u>span{padding:%upx}", i, i, i % 7u);
        CHECK(written > 0 && (size_t) written < capacity - used);
        used += (size_t) written;
    }
    CHECK(stylesheet_add_css(&sheet, css, used));
    free(css);
    CHECK(sheet.count >= 5000u);
    static const char *const names[] = {
        "data-message-id", "data-testid", "data-start", "data-end",
    };
    uint64_t started = tilefinch_platform_monotonic_time_ns();
    CHECK(!selector_references(&sheet, names[0]));
    uint64_t first = tilefinch_platform_monotonic_time_ns() - started;
    const unsigned iterations = 2000u;
    started = tilefinch_platform_monotonic_time_ns();
    for (unsigned i = 0; i < iterations; i++)
        CHECK(!selector_references(&sheet, names[i % 4u]));
    uint64_t elapsed = tilefinch_platform_monotonic_time_ns() - started;
    CHECK(selector_references(&sheet, "data-state"));
    /* Steady-state list rebuild cost, as after a later sheet append. */
    const unsigned rebuilds = 50u;
    uint64_t rebuild_started = tilefinch_platform_monotonic_time_ns();
    for (unsigned i = 0; i < rebuilds; i++) {
        sheet.selector_attribute_names_ready = false;
        CHECK(!selector_references(&sheet, names[1]));
    }
    uint64_t rebuild = (tilefinch_platform_monotonic_time_ns()
                        - rebuild_started) / rebuilds;
    printf("selector attribute names: rules=%zu first_us=%llu "
           "rebuild_us=%llu query_ns=%llu names=%u\n", sheet.count,
           (unsigned long long) (first / 1000u),
           (unsigned long long) (rebuild / 1000u),
           (unsigned long long) (elapsed / iterations),
           (unsigned) sheet.selector_attribute_name_count);
    stylesheet_destroy(&sheet);
    document_destroy(&document);
    CHECK(budget.current == 0 && budget_uninstall_lexbor(&budget));
    return 0;
}

/* Retained (unsupported-by-cascade) properties are resolved per element
   after the cascade, one custom-rule candidate at a time; each candidate's
   class key must hit the element's tokenized list, not rescan it. */
static int test_retained_properties_share_class_tokens(void)
{
    Budget budget;
    budget_init(&budget, 8u * MIB);
    CHECK(budget_install_lexbor(&budget));
    char css[4096];
    size_t used = 0;
    for (unsigned i = 0; i < 24; i++) {
        int written = snprintf(css + used, sizeof(css) - used,
            ".t%u{touch-action:none}.b%u{backdrop-filter:blur(2px)}", i, i);
        CHECK(written > 0 && (size_t) written < sizeof(css) - used);
        used += (size_t) written;
    }
    static const char body[] =
        "<p id=target class='filler-one filler-two filler-three "
        "filler-four t5 b7'>x</p>";
    char html[6144];
    int length = snprintf(html, sizeof(html),
                          "<!doctype html><style>%s</style>%s", css, body);
    CHECK(length > 0 && (size_t) length < sizeof(html));
    PocDocument document = {0};
    Stylesheet sheet = {0};
    CHECK(document_parse(&document, &budget, html, (size_t) length, 17)
          && stylesheet_build(&sheet, &budget, &document, 480));
    lxb_dom_node_t *node = find_id(lxb_dom_interface_node(document.html),
                                   "target");
    CHECK(node != NULL);
    uint64_t scans = sheet.selector_class_linear_scans;
    uint64_t builds = sheet.selector_class_token_builds;
    ComputedStyle style = style_for_node(&sheet, node, NULL);
    CHECK(style.touch_action == STYLE_TOUCH_ACTION_NONE && style.has_filter);
    /* 48 candidate keys: one tokenization, no linear rescans. */
    CHECK(sheet.selector_class_linear_scans == scans);
    CHECK(sheet.selector_class_token_builds - builds <= 1);
    /* A later resolution of the unchanged element recognizes its list by
       content instead of tokenizing it again. */
    builds = sheet.selector_class_token_builds;
    style = style_for_node(&sheet, node, NULL);
    CHECK(style.touch_action == STYLE_TOUCH_ACTION_NONE && style.has_filter);
    CHECK(sheet.selector_class_token_builds == builds);
    /* Different text in the same storage (the address and length a set is
       found by still match, as when freed attribute memory is reused) must
       not answer from the earlier scope's tokens. */
    size_t before_length = 0;
    char *before = (char *) document_attribute(node, "class", &before_length);
    static const char rewritten[] =
        "filler-one filler-two filler-three filler-four x5 x7";
    CHECK(before != NULL && before_length == sizeof(rewritten) - 1u);
    memcpy(before, rewritten, before_length);
    style = style_for_node(&sheet, node, NULL);
    CHECK(style.touch_action != STYLE_TOUCH_ACTION_NONE && !style.has_filter);
    CHECK(sheet.selector_class_token_builds == builds + 1u);
    /* And through the DOM, with new storage. */
    static const char replaced[] =
        "filler-one filler-two filler-three filler-four b5 x7";
    CHECK(lxb_dom_element_set_attribute(lxb_dom_interface_element(node),
          (const lxb_char_t *) "class", 5, (const lxb_char_t *) replaced,
          sizeof(replaced) - 1u) != NULL);
    style = style_for_node(&sheet, node, NULL);
    CHECK(style.touch_action != STYLE_TOUCH_ACTION_NONE && style.has_filter);
    static const char restored[] =
        "filler-one filler-two filler-three filler-four t9 x7";
    CHECK(lxb_dom_element_set_attribute(lxb_dom_interface_element(node),
          (const lxb_char_t *) "class", 5, (const lxb_char_t *) restored,
          sizeof(restored) - 1u) != NULL);
    style = style_for_node(&sheet, node, NULL);
    CHECK(style.touch_action == STYLE_TOUCH_ACTION_NONE && !style.has_filter);
    stylesheet_destroy(&sheet);
    document_destroy(&document);
    CHECK(budget.current == 0 && budget_uninstall_lexbor(&budget));
    return 0;
}

/* The comparison of the utility-class tests below: every field their rules
   can set, plus the generated boxes. */
static bool utility_styles_equal(const ComputedStyle *left,
                                 const ComputedStyle *right)
{
    return left->display == right->display
        && left->color == right->color
        && left->has_background == right->has_background
        && left->background == right->background
        && left->letter_spacing_fixed == right->letter_spacing_fixed
        && left->word_spacing == right->word_spacing
        && left->padding.top == right->padding.top
        && left->padding.right == right->padding.right
        && left->padding.bottom == right->padding.bottom
        && left->padding.left == right->padding.left
        && left->margin.top == right->margin.top
        && left->margin.left == right->margin.left
        && left->opacity == right->opacity
        && left->touch_action == right->touch_action
        && left->has_filter == right->has_filter
        && computed_style_user_select(left)
            == computed_style_user_select(right)
        && left->generated_content == right->generated_content
        && left->generated_text_length == right->generated_text_length
        && (left->generated_text_length == 0
            || memcmp(left->generated_text, right->generated_text,
                      left->generated_text_length) == 0);
}

static uint32_t utility_next(uint32_t *state)
{
    *state ^= *state << 13;
    *state ^= *state >> 17;
    *state ^= *state << 5;
    return *state;
}

enum { UTILITY_CLASSES = 120, UTILITY_ELEMENTS = 48 };

/* One random atomic-CSS class list of `count` tokens, `u<n>` names. */
static size_t utility_class_list(uint32_t *state, char *out, size_t size,
                                 unsigned count)
{
    size_t used = 0;
    out[0] = '\0';
    for (unsigned i = 0; i < count; i++) {
        int written = snprintf(out + used, size - used, "%su%u",
                               i == 0 ? "" : " ",
                               (unsigned) (utility_next(state)
                                           % UTILITY_CLASSES));
        if (written <= 0 || (size_t) written >= size - used) break;
        used += (size_t) written;
    }
    return used;
}

static size_t utility_elements(lxb_dom_node_t *root, lxb_dom_node_t **out,
                               size_t capacity)
{
    size_t count = 0;
    for (lxb_dom_node_t *at = root; at != NULL && count < capacity;) {
        if (at->type == LXB_DOM_NODE_TYPE_ELEMENT) out[count++] = at;
        if (at->first_child != NULL) { at = at->first_child; continue; }
        while (at != NULL && at != root && at->next == NULL) at = at->parent;
        at = at == NULL || at == root ? NULL : at->next;
    }
    return count;
}

/* Elements carrying more keyed classes than the rule-index plan's ranges
   (atomic CSS) take the merged candidate list; it must select exactly what
   the unindexed linear scan selects, through random class rewrites, and
   visit a small fraction of the rules. */
static int test_utility_class_candidates_match_linear(void)
{
    Budget budget;
    budget_init(&budget, 32u * MIB);
    CHECK(budget_install_lexbor(&budget));
    static char html[65536];
    size_t length = 0;
    int written = snprintf(html, sizeof(html),
        "<!doctype html><style>*{letter-spacing:1px}div{word-spacing:1px}"
        "#e3{padding-left:5px}");
    CHECK(written > 0);
    length = (size_t) written;
    for (unsigned i = 0; i < UTILITY_CLASSES; i++) {
        const char *declaration;
        switch (i % 12u) {
        case 0: declaration = "color:#%06x"; break;
        case 1: declaration = "padding-top:%upx"; break;
        case 2: declaration = "margin-left:%upx"; break;
        case 3: declaration = "word-spacing:%upx"; break;
        case 4: declaration = "background-color:#%06x"; break;
        case 5: declaration = "touch-action:none;--u%u:1"; break;
        case 6: declaration = "backdrop-filter:blur(%upx)"; break;
        case 7: declaration = "user-select:none;padding-right:%upx"; break;
        case 8: declaration = "display:flex;margin-top:%upx"; break;
        case 9: declaration = "opacity:0.5;padding-bottom:%upx"; break;
        case 10: declaration = "letter-spacing:%upx"; break;
        default: declaration = "color:var(--tone,#%06x)"; break;
        }
        char body[96];
        snprintf(body, sizeof(body), declaration, (i * 2654435761u) % 23u
                 + (i % 12u == 0 || i % 12u == 4 || i % 12u == 11
                    ? 0x101010u * (i % 13u) : 1u));
        written = snprintf(html + length, sizeof(html) - length,
                           ".u%u{%s}", i, body);
        CHECK(written > 0 && (size_t) written < sizeof(html) - length);
        length += (size_t) written;
    }
    written = snprintf(html + length, sizeof(html) - length,
        ".u1.u2{color:#abcdef}.u3 .u4{padding-top:9px}"
        ".u5>.u6{margin-left:7px}.u7+.u8{word-spacing:6px}"
        ".u9::before{content:'b';color:#123456}"
        ".u10::after{content:'a'}.u11.u12::after{content:'c'}"
        ".u13:has(.u14){background-color:#0f0f0f}"
        ".u15{--tone:#00ff00}.u16:not(.u17){opacity:0.25}"
        "div.u18{display:none}[data-k].u19{padding-left:3px}"
        "</style><body>");
    CHECK(written > 0);
    length += (size_t) written;
    uint32_t state = 20260929u;
    unsigned depth = 0;
    for (unsigned i = 0; i < UTILITY_ELEMENTS; i++) {
        if (depth > 0 && utility_next(&state) % 3u == 0) {
            memcpy(html + length, "</div>", 6);
            length += 6;
            depth--;
        }
        char classes[1024];
        unsigned count = utility_next(&state) % 4u == 0
            ? 1u + utility_next(&state) % 8u
            : 30u + utility_next(&state) % 50u;
        utility_class_list(&state, classes, sizeof(classes), count);
        written = snprintf(html + length, sizeof(html) - length,
                           "<div id=e%u %sclass='%s'>t%u", i,
                           i % 5u == 0 ? "data-k " : "", classes, i);
        CHECK(written > 0 && (size_t) written < sizeof(html) - length);
        length += (size_t) written;
        if (depth < 5 && utility_next(&state) % 2u == 0) {
            depth++;
        } else {
            memcpy(html + length, "</div>", 6);
            length += 6;
        }
    }
    PocDocument document = {0};
    CHECK(document_parse(&document, &budget, html, length, 19));
    Stylesheet indexed = {0}, linear = {0};
    CHECK(stylesheet_build(&indexed, &budget, &document, 480));
    CHECK(setenv("TILEFINCH_DISABLE_STYLE_INDEX", "1", 1) == 0);
    bool linear_built = stylesheet_build(&linear, &budget, &document, 480);
    CHECK(unsetenv("TILEFINCH_DISABLE_STYLE_INDEX") == 0);
    CHECK(linear_built);
    /* The reference scans class lists directly, independent of the
       class-token sets the indexed sheet keeps between resolutions. */
    linear.class_tokens_refused = true;
    lxb_dom_node_t *body = document_body_node(&document);
    CHECK(body != NULL);
    size_t compared = 0;
    for (unsigned round = 0; round < 24; round++) {
        lxb_dom_node_t *elements[UTILITY_ELEMENTS + 8];
        size_t count = utility_elements(body, elements,
                                        UTILITY_ELEMENTS + 8);
        uint64_t fallbacks = indexed.rule_index_fallbacks;
        uint64_t candidates = indexed.rule_index_candidates;
        for (size_t i = 0; i < count; i++) {
            ComputedStyle parent = {0};
            ComputedStyle fast = style_for_node(&indexed, elements[i], NULL);
            ComputedStyle slow = style_for_node(&linear, elements[i], NULL);
            parent = fast;
            if (!utility_styles_equal(&fast, &slow)) {
                printf("utility round %u element %zu diverged\n", round, i);
                CHECK(false);
            }
            for (PseudoElement pseudo = PSEUDO_BEFORE;
                 pseudo <= PSEUDO_AFTER; pseudo++) {
                ComputedStyle fast_pseudo = style_for_pseudo(
                    &indexed, elements[i], pseudo, &parent);
                ComputedStyle slow_pseudo = style_for_pseudo(
                    &linear, elements[i], pseudo, &parent);
                if (!utility_styles_equal(&fast_pseudo, &slow_pseudo)) {
                    printf("utility round %u element %zu pseudo %d "
                           "diverged\n", round, i, (int) pseudo);
                    CHECK(false);
                }
            }
            compared++;
        }
        /* Every element resolved through the index, none by the whole-
           range scan, visiting a small share of the rules. */
        CHECK(indexed.rule_index_fallbacks == fallbacks);
        CHECK(indexed.rule_index_candidates - candidates
              < (uint64_t) count * indexed.count / 2u);
        /* Rewrite a few class lists: new tokens, same-length swaps. */
        for (unsigned change = 0; change < 6; change++) {
            lxb_dom_node_t *node = elements[1u + utility_next(&state)
                                            % (count - 1u)];
            size_t old_length = 0;
            const lxb_char_t *old = lxb_dom_element_get_attribute(
                lxb_dom_interface_element(node),
                (const lxb_char_t *) "class", 5, &old_length);
            char classes[1024];
            size_t new_length;
            if (old != NULL && old_length < sizeof(classes)
                && utility_next(&state) % 2u == 0) {
                /* Same length and storage size: rotate one digit. */
                memcpy(classes, old, old_length);
                classes[old_length] = '\0';
                char *digit = strpbrk(classes + utility_next(&state)
                                      % (old_length == 0 ? 1 : old_length),
                                      "0123456789");
                if (digit != NULL) *digit = (char) ('0' + (*digit - '0' + 1) % 10);
                new_length = old_length;
                if (utility_next(&state) % 2u == 0) {
                    /* The same storage with other text, as when freed
                       attribute memory is reused: class-token sets kept
                       from the resolution just before must not answer. */
                    (void) style_for_node(&indexed, node, NULL);
                    (void) style_for_node(&linear, node, NULL);
                    memcpy((char *) old, classes, old_length);
                    ComputedStyle fast = style_for_node(&indexed, node, NULL);
                    ComputedStyle slow = style_for_node(&linear, node, NULL);
                    if (!utility_styles_equal(&fast, &slow)) {
                        printf("utility round %u rewritten element "
                               "diverged\n", round);
                        CHECK(false);
                    }
                    continue;
                }
            } else {
                unsigned tokens = utility_next(&state) % 3u == 0
                    ? 1u + utility_next(&state) % 6u
                    : 30u + utility_next(&state) % 50u;
                new_length = utility_class_list(&state, classes,
                                                sizeof(classes), tokens);
            }
            CHECK(lxb_dom_element_set_attribute(
                lxb_dom_interface_element(node),
                (const lxb_char_t *) "class", 5,
                (const lxb_char_t *) classes, new_length) != NULL);
        }
    }
    CHECK(compared > 24u * 40u);
    stylesheet_destroy(&linear);
    stylesheet_destroy(&indexed);
    document_destroy(&document);
    CHECK(budget.current == 0 && budget_uninstall_lexbor(&budget));
    return 0;
}

/* Utility CSS escapes its class names (`.w-\[20\%\]`, `.md\:flex`,
   `.\31 0x`). Those rules are indexed under the decoded class or ID their
   compound matcher requires, and `:is(.\*\*\:t-3 *)`-style rules carry the
   ancestor class they require in their ancestor filter, so an element
   tests about its own classes' rules rather than every escaped rule in the
   sheet. Every element and pseudo-element must still resolve exactly as the
   unindexed linear scan resolves it, through random class rewrites. */
enum { ESCAPED_UTILITIES = 48, ESCAPED_ELEMENTS = 72 };

static size_t escaped_utility_class_list(uint32_t *state, char *out,
                                         size_t size, unsigned count)
{
    static const char *const shapes[] = {
        "w-[%upx]", "md:p-%u", "hover:c-%u", "dark:c-%u", "1%ux", "x.y%u",
        "before:c-%u", "**:t-%u", "*:p-%u", "[&_*]:m-%u",
        "a-very-long-unescaped-utility-class-name-that-is-over-sixty-four-%u",
        "dark", "plain-%u"
    };
    size_t used = 0;
    out[0] = '\0';
    for (unsigned i = 0; i < count; i++) {
        unsigned shape = utility_next(state)
            % (unsigned) (sizeof(shapes) / sizeof(shapes[0]));
        char token[96];
        snprintf(token, sizeof(token), shapes[shape],
                 (unsigned) (utility_next(state) % ESCAPED_UTILITIES));
        int written = snprintf(out + used, size - used, "%s%s",
                               i == 0 ? "" : " ", token);
        if (written <= 0 || (size_t) written >= size - used) break;
        used += (size_t) written;
    }
    return used;
}

static int test_escaped_utility_classes_indexed(void)
{
    Budget budget;
    budget_init(&budget, 48u * MIB);
    CHECK(budget_install_lexbor(&budget));
    static char html[262144];
    size_t length = 0;
    int written = snprintf(html, sizeof(html),
        "<!doctype html><style>*{letter-spacing:1px}"
        ".a\\ b{color:#ff0000}.\\#{color:#00ff00}");
    CHECK(written > 0);
    length = (size_t) written;
    for (unsigned i = 0; i < ESCAPED_UTILITIES; i++) {
        unsigned v = (i * 2654435761u) % 23u + 1u;
        written = snprintf(html + length, sizeof(html) - length,
            ".w-\\[%upx\\]{padding-left:%upx}"
            ".md\\:p-%u{padding-top:%upx}"
            ".hover\\:c-%u:hover{color:#%06x}"
            ".dark\\:c-%u:where(.dark,.dark *){color:#%06x}"
            ".\\31 %ux{margin-left:%upx}"
            ".x\\.y%u{word-spacing:%upx}"
            "#id\\:%u{padding-bottom:%upx}"
            ".before\\:c-%u::before{content:'b%u';color:#%06x}"
            ":is(.\\*\\*\\:t-%u *){letter-spacing:%upx}"
            ":is(.\\*\\:p-%u > *){padding-right:%upx}"
            ".\\[\\&_\\*\\]\\:m-%u *{margin-top:%upx}"
            ".a-very-long-unescaped-utility-class-name-that-is-over-sixty-"
            "four-%u{opacity:0.5}"
            ".plain-%u{display:flex}",
            i, v, i, v + 1u, i, 0x010101u * v, i, 0x020202u * v, i, v,
            i, v, i, v, i, i, 0x030303u * v, i, v, i, v, i, v, i, i);
        CHECK(written > 0 && (size_t) written < sizeof(html) - length);
        length += (size_t) written;
    }
    written = snprintf(html + length, sizeof(html) - length,
                       "</style><body>");
    CHECK(written > 0);
    length += (size_t) written;
    uint32_t state = 20261006u;
    unsigned depth = 0;
    for (unsigned i = 0; i < ESCAPED_ELEMENTS; i++) {
        if (depth > 0 && utility_next(&state) % 3u == 0) {
            memcpy(html + length, "</div>", 6);
            length += 6;
            depth--;
        }
        char classes[2048];
        escaped_utility_class_list(&state, classes, sizeof(classes),
                                   1u + utility_next(&state) % 12u);
        written = snprintf(html + length, sizeof(html) - length,
                           "<div id='id:%u' class='%s'>t%u",
                           (unsigned) (utility_next(&state)
                                       % ESCAPED_UTILITIES),
                           classes, i);
        CHECK(written > 0 && (size_t) written < sizeof(html) - length);
        length += (size_t) written;
        if (depth < 8 && utility_next(&state) % 2u == 0) {
            depth++;
        } else {
            memcpy(html + length, "</div>", 6);
            length += 6;
        }
    }
    PocDocument document = {0};
    CHECK(document_parse(&document, &budget, html, length, 19));
    Stylesheet indexed = {0}, linear = {0};
    CHECK(stylesheet_build(&indexed, &budget, &document, 480));
    CHECK(setenv("TILEFINCH_DISABLE_STYLE_INDEX", "1", 1) == 0);
    bool linear_built = stylesheet_build(&linear, &budget, &document, 480);
    CHECK(unsetenv("TILEFINCH_DISABLE_STYLE_INDEX") == 0);
    CHECK(linear_built);
    linear.class_tokens_refused = true;
    /* The escaped and long class/ID rules are indexed under their decoded
       keys (eight per utility); only the two :is() forms and the
       descendant-of-utility rule, whose subject is `*`, stay universal.
       (`.\31 0x` is split at its escape's terminating space by the
       existing combinator scan, string matcher included, so it keeps the
       tag key that split gives it.) */
    stylesheet_prepare_rule_index(&indexed);
    CHECK(indexed.rule_index_ready);
    CHECK(indexed.rule_index_derived_keys >= 8u * ESCAPED_UTILITIES);
    CHECK(indexed.rule_index_universal_count <= 3u * ESCAPED_UTILITIES + 4u);
    const StyleRule *ancestor_rule = find_rule(&indexed, ":is(.\\*\\*\\:t-3 *)");
    CHECK(ancestor_rule != NULL && indexed.rule_filters != NULL);
    StyleTokenBloom required = style_compound_token_bloom(
        STYLE_SELECTOR_CLASS, "**:t-3", 6);
    CHECK(!style_token_bloom_missing(
        required, indexed.rule_filters[ancestor_rule - indexed.rules]
                      .ancestors));
    /* Universal-range rules also carry an exact ancestor token; more than
       63 distinct tokens share mask bits. */
    CHECK(indexed.rule_filters[ancestor_rule - indexed.rules].ancestor_token
          != 0 && indexed.rule_ancestor_tokens != NULL
          && indexed.rule_ancestor_tokens->count
             > STYLE_RULE_ANCESTOR_TOKEN_BITS);
    const StyleRule *program_rule = find_rule(&indexed, ".\\[\\&_\\*\\]\\:m-3 *");
    CHECK(program_rule != NULL
          && indexed.rule_filters[program_rule - indexed.rules].ancestor_token
             != 0);
    lxb_dom_node_t *body = document_body_node(&document);
    CHECK(body != NULL);
    StyleAncestorBloomCache ancestors;
    size_t compared = 0;
    for (unsigned round = 0; round < 12; round++) {
        lxb_dom_node_t *elements[ESCAPED_ELEMENTS + 8];
        size_t count = utility_elements(body, elements,
                                        ESCAPED_ELEMENTS + 8);
        uint64_t candidates = indexed.rule_index_candidates;
        uint64_t rejected = indexed.rule_ancestor_filter_rejections;
        CHECK(style_selector_cooperation_begin(
            &indexed, cache_test_cooperate, NULL, &ancestors));
        for (size_t i = 0; i < count; i++) {
            ComputedStyle fast = style_for_node(&indexed, elements[i], NULL);
            ComputedStyle slow = style_for_node(&linear, elements[i], NULL);
            if (!utility_styles_equal(&fast, &slow)
                || fast.margin.top != slow.margin.top
                || fast.margin.bottom != slow.margin.bottom) {
                printf("escaped round %u element %zu diverged\n", round, i);
                CHECK(false);
            }
            for (PseudoElement pseudo = PSEUDO_BEFORE;
                 pseudo <= PSEUDO_AFTER; pseudo++) {
                ComputedStyle fast_pseudo = style_for_pseudo(
                    &indexed, elements[i], pseudo, &fast);
                ComputedStyle slow_pseudo = style_for_pseudo(
                    &linear, elements[i], pseudo, &fast);
                if (!utility_styles_equal(&fast_pseudo, &slow_pseudo)) {
                    printf("escaped round %u element %zu pseudo %d "
                           "diverged\n", round, i, (int) pseudo);
                    CHECK(false);
                }
            }
            compared++;
        }
        style_selector_cooperation_end(&indexed);
        /* Each resolution (an element and its two pseudo-elements) sees
           the universal range (the three subject-free rules per utility)
           plus its own classes' few rules, not the escaped utilities, which
           used to be candidates for every element (more than five hundred
           of the sheet's six hundred rules). */
        uint64_t used = indexed.rule_index_candidates - candidates;
        if (used >= (uint64_t) count * 3u * (3u * ESCAPED_UTILITIES + 24u)) {
            printf("escaped round %u: %llu candidates for %zu elements "
                   "over %zu rules\n", round, (unsigned long long) used,
                   count, indexed.count);
            CHECK(false);
        }
        CHECK(indexed.rule_ancestor_filter_rejections > rejected);
        for (unsigned change = 0; change < 8; change++) {
            lxb_dom_node_t *node = elements[1u + utility_next(&state)
                                            % (count - 1u)];
            char classes[2048];
            size_t new_length = escaped_utility_class_list(
                &state, classes, sizeof(classes),
                1u + utility_next(&state) % 12u);
            CHECK(lxb_dom_element_set_attribute(
                lxb_dom_interface_element(node),
                (const lxb_char_t *) "class", 5,
                (const lxb_char_t *) classes, new_length) != NULL);
        }
    }
    CHECK(compared > 12u * 60u);
    stylesheet_destroy(&linear);
    stylesheet_destroy(&indexed);
    document_destroy(&document);
    CHECK(budget.current == 0 && budget_uninstall_lexbor(&budget));
    return 0;
}

/* Prepared query lists keep the rightmost compound's first attribute: an
   element without it is rejected before the text matcher, and `[name]`
   alone is answered by the attribute. Random selectors over a random tree
   (HTML and SVG attributes, quoted values, escapes, functional arguments)
   must agree with the unprepared matcher on every element. */
static int test_query_attribute_keys_match_direct(void)
{
    Budget budget;
    budget_init(&budget, 16u * MIB);
    CHECK(budget_install_lexbor(&budget));
    static const char *const names[] = {
        "data-a", "data-b", "data-role", "title", "hidden", "name"
    };
    static const char *const values[] = {"x", "y", "X", "a b", ""};
    static char html[32768];
    size_t length = 0;
    uint32_t state = 7160929u;
    int written = snprintf(html, sizeof(html), "<!doctype html><body>");
    CHECK(written > 0);
    length = (size_t) written;
    unsigned depth = 0;
    for (unsigned i = 0; i < 90; i++) {
        if (depth > 0 && utility_next(&state) % 3u == 0) {
            memcpy(html + length, "</div>", 6);
            length += 6;
            depth--;
        }
        char attributes[256];
        size_t used = 0;
        attributes[0] = '\0';
        for (unsigned a = 0; a < 6; a++) {
            if (utility_next(&state) % 3u != 0) continue;
            written = snprintf(attributes + used, sizeof(attributes) - used,
                " %s='%s'", names[a],
                values[utility_next(&state) % (sizeof(values)
                                               / sizeof(values[0]))]);
            if (written > 0 && (size_t) written < sizeof(attributes) - used)
                used += (size_t) written;
        }
        if (i % 17u == 5u) {
            written = snprintf(html + length, sizeof(html) - length,
                "<svg viewBox='0 0 1 1'%s><rect data-a=x></rect></svg>",
                attributes);
        } else {
            written = snprintf(html + length, sizeof(html) - length,
                               "<div id=q%u%s>t", i, attributes);
            if (depth < 5 && utility_next(&state) % 2u == 0) depth++;
            else {
                CHECK(written > 0);
                length += (size_t) written;
                written = snprintf(html + length, sizeof(html) - length,
                                   "</div>");
            }
        }
        CHECK(written > 0 && (size_t) written < sizeof(html) - length);
        length += (size_t) written;
    }
    PocDocument document = {0};
    CHECK(document_parse(&document, &budget, html, length, 23));
    static const char *const selectors[] = {
        "[data-a]", "[DATA-A]", "[ data-a ]", "[data-a=x]", "[data-a='x']",
        "[data-a=\"X\" i]", "[data-a~=a]", "[title^=a]", "[data\\-a]",
        "div[data-b]", "[data-a][data-b]", "[data-a] [data-b]",
        "[data-a] > [data-role]", "#q3 [data-a]", ":not([data-a])",
        ":is([data-a],[data-b])", "[data-a]:not([hidden])", "[viewBox]",
        "[viewbox]", "svg[viewBox] rect", "[data-a], [name]", "[*|data-a]",
        "[data-a|x]", "[data-missing]", "[hidden] ~ [data-b]",
        "div:has([data-role]) [title]", "[name=''], .none", "[x-y_z]",
        "rect[data-a=x]", "[data-role='a b'] + div[data-a]",
        "div[data-b=\"]\"]", "[data-a", "div:is(.c) [data-b]",
        "[data-role] :is([data-a], [data-b=x])", ":where([title^=a], [name])",
        ":is([data-a], .c)", ":is([DATA-A],[data-b])",
        "div:is( [data-a] , [name] )", ":is([data-a], :is([data-b]))",
        ":not(:is([data-a]))", ":is(div [data-a], [data-b])",
        ":is([data-a]):not([data-b])"
    };
    lxb_dom_node_t *root = lxb_dom_interface_node(document.html);
    lxb_dom_node_t *elements[256];
    size_t count = utility_elements(root, elements, 256);
    CHECK(count > 60u);
    size_t matched = 0;
    for (size_t p = 0; p < sizeof(selectors) / sizeof(selectors[0]); p++) {
        StyleQuerySelectorList list;
        size_t selector_length = strlen(selectors[p]);
        CHECK(style_query_selector_list_prepare(&list, selectors[p],
                                                selector_length));
        for (size_t i = 0; i < count; i++) {
            bool direct = false;
            for (size_t item = 0; item < list.count; item++) {
                direct = direct || style_selector_matches_scoped(
                    elements[i], list.items[item].text,
                    list.items[item].length, NULL);
            }
            bool prepared = style_query_selector_list_matches(
                &list, elements[i], NULL);
            if (direct != prepared) {
                fprintf(stderr, "query attribute key mismatch: %s\n",
                        selectors[p]);
                CHECK(false);
            }
            matched += prepared ? 1u : 0u;
        }
    }
    CHECK(matched > 100u);
    /* Which attribute each prepared selector keeps. */
    struct { const char *selector, *attribute; bool complete; } keys[] = {
        {"[data-a]", "data-a", true}, {"[DATA-A]", "data-a", true},
        {"[ data-a ]", "data-a", false}, {"[data-a=x]", "data-a", false},
        {"div[data-b]", "data-b", false},
        {"[data-a] [data-b]", "data-b", false},
        {"[viewBox]", "viewbox", true}, {"[data\\-a]", "", false},
        {":not([data-a])", "", false}, {":is([data-a])", "", false},
        {"[*|data-a]", "", false}, {"[data-a|x]", "data-a", false},
        {"div", "", false}
    };
    for (size_t k = 0; k < sizeof(keys) / sizeof(keys[0]); k++) {
        StyleQuerySelectorList list;
        CHECK(style_query_selector_list_prepare(
            &list, keys[k].selector, strlen(keys[k].selector)));
        CHECK(list.count == 1u);
        CHECK(list.items[0].attribute_length == strlen(keys[k].attribute)
              && memcmp(list.items[0].attribute, keys[k].attribute,
                        list.items[0].attribute_length) == 0
              && list.items[0].complete_attribute == keys[k].complete);
    }
    /* Which attributes a rightmost :is()/:where() without a top-level test
       requires one of: every argument's own rightmost attribute, as the
       text spells it in lower case; otherwise no filter. */
    struct { const char *selector; const char *names[4]; } any[] = {
        {"[data-message-role=\"assistant\"] :is([data-a], "
         "[data-b-c], [data-d=x])", {"data-a", "data-b-c", "data-d"}},
        {":where([title^=a], [name])", {"title", "name"}},
        {"div:is( [data-a] , .x [name] )", {"data-a", "name"}},
        {":is([data-a], .c)", {NULL}},
        {":is([DATA-A],[data-b])", {NULL}},
        {":not([data-a], [data-b])", {NULL}},
        {":is([data-a], :is([data-b]))", {NULL}},
        {"[data-a]:is([data-b])", {NULL}},
        {":is([a],[b],[c],[d],[e])", {NULL}},
        {":is([data\\-a], [b])", {NULL}},
    };
    for (size_t k = 0; k < sizeof(any) / sizeof(any[0]); k++) {
        StyleQuerySelectorList list;
        CHECK(style_query_selector_list_prepare(
            &list, any[k].selector, strlen(any[k].selector)));
        CHECK(list.count == 1u);
        const StyleQuerySelector *item = &list.items[0];
        size_t expected = 0;
        while (expected < 4 && any[k].names[expected] != NULL) expected++;
        if (item->any_attribute_count != expected) {
            fprintf(stderr, "any-attribute count %u for %s\n",
                    (unsigned) item->any_attribute_count, any[k].selector);
            CHECK(false);
        }
        for (size_t a = 0; a < expected; a++) {
            CHECK(item->any_attribute_length[a]
                      == strlen(any[k].names[a])
                  && memcmp(item->text + item->any_attribute_offset[a],
                            any[k].names[a],
                            item->any_attribute_length[a]) == 0);
        }
    }
    document_destroy(&document);
    CHECK(budget.current == 0 && budget_uninstall_lexbor(&budget));
    return 0;
}

/* A hostile selector nests :is() as deep as a prepared selector may be long
   (64 KiB). Preparing it must neither recurse per level nor rescan the
   tail per level: it runs on a 256 KiB stack (a PSP thread's order of
   size), where one frame per nesting level overflowed. */
typedef struct {
    char *text;
    size_t length;
    bool prepared;
    StyleQuerySelectorList list;
} DeepSelectorJob;

static void *prepare_deep_selector(void *argument)
{
    DeepSelectorJob *job = argument;
    job->prepared = style_query_selector_list_prepare(&job->list, job->text,
                                                      job->length);
    return NULL;
}

static int prepare_on_small_stack(DeepSelectorJob *job)
{
    pthread_attr_t attributes;
    pthread_t thread;
    CHECK(pthread_attr_init(&attributes) == 0);
    CHECK(pthread_attr_setstacksize(&attributes, 256u * 1024u) == 0);
    CHECK(pthread_create(&thread, &attributes, prepare_deep_selector, job)
          == 0);
    CHECK(pthread_join(thread, NULL) == 0);
    pthread_attr_destroy(&attributes);
    return 0;
}

static int test_query_deeply_nested_is_prepares_flat(void)
{
    static const char open[] = ":is(";
    const size_t levels = 13000u;
    size_t capacity = levels * 6u + 64u;
    char *text = malloc(capacity);
    CHECK(text != NULL);
    size_t length = 0;
    for (size_t i = 0; i < levels; i++) {
        memcpy(text + length, open, 4);
        length += 4;
    }
    memcpy(text + length, "[data-a]", 8);
    length += 8;
    for (size_t i = 0; i < levels; i++) text[length++] = ')';
    CHECK(length <= UINT16_MAX);
    DeepSelectorJob job = {.text = text, .length = length};
    CHECK(prepare_on_small_stack(&job) == 0);
    CHECK(job.prepared && job.list.count == 1u);
    /* A nested argument keeps no filter, however deep. */
    CHECK(job.list.items[0].any_attribute_count == 0);
    CHECK(job.list.items[0].attribute_length == 0);
    /* Wide rather than deep: many arguments, each an :is() of one
       attribute, still yields no filter (and stays linear). */
    length = 0;
    memcpy(text, ":is(", 4);
    length = 4;
    for (size_t i = 0; i < 4000u; i++) {
        memcpy(text + length, i == 0 ? ":is([a])" : ",:is([a])",
               i == 0 ? 8u : 9u);
        length += i == 0 ? 8u : 9u;
    }
    text[length++] = ')';
    job = (DeepSelectorJob) {.text = text, .length = length};
    CHECK(prepare_on_small_stack(&job) == 0);
    CHECK(job.prepared && job.list.count == 1u
          && job.list.items[0].any_attribute_count == 0);
    free(text);
    return 0;
}

static int test_class_token_byte_boundaries(void)
{
    /* Explicit lengths include embedded NUL and non-ASCII bytes. Pin both
       sides of a token, prefixes/suffixes, and the uncached/cache parity. */
    Budget budget;
    budget_init(&budget, 4u * MIB);
    Stylesheet sheet = {0};
    StyleResolveScratch scratch = {0};
    sheet.budget = &budget;
    sheet.resolve_scratch = &scratch;
    scratch.class_tokens_depth = 1;
    char storage[68];
    for (size_t alignment = 0; alignment < 4; alignment++) {
        char *classes = storage + alignment;
        for (size_t boundary = 52; boundary < 56; boundary++) {
            memset(classes, 'x', 64);
            for (unsigned byte = 0; byte < 256; byte++) {
                classes[boundary] = (char) byte;
                memcpy(classes + boundary + 1, "wanted", 6);
                classes[boundary + 7] = (char) byte;
                bool expected = isspace((unsigned char) byte) != 0;
                CHECK(class_contains_length(classes, 64, "wanted", 6)
                      == expected);
                CHECK(!class_contains_length(classes, 64, "want", 4));
                CHECK(!class_contains_length(classes, 64, "anted", 5));
                StyleMatchSubject subject = {0};
                subject.classes = classes;
                subject.classes_length = 64;
                if (sheet.class_tokens != NULL)
                    for (size_t i = 0; i < STYLE_CLASS_TOKEN_SETS; i++)
                        sheet.class_tokens->sets[i].source = NULL;
                CHECK(style_subject_has_class(&sheet, &subject, "wanted", 6)
                      == expected);
            }
        }
    }
    CHECK(class_contains_length("wanted", 6, "wanted", 6));
    CHECK(!class_contains_length("wanted", 5, "wanted", 6));
    CHECK(!class_contains_length("wanted other", 12, "wanted other", 12));
    CHECK(!class_contains_length("", 0, "wanted", 6));
    budget_free(&budget, sheet.class_tokens);
    CHECK(budget.current == 0);
    return 0;
}

static int test_full_selector_class_token_reuse(void)
{
    Budget budget;
    budget_init(&budget, 4u * MIB);
    CHECK(budget_install_lexbor(&budget));
    PocDocument document = {0};
    Stylesheet sheet = {0};
    static const char html[] =
        "<!doctype html><style>.wanted.other{color:red}"
        ":is(.missing,.wanted).other{background-color:blue}</style>"
        "<p id=target class='filler-one filler-two filler-three filler-four "
        "filler-five filler-six filler-seven filler-eight wanted other'>x</p>";
    CHECK(document_parse(&document, &budget, html, sizeof(html) - 1, 17)
          && stylesheet_build(&sheet, &budget, &document, 480));
    stylesheet_prepare_selector_program(&sheet);
    lxb_dom_node_t *node = find_id(lxb_dom_interface_node(document.html), "target");
    const StyleRule *rule = find_rule(&sheet, ".wanted.other");
    CHECK(node != NULL && rule != NULL && sheet.selector_program_ready
          && sheet.resolve_scratch != NULL && sheet.class_tokens == NULL);
    /* Matchers share the existing immutable-cascade scope. The candidate
       filter is deliberately bypassed, so it cannot populate the cache on
       behalf of an unoptimized full matcher. */
    sheet.resolve_scratch->class_tokens_depth = 1;
    uint64_t started = tilefinch_platform_monotonic_time_ns();
    for (unsigned i = 0; i < 20000; i++)
        CHECK(style_rule_selector_matches(&sheet, (size_t) (rule - sheet.rules), node));
    printf("compiled class matching: us=%llu\n", (unsigned long long)
           ((tilefinch_platform_monotonic_time_ns() - started) / 1000u));
    CHECK(sheet.class_tokens != NULL);
    for (size_t i = 0; i < STYLE_CLASS_TOKEN_SETS; i++)
        sheet.class_tokens->sets[i].source = NULL;
    static const char functional[] = ":is(.missing,.wanted).other";
    CHECK(style_selector_matches_profiled(&sheet, node, functional,
                                          sizeof(functional) - 1));
    bool populated = false;
    for (size_t i = 0; i < STYLE_CLASS_TOKEN_SETS; i++)
        populated |= sheet.class_tokens->sets[i].source != NULL;
    CHECK(populated);
    sheet.resolve_scratch->class_tokens_depth = 0;
    /* No cached text identity may survive a mutation between cascades. */
    CHECK(lxb_dom_element_set_attribute(lxb_dom_interface_element(node),
          (const lxb_char_t *) "class", 5,
          (const lxb_char_t *) "other", 5) != NULL);
    ComputedStyle style = style_for_node(&sheet, node, NULL);
    CHECK(style.color != 0xff0000);
    stylesheet_destroy(&sheet);
    document_destroy(&document);
    CHECK(budget.current == 0 && budget_uninstall_lexbor(&budget));
    return 0;
}

/* Long class lists answer class tests from a token table; a one-bit
   quick filter (length and end bytes) rejects absent names before
   hashing them. It must never reject a present one. */
static int test_class_token_quick_filter(void)
{
    Budget budget;
    budget_init(&budget, 4u * MIB);
    CHECK(budget_install_lexbor(&budget));
    static const char html[] =
        "<!doctype html><p id=target class='container__item "
        "container__item--type-media-image container_lead-plus-headlines__item "
        "a b9 card-x zz'>x</p>";
    PocDocument document = {0};
    Stylesheet sheet = {0};
    CHECK(document_parse(&document, &budget, html, sizeof(html) - 1, 18)
          && stylesheet_build(&sheet, &budget, &document, 480));
    lxb_dom_node_t *node = find_id(lxb_dom_interface_node(document.html),
                                   "target");
    CHECK(node != NULL);
    StyleMatchSubject subject;
    style_match_subject_prepare(node, &subject);
    static const char *const present[] = {
        "container__item", "container__item--type-media-image",
        "container_lead-plus-headlines__item", "a", "b9", "card-x", "zz"
    };
    static const char *const absent[] = {
        "container__items", "container__ite", "b", "card-y", "z",
        "container_lead-plus-headlines__iten", "A", "card"
    };
    sheet.resolve_scratch->class_tokens_depth = 1;
    for (size_t i = 0; i < sizeof(present) / sizeof(present[0]); i++)
        CHECK(style_subject_has_class(&sheet, &subject, present[i],
                                      strlen(present[i])));
    for (size_t i = 0; i < sizeof(absent) / sizeof(absent[0]); i++)
        CHECK(!style_subject_has_class(&sheet, &subject, absent[i],
                                       strlen(absent[i])));
    sheet.resolve_scratch->class_tokens_depth = 0;
    stylesheet_destroy(&sheet);
    document_destroy(&document);
    CHECK(budget.current == 0 && budget_uninstall_lexbor(&budget));
    return 0;
}

/* Compiled descendant and child walks reuse the subjects (tag, id,
   classes) of the ancestors they visit within one resolution scope. Every
   element of a nested document must match exactly as it does with the
   walk preparing each subject afresh (outside any scope). */
static int test_walk_subject_cache_matches_fresh(void)
{
    Budget budget;
    budget_init(&budget, 8u * MIB);
    CHECK(budget_install_lexbor(&budget));
    char html[32768];
    size_t used = (size_t) snprintf(html, sizeof(html),
        "<!doctype html><style>.zone .stack>.card .title{color:red}"
        ".zone>.stack .card>.title{background-color:blue}"
        "section .card~.card .title{border-top:1px solid red}"
        "#main .zone .card .title span{color:green}</style>"
        "<section id=main>");
    for (int z = 0; z < 6; z++) {
        used += (size_t) snprintf(html + used, sizeof(html) - used,
            "<div class='zone z%d zone--with-a-long-modifier zone__wrapper'>"
            "<div class='stack s%d stack--with-a-long-modifier stack__items'>",
            z, z);
        for (int c = 0; c < 8; c++)
            used += (size_t) snprintf(html + used, sizeof(html) - used,
                "<div class='card c%d card--with-a-long-modifier container__item'>"
                "<p class='title t%d title--with-a-long-modifier headline'>"
                "<span>x</span></p></div>", c, c);
        used += (size_t) snprintf(html + used, sizeof(html) - used,
                                  "</div></div>");
    }
    used += (size_t) snprintf(html + used, sizeof(html) - used, "</section>");
    PocDocument document = {0};
    Stylesheet sheet = {0};
    CHECK(document_parse(&document, &budget, html, used, 21)
          && stylesheet_build(&sheet, &budget, &document, 480));
    stylesheet_prepare_rule_index(&sheet);
    CHECK(sheet.selector_program_ready
          && sheet.selector_program_rule_count == sheet.count);
    size_t compared = 0, matched = 0;
    lxb_dom_node_t *root = lxb_dom_interface_node(document.html);
    for (lxb_dom_node_t *node = root; node != NULL;) {
        if (node->type == LXB_DOM_NODE_TYPE_ELEMENT) {
            for (size_t rule = 0; rule < sheet.count; rule++) {
                bool fresh = style_rule_selector_matches(&sheet, rule, node);
                style_class_tokens_scope_begin(&sheet);
                bool cached = style_rule_selector_matches(&sheet, rule, node);
                bool again = style_rule_selector_matches(&sheet, rule, node);
                style_class_tokens_scope_end(&sheet);
                CHECK(fresh == cached && cached == again);
                compared++;
                matched += fresh ? 1u : 0u;
            }
        }
        if (node->first_child != NULL) { node = node->first_child; continue; }
        while (node != root && node->next == NULL) node = node->parent;
        if (node == root) break;
        node = node->next;
    }
    CHECK(compared > 500u && matched > 50u && sheet.class_tokens != NULL);
    stylesheet_destroy(&sheet);
    document_destroy(&document);
    CHECK(budget.current == 0 && budget_uninstall_lexbor(&budget));
    return 0;
}

/* A front page's sheet (CNN home: 11,700 rules, about 65,000 selector
   instructions) must compile whole. The program was capped at 256 KiB
   (59,678 instructions beside that many rule offsets), so the last 650 rules
   in document order fell back to text matching, 37% of all selector calls
   on that page. The cap now covers what the 16-bit offsets can address. */
static int test_large_sheet_compiles_every_rule(void)
{
    Budget budget;
    budget_init(&budget, 32u * MIB);
    CHECK(budget_install_lexbor(&budget));
    size_t capacity = 1400u * 1024u;
    char *html = malloc(capacity);
    CHECK(html != NULL);
    size_t used = (size_t) snprintf(html, capacity, "<!doctype html><style>");
    for (unsigned i = 0; i < 7800u && used < capacity - 200u; i++) {
        used += (size_t) snprintf(
            html + used, capacity - used,
            ".zone-%u .stack-%u>.card-%u .title-%u{color:#123456}",
            i, i, i, i);
    }
    used += (size_t) snprintf(html + used, capacity - used,
                              "</style><p class=card-1>x</p>");
    PocDocument document = {0};
    Stylesheet sheet = {0};
    CHECK(document_parse(&document, &budget, html, used, 19)
          && stylesheet_build(&sheet, &budget, &document, 480));
    stylesheet_prepare_selector_program(&sheet);
    printf("large sheet program: rules=%zu/%zu instructions=%zu bytes=%zu\n",
           sheet.selector_program_rule_count, sheet.count,
           sheet.selector_program_instruction_count,
           sheet.selector_program_bytes);
    /* More than the former 256 KiB cap held beside this many offsets. */
    size_t former_cap = (256u * 1024u - sheet.count * sizeof(uint16_t))
        / sizeof(StyleSelectorInstruction);
    CHECK(sheet.count == 7800u && sheet.selector_program_ready
          && sheet.selector_program_instruction_count > former_cap
          && sheet.selector_program_rule_count == sheet.count
          && sheet.selector_program_bytes <= 320u * 1024u);
    stylesheet_destroy(&sheet);
    document_destroy(&document);
    free(html);
    CHECK(budget.current == 0 && budget_uninstall_lexbor(&budget));
    return 0;
}


/* Repeated selector bytes share instructions, not cascade state. The old
   per-rule programs exhaust the instruction ceiling on this exact input. */
static int test_shared_selector_programs(void)
{
    Budget budget;
    budget_init(&budget, 32u * MIB);
    CHECK(budget_install_lexbor(&budget));
    const size_t capacity = 1024u * 1024u;
    char *html = malloc(capacity);
    CHECK(html != NULL);
    size_t used = (size_t) snprintf(html, capacity, "<!doctype html><style>");
    for (unsigned pass = 0; pass < 3; pass++) {
        for (unsigned i = 0; i < 3000; i++) {
            CHECK(used < capacity - 128u);
            used += (size_t) snprintf(html + used, capacity - used,
                ".zone-%u .stack-%u>span.item-%u{color:#345678;margin-top:%upx}",
                i, i, i, pass);
        }
    }
    used += (size_t) snprintf(html + used, capacity - used,
        ".zone-7 .stack-7>span.item-7{padding-left:9px!important}"
        "</style><body class=zone-7><div class=stack-7>"
        "<span id=target class=item-7>target</span>"
        "<span id=other class=item-8>other</span></div>");
    CHECK(used < capacity);
    for (size_t fault = 0; fault < 5; fault++) {
        PocDocument document = {0};
        Stylesheet sheet = {0};
        CHECK(document_parse(&document, &budget, html, used, 31)
            && stylesheet_build(&sheet, &budget, &document, 480));
        /* Offsets, optional interning, and final program all refuse cleanly. */
        if (fault != 0) budget_inject_failure_after(&budget, fault - 1u);
        stylesheet_prepare_selector_program(&sheet);
        budget_clear_failure_injection(&budget);
        if (fault == 0) {
            CHECK(sheet.count == 9001u && sheet.selector_program_ready
                && sheet.selector_program_rule_count == sheet.count
                && sheet.selector_program_instruction_count < 40000u);
            lxb_dom_node_t *target = find_id(
                lxb_dom_interface_node(document.html), "target");
            lxb_dom_node_t *other = find_id(
                lxb_dom_interface_node(document.html), "other");
            CHECK(target != NULL && other != NULL);
            for (size_t i = 0; i < sheet.count; i++) {
                CHECK(style_rule_selector_matches(&sheet, i, target)
                    == style_selector_matches_profiled(&sheet, target,
                        sheet.rules[i].selector, sheet.rules[i].selector_length));
                CHECK(style_rule_selector_matches(&sheet, i, other)
                    == style_selector_matches_profiled(&sheet, other,
                        sheet.rules[i].selector, sheet.rules[i].selector_length));
            }
        }
        lxb_dom_node_t *target = find_id(
            lxb_dom_interface_node(document.html), "target");
        ComputedStyle style = style_for_node(&sheet, target, NULL);
        CHECK(style.color == 0x345678 && style.margin.top == 2
            && style.padding.left == 9);
        stylesheet_destroy(&sheet);
        document_destroy(&document);
        CHECK(budget.current == 0);
    }
    free(html);
    CHECK(budget_uninstall_lexbor(&budget));
    return 0;
}

/* A sparse key space and one cold dependency per rule must not reserve
   empty bucket payloads or eight dependency hashes for every rule. */
static int test_sparse_selector_metadata(void)
{
    enum { RULES = 4096, CAPACITY = RULES * 64 };
    char *css = malloc(CAPACITY);
    CHECK(css != NULL);
    size_t used = 0;
    for (unsigned i = 0; i < RULES; i++) {
        int written = snprintf(css + used, CAPACITY - used,
            ".parent-%u .subject-%u{color:#123456}", i, i);
        CHECK(written > 0 && (size_t) written < CAPACITY - used);
        used += (size_t) written;
    }
    Budget budget;
    budget_init(&budget, 32u * MIB);
    CHECK(budget_install_lexbor(&budget));
    PocDocument document = {0};
    Stylesheet sheet = {0};
    static const char html[] =
        "<div class=parent-4095><p id=target class=subject-4095>x</p></div>";
    CHECK(document_parse(&document, &budget, html, sizeof(html) - 1u, 17)
          && stylesheet_build(&sheet, &budget, &document, 480)
          && stylesheet_add_css(&sheet, css, used));
    stylesheet_prepare_rule_index(&sheet);
    CHECK(sheet.count == RULES && sheet.rule_index_ready
          && sheet.rule_filters != NULL);
    printf("sparse selector metadata: rules=%zu slots=%zu bytes=%zu\n",
           sheet.count, sheet.rule_index_bucket_count, sheet.rule_index_bytes);
    CHECK(sheet.rule_index_bytes < 270u * 1024u);
    /* Includes collisions and both ends of each growth boundary. */
    for (unsigned i = 0; i < RULES; i++) {
        char key[32];
        int length = snprintf(key, sizeof(key), "subject-%u", i);
        StyleRuleIndexBucket *bucket = style_rule_find_bucket(
            &sheet, SELECTOR_CLASS, key, (size_t) length, false, PSEUDO_NONE);
        CHECK(bucket != NULL && bucket->count == 1 && bucket->unscoped == 1
              && bucket->representative < sheet.count
              && sheet.rule_index_entries[bucket->first]
                 == bucket->representative);
        length = snprintf(key, sizeof(key), "parent-%u", i);
        uint32_t token = stylesheet_identity_token_hash(
            false, key, (size_t) length), affected[2];
        CHECK(stylesheet_rules_affected_by_tokens(
            &sheet, &token, 1, affected, 2) == 1
            && affected[0] == bucket->representative);
    }
    lxb_dom_node_t *target = find_id(
        lxb_dom_interface_node(document.html), "target");
    CHECK(target != NULL && style_for_node(&sheet, target, NULL).color == 0x123456);
    /* An append discards and rebuilds both optional owners. */
    static const char added[] = ".parent-4095 .subject-4095{color:#654321}";
    CHECK(stylesheet_add_css(&sheet, added, sizeof(added) - 1u)
          && style_for_node(&sheet, target, NULL).color == 0x654321);
    stylesheet_destroy(&sheet);
    document_destroy(&document);
    free(css);
    CHECK(budget.current == 0 && budget_uninstall_lexbor(&budget));
    return 0;
}

static int test_selector_metadata_refusal(void)
{
    enum { RULES = 300, CAPACITY = RULES * 64 };
    char css[CAPACITY];
    size_t used = 0;
    for (unsigned i = 0; i < RULES; i++) {
        int written = snprintf(css + used, CAPACITY - used,
            ".ancestor-%u .item-%u{color:#123456}", i, i);
        CHECK(written > 0 && (size_t) written < CAPACITY - used);
        used += (size_t) written;
    }
    bool refused_index = false, partial_tokens = false, complete_tokens = false;
    for (size_t allowance = 0; allowance < 40000u; allowance += 256u) {
        Budget budget;
        budget_init(&budget, 8u * MIB);
        CHECK(budget_install_lexbor(&budget));
        PocDocument document = {0};
        Stylesheet sheet = {0};
        static const char html[] =
            "<div class=ancestor-299><p id=target class=item-299>x</p></div>";
        CHECK(document_parse(&document, &budget, html, sizeof(html) - 1u, 17)
              && stylesheet_build(&sheet, &budget, &document, 480)
              && stylesheet_add_css(&sheet, css, used));
        stylesheet_prepare_selector_program(&sheet);
        size_t limit = budget.limit;
        budget.limit = budget.current + allowance;
        stylesheet_prepare_rule_index(&sheet);
        if (!sheet.rule_index_ready) refused_index = true;
        if (sheet.rule_filters != NULL) {
            uint32_t token = stylesheet_identity_token_hash(
                false, "ancestor-299", strlen("ancestor-299"));
            uint32_t affected[RULES];
            size_t count = stylesheet_rules_affected_by_tokens(
                &sheet, &token, 1, affected, RULES);
            CHECK(count >= 1 && count <= RULES);
            const StyleRule *rule = find_rule(&sheet, ".ancestor-299 .item-299");
            CHECK(rule != NULL);
            bool found = false;
            for (size_t i = 0; i < count; i++)
                found = found || affected[i] == (uint32_t) (rule - sheet.rules);
            CHECK(found);
            if (count == 1) complete_tokens = true;
            else partial_tokens = true;
        }
        size_t failures = budget.failure_count;
        stylesheet_prepare_rule_index(&sheet);
        CHECK(budget.failure_count == failures);
        budget.limit = limit;
        lxb_dom_node_t *target = find_id(
            lxb_dom_interface_node(document.html), "target");
        CHECK(target != NULL && style_for_node(&sheet, target, NULL).color == 0x123456);
        stylesheet_destroy(&sheet);
        document_destroy(&document);
        CHECK(budget.current == 0 && budget_uninstall_lexbor(&budget));
    }
    CHECK(refused_index && partial_tokens && complete_tokens);
    return 0;
}

static int test_transition_candidate_rejection(void)
{
    Budget budget;
    budget_init(&budget, 16u * MIB);
    budget_install_lexbor(&budget);
    char html[16384];
    size_t html_length = (size_t) snprintf(html, sizeof(html),
        "<section class='parent'><div id='target' class='hot a:b'>x</div>"
        "<p id='other'>y</p></section><aside class='");
    for (unsigned i = 0; i < 512u; i++) {
        int length = snprintf(html + html_length, sizeof(html) - html_length,
                              "absent%u ", i);
        CHECK(length > 0 && (size_t) length < sizeof(html) - html_length);
        html_length += (size_t) length;
    }
    CHECK(sizeof(html) - html_length > sizeof("'>z</aside>"));
    memcpy(html + html_length, "'>z</aside>", sizeof("'>z</aside>"));
    html_length += sizeof("'>z</aside>") - 1u;
    PocDocument document = {0};
    CHECK(document_parse(&document, &budget, html, html_length, 17));
    Stylesheet sheet = {0};
    CHECK(stylesheet_build(&sheet, &budget, &document, 480));
    for (unsigned i = 0; i < 512u; i++) {
        char css[128];
        int length = snprintf(css, sizeof(css),
            ".absent%u:not(.off){transition:opacity 1s}", i);
        CHECK(length > 0 && (size_t) length < sizeof(css));
        CHECK(stylesheet_add_css(&sheet, css, (size_t) length));
    }
    static const char css[] =
        ".parent>.hot{transition:color 150ms}"
        "#target{transition-delay:20ms}"
        "DIV{transition-timing-function:linear}"
        ".a\\:b{transition-duration:250ms}"
        ":is(.hot,p){transition-property:opacity}"
        ":not(.hot){transition-duration:350ms}"
        ".parent:has(.hot) p{transition-delay:40ms}"
        ".hot::before{transition:transform 450ms}";
    CHECK(stylesheet_add_css(&sheet, css, sizeof(css) - 1u));
    lxb_dom_node_t *nodes[] = {
        find_id(lxb_dom_interface_node(document.html), "target"),
        find_id(lxb_dom_interface_node(document.html), "other")
    };
    for (unsigned mutation = 0; mutation < 2u; mutation++) {
        if (mutation != 0) {
            CHECK(lxb_dom_element_set_attribute(
                lxb_dom_interface_element(nodes[0]),
                (const lxb_char_t *) "class", 5,
                (const lxb_char_t *) "cold", 4) != NULL);
        }
        for (size_t n = 0; n < 2u; n++) {
            for (PseudoElement pseudo = PSEUDO_NONE; pseudo <= PSEUDO_BEFORE;
                 pseudo++) {
                /* Text past timing_text_used is never written or read:
                   start both alike so the whole records compare. */
                StyleTransitionComputed fast, linear;
                memset(&fast, 0, sizeof(fast));
                memset(&linear, 0, sizeof(linear));
                uint64_t before = sheet.selector_match_calls;
                CHECK(style_transition_computed(&sheet, nodes[n], pseudo,
                                                &fast));
                uint64_t fast_calls = sheet.selector_match_calls - before;
                /* Disable only rejection metadata in this same sheet. The
                   full matcher remains the oracle for every declaration. */
                uint8_t suffix[520];
                CHECK(sheet.transition_rule_count <= sizeof(suffix));
                for (size_t r = 0; r < sheet.transition_rule_count; r++) {
                    suffix[r] = sheet.transition_rules[r].fast_key_suffix_bytes;
                    sheet.transition_rules[r].fast_key_suffix_bytes = UINT8_MAX;
                }
                before = sheet.selector_match_calls;
                CHECK(style_transition_computed(&sheet, nodes[n], pseudo,
                                                &linear));
                uint64_t linear_calls = sheet.selector_match_calls - before;
                for (size_t r = 0; r < sheet.transition_rule_count; r++)
                    sheet.transition_rules[r].fast_key_suffix_bytes = suffix[r];
                CHECK(memcmp(&fast, &linear, sizeof(fast)) == 0);
                if (pseudo == PSEUDO_NONE) {
                    if (linear_calls < fast_calls + 512u)
                        fprintf(stderr, "transition candidates rules=%zu fast=%llu linear=%llu\n",
                            sheet.transition_rule_count,
                            (unsigned long long) fast_calls,
                            (unsigned long long) linear_calls);
                    CHECK(linear_calls >= fast_calls + 512u);
                }
            }
        }
    }
    stylesheet_destroy(&sheet);
    document_destroy(&document);
    CHECK(budget.current == 0);
    return 0;
}

static int test_shadow_scope_allocation_refusal(void)
{
    Budget budget;
    budget_init(&budget, 8u * MIB);
    CHECK(budget_install_lexbor(&budget));
    PocDocument document = {0};
    static const char html[] =
        "<body><p id=outside>Outside</p><x-host><div id=carrier>"
        "<style>p{color:#ff0000}</style><p id=inside>Inside</p>"
        "</div></x-host></body>";
    CHECK(document_parse(&document, &budget, html, sizeof(html) - 1u, 17));
    lxb_dom_node_t *root = lxb_dom_interface_node(document.html);
    lxb_dom_node_t *carrier = find_id(root, "carrier");
    lxb_dom_node_t *inside = find_id(root, "inside");
    lxb_dom_node_t *outside = find_id(root, "outside");
    CHECK(carrier != NULL && inside != NULL && outside != NULL);
    CHECK(document_mark_shadow_carrier(carrier));
    size_t document_bytes = budget.current;
    bool saw_refusal = false;
    for (size_t failure_at = 0; failure_at < 256u; failure_at++) {
        Stylesheet sheet = {0};
        size_t before = budget.injected_failure_count;
        budget_inject_failure_after(&budget, failure_at);
        bool built = stylesheet_build(&sheet, &budget, &document, 480);
        budget_clear_failure_injection(&budget);
        if (built) {
            ComputedStyle outside_style = style_for_node(&sheet, outside, NULL);
            CHECK(outside_style.color == 0x000000u);
            if (before != budget.injected_failure_count)
                saw_refusal = true;
        }
        stylesheet_destroy(&sheet);
        CHECK(budget.current == document_bytes);
    }
    CHECK(saw_refusal);
    Stylesheet complete = {0};
    CHECK(stylesheet_build(&complete, &budget, &document, 480));
    CHECK(complete.shadow_scope_count == 1u);
    CHECK(style_for_node(&complete, inside, NULL).color == 0xff0000u);
    CHECK(style_for_node(&complete, outside, NULL).color == 0x000000u);
    stylesheet_destroy(&complete);
    CHECK(budget.current == document_bytes);
    document_destroy(&document);
    CHECK(budget.current == 0 && budget_uninstall_lexbor(&budget));
    return 0;
}

/* One angle grammar: @supports answers what the rotate and scale
   properties apply (grad, rad, turn, percentages), hue units are ASCII
   case-insensitive, and a huge hue normalizes instead of looping. */
static int test_angle_grammar_agreement(void)
{
    Budget budget;
    budget_init(&budget, 4u * MIB);
    CHECK(budget_install_lexbor(&budget));
    static const char html[] = "<!doctype html><style>"
        "@supports (rotate: 100grad){#grad{color:#ff0000}}"
        "@supports (rotate: 3.141592653589793rad){#rad{color:#ff0000}}"
        "@supports (rotate: 0.75TURN){#turn{color:#ff0000}}"
        "@supports (scale: 50%){#percent{color:#ff0000}}"
        "@supports (rotate: 45deg){#oblique{color:#ff0000}}"
        "#applied{rotate:100grad;scale:50%}"
        "#hue{color:hsl(120DEG 100% 50%)}"
        "#huge{color:hsl(1e300 100% 50%)}"
        "</style><p id=grad>A</p><p id=rad>B</p><p id=turn>C</p>"
        "<p id=percent>D</p><p id=oblique>E</p><p id=applied>F</p>"
        "<p id=hue>G</p><p id=huge>H</p>";
    PocDocument document = {0};
    Stylesheet sheet = {0};
    CHECK(document_parse(&document, &budget, html, sizeof(html) - 1u, 23)
        && stylesheet_build(&sheet, &budget, &document, 480));
    lxb_dom_node_t *root = lxb_dom_interface_node(document.html);
    static const char *const supported[] = {"grad", "rad", "turn", "percent"};
    for (size_t i = 0; i < sizeof(supported) / sizeof(supported[0]); i++) {
        ComputedStyle style = style_for_node(
            &sheet, find_id(root, supported[i]), NULL);
        if (style.color != 0xff0000)
            fprintf(stderr, "@supports disagrees for #%s\n", supported[i]);
        CHECK(style.color == 0xff0000);
    }
    ComputedStyle oblique = style_for_node(&sheet, find_id(root, "oblique"), NULL);
    ComputedStyle applied = style_for_node(&sheet, find_id(root, "applied"), NULL);
    ComputedStyle hue = style_for_node(&sheet, find_id(root, "hue"), NULL);
    ComputedStyle huge = style_for_node(&sheet, find_id(root, "huge"), NULL);
    CHECK(oblique.color != 0xff0000
          && applied.individual_rotate_quadrants == 1
          && applied.transform_scale_q14 == 8192);
    if (hue.color != 0x00ff00)
        fprintf(stderr, "hsl(120DEG ...)=%06x\n", (unsigned) hue.color);
    CHECK(hue.color == 0x00ff00);
    (void) huge;
    stylesheet_destroy(&sheet);
    document_destroy(&document);
    CHECK(budget.current == 0 && budget_uninstall_lexbor(&budget));
    return 0;
}

/* letter-spacing accepts any expression the math compiler does, not just
   one that fits a short buffer once var() is substituted. */
static int test_long_math_letter_spacing(void)
{
    Budget budget;
    budget_init(&budget, 4u * MIB);
    CHECK(budget_install_lexbor(&budget));
    static const char html[] = "<!doctype html><style>"
        "#sum{letter-spacing:calc(1px + 1px + 1px + 1px + 1px + 1px + 1px"
        " + 1px + 1px + 1px)}"
        ":root{--track:calc(0.25px + 0.25px + 0.25px + 0.25px)}"
        "#var{letter-spacing:clamp(1px, calc(var(--track) + 1px), 3px)}"
        "</style><p id=sum>A</p><p id=var>B</p>";
    PocDocument document = {0};
    Stylesheet sheet = {0};
    CHECK(document_parse(&document, &budget, html, sizeof(html) - 1u, 23)
        && stylesheet_build(&sheet, &budget, &document, 480));
    lxb_dom_node_t *root = lxb_dom_interface_node(document.html);
    ComputedStyle sum = style_for_node(&sheet, find_id(root, "sum"), NULL);
    ComputedStyle var = style_for_node(&sheet, find_id(root, "var"), NULL);
    if (sum.letter_spacing_fixed != 10 * 64
        || var.letter_spacing_fixed != 2 * 64)
        fprintf(stderr, "long math letter-spacing: %d %d\n",
                (int) sum.letter_spacing_fixed,
                (int) var.letter_spacing_fixed);
    CHECK(sum.letter_spacing_fixed == 10 * 64
          && var.letter_spacing_fixed == 2 * 64);
    stylesheet_destroy(&sheet);
    document_destroy(&document);
    CHECK(budget.current == 0 && budget_uninstall_lexbor(&budget));
    return 0;
}

static int test_padding_initial_shorthand(void)
{
    Budget budget;
    budget_init(&budget, 4u * MIB);
    CHECK(budget_install_lexbor(&budget));
    static const char html[] = "<!doctype html><style>"
        "button{padding:8px;box-sizing:content-box;width:24px;height:24px;"
        "border:0}#reset{padding:unset}#initial{padding:initial}"
        "#invalid{padding:unset 4px}"
        "input{border:2px solid #123456}"
        "#border-invalid{border:1px solid var(--missing-border-color)}"
        "#border-initial{border:initial}"
        ".display-reset{display:flex}"
        "#display-initial{display:initial}#display-unset{display:unset}"
        "#display-invalid{display:var(--missing-display)}</style>"
        "<button id=reset></button><button id=initial></button>"
        "<button id=invalid></button>"
        "<input id=border-invalid><input id=border-initial>"
        "<div class=display-reset id=display-initial>A</div>"
        "<div class=display-reset id=display-unset>B</div>"
        "<div class=display-reset id=display-invalid>C</div>";
    PocDocument document = {0};
    Stylesheet sheet = {0};
    LayoutDocument layout = {0};
    CHECK(document_parse(&document, &budget, html, sizeof(html)-1u, 23)
        && stylesheet_build(&sheet, &budget, &document, 480)
        && layout_build(&layout, &budget, &document, &sheet, NULL, NULL, 480));
    lxb_dom_node_t *root = lxb_dom_interface_node(document.html);
    const char *ids[] = {"reset", "initial", "invalid"};
    for (size_t i = 0; i < 3; i++) {
        lxb_dom_node_t *node = find_id(root, ids[i]);
        ComputedStyle style = style_for_node(&sheet, node, NULL);
        const LayoutNodeBox *box = layout_box_for_node(&layout, node);
        CHECK(style.padding.top == (i == 2 ? 8 : 0)
            && style.padding.right == style.padding.top
            && style.padding.bottom == style.padding.top
            && style.padding.left == style.padding.top);
        CHECK(box != NULL && box->width == (i == 2 ? 40 : 24)
            && box->height == (i == 2 ? 40 : 24));
    }
    const char *border_ids[] = {"border-invalid", "border-initial"};
    const char *display_ids[] = {
        "display-initial", "display-unset", "display-invalid"
    };
    for (size_t i = 0; i < 3; i++)
        CHECK(style_for_node(&sheet, find_id(root, display_ids[i]), NULL).display
              == DISPLAY_INLINE);
    for (size_t i = 0; i < 2; i++) {
        ComputedStyle style = style_for_node(
            &sheet, find_id(root, border_ids[i]), NULL);
        CHECK(style.border.top == 0 && style.border.right == 0
              && style.border.bottom == 0 && style.border.left == 0);
        for (unsigned side = 0; side < STYLE_BORDER_SIDE_COUNT; side++)
            CHECK(computed_style_border_line(
                      &style, (StyleBorderSide) side) == STYLE_BORDER_NONE);
    }
    layout_destroy(&layout);
    stylesheet_destroy(&sheet);
    document_destroy(&document);
    CHECK(budget.current == 0 && budget_uninstall_lexbor(&budget));
    return 0;
}

static int test_variable_background_shorthand(void)
{
    Budget budget;
    budget_init(&budget, 8u * MIB);
    CHECK(budget_install_lexbor(&budget));
    static const char html[] = "<!doctype html><html data-theme=light><style>"
        ".bar{display:flex;position:fixed;left:0;right:0;bottom:0;z-index:99;"
        "background:var(--theme-panel-background-color);"
        "box-shadow:0 -2px 8px #ddd;padding:0 16px;"
        "-webkit-transform:translateY(0);transform:translateY(0);transition-duration:200ms;"
        "height:var(--bar-height)}"
        "[data-theme=light]{--theme-panel-background-color:#ffffff;--bar-height:72px}"
        "</style><nav id=probe class=bar>Menu</nav>";
    PocDocument document = {0};
    Stylesheet sheet = {0};
    CHECK(document_parse(&document, &budget, html, sizeof(html)-1u, 23)
        && stylesheet_build(&sheet, &budget, &document, 480));
    lxb_dom_node_t *node = find_id(lxb_dom_interface_node(document.html), "probe");
    CHECK(node != NULL);
    ComputedStyle style = style_for_node(&sheet, node, NULL);
    CHECK(style.has_background && style.background == 0xffffff
        && style.background_alpha == 255);
    static const struct {
        const char *alias, *canonical, *value;
    } aliases[] = {
        {"-webkit-flex", "flex", "1 1 auto"},
        {"-ms-flex", "flex", "1 1 auto"},
        {"-webkit-flex-direction", "flex-direction", "column"},
        {"-ms-flex-direction", "flex-direction", "column"},
        {"-webkit-flex-wrap", "flex-wrap", "wrap"},
        {"-ms-flex-wrap", "flex-wrap", "wrap"},
        {"-webkit-flex-grow", "flex-grow", "2"},
        {"-ms-flex-positive", "flex-grow", "2"},
        {"-webkit-flex-shrink", "flex-shrink", "2"},
        {"-ms-flex-negative", "flex-shrink", "2"},
        {"-webkit-flex-basis", "flex-basis", "20px"},
        {"-ms-flex-preferred-size", "flex-basis", "20px"},
        {"-webkit-justify-content", "justify-content", "center"},
        {"-ms-flex-pack", "-webkit-box-pack", "justify"},
        {"-webkit-align-items", "align-items", "center"},
        {"-ms-flex-align", "-webkit-box-align", "center"},
        {"-webkit-align-self", "align-self", "center"},
        {"-ms-flex-item-align", "align-self", "center"},
        {"-webkit-align-content", "align-content", "center"},
        {"-webkit-order", "order", "2"},
        {"-ms-flex-order", "order", "2"},
        {"-webkit-transform", "transform", "translateY(3px)"},
        {"-webkit-transform-origin", "transform-origin", "left top"},
        {"-webkit-box-shadow", "box-shadow", "0 1px 2px #123456"},
        {"-webkit-box-sizing", "box-sizing", "border-box"}
    };
    /* Every supported alias must compile to the canonical instruction, not
       an index derived from a different array. Mix it with a variable so the
       complete block takes the deferred-program path. */
    for (size_t i = 0; i < sizeof(aliases) / sizeof(aliases[0]); i++) {
        char css[512], alias_selector[32], canonical_selector[32];
        snprintf(alias_selector, sizeof(alias_selector), ".alias-%zu", i);
        snprintf(canonical_selector, sizeof(canonical_selector), ".canonical-%zu", i);
        int length = snprintf(css, sizeof(css),
            "%s{background:var(--theme-panel-background-color);%s:%s}"
            "%s{background:var(--theme-panel-background-color);%s:%s}",
            alias_selector, aliases[i].alias, aliases[i].value,
            canonical_selector, aliases[i].canonical, aliases[i].value);
        CHECK(length > 0 && (size_t) length < sizeof(css)
            && stylesheet_add_css(&sheet, css, (size_t) length));
        const StyleRule *a = find_rule(&sheet, alias_selector);
        const StyleRule *c = find_rule(&sheet, canonical_selector);
        CHECK(a != NULL && c != NULL);
        const StyleDeclaration *ad = stylesheet_rule_declaration(&sheet, a);
        const StyleDeclaration *cd = stylesheet_rule_declaration(&sheet, c);
        CHECK(ad != NULL && cd != NULL
            && ad->deferred_program_offset != UINT32_MAX
            && cd->deferred_program_offset != UINT32_MAX
            && ad->deferred_program_count == 2
            && cd->deferred_program_count == 2);
        for (size_t op = 0; op < 2; op++) {
            CHECK(sheet.deferred_instructions[ad->deferred_program_offset + op].property_index
                == sheet.deferred_instructions[cd->deferred_program_offset + op].property_index);
        }
    }
    stylesheet_destroy(&sheet);
    document_destroy(&document);
    CHECK(budget.current == 0 && budget_uninstall_lexbor(&budget));
    return 0;
}

static int test_fixed_implicit_column_overflow(void)
{
    static const size_t counts[] = {24, 30, 300};
    for (size_t variant = 0; variant < sizeof(counts) / sizeof(counts[0]); variant++) {
        Budget budget;
        budget_init(&budget, 8u * MIB);
        CHECK(budget_install_lexbor(&budget));
        char html[16384];
        size_t length = (size_t) snprintf(html, sizeof(html),
            "<!doctype html><style>body{margin:0}.outer{display:flex;"
            "flex-direction:column}.wrapper{position:relative}"
            "#grid{display:grid;grid-auto-flow:column;grid-auto-columns:37px;"
            "gap:3px;overflow:hidden;align-items:start}"
            ".item{display:block;height:20px}.item:last-child{height:67px}"
            "#after{height:10px}</style><div class=outer><div class=wrapper>"
            "<div id=grid>");
        for (size_t i = 0; i < counts[variant]; i++) {
            int written = snprintf(html + length, sizeof(html) - length,
                "<i class=item id=item%zu></i>", i);
            CHECK(written > 0 && (size_t) written < sizeof(html) - length);
            length += (size_t) written;
        }
        int written = snprintf(html + length, sizeof(html) - length,
            "</div></div><div id=after></div></div>");
        CHECK(written > 0 && (size_t) written < sizeof(html) - length);
        length += (size_t) written;
        PocDocument document = {0};
        Stylesheet sheet = {0};
        LayoutDocument layout = {0};
        CHECK(document_parse(&document, &budget, html, length, 29)
            && stylesheet_build(&sheet, &budget, &document, 480)
            && layout_build(&layout, &budget, &document, &sheet, NULL, NULL, 480));
        lxb_dom_node_t *root = lxb_dom_interface_node(document.html);
        const LayoutNodeBox *grid = layout_box_for_node(&layout, find_id(root, "grid"));
        const LayoutNodeBox *after = layout_box_for_node(&layout, find_id(root, "after"));
        CHECK(grid != NULL && grid->height == 67 && after != NULL
            && after->y == grid->y + 67);
        for (size_t i = 0; i < counts[variant]; i++) {
            char id[32];
            snprintf(id, sizeof(id), "item%zu", i);
            const LayoutNodeBox *box = layout_box_for_node(&layout, find_id(root, id));
            CHECK(box != NULL && box->x == grid->x + (int) i * 40
                && box->y == grid->y && box->width == 37
                && box->height == (i + 1 == counts[variant] ? 67 : 20));
        }
        layout_destroy(&layout);
        stylesheet_destroy(&sheet);
        document_destroy(&document);
        CHECK(budget.current == 0 && budget_uninstall_lexbor(&budget));
    }
    return 0;
}

static int test_legacy_grid_gap_shorthand(void)
{
    Budget budget;
    budget_init(&budget, 4u * MIB);
    CHECK(budget_install_lexbor(&budget));
    static const char html[] = "<!doctype html><style>"
        "body{margin:0}:root{--gutter:5px 12px}"
        ".grid{display:grid;grid-template-columns:30px 30px;"
        "grid-template-rows:20px 20px;grid-gap:var(--gutter)}"
        "#override{grid-gap:7px;column-gap:9px}"
        "#important{grid-gap:4px 11px!important;gap:1px}"
        "#reset{grid-gap:12px;grid-gap:initial}"
        "#invalid{grid-gap:3px;grid-gap:1px 2px 3px}"
        ".grid i{display:block;height:20px}"
        "</style><div class=grid id=probe><i id=a></i><i id=b></i><i id=c></i></div>"
        "<div class=grid id=override></div><div class=grid id=important></div>"
        "<div class=grid id=reset></div><div class=grid id=invalid></div>";
    PocDocument document = {0};
    Stylesheet sheet = {0};
    LayoutDocument layout = {0};
    CHECK(document_parse(&document, &budget, html, sizeof(html)-1u, 23)
        && stylesheet_build(&sheet, &budget, &document, 480)
        && layout_build(&layout, &budget, &document, &sheet, NULL, NULL, 480));
    lxb_dom_node_t *root = lxb_dom_interface_node(document.html);
    const LayoutNodeBox *a = layout_box_for_node(&layout, find_id(root, "a"));
    const LayoutNodeBox *b = layout_box_for_node(&layout, find_id(root, "b"));
    const LayoutNodeBox *c = layout_box_for_node(&layout, find_id(root, "c"));
    CHECK(a != NULL && b != NULL && c != NULL
        && b->x == a->x + 42 && c->y == a->y + 25);
    const char *ids[] = {"override", "important", "reset", "invalid"};
    const int rows[] = {7, 4, 0, 3}, columns[] = {9, 11, 0, 3};
    for (size_t i = 0; i < 4; i++) {
        ComputedStyle style = style_for_node(&sheet, find_id(root, ids[i]), NULL);
        CHECK(style.row_gap == rows[i] && style.gap == columns[i]);
    }
    layout_destroy(&layout);
    stylesheet_destroy(&sheet);
    document_destroy(&document);
    CHECK(budget.current == 0 && budget_uninstall_lexbor(&budget));
    return 0;
}

static int test_grid_auto_repeat_whitespace(void)
{
    static const char *const values[] = {
        "repeat(auto-fill,minmax(160px,1fr))",
        "repeat(\n auto-fill,\n minmax(160px,1fr)\n)",
        "repeat(\t auto-fit, minmax(var(--minimum), 1fr))"
    };
    for (size_t i = 0; i < sizeof(values) / sizeof(values[0]); i++) {
        Budget budget;
        budget_init(&budget, 8u * MIB);
        CHECK(budget_install_lexbor(&budget));
        char html[1024];
        int length = snprintf(html, sizeof(html),
            "<!doctype html><style>body{margin:0}#grid{--minimum:160px;"
            "display:grid;width:480px;gap:20px;grid-template-columns:%s}"
            "#grid>div{height:30px}</style><div id=grid>"
            "<div id=a>A</div><div id=b>B</div><div id=c>C</div></div>",
            values[i]);
        CHECK(length > 0 && (size_t) length < sizeof(html));
        PocDocument document = {0}; Stylesheet sheet = {0};
        LayoutDocument layout = {0};
        CHECK(document_parse(&document, &budget, html, (size_t) length, 23)
            && stylesheet_build(&sheet, &budget, &document, 480)
            && layout_build(&layout, &budget, &document, &sheet, NULL, NULL, 480));
        lxb_dom_node_t *root = lxb_dom_interface_node(document.html);
        const LayoutNodeBox *a = layout_box_for_node(&layout, find_id(root, "a"));
        const LayoutNodeBox *b = layout_box_for_node(&layout, find_id(root, "b"));
        const LayoutNodeBox *c = layout_box_for_node(&layout, find_id(root, "c"));
        CHECK(a != NULL && b != NULL && c != NULL
            && a->width == 230 && b->width == 230 && b->x == 250
            && a->y == b->y && c->x == a->x && c->y == a->y + 50);
        layout_destroy(&layout); stylesheet_destroy(&sheet);
        document_destroy(&document);
        CHECK(budget.current == 0 && budget_uninstall_lexbor(&budget));
    }
    return 0;
}

static int test_unused_symbol_rules_do_not_exhaust_live_content(void)
{
    Budget budget;
    budget_init(&budget, 8u * MIB);
    CHECK(budget_install_lexbor(&budget));
    PocDocument document = {0};
    Stylesheet sheet = {0};
    static const char html[] = "<!doctype html><body><i id=target class=s2047></i>";
    CHECK(document_parse(&document, &budget, html, sizeof(html) - 1, 17)
          && stylesheet_build(&sheet, &budget, &document, 480));
    char css[100000];
    size_t length = 0;
    for (unsigned i = 0; i < 2048; i++) {
        int count = snprintf(css + length, sizeof(css) - length,
                             ".s%u::before{content:'\\%x'}", i, 0xe000u + i);
        CHECK(count > 0 && (size_t) count < sizeof(css) - length);
        length += (size_t) count;
    }
    CHECK(stylesheet_add_css(&sheet, css, length));
    lxb_dom_node_t *target = find_id(lxb_dom_interface_node(document.html), "target");
    ComputedStyle parent = style_for_node(&sheet, target, NULL);
    ComputedStyle pseudo = style_for_pseudo(&sheet, target, PSEUDO_BEFORE, &parent);
    CHECK(pseudo.generated_text_length == 3 && pseudo.generated_text != NULL
          && memcmp(pseudo.generated_text, "\xee\x9f\xbf", 3) == 0);
    CHECK(sheet.generated_text_count == 1 && sheet.generated_text_bytes == 4);
    const char *retained = pseudo.generated_text;
    pseudo = style_for_pseudo(&sheet, target, PSEUDO_BEFORE, &parent);
    CHECK(pseudo.generated_text == retained && sheet.generated_text_count == 1);
    static const char replacement[] = ".s2047::before{content:'\\e001'}";
    CHECK(stylesheet_add_css(&sheet, replacement, sizeof(replacement) - 1));
    budget_inject_failure_after(&budget, 0);
    pseudo = style_for_pseudo(&sheet, target, PSEUDO_BEFORE, &parent);
    budget_clear_failure_injection(&budget);
    CHECK(pseudo.generated_text == NULL && pseudo.generated_text_length == 0);
    pseudo = style_for_pseudo(&sheet, target, PSEUDO_BEFORE, &parent);
    CHECK(pseudo.generated_text != NULL && pseudo.generated_text_length == 3
          && memcmp(pseudo.generated_text, "\xee\x80\x81", 3) == 0
          && sheet.generated_text_count == 2
          && memcmp(retained, "\xee\x9f\xbf", 3) == 0);
    stylesheet_destroy(&sheet);
    document_destroy(&document);
    CHECK(budget.current == 0 && budget_uninstall_lexbor(&budget));
    return 0;
}

static int test_centered_content_sized_grid_rows(void)
{
    Budget budget;
    budget_init(&budget, 8u * MIB);
    CHECK(budget_install_lexbor(&budget));
    PocDocument document = {0}; Stylesheet sheet = {0}; LayoutDocument layout = {0};
    static const char html[] = "<!doctype html><style>body{margin:0;line-height:1.5}"
        ".grid{display:grid;width:40px;height:40px;place-content:center}"
        "i{font-size:20px}i::before{content:'+';"
        "display:inline-block;line-height:1;vertical-align:-.125em}"
        ".end{align-content:end}.two{height:80px;row-gap:10px}"
        "</style><div class=grid><i id=center></i></div>"
        "<div class='grid end'><i id=end></i></div>"
        "<div class='grid two'><i id=first></i><i id=second></i></div>";
    CHECK(document_parse(&document, &budget, html, sizeof(html) - 1, 17)
          && stylesheet_build(&sheet, &budget, &document, 480)
          && layout_build(&layout, &budget, &document, &sheet, NULL, NULL, 480));
    lxb_dom_node_t *root = lxb_dom_interface_node(document.html);
    const LayoutNodeBox *center = layout_box_for_node(&layout, find_id(root, "center"));
    const LayoutNodeBox *end = layout_box_for_node(&layout, find_id(root, "end"));
    const LayoutNodeBox *first = layout_box_for_node(&layout, find_id(root, "first"));
    const LayoutNodeBox *second = layout_box_for_node(&layout, find_id(root, "second"));
    CHECK(center && end && first && second);
    CHECK(center->height > 0 && center->height < 40
          && center->y == (40 - center->height) / 2);
    CHECK(end->y + end->height == 80);
    CHECK(first->y == 80 + (80 - first->height - second->height - 10) / 2
          && second->y == first->y + first->height + 10);
    layout_destroy(&layout); stylesheet_destroy(&sheet); document_destroy(&document);
    CHECK(budget.current == 0 && budget_uninstall_lexbor(&budget));
    return 0;
}

static int test_external_stylesheet_utf8_signature(void)
{
    Budget budget; budget_init(&budget, 8u * MIB);
    CHECK(budget_install_lexbor(&budget));
    PocDocument document = {0}; Stylesheet sheet = {0};
    static const char html[] = "<!doctype html><body><div id=target></div>";
    static const char css[] = "\xef\xbb\xbf/* sheet */:root{--rgb:255,255,255}"
        "#target{background-color:rgba(var(--rgb),.75);color:#112233}";
    CHECK(document_parse(&document, &budget, html, sizeof(html) - 1, 17)
          && stylesheet_build(&sheet, &budget, &document, 480)
          && stylesheet_add_css_from(&sheet, css, sizeof(css) - 1,
                                     "https://styles.test/main.css"));
    lxb_dom_node_t *target = find_id(lxb_dom_interface_node(document.html), "target");
    ComputedStyle style = style_for_node(&sheet, target, NULL);
    CHECK(style.has_background && style.background == 0xffffff
          && style.background_alpha == 191 && style.color == 0x112233);
    stylesheet_destroy(&sheet);
    CHECK(stylesheet_build(&sheet, &budget, &document, 480));
    /* An embedded U+FEFF is an identifier character, not whitespace. */
    static const char embedded[] = "#target\xef\xbb\xbf{color:red}";
    CHECK(stylesheet_add_css_from(&sheet, embedded, sizeof(embedded) - 1,
                                 "https://styles.test/main.css"));
    style = style_for_node(&sheet, target, NULL);
    CHECK(style.color != 0xff0000);
    stylesheet_destroy(&sheet); document_destroy(&document);
    CHECK(budget.current == 0 && budget_uninstall_lexbor(&budget));
    return 0;
}

static int test_focus_shadow_probe_work(size_t first)
{
    Budget budget;
    budget_init(&budget, 8u * MIB);
    CHECK(budget_install_lexbor(&budget));
    PocDocument document = {0};
    static const char html[] = "<style>"
        ":root{--ring:0 0 0 1px red,0 0 0 3px blue,0 0 0 5px green}"
        ".other :is(a,button):focus-visible{box-shadow:0 0 0 2px red}"
        ".layered:focus-visible{box-shadow:var(--ring)}"
        ".simple:focus-visible{box-shadow:0 0 0 3px blue}"
        ".relative{font-size:10px}.relative:focus-visible{font-size:20px;box-shadow:0 0 0 1em red}"
        ".context{--ring:0 0 0 2px red}.context:has(.state:focus-visible){--ring:0 0 0 7px red}"
        ".state:focus-visible{box-shadow:var(--ring)}"
        "</style><a id=unrelated>plain</a><a id=layered class=layered>layers</a>"
        "<a id=simple class=simple>simple</a><a id=relative class=relative>relative</a>"
        "<section class=context><a id=state class=state>state</a></section>";
    CHECK(document_parse(&document, &budget, html, sizeof(html) - 1u, 17));
    Stylesheet sheet = {0};
    CHECK(stylesheet_build(&sheet, &budget, &document, 480));
    CHECK(style_variable_cache_begin(&sheet, &budget));
    static const char *ids[] = {"unrelated", "layered", "simple", "relative", "state"};
    for (size_t i = first; i < sizeof(ids) / sizeof(ids[0]); i++) {
        lxb_dom_node_t *node = find_id(lxb_dom_interface_node(document.html), ids[i]);
        CHECK(node != NULL);
        ComputedStyle normal = style_for_node(&sheet, node, NULL);
        char ring[128];
        CHECK(custom_property_text(&sheet, node, "--ring", ring, sizeof(ring)) > 0);
        uint64_t candidates = sheet.rule_index_candidates;
        StyleBoxShadow shadow;
        size_t layer = SIZE_MAX;
        bool admitted = style_node_focus_outset_shadow(&sheet, node, NULL,
                                                       &normal, &shadow, &layer);
        CHECK(document_attribute(node, "data-tilefinch-focus", NULL) == NULL);
        if (i < 2) {
            CHECK(!admitted && sheet.rule_index_candidates == candidates);
        } else {
            CHECK(admitted && layer == 0 && shadow.spread == (i == 2 ? 3 : i == 3 ? 20 : 7));
        }
        uint64_t hits = sheet.variable_cache_hits;
        char restored_ring[128];
        CHECK(custom_property_text(&sheet, node, "--ring", restored_ring,
                  sizeof(restored_ring)) == strlen(ring)
              && strcmp(ring, restored_ring) == 0 && sheet.variable_cache_hits > hits);
        ComputedStyle restored = style_for_node(&sheet, node, NULL);
        CHECK(memcmp(&normal, &restored, sizeof(normal)) == 0);
        /* Declining a retained slot must not suppress the actual focused
           cascade, including layered rings and focus-relative units. */
        CHECK(lxb_dom_element_set_attribute(lxb_dom_interface_element(node),
            (const lxb_char_t *) "data-tilefinch-focus", 20,
            (const lxb_char_t *) "true", 4) != NULL);
        style_variable_cache_invalidate_node(&sheet, node);
        ComputedStyle focused = style_for_node(&sheet, node, NULL);
        CHECK(stylesheet_box_shadow_count(&sheet, &focused) == (i == 1 ? 3 : i == 0 ? 0 : 1));
        CHECK(lxb_dom_element_remove_attribute(lxb_dom_interface_element(node),
            (const lxb_char_t *) "data-tilefinch-focus", 20) == LXB_STATUS_OK);
        style_variable_cache_invalidate_node(&sheet, node);
    }
    style_variable_cache_end(&sheet);
    stylesheet_destroy(&sheet);
    document_destroy(&document);
    CHECK(budget.current == 0 && budget_uninstall_lexbor(&budget));
    return 0;
}

static int test_focus_has_anchor_scope(void)
{
    static const struct { const char *css; bool outside_local; } cases[] = {
        {".menu:has(:focus){padding:7px}.menu:has(:focus){--ink:red}", true},
        {".wrap > .menu:has(> :focus) + .result{color:red}", true},
        {".menu:has(:focus-within){background:red}", true},
        {".wrap[data-state=ready] .menu:first-child:has(button:focus-visible) + .result{color:red}", true},
        {".menu:has(+ #outside:focus){color:red}", false},
        {".menu:has(~ #outside:focus){color:red}", false},
        {".menu:not(:has(:focus)) .child{color:red}", false},
        {".menu:has(:is(:focus)){color:red}", false}
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        Budget budget;
        budget_init(&budget, 8u * MIB);
        CHECK(budget_install_lexbor(&budget));
        PocDocument document = {0};
        Stylesheet sheet = {0};
        const char *html = "<!doctype html><div class=wrap data-state=ready><div class=menu>"
            "<button id=inside>Inside</button><span class=child>Child</span>"
            "</div><button id=outside>Outside</button><div class=result>Result</div></div>";
        CHECK(document_parse(&document, &budget, html, strlen(html), 32u)
            && stylesheet_build(&sheet, &budget, &document, 480)
            && stylesheet_add_css(&sheet, cases[i].css, strlen(cases[i].css)));
        lxb_dom_node_t *root = lxb_dom_interface_node(document.html);
        lxb_dom_node_t *outside = find_id(root, "outside");
        lxb_dom_node_t *inside = find_id(root, "inside");
        CHECK(outside != NULL && inside != NULL);
        bool local = style_focus_change_is_local(&sheet, outside);
        if (local != cases[i].outside_local) {
            fprintf(stderr, "focus anchor case %zu local=%u\n", i, local);
            return 1;
        }
        bool inside_local = style_focus_change_is_local(&sheet, inside);
        if (i < 4u) CHECK(!inside_local);
        size_t length = 0;
        CHECK(document_attribute(outside, "data-tilefinch-focus", &length) == NULL
            && document_attribute(inside, "data-tilefinch-focus", &length) == NULL);
        stylesheet_destroy(&sheet);
        document_destroy(&document);
        CHECK(budget.current == 0 && budget_uninstall_lexbor(&budget));
    }
    return 0;
}

int main(int argc, char **argv)
{
    if (argc == 2 && strcmp(argv[1], "--letter-spacing-only") == 0)
        return test_long_math_letter_spacing();
    if (argc == 2 && strcmp(argv[1], "--angle-grammar-only") == 0)
        return test_angle_grammar_agreement();
    if (argc == 2 && strcmp(argv[1], "--container-grammar-only") == 0)
        return test_container_condition_grammar();
    if (argc == 2 && strcmp(argv[1], "--long-var-properties-only") == 0)
        return test_long_var_values_per_property();
    if (argc == 2 && strcmp(argv[1], "--container-keywords-only") == 0)
        return test_container_condition_keywords();
    if (argc == 2 && strcmp(argv[1], "--container-ratio-only") == 0)
        return test_container_ratio_features();
    if (argc == 2 && strcmp(argv[1], "--container-dedup-only") == 0)
        return test_container_query_registration_dedup();
    if (argc == 2 && strcmp(argv[1], "--shorthand-components-only") == 0)
        return test_shorthand_top_level_components();
    if (argc == 2 && strcmp(argv[1], "--line-width-keywords-only") == 0)
        return test_line_width_keywords();
    if (argc == 2 && strcmp(argv[1], "--box-component-limits-only") == 0)
        return test_box_shorthand_component_limits();
    if (argc == 2 && strcmp(argv[1], "--timing-function-grammar-only") == 0)
        return test_transition_timing_function_grammar();
    if (argc == 2 && strcmp(argv[1], "--long-linear-easing-only") == 0)
        return test_transition_long_linear_easing();
    if (argc == 2 && strcmp(argv[1], "--untimed-transitions-only") == 0)
        return test_transition_untimed_matches_full();
    if (argc == 2 && strcmp(argv[1], "--two-maximal-easings-only") == 0)
        return test_transition_two_maximal_linear_easings();
    if (argc == 2 && strcmp(argv[1], "--grid-minmax-only") == 0)
        return test_grid_minmax_arguments();
    if (argc == 2 && strcmp(argv[1], "--grid-auto-tracks-only") == 0)
        return test_grid_auto_track_lists();
    if (argc == 2 && strcmp(argv[1], "--length-parsers-only") == 0)
        return test_length_parsers_agree();
    if (argc == 2 && strcmp(argv[1], "--layer-escapes-only") == 0)
        return test_layer_components_skip_escapes();
    if (argc == 2 && strcmp(argv[1], "--whitespace-components-only") == 0)
        return test_whitespace_separated_components();
    if (argc == 2 && strcmp(argv[1], "--top-level-escapes-only") == 0)
        return test_top_level_scanner_escapes();
    if (argc == 2 && strcmp(argv[1], "--uppercase-tags-only") == 0)
        return test_uppercase_type_selectors();
    CHECK(test_uppercase_type_selectors() == 0);
    if (argc == 2 && strcmp(argv[1], "--focus-has-anchor-only") == 0)
        return test_focus_has_anchor_scope();
    CHECK(test_focus_has_anchor_scope() == 0);
    if (argc == 2 && strcmp(argv[1], "--focus-shadow-probe-only") == 0)
        return test_focus_shadow_probe_work(0);
    if (argc == 2 && strcmp(argv[1], "--focus-shadow-layered-only") == 0)
        return test_focus_shadow_probe_work(1);
    CHECK(test_focus_shadow_probe_work(0) == 0);
    if (argc == 2 && strcmp(argv[1], "--external-bom-only") == 0)
        return test_external_stylesheet_utf8_signature();
    CHECK(test_external_stylesheet_utf8_signature() == 0);
    if (argc == 2 && strcmp(argv[1], "--centered-auto-rows-only") == 0)
        return test_centered_content_sized_grid_rows();
    CHECK(test_centered_content_sized_grid_rows() == 0);
    if (argc == 2 && strcmp(argv[1], "--live-symbols-only") == 0)
        return test_unused_symbol_rules_do_not_exhaust_live_content();
    CHECK(test_unused_symbol_rules_do_not_exhaust_live_content() == 0);
    if (argc == 2 && strcmp(argv[1], "--auto-repeat-only") == 0)
        return test_grid_auto_repeat_whitespace();
    CHECK(test_grid_auto_repeat_whitespace() == 0);
    if (argc == 2 && strcmp(argv[1], "--grid-gap-only") == 0)
        return test_legacy_grid_gap_shorthand();
    if (argc == 2 && strcmp(argv[1], "--implicit-column-overflow-only") == 0)
        return test_fixed_implicit_column_overflow();
    if (argc == 2 && strcmp(argv[1], "--variable-background-only") == 0)
        return test_variable_background_shorthand();
    CHECK(test_fixed_implicit_column_overflow() == 0);
    CHECK(test_variable_background_shorthand() == 0);
    CHECK(test_legacy_grid_gap_shorthand() == 0);
    CHECK(test_padding_initial_shorthand() == 0);
    CHECK(test_long_math_letter_spacing() == 0);
    CHECK(test_angle_grammar_agreement() == 0);
    CHECK(test_container_condition_grammar() == 0);
    CHECK(test_container_condition_keywords() == 0);
    CHECK(test_container_ratio_features() == 0);
    CHECK(test_container_query_registration_dedup() == 0);
    CHECK(test_container_condition_cache_follows_state() == 0);
    CHECK(test_shorthand_top_level_components() == 0);
    CHECK(test_line_width_keywords() == 0);
    CHECK(test_box_shorthand_component_limits() == 0);
    CHECK(test_transition_timing_function_grammar() == 0);
    CHECK(test_transition_long_linear_easing() == 0);
    CHECK(test_transition_two_maximal_linear_easings() == 0);
    CHECK(test_transition_untimed_matches_full() == 0);
    CHECK(test_grid_minmax_arguments() == 0);
    CHECK(test_grid_auto_track_lists() == 0);
    CHECK(test_length_parsers_agree() == 0);
    CHECK(test_layer_components_skip_escapes() == 0);
    CHECK(test_whitespace_separated_components() == 0);
    CHECK(test_top_level_scanner_escapes() == 0);
    CHECK(test_long_var_values_per_property() == 0);
    if (argc == 2 && strcmp(argv[1], "--padding-reset-only") == 0) return 0;
    /* The selector offset and container query share one field. Exercise the
       full admitted offset range so longer ordinary selectors cannot silently
       wrap or overwrite a query id. */
    for (uint16_t offset = 0; offset < STYLE_SELECTOR_TEXT_CAPACITY; offset++) {
        for (uint8_t query = 0; query <= STYLE_CONTAINER_QUERY_LIMIT; query++) {
            StyleRule rule = {.rightmost_compound_offset = offset};
            style_rule_set_container_query(&rule, query);
            CHECK(style_rule_rightmost_compound(&rule) == offset
                  && style_rule_container_query(&rule) == query);
        }
    }
    if (argc == 2 && strcmp(argv[1], "--computed-storage-only") == 0)
        return test_computed_style_payload_storage();
    if (argc == 2 && strcmp(argv[1], "--selector-metadata-only") == 0) {
        CHECK(test_sparse_selector_metadata() == 0);
        return test_selector_metadata_refusal();
    }
    if (argc == 2 && strcmp(argv[1], "--retained-handoff-only") == 0)
        return test_retained_range_cache_handoff();
    if (argc == 2 && strcmp(argv[1], "--retained-nested-only") == 0)
        return test_retained_nested_selector_invalidation();
    if (argc == 2 && strcmp(argv[1], "--quoted-punctuation-only") == 0)
        return test_quoted_pseudo_element_punctuation();
    if (argc == 2 && strcmp(argv[1], "--retained-focus-only") == 0)
        return test_retained_focus_descendants();
    if (argc == 2 && strcmp(argv[1], "--declaration-footprint-only") == 0)
        return test_unmatched_declaration_footprint();
    if (argc == 2 && strcmp(argv[1], "--custom-footprint-only") == 0)
        return test_custom_rule_footprint();
    if (argc == 2 && strcmp(argv[1], "--long-custom-only") == 0)
        return test_long_custom_property_values();
    if (argc == 2 && strcmp(argv[1], "--registered-only") == 0)
        return test_registered_custom_properties();
    if (argc == 2 && strcmp(argv[1], "--registered-reuse-only") == 0)
        return test_registered_length_ancestor_reuse();
    if (argc == 2 && strcmp(argv[1], "--long-variable-names-only") == 0)
        return test_long_variable_names_cached();
    if (argc == 2 && strcmp(argv[1], "--motion-source-only") == 0)
        return test_motion_source_declarations();
    if (argc == 2 && strcmp(argv[1], "--attribute-names-only") == 0)
        return test_selector_attribute_names();
    if (argc == 2 && strcmp(argv[1], "--shadow-scope-refusal-only") == 0)
        return test_shadow_scope_allocation_refusal();
    if (argc == 2 && strcmp(argv[1], "--attribute-bench-only") == 0)
        return benchmark_selector_attribute_names();
    if (argc == 2 && strcmp(argv[1], "--escaped-utilities-only") == 0)
        return test_escaped_utility_classes_indexed();
    if (argc == 2 && strcmp(argv[1], "--transition-candidates-only") == 0)
        return test_transition_candidate_rejection();
    CHECK(test_transition_candidate_rejection() == 0);
    CHECK(test_selector_attribute_names() == 0);
    CHECK(test_shadow_scope_allocation_refusal() == 0);
    CHECK(test_computed_style_payload_storage() == 0);
    CHECK(test_sparse_selector_metadata() == 0);
    CHECK(test_selector_metadata_refusal() == 0);
    CHECK(benchmark_selector_attribute_names() == 0);
    CHECK(test_retained_cache_eligibility() == 0);
    CHECK(test_retained_container_selectors() == 0);
    CHECK(test_unrelated_container_custom_rules() == 0);
    CHECK(test_retained_dense_storage() == 0);
    CHECK(test_retained_range_cache_handoff() == 0);
    CHECK(test_class_token_byte_boundaries() == 0);
    CHECK(test_retained_properties_share_class_tokens() == 0);
    CHECK(test_utility_class_candidates_match_linear() == 0);
    CHECK(test_escaped_utility_classes_indexed() == 0);
    CHECK(test_query_attribute_keys_match_direct() == 0);
    CHECK(test_query_deeply_nested_is_prepares_flat() == 0);
    CHECK(test_full_selector_class_token_reuse() == 0);
    CHECK(test_functional_selector_keys() == 0);
    CHECK(test_deferred_font_basis() == 0);
    CHECK(test_retained_retirement_probe_holes() == 0);
    CHECK(test_retained_mixed_importance_append() == 0);
    CHECK(test_append_identity_fallback() == 0);
    CHECK(test_retained_nested_selector_invalidation() == 0);
    CHECK(test_quoted_pseudo_element_punctuation() == 0);
    CHECK(test_retained_focus_descendants() == 0);
    if (argc == 2 && strcmp(argv[1], "--bounded-out-focus-only") == 0)
        return test_bounded_out_sheet_keeps_focus_fast_path();
    CHECK(test_bounded_out_sheet_keeps_focus_fast_path() == 0);
    CHECK(test_retained_keyless_lost_match() == 0);
    CHECK(test_retained_scoped_invalidation() == 0);
    CHECK(test_ancestor_filter_canonical_tokens() == 0);
    CHECK(test_compiled_attribute_and_pseudo_instructions() == 0);
    CHECK(test_quoted_declaration_boundaries() == 0);
    CHECK(test_pseudo_state_work() == 0);
    CHECK(test_layout_scoped_selector_work() == 0);
    CHECK(test_pseudo_absence_proof() == 0);
    CHECK(test_deferred_expansion_reuse() == 0);
    CHECK(test_head_script_dependency_cache() == 0);
    CHECK(test_unmatched_declaration_footprint() == 0);
    CHECK(test_custom_rule_footprint() == 0);
    CHECK(test_long_custom_property_values() == 0);
    CHECK(test_registered_custom_properties() == 0);
    CHECK(test_registered_length_ancestor_reuse() == 0);
    CHECK(test_long_variable_names_cached() == 0);
    CHECK(test_motion_source_declarations() == 0);
    CHECK(test_svg_raster_token_gate() == 0);
    CHECK(test_large_sheet_compiles_every_rule() == 0);
    CHECK(test_shared_selector_programs() == 0);
    CHECK(test_class_token_quick_filter() == 0);
    CHECK(test_walk_subject_cache_matches_fresh() == 0);
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
          && hyphens_rule->fast_key_suffix_bytes != UINT8_MAX
          && strcmp(hyphens_rule->selector
                        + hyphens_rule->selector_length
                        - hyphens_rule->fast_key_suffix_bytes
                        - hyphens_rule->fast_key_length,
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
          && indexed.selector_program_bytes <= 320u * 1024u
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
          && indexed_target.letter_spacing_fixed == 6 * 64
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
