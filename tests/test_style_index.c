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
    CHECK(stacked.font_family == FONT_METRIC_SANS);
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
        /* No initial-value for a typed syntax: invalid, ignored. */
        "@property --reg-bad{syntax:\"<length>\";inherits:false}"
        "@layer properties{@supports (-webkit-hyphens:none){"
        "*{--reg-color:#123456}}}"
        ".outer{--reg-color:#ff0000;--reg-any:abc;--reg-bad:outer}"
        ".reset{--reg-length:initial}";
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
    CHECK(sheet.registered_property_count == 3
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
    CHECK(reuse->matches == NULL && !reuse->matches_attempted);
    layout_reuse_cache_prepare(reuse, &ordinary, NULL, NULL, 480);
    CHECK(reuse->matches != NULL);
    LayoutReuseStats eligible, ineligible;
    layout_reuse_cache_stats(reuse, &eligible);
    size_t match_bytes = style_retained_matches_bytes(reuse->matches);
    layout_reuse_cache_prepare(reuse, &containers, NULL, NULL, 480);
    layout_reuse_cache_stats(reuse, &ineligible);
    CHECK(reuse->matches == NULL && !reuse->matches_attempted
          && eligible.retained_bytes - ineligible.retained_bytes == match_bytes);
    layout_reuse_cache_prepare(reuse, &ordinary, NULL, NULL, 480);
    CHECK(reuse->matches != NULL);
    layout_reuse_cache_prepare(reuse, &containers, NULL, NULL, 480);
    size_t limit = budget.limit;
    budget.limit = budget.current;
    layout_reuse_cache_prepare(reuse, &ordinary, NULL, NULL, 480);
    CHECK(reuse->matches == NULL && reuse->matches_attempted);
    budget.limit = limit;
    layout_reuse_cache_destroy(reuse);
    stylesheet_destroy(&ordinary);
    stylesheet_destroy(&containers);
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
        && left->letter_spacing == right->letter_spacing
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
    if (argc == 2 && strcmp(argv[1], "--declaration-footprint-only") == 0)
        return test_unmatched_declaration_footprint();
    if (argc == 2 && strcmp(argv[1], "--custom-footprint-only") == 0)
        return test_custom_rule_footprint();
    if (argc == 2 && strcmp(argv[1], "--long-custom-only") == 0)
        return test_long_custom_property_values();
    if (argc == 2 && strcmp(argv[1], "--registered-only") == 0)
        return test_registered_custom_properties();
    if (argc == 2 && strcmp(argv[1], "--attribute-names-only") == 0)
        return test_selector_attribute_names();
    if (argc == 2 && strcmp(argv[1], "--attribute-bench-only") == 0)
        return benchmark_selector_attribute_names();
    if (argc == 2 && strcmp(argv[1], "--escaped-utilities-only") == 0)
        return test_escaped_utility_classes_indexed();
    CHECK(test_selector_attribute_names() == 0);
    CHECK(benchmark_selector_attribute_names() == 0);
    CHECK(test_retained_cache_eligibility() == 0);
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
    CHECK(test_retained_nested_selector_invalidation() == 0);
    CHECK(test_quoted_pseudo_element_punctuation() == 0);
    CHECK(test_retained_focus_descendants() == 0);
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
    CHECK(test_svg_raster_token_gate() == 0);
    CHECK(test_large_sheet_compiles_every_rule() == 0);
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
