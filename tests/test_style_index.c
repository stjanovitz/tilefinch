#include "tilefinch/budget.h"
#include "tilefinch/document.h"
#include "tilefinch/layout.h"
#include "tilefinch/render.h"
#include "tilefinch/style.h"

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

int main(void)
{
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
        ".special.button{margin-left:99px}"
        ".missing section .card{margin-right:99px}"
        "</style><section>"
        "<a id=primary class=button>Primary</a>"
        "<a id=secondary class='button button-secondary'>Login</a>"
        "<div id=target class='card hot' data-x=1></div>"
        "<p id=other class=cold></p>"
        "</section>";
    PocDocument document = {0};
    CHECK(document_parse(&document, &budget, html, sizeof(html) - 1u, 17));
    lxb_dom_node_t *root = lxb_dom_interface_node(document.html);
    lxb_dom_node_t *target = find_id(root, "target");
    lxb_dom_node_t *other = find_id(root, "other");
    lxb_dom_node_t *primary = find_id(root, "primary");
    lxb_dom_node_t *secondary = find_id(root, "secondary");
    CHECK(target != NULL && other != NULL && primary != NULL
          && secondary != NULL);

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
