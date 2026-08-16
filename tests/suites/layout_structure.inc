static int test_modern_conditional_cascade(Budget *budget)
{
    static const char page[] =
        "<!doctype html><style>"
        "@layer framework,theme;"
        "@layer framework{"
        " #cascade{color:#112233!important;background:#223344}"
        " @layer base{#cascade{color:#334455!important;"
        "background:#445566}}"
        "}"
        "@layer theme{#cascade{color:#556677!important;"
        "background:#667788}}"
        "#cascade{background:#778899}"
        "@supports (DISPLAY:grid) and (not (display:subgrid)){"
        " #supports{width:73px}"
        "}"
        "@supports (display:grid) or (display:subgrid){"
        " #supports{height:29px}"
        "}"
        "@supports selector(.card > .title){#supports{min-width:11px}}"
        "@supports selector(.card:unknownpseudo){#supports{display:none}}"
        "#revert-later{color:revert-rule!important}"
        "#revert-later{color:#008000}"
        "#revert-same,#revert-inline,#revert-normal{color:#112233}"
        "#revert-same{color:#445566;color:revert-rule!important;"
        "background:#778899}"
        "#revert-normal{color:#445566;color:revert-rule;"
        "background:#667788}"
        ".card{color:#010203;"
        " &.active{color:#112233}"
        " > .title{background:#223344}"
        " .desc{border-left:3px solid #334455}"
        " @media (max-width:500px){"
        "  &{padding-left:7px}"
        "  & .inner{margin-left:9px}"
        " }"
        " @supports (display:grid){& .grid{width:61px}}"
        " background:#445566"
        "}"
        "</style><div id=cascade></div><div id=supports></div>"
        "<div id=revert-later></div><div id=revert-same></div>"
        "<div id=revert-normal></div>"
        "<div id=revert-inline style='color:#445566;"
        "color:revert-rule!important;background:#556677'></div>"
        "<section id=card class='card active'><b id=title class=title></b>"
        "<i id=desc class=desc></i><span id=inner class=inner></span>"
        "<em id=grid class=grid></em></section>";
    PocDocument document = {0};
    Stylesheet sheet = {0};
    CHECK(document_parse(&document, budget, page, strlen(page), 443)
          && stylesheet_build(&sheet, budget, &document, 480));
    lxb_dom_node_t *root = lxb_dom_interface_node(document.html);
    ComputedStyle cascade = style_for_node(
        &sheet, find_id(root, "cascade"), NULL);
    ComputedStyle supports = style_for_node(
        &sheet, find_id(root, "supports"), NULL);
    ComputedStyle revert_later = style_for_node(
        &sheet, find_id(root, "revert-later"), NULL);
    ComputedStyle revert_same = style_for_node(
        &sheet, find_id(root, "revert-same"), NULL);
    ComputedStyle revert_normal = style_for_node(
        &sheet, find_id(root, "revert-normal"), NULL);
    ComputedStyle revert_inline = style_for_node(
        &sheet, find_id(root, "revert-inline"), NULL);
    ComputedStyle card = style_for_node(&sheet, find_id(root, "card"), NULL);
    ComputedStyle title = style_for_node(
        &sheet, find_id(root, "title"), &card);
    ComputedStyle desc = style_for_node(
        &sheet, find_id(root, "desc"), &card);
    ComputedStyle inner = style_for_node(
        &sheet, find_id(root, "inner"), &card);
    ComputedStyle grid = style_for_node(
        &sheet, find_id(root, "grid"), &card);
    CHECK(cascade.color == 0x334455
          && cascade.background == 0x778899
          && supports.width == 73 && supports.height == 29
          && supports.min_width == 11
          && revert_later.color == 0x008000
          && revert_same.color == 0x112233
          && revert_same.background == 0x778899
          && revert_normal.color == 0x112233
          && revert_normal.background == 0x667788
          && revert_inline.color == 0x112233
          && revert_inline.background == 0x556677
          && card.color == 0x112233
          && card.background == 0x445566
          && card.padding.left == 7
          && title.background == 0x223344
          && desc.border.left == 3
          && inner.margin.left == 9
          && grid.width == 61
          && stylesheet_supports_matches(
                 &sheet, "(DISPLAY:grid) AND (NOT (display:subgrid))",
                 strlen("(DISPLAY:grid) AND (NOT (display:subgrid))"))
          && stylesheet_supports_matches(
                 &sheet, "(color:revert-rule)",
                 strlen("(color:revert-rule)"))
          && !stylesheet_supports_matches(
                 &sheet, "(display:grid) and (display:flex) or "
                         "(display:block)",
                 strlen("(display:grid) and (display:flex) or "
                        "(display:block)")));
    stylesheet_destroy(&sheet);
    document_destroy(&document);
    CHECK(budget->current == 0);
    return 0;
}

static int test_bounded_container_queries(Budget *budget)
{
    size_t baseline = budget->current;
    static const char passive_page[] =
        "<!doctype html><style>#passive{container-type:inline-size}"
        "</style><div id=passive></div>";
    PocDocument passive_document = {0};
    Stylesheet passive_sheet = {0};
    CHECK(document_parse(
              &passive_document, budget, passive_page,
              strlen(passive_page), 30)
          && stylesheet_build(
              &passive_sheet, budget, &passive_document, 480)
          && !stylesheet_has_container_queries(&passive_sheet));
    stylesheet_destroy(&passive_sheet);
    document_destroy(&passive_document);
    CHECK(budget->current == baseline);

    static const char page[] =
        "<!doctype html><style>body{margin:0}"
        "#outer{container: shell / size;width:300px;height:100px}"
        "#inner{container-type:inline-size;width:120px;padding:0 10px;"
        "box-sizing:border-box}"
        "@container shell (width >= 300px){"
        " #target{background:#123456;width:10cqw}"
        " @container (width < 150px){#target{height:17px}}"
        "}"
        "@container missing (width > 1px){#target{display:none}}"
        "</style><div id=outer><div id=inner>"
        "<div id=target></div></div></div>";
    size_t stylesheet_injected_failures = 0;
    size_t stylesheet_uninjected_builds = 0;
    for (size_t fail_after = 0; fail_after < 64; fail_after++) {
        PocDocument fault_document = {0};
        Stylesheet fault_sheet = {0};
        CHECK(document_parse(
            &fault_document, budget, page, strlen(page), 31));
        size_t failures_before = budget->injected_failure_count;
        budget_inject_failure_after(budget, fail_after);
        bool built = stylesheet_build(
            &fault_sheet, budget, &fault_document, 480);
        bool injected =
            budget->injected_failure_count != failures_before;
        budget_clear_failure_injection(budget);
        if (built) CHECK(stylesheet_has_container_queries(&fault_sheet));
        if (built || fault_sheet.budget != NULL) {
            stylesheet_destroy(&fault_sheet);
        }
        document_destroy(&fault_document);
        CHECK(budget->current == baseline);
        if (injected) {
            stylesheet_injected_failures++;
            stylesheet_uninjected_builds = 0;
        } else if (++stylesheet_uninjected_builds == 2) {
            break;
        }
    }
    CHECK(stylesheet_injected_failures >= 4
          && stylesheet_uninjected_builds == 2);

    PocDocument document = {0};
    Stylesheet sheet = {0};
    LayoutDocument layout = {0};
    CHECK(document_parse(&document, budget, page, strlen(page), 31)
          && stylesheet_build(&sheet, budget, &document, 480)
          && stylesheet_has_container_queries(&sheet)
          && layout_build(&layout, budget, &document, &sheet,
                          NULL, NULL, 480));
    lxb_dom_node_t *target = find_id(
        lxb_dom_interface_node(document.html), "target");
    const LayoutNodeBox *box = layout_box_for_node(&layout, target);
    ComputedStyle style = style_for_node(&sheet, target, NULL);
    CHECK(target != NULL && box != NULL
          && style.display != DISPLAY_NONE
          && style.background == 0x123456
          /* cqw resolves against the 100px content box, not the 120px
             padding box retained by LayoutNodeBox. */
          && box->width == 10 && box->height == 17
          && stylesheet_supports_matches(
                 &sheet, "(container-type:inline-size)",
                 strlen("(container-type:inline-size)")));
    layout_destroy(&layout);
    stylesheet_destroy(&sheet);
    document_destroy(&document);
    CHECK(budget->current == baseline);

    size_t injected_failures = 0;
    size_t uninjected_builds = 0;
    for (size_t fail_after = 0; fail_after < 64; fail_after++) {
        PocDocument fault_document = {0};
        Stylesheet fault_sheet = {0};
        LayoutDocument faulted = {0};
        CHECK(document_parse(
                  &fault_document, budget, page, strlen(page), 31)
              && stylesheet_build(
                  &fault_sheet, budget, &fault_document, 480));
        size_t failures_before = budget->injected_failure_count;
        budget_inject_failure_after(budget, fail_after);
        bool built = layout_build(
            &faulted, budget, &fault_document, &fault_sheet,
            NULL, NULL, 480);
        bool injected =
            budget->injected_failure_count != failures_before;
        budget_clear_failure_injection(budget);
        if (built) layout_destroy(&faulted);
        stylesheet_destroy(&fault_sheet);
        document_destroy(&fault_document);
        CHECK(budget->current == baseline);
        if (injected) {
            injected_failures++;
            uninjected_builds = 0;
        } else if (++uninjected_builds == 2) {
            break;
        }
    }
    CHECK(injected_failures >= 8 && uninjected_builds == 2);
    return 0;
}

static int test_bounded_multicolumn_blocks(Budget *budget)
{
    size_t baseline = budget->current;
    static const char page[] =
        "<!doctype html><style>body{margin:0}"
        "#columns{width:210px;columns:2;column-gap:10px}"
        "#columns>div{height:20px;margin:0;break-inside:avoid}"
        "</style><div id=columns><div id=a></div><div id=b></div>"
        "<div id=c></div><div id=d></div></div>";
    PocDocument document = {0};
    Stylesheet sheet = {0};
    LayoutDocument layout = {0};
    CHECK(document_parse(&document, budget, page, strlen(page), 32)
          && stylesheet_build(&sheet, budget, &document, 480)
          && sheet.has_multicolumn_rules
          && layout_build(&layout, budget, &document, &sheet,
                          NULL, NULL, 480));
    lxb_dom_node_t *root = lxb_dom_interface_node(document.html);
    const LayoutNodeBox *columns = layout_box_for_node(
        &layout, find_id(root, "columns"));
    const LayoutNodeBox *a = layout_box_for_node(&layout, find_id(root, "a"));
    const LayoutNodeBox *b = layout_box_for_node(&layout, find_id(root, "b"));
    const LayoutNodeBox *c = layout_box_for_node(&layout, find_id(root, "c"));
    const LayoutNodeBox *d = layout_box_for_node(&layout, find_id(root, "d"));
    CHECK(columns != NULL && a != NULL && b != NULL && c != NULL && d != NULL
          && a->width == 100 && b->width == 100
          && c->width == 100 && d->width == 100
          && a->x == b->x && c->x == d->x
          && c->x - a->x == 110
          && b->y - a->y == 20 && d->y - c->y == 20
          && c->y == a->y
          && columns->height == 40
          && stylesheet_supports_matches(
                 &sheet, "(column-count:2)",
                 strlen("(column-count:2)"))
          && stylesheet_supports_matches(
                 &sheet, "(break-inside:avoid)",
                 strlen("(break-inside:avoid)")));
    layout_destroy(&layout);
    stylesheet_destroy(&sheet);
    document_destroy(&document);
    CHECK(budget->current == baseline);
    return 0;
}

static int test_directional_logical_properties(Budget *budget)
{
    static const char page[] =
        "<!doctype html><style>"
        "#vertical{writing-mode:vertical-rl;direction:rtl}"
        "#logical{inline-size:41px;block-size:83px;"
        "min-inline-size:31px;max-inline-size:51px;"
        "min-block-size:73px;max-block-size:93px;"
        "margin-inline:5px 7px;margin-block:11px 13px;"
        "padding-inline:2px 3px;padding-block:4px 6px;"
        "position:absolute;inset-inline:17px 19px;"
        "inset-block:23px 29px}"
        "#late{margin-inline-start:8px;margin-left:37px;"
        "padding-block-end:9px;inset-inline-end:10px;"
        "inline-size:42px;writing-mode:vertical-lr;direction:ltr}"
        "#horizontal{direction:rtl;margin-inline:12px 14px;"
        "padding-inline-start:15px;position:absolute;"
        "inset-inline:16px 18px}"
        "#cross-rule{margin-inline-start:21px}"
        "#cross-rule{writing-mode:vertical-rl}"
        "#inherit-logical{direction:unset;writing-mode:unset;"
        "margin-inline-start:24px}"
        "#initial-logical{direction:initial;writing-mode:initial;"
        "margin-inline-start:25px}"
        ":root{--safe-end:env(safe-area-inset-right)}"
        "#safe-env{position:absolute;"
        "inset-inline-end:max(0px,var(--safe-end) - 16px)}"
        "#unknown-env{position:absolute;"
        "inset-inline-start:env(tilefinch-unknown,13px)}"
        "</style><section id=vertical><div id=logical></div></section>"
        "<div id=late></div><div id=horizontal></div>"
        "<div id=cross-rule></div>"
        "<div id=inline style='margin-inline-start:22px;"
        "writing-mode:vertical-lr'></div>"
        "<section id=inherit-parent style='writing-mode:vertical-rl;"
        "direction:rtl'><div id=inherit-logical></div>"
        "<div id=initial-logical></div></section>"
        "<div id=safe-env></div><div id=unknown-env></div>";
    PocDocument document = {0};
    Stylesheet sheet = {0};
    CHECK(document_parse(&document, budget, page, strlen(page), 443)
          && stylesheet_build(&sheet, budget, &document, 480));
    lxb_dom_node_t *root = lxb_dom_interface_node(document.html);
    lxb_dom_node_t *vertical_node = find_id(root, "vertical");
    lxb_dom_node_t *logical_node = find_id(root, "logical");
    lxb_dom_node_t *late_node = find_id(root, "late");
    lxb_dom_node_t *horizontal_node = find_id(root, "horizontal");
    lxb_dom_node_t *cross_rule_node = find_id(root, "cross-rule");
    lxb_dom_node_t *inline_node = find_id(root, "inline");
    lxb_dom_node_t *inherit_parent_node = find_id(root, "inherit-parent");
    lxb_dom_node_t *inherit_logical_node = find_id(root, "inherit-logical");
    lxb_dom_node_t *initial_logical_node = find_id(root, "initial-logical");
    lxb_dom_node_t *safe_env_node = find_id(root, "safe-env");
    lxb_dom_node_t *unknown_env_node = find_id(root, "unknown-env");
    CHECK(vertical_node != NULL && logical_node != NULL
          && late_node != NULL && horizontal_node != NULL
          && cross_rule_node != NULL && inline_node != NULL
          && inherit_parent_node != NULL && inherit_logical_node != NULL
          && initial_logical_node != NULL && safe_env_node != NULL
          && unknown_env_node != NULL);

    ComputedStyle vertical = style_for_node(&sheet, vertical_node, NULL);
    ComputedStyle logical = style_for_node(
        &sheet, logical_node, &vertical);
    ComputedStyle late = style_for_node(&sheet, late_node, NULL);
    ComputedStyle horizontal = style_for_node(
        &sheet, horizontal_node, NULL);
    ComputedStyle cross_rule = style_for_node(
        &sheet, cross_rule_node, NULL);
    ComputedStyle inline_style = style_for_node(
        &sheet, inline_node, NULL);
    ComputedStyle inherit_parent = style_for_node(
        &sheet, inherit_parent_node, NULL);
    ComputedStyle inherit_logical = style_for_node(
        &sheet, inherit_logical_node, &inherit_parent);
    ComputedStyle initial_logical = style_for_node(
        &sheet, initial_logical_node, &inherit_parent);
    ComputedStyle safe_env = style_for_node(&sheet, safe_env_node, NULL);
    ComputedStyle unknown_env = style_for_node(
        &sheet, unknown_env_node, NULL);
    CHECK(computed_style_writing_mode(&logical)
              == STYLE_WRITING_VERTICAL_RL
          && computed_style_direction_rtl(&logical)
          && logical.has_width && logical.width == 83
          && logical.has_height && logical.height == 41
          && logical.min_width == 73 && logical.max_width == 93
          && logical.min_height == 31 && logical.max_height == 51
          && logical.margin.top == 7 && logical.margin.right == 11
          && logical.margin.bottom == 5 && logical.margin.left == 13
          && logical.padding.top == 3 && logical.padding.right == 4
          && logical.padding.bottom == 2 && logical.padding.left == 6
          && logical.has_top && logical.top == 19
          && logical.has_right && logical.right == 23
          && logical.has_bottom && logical.bottom == 17
          && logical.has_left && logical.left == 29);
    /* Axis selection is computed-value based, not declaration-order based.
       The later physical alias still wins its own left edge. */
    CHECK(computed_style_writing_mode(&late) == STYLE_WRITING_VERTICAL_LR
          && !computed_style_direction_rtl(&late)
          && late.has_height && late.height == 42
          && late.margin.top == 8 && late.margin.left == 37
          && late.padding.right == 9
          && late.has_bottom && late.bottom == 10);
    CHECK(computed_style_direction_rtl(&horizontal)
          && horizontal.margin.right == 12
          && horizontal.margin.left == 14
          && horizontal.padding.right == 15
          && horizontal.has_right && horizontal.right == 16
          && horizontal.has_left && horizontal.left == 18);
    CHECK(computed_style_writing_mode(&cross_rule)
              == STYLE_WRITING_VERTICAL_RL
          && cross_rule.margin.top == 21
          && computed_style_writing_mode(&inline_style)
              == STYLE_WRITING_VERTICAL_LR
          && inline_style.margin.top == 22);
    CHECK(computed_style_writing_mode(&inherit_logical)
              == STYLE_WRITING_VERTICAL_RL
          && computed_style_direction_rtl(&inherit_logical)
          && inherit_logical.margin.bottom == 24
          && computed_style_writing_mode(&initial_logical)
              == STYLE_WRITING_HORIZONTAL_TB
          && !computed_style_direction_rtl(&initial_logical)
          && initial_logical.margin.left == 25);
    CHECK(safe_env.has_right && safe_env.right == 0
          && unknown_env.has_left && unknown_env.left == 13);

    stylesheet_destroy(&sheet);
    document_destroy(&document);
    CHECK(budget->current == 0);
    return 0;
}

static int test_international_line_flow(Budget *budget)
{
    static const char page[] =
        "<!doctype html><style>"
        "body{margin:0;font-size:16px}"
        "#cjk,#zwsp{display:block;width:18px}"
        "#opening{display:block;width:80px}"
        "#rtl-start,#rtl-end{display:block;width:120px;direction:rtl}"
        "#rtl-start{text-align:start}#rtl-end{text-align:end}"
        "</style>"
        "<div id=cjk>\xe6\xbc\xa2\xe5\xad\x97</div>"
        "<div id=opening>\xe3\x80\x8c\xe6\xbc\xa2\xe5\xad\x97</div>"
        "<div id=zwsp>AA\xe2\x80\x8b" "BB</div>"
        "<div id=nbsp>AA\xc2\xa0" "BB</div>"
        "<div id=rtl-start>FIRST SECOND</div>"
        "<div id=rtl-end>LEFT EDGE</div>"
        "<div id=hebrew dir=rtl>\xd7\x90\xd7\x91\xd7\x92</div>";
    PocDocument document = {0};
    Stylesheet sheet = {0};
    LayoutDocument layout = {0};
    CHECK(document_parse(&document, budget, page, strlen(page), 443)
          && stylesheet_build(&sheet, budget, &document, 480)
          && layout_build(
              &layout, budget, &document, &sheet, NULL, NULL, 480));

    const DrawCommand *han = text_command(&layout, "\xe6\xbc\xa2");
    const DrawCommand *zi = text_command(&layout, "\xe5\xad\x97");
    const DrawCommand *opening = text_command(
        &layout, "\xe3\x80\x8c\xe6\xbc\xa2");
    const DrawCommand *aa = text_command(&layout, "AA");
    const DrawCommand *bb = text_command(&layout, "BB");
    const DrawCommand *nbsp = text_command(
        &layout, "AA\xc2\xa0" "BB");
    const DrawCommand *first = text_command(&layout, "FIRST");
    const DrawCommand *second = text_command(&layout, "SECOND");
    const DrawCommand *left = text_command(&layout, "LEFT");
    const DrawCommand *edge = text_command(&layout, "EDGE");
    const DrawCommand *hebrew = text_command(
        &layout, "\xd7\x90\xd7\x91\xd7\x92");
    lxb_dom_node_t *hebrew_node = find_id(
        lxb_dom_interface_node(document.html), "hebrew");
    ComputedStyle hebrew_style = style_for_node(
        &sheet, hebrew_node, NULL);
    CHECK(han != NULL && zi != NULL && zi->y > han->y
          && opening != NULL && opening->text_length == 6
          && aa != NULL && bb != NULL && bb->y > aa->y
          && nbsp != NULL && nbsp->text_length == 6
          && first != NULL && second != NULL && first->x > second->x
          && left != NULL && edge != NULL && left->x > edge->x
          && first->x + first->width > 100
          && edge->x < 60
          && hebrew != NULL && draw_command_text_rtl(hebrew)
          && computed_style_direction_rtl(&hebrew_style));
    for (size_t i = 0; i < layout.count; i++) {
        const DrawCommand *command = &layout.commands[i];
        CHECK(command->type != DRAW_TEXT
              || command->text_length != 3
              || memcmp(command->text, "\xe2\x80\x8b", 3) != 0);
    }

    layout_destroy(&layout);
    stylesheet_destroy(&sheet);
    document_destroy(&document);
    CHECK(budget->current == 0);
    return 0;
}

static int test_reuse_rejects_same_address_stylesheet_rebuild(Budget *budget)
{
    char page[16384];
    size_t used = (size_t) snprintf(
        page, sizeof(page),
        "<!doctype html><body><div class=probe>CACHE-TARGET</div>");
    for (unsigned i = 0; i < 300u && used < sizeof(page); i++) {
        int added = snprintf(
            page + used, sizeof(page) - used,
            "<div class=probe>filler-%u</div>", i);
        CHECK(added > 0 && (size_t) added < sizeof(page) - used);
        used += (size_t) added;
    }
    CHECK(used + strlen("</body>") < sizeof(page));
    memcpy(page + used, "</body>", strlen("</body>") + 1u);
    used += strlen("</body>");

    PocDocument document = {0};
    Stylesheet sheet = {0};
    LayoutDocument before = {0}, after = {0};
    LayoutReuseCache *reuse = NULL;
    ViewportContext viewport;
    static const char first_css[] =
        "body{margin:0}.probe{display:block;color:#112233}";
    static const char second_css[] =
        "body{margin:0}.probe{display:block;color:#a1b2c3}";
    CHECK(document_parse(&document, budget, page, used, 19)
          && viewport_context_init(&viewport, 480, 272, 480, 272)
          && stylesheet_build_context_deferred(
              &sheet, budget, &document, &viewport)
          && stylesheet_add_css(
              &sheet, first_css, strlen(first_css)));
    reuse = layout_reuse_cache_create(budget);
    CHECK(reuse != NULL
          && layout_build_context_reuse(
              &before, budget, &document, &sheet, NULL, NULL,
              &viewport, reuse));
    const DrawCommand *old_text = text_command(&before, "CACHE-TARGET");
    CHECK(old_text != NULL);
    uint32_t old_color = old_text->color;
    uint64_t old_generation = sheet.build_generation;
    layout_destroy(&before);

    stylesheet_destroy(&sheet);
    CHECK(stylesheet_build_context_deferred(
              &sheet, budget, &document, &viewport)
          && stylesheet_add_css(
              &sheet, second_css, strlen(second_css))
          && sheet.build_generation != old_generation
          && layout_build_context_reuse(
              &after, budget, &document, &sheet, NULL, NULL,
              &viewport, reuse));
    const DrawCommand *new_text = text_command(&after, "CACHE-TARGET");
    CHECK(new_text != NULL && new_text->color != old_color);

    layout_destroy(&after);
    layout_reuse_cache_destroy(reuse);
    stylesheet_destroy(&sheet);
    document_destroy(&document);
    CHECK(budget->current == 0);
    return 0;
}

static int test_reuse_preserves_generation_across_stylesheet_move(
    Budget *budget)
{
    static const char page[] =
        "<!doctype html><body><div class=probe>MOVE-TARGET</div></body>";
    PocDocument document = {0};
    Stylesheet source = {0}, adopted = {0};
    LayoutDocument before = {0}, after = {0};
    LayoutReuseCache *reuse = NULL;
    ViewportContext viewport;
    CHECK(document_parse(
              &document, budget, page, strlen(page), 13)
          && viewport_context_init(&viewport, 480, 272, 480, 272)
          && stylesheet_build_context_deferred(
              &source, budget, &document, &viewport)
          && stylesheet_add_css(
              &source, ".probe{color:#112233}", 22));
    reuse = layout_reuse_cache_create(budget);
    CHECK(reuse != NULL
          && layout_build_context_reuse(
              &before, budget, &document, &source, NULL, NULL,
              &viewport, reuse));
    const DrawCommand *old_text = text_command(&before, "MOVE-TARGET");
    CHECK(old_text != NULL);
    uint32_t old_color = old_text->color;
    uint64_t cached_generation = source.build_generation;
    layout_destroy(&before);

    CHECK(stylesheet_add_css(
              &source, ".probe{color:#a1b2c3}", 22)
          && source.build_generation != cached_generation);
    adopted = source;
    memset(&source, 0, sizeof(source));
    layout_reuse_cache_rebind_stylesheet(reuse, &source, &adopted);
    CHECK(layout_build_context_reuse(
              &after, budget, &document, &adopted, NULL, NULL,
              &viewport, reuse));
    const DrawCommand *new_text = text_command(&after, "MOVE-TARGET");
    CHECK(new_text != NULL && new_text->color != old_color);

    layout_destroy(&after);
    layout_reuse_cache_destroy(reuse);
    stylesheet_destroy(&adopted);
    document_destroy(&document);
    CHECK(budget->current == 0);
    return 0;
}

static int test_has_attribute_dependency_classification(Budget *budget)
{
    static const char page[] = "<!doctype html><body><div></div></body>";
    static const char css[] =
        "div.mf-collapsible-content:has(:target){display:block}"
        ".host:has(.needle) .result{color:red}"
        ".host:has([aria-expanded]){outline:1px solid}"
        ".host:has(#chosen){padding:1px}"
        ".repeat:has(.repeat){padding:2px}";
    PocDocument document = {0};
    Stylesheet sheet = {0};
    ViewportContext viewport;
    CHECK(document_parse(
              &document, budget, page, strlen(page), 23)
          && viewport_context_init(&viewport, 480, 272, 480, 272)
          && stylesheet_build_context_deferred(
              &sheet, budget, &document, &viewport)
          && stylesheet_add_css(&sheet, css, strlen(css)));

    CHECK(!stylesheet_attribute_change_may_affect_has(
              &sheet, "class", 5,
              "mf-collapsible-content", 22,
              "mf-collapsible-content open-block", 33)
          && stylesheet_attribute_change_may_affect_has(
              &sheet, "class", 5, "needle", 6, "", 0)
          && stylesheet_attribute_change_may_affect_has(
              &sheet, "aria-expanded", 13, NULL, 0, "true", 4)
          && stylesheet_attribute_change_may_affect_has(
              &sheet, "id", 2, "other", 5, "chosen", 6)
          && stylesheet_attribute_change_may_affect_has(
              &sheet, "class", 5, "plain", 5, "repeat", 6)
          && !stylesheet_attribute_change_may_affect_has(
              &sheet, "title", 5, "before", 6, "after", 5)
          && !stylesheet_attribute_change_may_affect_has(
              &sheet, "class", 5, "same tokens", 11,
              "same   tokens", 13));
    static const char escaped_css[] =
        ".host:has(.escaped\\:token){margin:1px}";
    CHECK(stylesheet_add_css(
              &sheet, escaped_css, sizeof(escaped_css) - 1)
          && stylesheet_attribute_change_may_affect_has(
              &sheet, "class", 5, "plain", 5, "escaped:token", 13)
          && !stylesheet_attribute_change_may_affect_has(
              &sheet, "class", 5, "plain", 5, "different", 9));

    stylesheet_destroy(&sheet);
    document_destroy(&document);
    CHECK(budget->current == 0);
    return 0;
}

static int test_level_four_selector_matching(Budget *budget)
{
    static const char page[] =
        "<!doctype html><body>"
        "<section id=subject class=subject data-mode=LOUD>"
        "<div class=a></div><div class='b eligible'></div>"
        "<div class=eligible></div><div class=nest>"
        "<b id=deep-selector class=deepchild></b>"
        "</div></section>"
        "<section id=adjacent class=next><i class=child></i></section>"
        "<section id=later><div><b class=deep></b></div></section>"
        "<div id=pair-a class=pair></div>"
        "<div id=pair-b class='pair chosen'></div>"
        "</body>";
    PocDocument document = {0};
    CHECK(document_parse(
              &document, budget, page, strlen(page), 37));
    lxb_dom_node_t *root = lxb_dom_interface_node(document.html);
    lxb_dom_node_t *subject = find_id(root, "subject");
    lxb_dom_node_t *pair_b = find_id(root, "pair-b");
    lxb_dom_node_t *deep_selector = find_id(root, "deep-selector");
    lxb_dom_node_t *eligible = subject == NULL ? NULL
        : subject->first_child == NULL ? NULL
        : subject->first_child->next;
    CHECK(subject != NULL && pair_b != NULL && deep_selector != NULL
          && eligible != NULL);
    CHECK(style_selector_matches(
        subject, ".subject:has(> .a + .b)",
        strlen(".subject:has(> .a + .b)")));
    CHECK(style_selector_matches(
        subject, ".subject:has(> .nest > .deepchild)",
        strlen(".subject:has(> .nest > .deepchild)")));
    CHECK(style_selector_matches(
        subject, ".subject:has(+ .next > .child)",
        strlen(".subject:has(+ .next > .child)")));
    CHECK(style_selector_matches(
        subject, ".subject:has(~ #later .deep)",
        strlen(".subject:has(~ #later .deep)")));
    CHECK(style_selector_matches(
        pair_b, ":is(.pair + .chosen, .missing)",
        strlen(":is(.pair + .chosen, .missing)")));
    CHECK(!style_selector_matches(
        pair_b, ":not(.pair + .chosen)",
        strlen(":not(.pair + .chosen)")));
    CHECK(style_selector_matches(
        subject, "[data-mode='loud' i]",
        strlen("[data-mode='loud' i]")));
    CHECK(!style_selector_matches(
        subject, "[data-mode='loud' s]",
        strlen("[data-mode='loud' s]")));
    CHECK(style_selector_matches(
        subject, "[data\\-mode='LO\\55 D' s]",
        strlen("[data\\-mode='LO\\55 D' s]")));
    CHECK(style_selector_matches(
        pair_b, ".cho\\sen",
        strlen(".cho\\sen")));
    CHECK(style_selector_matches(
        eligible, ".eligible:nth-child(odd of .eligible)",
        strlen(".eligible:nth-child(odd of .eligible)")));
    size_t selector_memory = budget->current;
    for (size_t iteration = 0; iteration < 1000; iteration++) {
        CHECK(style_selector_matches(
            subject, ".subject:has(> .a + .b)",
            strlen(".subject:has(> .a + .b)")));
    }
    CHECK(budget->current == selector_memory);

    Stylesheet sheet = {0};
    static const char cached_css[] =
        ".subject:has(.deepchild){color:#123456}"
        ".subject:has(.deepchild){padding-left:7px}";
    CHECK(stylesheet_build(&sheet, budget, &document, 480)
          && stylesheet_add_css(
              &sheet, cached_css, sizeof(cached_css) - 1));
    uint64_t queries_before = sheet.selector_relative_queries;
    uint64_t hits_before = sheet.selector_relative_cache_hits;
    ComputedStyle cached_style = style_for_node(&sheet, subject, NULL);
    CHECK(cached_style.color == 0x123456
          && cached_style.padding.left == 7
          && sheet.selector_relative_queries >= queries_before + 2
          && sheet.selector_relative_cache_hits >= hits_before + 1
          && sheet.relative_selector_cache_depth == 0);
    CHECK(lxb_dom_element_set_attribute(
              lxb_dom_interface_element(deep_selector),
              (const lxb_char_t *) "class", 5,
              (const lxb_char_t *) "gone", 4) != NULL);
    ComputedStyle changed_style = style_for_node(&sheet, subject, NULL);
    CHECK(changed_style.color != 0x123456
          && changed_style.padding.left != 7
          && sheet.relative_selector_cache_depth == 0);
    stylesheet_destroy(&sheet);

    document_destroy(&document);
    CHECK(budget->current == 0);
    return 0;
}

static int test_selector_walk_bounds(Budget *budget)
{
    char page[4096];
    size_t page_used = (size_t) snprintf(
        page, sizeof(page), "<!doctype html><body><div id=root>");
    for (size_t depth = 0; depth < 70 && page_used < sizeof(page); depth++) {
        page_used += (size_t) snprintf(
            page + page_used, sizeof(page) - page_used, "<div>");
    }
    page_used += (size_t) snprintf(
        page + page_used, sizeof(page) - page_used, "<b id=deep></b>");
    for (size_t depth = 0; depth < 70 && page_used < sizeof(page); depth++) {
        page_used += (size_t) snprintf(
            page + page_used, sizeof(page) - page_used, "</div>");
    }
    page_used += (size_t) snprintf(
        page + page_used, sizeof(page) - page_used, "</div></body>");
    CHECK(page_used < sizeof(page));

    PocDocument document = {0};
    CHECK(document_parse(&document, budget, page, page_used, 38));
    lxb_dom_node_t *tree = lxb_dom_interface_node(document.html);
    lxb_dom_node_t *root = find_id(tree, "root");
    lxb_dom_node_t *deep = find_id(tree, "deep");
    CHECK(root != NULL && deep != NULL);

    char relative[512] = "#root:has(";
    size_t relative_used = strlen(relative);
    char ancestry[512] = "#root";
    size_t ancestry_used = strlen(ancestry);
    for (size_t depth = 0; depth < 70; depth++) {
        relative_used += (size_t) snprintf(
            relative + relative_used, sizeof(relative) - relative_used,
            ">div");
        ancestry_used += (size_t) snprintf(
            ancestry + ancestry_used, sizeof(ancestry) - ancestry_used,
            " > div");
    }
    relative_used += (size_t) snprintf(
        relative + relative_used, sizeof(relative) - relative_used,
        ">#deep)");
    ancestry_used += (size_t) snprintf(
        ancestry + ancestry_used, sizeof(ancestry) - ancestry_used,
        " > #deep");
    CHECK(relative_used < sizeof(relative)
          && ancestry_used < sizeof(ancestry)
          && !style_selector_matches(root, relative, relative_used)
          && !style_selector_matches(deep, ancestry, ancestry_used));

    document_destroy(&document);
    CHECK(budget->current == 0);
    return 0;
}

static int test_has_unrelated_scoped_reuse_converges(Budget *budget)
{
    static const char page[] =
        "<!doctype html><body><div class=heading></div>"
        "<div id=content><span id=target>TARGET</span></div>"
        "<p>UNCHANGED</p></body>";
    static const char css[] =
        ".heading:has([aria-expanded=true]) + "
        ".mf-collapsible-content #target{color:#aabbcc}"
        ".mf-collapsible-content #target{margin-left:17px}";
    PocDocument document = {0};
    Stylesheet sheet = {0};
    LayoutDocument initial = {0}, reused = {0}, clean = {0};
    LayoutReuseCache *reuse = NULL;
    ViewportContext viewport;
    CHECK(document_parse(
              &document, budget, page, strlen(page), 29)
          && viewport_context_init(&viewport, 480, 272, 480, 272)
          && stylesheet_build_context_deferred(
              &sheet, budget, &document, &viewport)
          && stylesheet_add_css(&sheet, css, strlen(css)));
    lxb_dom_node_t *content = find_id(
        lxb_dom_interface_node(document.html), "content");
    CHECK(content != NULL);
    reuse = layout_reuse_cache_create(budget);
    CHECK(reuse != NULL
          && layout_build_context_reuse(
              &initial, budget, &document, &sheet, NULL, NULL,
              &viewport, reuse));
    layout_destroy(&initial);

    static const char class_name[] = "mf-collapsible-content";
    CHECK(lxb_dom_element_set_attribute(
              lxb_dom_interface_element(content),
              (const lxb_char_t *) "class", 5,
              (const lxb_char_t *) class_name,
              sizeof(class_name) - 1) != NULL
          && !stylesheet_attribute_change_may_affect_has(
              &sheet, "class", 5, NULL, 0,
              class_name, sizeof(class_name) - 1));
    layout_reuse_cache_invalidate_node_scoped(reuse, content, false);
    CHECK(layout_build_context_reuse(
              &reused, budget, &document, &sheet, NULL, NULL,
              &viewport, reuse)
          && layout_build_context(
              &clean, budget, &document, &sheet, NULL, NULL, &viewport));
    const DrawCommand *reused_target = text_command(&reused, "TARGET");
    const DrawCommand *clean_target = text_command(&clean, "TARGET");
    const DrawCommand *reused_unchanged =
        text_command(&reused, "UNCHANGED");
    const DrawCommand *clean_unchanged = text_command(&clean, "UNCHANGED");
    CHECK(reused_target != NULL && clean_target != NULL
          && reused_unchanged != NULL && clean_unchanged != NULL
          && reused_target->x == clean_target->x
          && reused_target->y == clean_target->y
          && reused_target->color == clean_target->color
          && reused_unchanged->x == clean_unchanged->x
          && reused_unchanged->y == clean_unchanged->y
          && reused_unchanged->color == clean_unchanged->color);

    layout_destroy(&clean);
    layout_destroy(&reused);
    layout_reuse_cache_destroy(reuse);
    stylesheet_destroy(&sheet);
    document_destroy(&document);
    CHECK(budget->current == 0);
    return 0;
}

static int test_bounded_transformed_text_and_radius(Budget *budget)
{
    static const char page[] =
        "<!doctype html><style>body{margin:0;padding:0}"
        ".scaled{display:block;width:64px;height:48px;"
        "transform:scale(3.98)}"
        "#hostile{overflow:hidden;border-radius:32767px;"
        "background:#123456;font-size:64px;text-decoration:underline}"
        "</style><body><div class=scaled><div class=scaled>"
        "<div class=scaled><div class=scaled><div class=scaled>"
        "<div class=scaled><div class=scaled><div id=hostile class=scaled>"
        "BOUNDED</div></div></div></div></div></div></div></body>";
    PocDocument document = {0};
    Stylesheet sheet = {0};
    LayoutDocument layout = {0};
    TileCache cache = {0};
    uint16_t *frame = NULL;
    CHECK(document_parse(&document, budget, page, strlen(page), 23)
          && stylesheet_build(&sheet, budget, &document, 64)
          && layout_build(&layout, budget, &document, &sheet,
                          NULL, NULL, 64));
    const DrawCommand *text = text_command(&layout, "BOUNDED");
    const LayoutNodeBox *hostile = layout_box_for_node(
        &layout, find_id(lxb_dom_interface_node(document.html), "hostile"));
    CHECK(text != NULL && hostile != NULL
          && text->font_size > 0
          && text->font_size <= TILEFINCH_FONT_RASTER_PIXEL_LIMIT
          && text->scale > 0
          && text->scale <= TILEFINCH_BITMAP_FONT_SCALE_LIMIT
          && layout_node_box_clip_radius(hostile)
             == LAYOUT_CLIP_RADIUS_MASK);
    frame = budget_malloc(budget, 64u * 64u * sizeof(*frame));
    CHECK(frame != NULL && tile_cache_init(&cache, budget, &layout, 4)
          && tile_cache_set_frame(&cache, frame, 64u * 64u)
          && tile_cache_render_frame(&cache, 0, 64, 64, NULL));
    tile_cache_destroy(&cache);
    budget_free(budget, frame);
    layout_destroy(&layout);
    stylesheet_destroy(&sheet);
    document_destroy(&document);
    CHECK(budget->current == 0);
    return 0;
}
static int test_css_table_implicit_row(Budget *budget)
{
    static const char page[] =
        "<!doctype html><style>body{margin:0;padding:0}.action{display:table;"
        "max-width:410px}.icon,.copy{display:table-cell}.icon{width:56px;"
        "height:40px}.copy{font-size:19px;line-height:25px}"
        ".search{display:table;width:432px}.entry{display:block}"
        ".entry input{display:block;width:100%;height:50px}"
        ".submit{display:table-cell;width:1%}.submit button{display:block;"
        "width:50px;height:50px}</style>"
        "<body><div id=action class=action><span id=icon class=icon></span>"
        "<span id=copy class=copy>ACTION LINK</span></div>"
        "<div id=search class=search><div id=entry class=entry>"
        "<input></div><span id=submit class=submit><button></button>"
        "</span></div></body>";
    PocDocument document = {0};
    Stylesheet sheet = {0};
    LayoutDocument layout = {0};
    CHECK(document_parse(&document, budget, page, strlen(page), 19)
          && stylesheet_build(&sheet, budget, &document, 480)
          && layout_build(&layout, budget, &document, &sheet,
                          NULL, NULL, 480));
    lxb_dom_node_t *root = lxb_dom_interface_node(document.html);
    const LayoutNodeBox *action = layout_box_for_node(
        &layout, find_id(root, "action"));
    const LayoutNodeBox *icon = layout_box_for_node(
        &layout, find_id(root, "icon"));
    const LayoutNodeBox *copy = layout_box_for_node(
        &layout, find_id(root, "copy"));
    const LayoutNodeBox *search = layout_box_for_node(
        &layout, find_id(root, "search"));
    const LayoutNodeBox *entry = layout_box_for_node(
        &layout, find_id(root, "entry"));
    const LayoutNodeBox *submit = layout_box_for_node(
        &layout, find_id(root, "submit"));
    CHECK(action != NULL && icon != NULL && copy != NULL
          && icon->x == action->x && icon->width == 56
          && copy->x == icon->x + icon->width
          && copy->y == icon->y
          && action->height == 40
          && search != NULL && entry != NULL && submit != NULL
          && entry->x == search->x && entry->width == 382
          && submit->x == entry->x + entry->width
          && submit->width == 50 && submit->y == entry->y
          && search->height == 50);
    layout_destroy(&layout);
    stylesheet_destroy(&sheet);
    document_destroy(&document);
    CHECK(budget->current == 0);
    return 0;
}
static int test_padding_scroll_extent(Budget *budget)
{
    static const char page[] =
        "<!doctype html><style>body{margin:0;padding:0}"
        "#padded{padding-bottom:15px}</style>"
        "<body><div id=padded></div></body>";
    PocDocument document = {0};
    Stylesheet sheet = {0};
    LayoutDocument layout = {0};
    CHECK(document_parse(&document, budget, page, strlen(page), 19)
          && stylesheet_build(&sheet, budget, &document, 480)
          && layout_build(&layout, budget, &document, &sheet,
                          NULL, NULL, 480));
    const LayoutNodeBox *padded = layout_box_for_node(
        &layout, find_id(lxb_dom_interface_node(document.html), "padded"));
    CHECK(padded != NULL && padded->height == 15
          && padded->content_height == 15 && layout.height == 272);
    layout_destroy(&layout);
    stylesheet_destroy(&sheet);
    document_destroy(&document);
    CHECK(budget->current == 0);
    return 0;
}
static int test_vertical_margin_collapse(Budget *budget)
{
    static const char page[] =
        "<!doctype html><style>body{margin:0;padding:0}"
        ".boundary{padding-top:1px;padding-bottom:1px}"
        ".ten{height:10px}.twenty{height:20px}"
        "#positive-a{margin-bottom:30px}"
        "#positive-b{margin-top:50px}"
        "#mixed-a{margin-bottom:20px}"
        "#mixed-empty{height:0;margin-top:-8px;margin-bottom:35px}"
        "#mixed-b{margin-top:-12px}"
        "#parent{margin-top:20px;margin-bottom:25px}"
        "#parent-first{margin-top:30px}"
        "#parent-last{margin-bottom:40px}"
        "#flex-host{display:flex;flex-direction:column}"
        "#flex-a{margin-bottom:30px}"
        "#flex-b{margin-top:50px}"
        "#max-height-parent{max-height:50px}"
        "#max-height-child{height:51px;margin-bottom:10px}"
        "</style><body>"
        "<div id=positive-host class=boundary>"
        "<div id=positive-a class=ten></div>"
        "<div id=positive-b class=twenty></div></div>"
        "<div id=mixed-host class=boundary>"
        "<div id=mixed-a class=ten></div>"
        "<div id=mixed-empty></div>"
        "<div id=mixed-b class=ten></div></div>"
        "<div id=parent-host class=boundary>"
        "<div id=parent-before class=ten></div>"
        "<div id=parent><div id=parent-first class=ten></div>"
        "<div id=parent-last class=ten></div></div>"
        "<div id=parent-after class=ten></div></div>"
        "<div id=flex-host class=boundary>"
        "<div id=flex-a class=ten></div>"
        "<div id=flex-b class=twenty></div></div>"
        "<div id=empty-parent-host class=boundary>"
        "<figure id=empty-parent style=\"margin:0\">"
        "<div id=empty-child style=\"height:0;margin:10px 0\"></div>"
        "</figure></div>"
        "<div id=max-height-host class=boundary>"
        "<div id=max-height-parent><div id=max-height-child></div></div>"
        "<div id=max-height-after class=twenty></div></div>"
        "</body>";
    PocDocument document = {0};
    Stylesheet sheet = {0};
    LayoutDocument layout = {0};
    CHECK(document_parse(&document, budget, page, strlen(page), 19)
          && stylesheet_build(&sheet, budget, &document, 480)
          && layout_build(&layout, budget, &document, &sheet,
                          NULL, NULL, 480));
    lxb_dom_node_t *root = lxb_dom_interface_node(document.html);
    const LayoutNodeBox *positive_host = layout_box_for_node(
        &layout, find_id(root, "positive-host"));
    const LayoutNodeBox *positive_a = layout_box_for_node(
        &layout, find_id(root, "positive-a"));
    const LayoutNodeBox *positive_b = layout_box_for_node(
        &layout, find_id(root, "positive-b"));
    const LayoutNodeBox *mixed_host = layout_box_for_node(
        &layout, find_id(root, "mixed-host"));
    const LayoutNodeBox *mixed_a = layout_box_for_node(
        &layout, find_id(root, "mixed-a"));
    const LayoutNodeBox *mixed_empty = layout_box_for_node(
        &layout, find_id(root, "mixed-empty"));
    const LayoutNodeBox *mixed_b = layout_box_for_node(
        &layout, find_id(root, "mixed-b"));
    const LayoutNodeBox *parent_host = layout_box_for_node(
        &layout, find_id(root, "parent-host"));
    const LayoutNodeBox *parent_before = layout_box_for_node(
        &layout, find_id(root, "parent-before"));
    const LayoutNodeBox *parent = layout_box_for_node(
        &layout, find_id(root, "parent"));
    const LayoutNodeBox *parent_first = layout_box_for_node(
        &layout, find_id(root, "parent-first"));
    const LayoutNodeBox *parent_last = layout_box_for_node(
        &layout, find_id(root, "parent-last"));
    const LayoutNodeBox *parent_after = layout_box_for_node(
        &layout, find_id(root, "parent-after"));
    const LayoutNodeBox *flex_host = layout_box_for_node(
        &layout, find_id(root, "flex-host"));
    const LayoutNodeBox *flex_a = layout_box_for_node(
        &layout, find_id(root, "flex-a"));
    const LayoutNodeBox *flex_b = layout_box_for_node(
        &layout, find_id(root, "flex-b"));
    const LayoutNodeBox *empty_parent = layout_box_for_node(
        &layout, find_id(root, "empty-parent"));
    const LayoutNodeBox *empty_child = layout_box_for_node(
        &layout, find_id(root, "empty-child"));
    const LayoutNodeBox *max_height_parent = layout_box_for_node(
        &layout, find_id(root, "max-height-parent"));
    const LayoutNodeBox *max_height_child = layout_box_for_node(
        &layout, find_id(root, "max-height-child"));
    const LayoutNodeBox *max_height_after = layout_box_for_node(
        &layout, find_id(root, "max-height-after"));

    /* Adjacent positive margins collapse to the larger margin, not their
       sum. The boundary padding prevents either margin escaping the host. */
    CHECK(positive_host != NULL && positive_a != NULL && positive_b != NULL
          && positive_a->y == positive_host->y + 1
          && positive_b->y == positive_a->y + positive_a->height + 50
          && positive_host->height == 82);

    /* An empty block joins both of its margins to the adjoining set. One
       collapsed set uses the greatest positive and most-negative extrema:
       max(20, 35) + min(-8, -12) == 23. */
    CHECK(mixed_host != NULL && mixed_a != NULL && mixed_empty != NULL
          && mixed_b != NULL && mixed_empty->height == 0
          && mixed_a->y == mixed_host->y + 1
          && mixed_b->y == mixed_a->y + mixed_a->height + 23
          && mixed_host->height == 45);

    /* Flex items never collapse margins, including in a column container. */
    CHECK(flex_host != NULL && flex_a != NULL && flex_b != NULL
          && flex_a->y == flex_host->y + 1
          && flex_b->y == flex_a->y + flex_a->height + 80
          && flex_host->height == 112);

    /* A single empty descendant exposes one collapsed margin set through an
       empty parent; neither border box may acquire a negative height. */
    CHECK(empty_parent != NULL && empty_child != NULL
          && empty_parent->height == 0 && empty_child->height == 0
          && empty_parent->y == empty_child->y);

    /* A max-height that actually clamps auto content establishes the
       parent's flow bottom. The overflowing child still paints at 51px, but
       its 10px end margin cannot push the following sibling past 50px. */
    CHECK(max_height_parent != NULL && max_height_child != NULL
          && max_height_after != NULL
          && max_height_parent->height == 50
          && max_height_child->height == 51
          && max_height_after->y
                 == max_height_parent->y + max_height_parent->height);

    /* A borderless, padding-free parent collapses with its first and last
       in-flow children. The child extrema therefore establish the 30px and
       40px gaps outside the parent's 20px content box. */
    CHECK(parent_host != NULL && parent_before != NULL && parent != NULL
          && parent_first != NULL && parent_last != NULL
          && parent_after != NULL
          && parent->y == parent_before->y + parent_before->height + 30
          && parent_first->y == parent->y
          && parent_last->y == parent_first->y + parent_first->height
          && parent->height == 20
          && parent_after->y == parent->y + parent->height + 40
          && parent_host->height == 112);
    layout_destroy(&layout);
    stylesheet_destroy(&sheet);
    document_destroy(&document);
    CHECK(budget->current == 0);
    return 0;
}
static int test_reference_resolved_css_math(Budget *budget)
{
    static const char page[] =
        "<!doctype html><style>body{margin:0;padding:0}"
        ".clamp-host{height:1px}.clamp-child{height:1px;"
        "width:clamp(130px,39.4%,159px)}"
        "#clamp-300{width:300px}#clamp-400{width:400px}"
        "#clamp-500{width:500px}"
        "#clamp-inverted-host{width:300px;height:1px}"
        "#clamp-inverted{width:clamp(80%,50%,20%);height:1px}"
        "#basis-host{display:flex;width:398px;height:1px}"
        ".basis-child{height:1px;min-width:0;"
        "flex:0 0 calc((100% - (3 * 16px)) / 3.5)}"
        "#aspect-host{width:320px}#aspect{--source-width:16;"
        "--source-height:9;width:320px;height:0;"
        "padding-bottom:calc(100% * (var(--source-height) / "
        "var(--source-width)))}"
        "#supports-host{width:480px}"
        "@supports (height:var(--source-height)){#supports-ratio{height:0;"
        "position:relative;"
        "padding-bottom:calc(100%*(var(--source-height)/"
        "var(--source-width)))}}"
        "#nested-positioned-fill{display:block;position:absolute;left:0;top:0;"
        "width:100%;height:100%;background:#123456}"
        "#height-definite-host{height:400px}"
        ".mixed-height{height:calc(100% - 155px)}"
        "#height-indefinite-content{height:23px}"
        "#zero-host{width:240px}#width-zero{width:0;height:1px}"
        "#width-auto{width:auto;height:1px}"
        "#max-zero{width:200px;max-width:0;height:1px}"
        "#max-none{width:200px;max-width:none;height:1px}"
        "#negative-width{width:calc(10% - 100px);height:1px}"
        "#negative-min{width:20px;min-width:calc(10% - 100px);height:1px}"
        "#negative-max{width:20px;max-width:calc(10% - 100px);height:1px}"
        "#negative-flex-host{display:flex;width:100px;height:1px}"
        "#negative-flex{min-width:0;flex:0 0 calc(10% - 100px);height:1px}"
        "#invalid-typed{width:73px;width:calc(2px * 3px);height:1px}"
        "#invalid-unit{width:74px;width:12qu;height:1px}"
        "#invalid-divide{width:75px;width:calc(20px / 0);height:1px}"
        "#bounded-depth{width:61px;width:calc(calc(calc(calc(calc(calc("
        "calc(calc(calc(calc(calc(calc(1px))))))))))));height:1px}"
        "#bounded-arguments{width:62px;width:min(1px,2px,3px,4px,5px,"
        "6px,7px,8px,9px,10px,11px,12px);height:1px}"
        "</style><body>"
        "<div id=clamp-300 class=clamp-host><div id=clamp-300-child "
        "class=clamp-child></div></div>"
        "<div id=clamp-400 class=clamp-host><div id=clamp-400-child "
        "class=clamp-child></div></div>"
        "<div id=clamp-500 class=clamp-host><div id=clamp-500-child "
        "class=clamp-child></div></div>"
        "<div id=clamp-inverted-host><div id=clamp-inverted></div></div>"
        "<div id=basis-host><div id=basis-a class=basis-child></div>"
        "<div id=basis-b class=basis-child></div></div>"
        "<div id=aspect-host><div id=aspect></div></div>"
        "<div id=supports-host><div id=supports-ratio "
        "style='--source-width:8197;--source-height:4611'>"
        "<a><picture><span id=nested-positioned-fill></span></picture></a>"
        "</div></div>"
        "<div id=height-definite-host><div id=height-definite "
        "class=mixed-height></div></div>"
        "<div id=height-indefinite-host><div id=height-indefinite "
        "class=mixed-height><div id=height-indefinite-content></div>"
        "</div></div>"
        "<div id=zero-host><div id=width-zero></div>"
        "<div id=width-auto></div><div id=max-zero></div>"
        "<div id=max-none></div></div>"
        "<div id=negative-width></div><div id=negative-min></div>"
        "<div id=negative-max></div><div id=negative-flex-host>"
        "<div id=negative-flex></div></div>"
        "<div id=invalid-typed></div><div id=invalid-unit></div>"
        "<div id=invalid-divide></div><div id=bounded-depth></div>"
        "<div id=bounded-arguments></div>"
        "</body>";
    PocDocument document = {0};
    Stylesheet sheet = {0};
    LayoutDocument layout = {0};
    CHECK(document_parse(&document, budget, page, strlen(page), 23)
          && stylesheet_build(&sheet, budget, &document, 520)
          && layout_build(&layout, budget, &document, &sheet,
                          NULL, NULL, 520));
    lxb_dom_node_t *root = lxb_dom_interface_node(document.html);
    const LayoutNodeBox *clamp_300 = layout_box_for_node(
        &layout, find_id(root, "clamp-300-child"));
    const LayoutNodeBox *clamp_400 = layout_box_for_node(
        &layout, find_id(root, "clamp-400-child"));
    const LayoutNodeBox *clamp_500 = layout_box_for_node(
        &layout, find_id(root, "clamp-500-child"));
    const LayoutNodeBox *clamp_inverted = layout_box_for_node(
        &layout, find_id(root, "clamp-inverted"));
    const LayoutNodeBox *basis_a = layout_box_for_node(
        &layout, find_id(root, "basis-a"));
    const LayoutNodeBox *basis_b = layout_box_for_node(
        &layout, find_id(root, "basis-b"));
    const LayoutNodeBox *aspect = layout_box_for_node(
        &layout, find_id(root, "aspect"));
    const LayoutNodeBox *supports_ratio = layout_box_for_node(
        &layout, find_id(root, "supports-ratio"));
    const LayoutNodeBox *nested_positioned_fill = layout_box_for_node(
        &layout, find_id(root, "nested-positioned-fill"));
    const LayoutNodeBox *height_definite = layout_box_for_node(
        &layout, find_id(root, "height-definite"));
    const LayoutNodeBox *height_indefinite = layout_box_for_node(
        &layout, find_id(root, "height-indefinite"));
    const LayoutNodeBox *width_zero = layout_box_for_node(
        &layout, find_id(root, "width-zero"));
    const LayoutNodeBox *width_auto = layout_box_for_node(
        &layout, find_id(root, "width-auto"));
    const LayoutNodeBox *max_zero = layout_box_for_node(
        &layout, find_id(root, "max-zero"));
    const LayoutNodeBox *max_none = layout_box_for_node(
        &layout, find_id(root, "max-none"));
    const LayoutNodeBox *negative_width = layout_box_for_node(
        &layout, find_id(root, "negative-width"));
    const LayoutNodeBox *negative_min = layout_box_for_node(
        &layout, find_id(root, "negative-min"));
    const LayoutNodeBox *negative_max = layout_box_for_node(
        &layout, find_id(root, "negative-max"));
    const LayoutNodeBox *negative_flex = layout_box_for_node(
        &layout, find_id(root, "negative-flex"));
    const LayoutNodeBox *invalid_typed = layout_box_for_node(
        &layout, find_id(root, "invalid-typed"));
    const LayoutNodeBox *invalid_unit = layout_box_for_node(
        &layout, find_id(root, "invalid-unit"));
    const LayoutNodeBox *invalid_divide = layout_box_for_node(
        &layout, find_id(root, "invalid-divide"));
    const LayoutNodeBox *bounded_depth = layout_box_for_node(
        &layout, find_id(root, "bounded-depth"));
    const LayoutNodeBox *bounded_arguments = layout_box_for_node(
        &layout, find_id(root, "bounded-arguments"));

    /* The percentage arm remains reference-dependent until used-value time. */
    CHECK(clamp_300 != NULL && clamp_300->width == 130
          && clamp_400 != NULL && clamp_400->width == 158
          && clamp_500 != NULL && clamp_500->width == 159
          /* CSS clamp is max(MIN, min(VAL, MAX)); MIN wins when its
             bound is greater than MAX. */
          && clamp_inverted != NULL && clamp_inverted->width == 240);

    /* (398 - 3 * 16) / 3.5 is exactly 100 CSS pixels. */
    CHECK(basis_a != NULL && basis_b != NULL
          && basis_a->width == 100 && basis_b->width == 100
          && basis_b->x == basis_a->x + 100);

    /* Block-axis percentage padding uses the containing inline width. */
    CHECK(aspect != NULL && aspect->width == 320 && aspect->height == 180);

    /* Supports validates a pending var() declaration syntactically; the
       guarded math resolves later from the matched element's inline custom
       properties. */
    CHECK(stylesheet_supports_matches(
              &sheet, "(height:var(--unresolved))",
              strlen("(height:var(--unresolved))"))
          && !stylesheet_supports_matches(
              &sheet, "(tilefinch-unknown:var(--unresolved))",
              strlen("(tilefinch-unknown:var(--unresolved))"))
          && supports_ratio != NULL && supports_ratio->width == 480
          && supports_ratio->height == 270
          && nested_positioned_fill != NULL
          && nested_positioned_fill->x == supports_ratio->x
          && nested_positioned_fill->y == supports_ratio->y
          && nested_positioned_fill->width == 480
          && nested_positioned_fill->height == 270);
    bool painted_positioned_fill = false;
    if (nested_positioned_fill != NULL) {
        for (size_t i = nested_positioned_fill->command_start;
             i < nested_positioned_fill->command_end && i < layout.count;
             i++) {
            const DrawCommand *command = &layout.commands[i];
            if (command->type == DRAW_FILL_RECT
                && command->color == 0x123456
                && command->width == 480 && command->height == 270) {
                painted_positioned_fill = true;
            }
        }
    }
    CHECK(painted_positioned_fill);
    CHECK(sheet.math_program_count > 0 && sheet.math_program_count <= 256
          && sheet.math_instruction_count > 0
          && sheet.math_instruction_count <= 2048);
    size_t interned_programs = sheet.math_program_count;
    size_t interned_instructions = sheet.math_instruction_count;
    for (unsigned probe = 0; probe < 300; probe++) {
        char condition[96];
        int written = snprintf(condition, sizeof(condition),
                               "(width:calc(100%% - %upx))", probe + 1);
        CHECK(written > 0 && (size_t) written < sizeof(condition)
              && stylesheet_supports_matches(
                  &sheet, condition, (size_t) written));
    }
    CHECK(sheet.math_program_count == interned_programs
          && sheet.math_instruction_count == interned_instructions);
    lxb_dom_node_t *supports_ratio_node = find_id(root, "supports-ratio");
    CHECK(supports_ratio_node != NULL);
    (void) style_for_node(&sheet, supports_ratio_node, NULL);
    (void) style_for_node(&sheet, supports_ratio_node, NULL);
    CHECK(sheet.math_program_count == interned_programs
          && sheet.math_instruction_count == interned_instructions);

    /* A percentage height resolves only from a definite block-size. */
    CHECK(height_definite != NULL && height_definite->height == 245
          && height_indefinite != NULL && height_indefinite->height == 23);

    /* Zero is a valid used value, distinct from auto and from max:none. */
    CHECK(width_zero != NULL && width_zero->width == 0
          && width_auto != NULL && width_auto->width == 240
          && max_zero != NULL && max_zero->width == 0
          && max_none != NULL && max_none->width == 200);
    CHECK(negative_width != NULL && negative_width->width == 0
          && negative_min != NULL && negative_min->width == 20
          && negative_max != NULL && negative_max->width == 0
          && negative_flex != NULL && negative_flex->width == 8);

    /* Invalid later declarations do not partially commit over valid ones. */
    CHECK(invalid_typed != NULL && invalid_typed->width == 73
          && invalid_unit != NULL && invalid_unit->width == 74
          && invalid_divide != NULL && invalid_divide->width == 75);

    /* Deliberately bounded parser inputs are rejected transactionally. */
    CHECK(bounded_depth != NULL && bounded_depth->width == 61
          && bounded_arguments != NULL && bounded_arguments->width == 62);

    layout_destroy(&layout);
    stylesheet_destroy(&sheet);
    document_destroy(&document);
    CHECK(budget->current == 0);
    return 0;
}
static int test_bounded_large_layout_values(Budget *budget)
{
    enum { TALL_BLOCK_COUNT = 2100, EXTENT_LIMIT = 1048575 };
    static const char prefix[] =
        "<!doctype html><style>body{margin:0}.tall{height:1048575px}"
        "#large-ratio{display:block;width:1048575px}</style><body>";
    static const char block[] = "<div class=tall></div>";
    static const char tail[] =
        "<div id=last-tall class=tall></div><img id=large-ratio></body>";
    size_t page_length = sizeof(prefix) - 1
        + (TALL_BLOCK_COUNT - 1) * (sizeof(block) - 1)
        + sizeof(tail) - 1;
    char *page = malloc(page_length + 1);
    CHECK(page != NULL);
    size_t at = 0;
    memcpy(page + at, prefix, sizeof(prefix) - 1);
    at += sizeof(prefix) - 1;
    for (size_t i = 1; i < TALL_BLOCK_COUNT; i++) {
        memcpy(page + at, block, sizeof(block) - 1);
        at += sizeof(block) - 1;
    }
    memcpy(page + at, tail, sizeof(tail) - 1);
    at += sizeof(tail) - 1;
    page[at] = '\0';
    CHECK(at == page_length);

    PocDocument document = {0};
    Stylesheet sheet = {0};
    LayoutDocument layout = {0};
    CHECK(document_parse(&document, budget, page, page_length, 257)
          && stylesheet_build(&sheet, budget, &document, 480));
    lxb_dom_node_t *root = lxb_dom_interface_node(document.html);
    lxb_dom_node_t *last_tall = find_id(root, "last-tall");
    lxb_dom_node_t *large_ratio = find_id(root, "large-ratio");
    static unsigned char rgba[] = {0x12, 0x34, 0x56, 0xff};
    ImageResource item = {
        .node = large_ratio, .pixels = rgba,
        .width = 1, .height = EXTENT_LIMIT
    };
    ImageResources images = {.items = &item, .count = 1};
    CHECK(last_tall != NULL && large_ratio != NULL
          && layout_build(&layout, budget, &document, &sheet,
                          NULL, &images, 480));
    const LayoutNodeBox *last_tall_box = layout_box_for_node(
        &layout, last_tall);
    const DrawCommand *large_ratio_command = NULL;
    for (size_t i = 0; i < layout.count; i++) {
        const DrawCommand *command = &layout.commands[i];
        CHECK(command->x >= -EXTENT_LIMIT && command->x <= EXTENT_LIMIT
              && command->y >= -EXTENT_LIMIT && command->y <= EXTENT_LIMIT
              && command->width >= 0 && command->width <= EXTENT_LIMIT
              && command->height >= 0 && command->height <= EXTENT_LIMIT);
        if (command->type == DRAW_IMAGE && command->image == &item) {
            large_ratio_command = command;
        }
    }
    for (size_t i = 0; i < layout.node_box_count; i++) {
        const LayoutNodeBox *box = &layout.node_boxes[i];
        CHECK(box->x >= -EXTENT_LIMIT && box->x <= EXTENT_LIMIT
              && box->y >= -EXTENT_LIMIT && box->y <= EXTENT_LIMIT
              && box->width >= 0 && box->width <= EXTENT_LIMIT
              && box->height >= 0 && box->height <= EXTENT_LIMIT
              && box->content_height >= 0
              && box->content_height <= EXTENT_LIMIT);
    }
    CHECK(layout.height == EXTENT_LIMIT
          && layout.scroll_width == EXTENT_LIMIT
          && last_tall_box != NULL && last_tall_box->y == EXTENT_LIMIT
          && last_tall_box->height == 0
          && large_ratio_command != NULL
          && large_ratio_command->width == EXTENT_LIMIT
          && large_ratio_command->height == EXTENT_LIMIT);
    layout_destroy(&layout);
    stylesheet_destroy(&sheet);
    document_destroy(&document);
    free(page);
    CHECK(budget->current == 0);
    return 0;
}
static int test_table_and_control_ua_box_models(Budget *budget)
{
    static const char page[] =
        "<!doctype html><style>body{margin:0;padding:0}"
        "#full-table{width:100%;padding:2px}"
        "#control-line{width:480px;text-align:center}"
        "#wide-input{width:90%}</style><body>"
        "<table id=full-table><tr><td>TABLE</td></tr></table>"
        "<div id=control-line>Long label: <input id=wide-input></div>"
        "</body>";
    PocDocument document = {0};
    Stylesheet sheet = {0};
    LayoutDocument layout = {0};
    CHECK(document_parse(&document, budget, page, strlen(page), 19)
          && stylesheet_build(&sheet, budget, &document, 480)
          && layout_build(&layout, budget, &document, &sheet,
                          NULL, NULL, 480));
    lxb_dom_node_t *root = lxb_dom_interface_node(document.html);
    lxb_dom_node_t *table = find_id(root, "full-table");
    lxb_dom_node_t *input = find_id(root, "wide-input");
    const LayoutNodeBox *table_box = layout_box_for_node(&layout, table);
    const DrawCommand *label = text_command(&layout, "label:");
    const ControlRegion *input_control = NULL;
    for (size_t i = 0; i < layout.control_count; i++) {
        if (layout.controls[i].node == input) {
            input_control = &layout.controls[i];
            break;
        }
    }
    ComputedStyle table_style = style_for_node(&sheet, table, NULL);
    ComputedStyle input_style = style_for_node(&sheet, input, NULL);
    CHECK(table != NULL && input != NULL && table_box != NULL
          && table_style.box_sizing_border_box
          && table_box->width == 480 && layout.scroll_width == 480
          && input_style.has_width && input_style.width_percent
          && input_style.box_sizing_border_box
          && input_style.border.left == 2 && input_style.padding.left == 2
          && label != NULL && input_control != NULL
          && input_control->y > label->y
          && input_control->x == 24 && input_control->width == 432
          && input_control->height == 22);
    layout_destroy(&layout);
    stylesheet_destroy(&sheet);
    document_destroy(&document);
    CHECK(budget->current == 0);
    return 0;
}

static int test_native_form_control_fidelity(Budget *budget)
{
    size_t baseline = budget->current;
    static const char page[] =
        "<!doctype html><style>html,body{margin:0;padding:0}"
        ".row{display:flex;align-items:center;gap:6px}"
        "#slider{display:block;width:160px}"
        "#choice{display:block;width:140px;height:30px;padding:6px}"
        "#area{display:block;width:140px;height:50px;padding:6px}"
        "#transparent-parent{display:inline-block;position:relative;"
        "width:50px;height:44px;background:#febd69}"
        "#transparent-submit{position:absolute;inset:0;width:50px;height:44px;border:0;"
        "background-color:transparent;text-indent:-1000px}"
        "</style><div class=row><input id=check type=checkbox checked>"
        "<span>LABEL</span></div><input id=slider type=range value=40>"
        "<select id=choice><option>CHOICE</option></select>"
        "<textarea id=area>AREA-TEXT</textarea>"
        "<span><input id=disabled disabled value=NO></span>"
        "<span id=transparent-parent><input id=transparent-submit "
        "type=submit value=Go></span>";
    PocDocument document = {0};
    Stylesheet sheet = {0};
    LayoutDocument layout = {0};
    CHECK(document_parse(&document, budget, page, strlen(page), 117)
          && stylesheet_build(&sheet, budget, &document, 480)
          && layout_build(&layout, budget, &document, &sheet,
                          NULL, NULL, 480));
    lxb_dom_node_t *root = lxb_dom_interface_node(document.html);
    lxb_dom_node_t *check = find_id(root, "check");
    lxb_dom_node_t *slider = find_id(root, "slider");
    lxb_dom_node_t *choice = find_id(root, "choice");
    lxb_dom_node_t *area = find_id(root, "area");
    lxb_dom_node_t *disabled = find_id(root, "disabled");
    lxb_dom_node_t *transparent_submit = find_id(
        root, "transparent-submit");
    const LayoutNodeBox *check_box = layout_box_for_node(&layout, check);
    const LayoutNodeBox *slider_box = layout_box_for_node(&layout, slider);
    const LayoutNodeBox *choice_box = layout_box_for_node(&layout, choice);
    const LayoutNodeBox *area_box = layout_box_for_node(&layout, area);
    CHECK(check_box != NULL && check_box->width >= 18
          && check_box->height >= 18
          && slider_box != NULL && choice_box != NULL && area_box != NULL);
    CHECK(transparent_submit != NULL);

    ComputedStyle check_style = style_for_node(&sheet, check, NULL);
    CHECK(check_style.padding.left == 0 && check_style.padding.right == 0
          && check_style.border.left == 0 && check_style.border.right == 0
          && !check_style.has_background);

    size_t range_strokes = 0;
    for (size_t i = slider_box->command_start;
         i < slider_box->command_end && i < layout.count; i++) {
        if (layout.commands[i].type == DRAW_STROKE_RECT) range_strokes++;
    }
    CHECK(range_strokes == 0);

    size_t indicator_rows = 0;
    for (size_t i = choice_box->command_start;
         i < choice_box->command_end && i < layout.count; i++) {
        const DrawCommand *command = &layout.commands[i];
        if (command->type == DRAW_FILL_RECT && command->height == 1
            && command->x >= choice_box->x + choice_box->width - 14) {
            indicator_rows++;
        }
    }
    CHECK(indicator_rows >= 4);

    const DrawCommand *area_text = NULL;
    size_t area_text_count = 0;
    for (size_t i = 0; i < layout.count; i++) {
        const DrawCommand *command = &layout.commands[i];
        if (command->type == DRAW_TEXT && command->text != NULL
            && command->text_length == 9
            && memcmp(command->text, "AREA-TEXT", 9) == 0) {
            area_text = command;
            area_text_count++;
        }
    }
    ComputedStyle area_style = style_for_node(&sheet, area, NULL);
    CHECK(area_text_count == 1 && area_text != NULL
          && area_text->x == area_box->x + area_style.border.left
                             + area_style.padding.left
          && area_text->y == area_box->y + area_style.border.top
                             + area_style.padding.top);
    const DrawCommand *disabled_text = text_command(&layout, "NO");
    ComputedStyle disabled_style = style_for_node(
        &sheet, disabled, NULL);
    CHECK(disabled_style.opacity == 128 && disabled_text != NULL
          && disabled_text->opacity_scale >= 127
          && disabled_text->opacity_scale <= 129);
    ComputedStyle transparent_style = style_for_node(
        &sheet, transparent_submit, NULL);
    bool transparent_shell = false;
    bool indented_submit_text = false;
    for (size_t i = 0; i < layout.count; i++) {
        const DrawCommand *command = &layout.commands[i];
        if (command->type == DRAW_FILL_RECT
            && command->width == 50 && command->height == 44) {
            transparent_shell = command->color == 0xfebd69;
        }
        if (command->type == DRAW_TEXT && command->text_length == 2
            && memcmp(command->text, "Go", 2) == 0) {
            indented_submit_text = command->x < -500;
        }
    }
    CHECK(!transparent_style.has_background && transparent_shell
          && indented_submit_text);

    layout_destroy(&layout);
    stylesheet_destroy(&sheet);
    document_destroy(&document);
    CHECK(budget->current == baseline);
    return 0;
}

static int test_table_cell_vertical_alignment(Budget *budget)
{
    static const char page[] =
        "<!doctype html><style>body{margin:0;padding:0}"
        "table{width:320px}tr{height:60px}td{padding:0}"
        ".marker{display:block;width:8px;height:20px}"
        "#top-cell{vertical-align:top}"
        "#bottom-cell{vertical-align:bottom}"
        "#author-section{vertical-align:bottom}"
        "#author-top{vertical-align:top}"
        "#legacy-override{vertical-align:bottom}"
        "#position-cell{vertical-align:bottom;position:relative}"
        "#positioned-marker{position:absolute;left:0;top:0}"
        "</style><body><table>"
        "<tbody id=default-section><tr id=default-row>"
        "<td id=middle-cell><span class=marker id=middle-marker></span></td>"
        "<td id=top-cell><span class=marker id=top-marker></span></td>"
        "<td id=bottom-cell><span class=marker id=bottom-marker></span></td>"
        "</tr></tbody>"
        "<tbody id=author-section><tr id=author-row>"
        "<td id=author-bottom><span class=marker id=author-bottom-marker></span></td>"
        "<td id=author-top><span class=marker id=author-top-marker></span></td>"
        "<td id=legacy-top valign=top><span class=marker id=legacy-top-marker></span></td>"
        "<td id=legacy-override valign=top><span class=marker id=legacy-override-marker></span></td>"
        "</tr></tbody>"
        "<tbody><tr id=position-row><td id=position-cell>"
        "<a class=marker id=positioned-marker href=/positioned></a>"
        "<span class=marker id=position-flow-marker></span>"
        "</td></tr></tbody></table></body>";
    PocDocument document = {0};
    Stylesheet sheet = {0};
    LayoutDocument layout = {0};
    CHECK(document_parse(&document, budget, page, strlen(page), 19)
          && stylesheet_build(&sheet, budget, &document, 480)
          && layout_build(&layout, budget, &document, &sheet,
                          NULL, NULL, 480));
    lxb_dom_node_t *root = lxb_dom_interface_node(document.html);
    lxb_dom_node_t *default_section = find_id(root, "default-section");
    lxb_dom_node_t *table = default_section == NULL
                            ? NULL : default_section->parent;
    lxb_dom_node_t *default_row = find_id(root, "default-row");
    lxb_dom_node_t *middle_cell = find_id(root, "middle-cell");
    lxb_dom_node_t *top_cell = find_id(root, "top-cell");
    lxb_dom_node_t *bottom_cell = find_id(root, "bottom-cell");
    lxb_dom_node_t *author_section = find_id(root, "author-section");
    lxb_dom_node_t *author_row = find_id(root, "author-row");
    lxb_dom_node_t *author_bottom = find_id(root, "author-bottom");
    lxb_dom_node_t *author_top = find_id(root, "author-top");
    lxb_dom_node_t *legacy_top = find_id(root, "legacy-top");
    lxb_dom_node_t *legacy_override = find_id(root, "legacy-override");
    lxb_dom_node_t *position_row = find_id(root, "position-row");
    lxb_dom_node_t *position_cell = find_id(root, "position-cell");
    lxb_dom_node_t *positioned_marker = find_id(root, "positioned-marker");
    CHECK(table != NULL && default_section != NULL && default_row != NULL
          && middle_cell != NULL && top_cell != NULL && bottom_cell != NULL
          && author_section != NULL && author_row != NULL
          && author_bottom != NULL && author_top != NULL
          && legacy_top != NULL && legacy_override != NULL
          && position_row != NULL && position_cell != NULL
          && positioned_marker != NULL);

    ComputedStyle table_style = style_for_node(&sheet, table, NULL);
    ComputedStyle default_section_style = style_for_node(
        &sheet, default_section, &table_style);
    ComputedStyle default_row_style = style_for_node(
        &sheet, default_row, &default_section_style);
    ComputedStyle middle_style = style_for_node(
        &sheet, middle_cell, &default_row_style);
    ComputedStyle top_style = style_for_node(
        &sheet, top_cell, &default_row_style);
    ComputedStyle bottom_style = style_for_node(
        &sheet, bottom_cell, &default_row_style);
    ComputedStyle author_section_style = style_for_node(
        &sheet, author_section, &table_style);
    ComputedStyle author_row_style = style_for_node(
        &sheet, author_row, &author_section_style);
    ComputedStyle author_bottom_style = style_for_node(
        &sheet, author_bottom, &author_row_style);
    ComputedStyle author_top_style = style_for_node(
        &sheet, author_top, &author_row_style);
    ComputedStyle legacy_top_style = style_for_node(
        &sheet, legacy_top, &author_row_style);
    ComputedStyle legacy_override_style = style_for_node(
        &sheet, legacy_override, &author_row_style);
    CHECK(default_section_style.display == DISPLAY_TABLE_ROW_GROUP
          && default_section_style.vertical_align == VERTICAL_MIDDLE
          && default_row_style.vertical_align == VERTICAL_MIDDLE
          && middle_style.vertical_align == VERTICAL_MIDDLE
          && top_style.vertical_align == VERTICAL_TOP
          && bottom_style.vertical_align == VERTICAL_BOTTOM
          && author_section_style.vertical_align == VERTICAL_BOTTOM
          && author_row_style.vertical_align == VERTICAL_BOTTOM
          && author_bottom_style.vertical_align == VERTICAL_BOTTOM
          && author_top_style.vertical_align == VERTICAL_TOP
          && legacy_top_style.vertical_align == VERTICAL_TOP
          && legacy_override_style.vertical_align == VERTICAL_BOTTOM);

    const LayoutNodeBox *default_row_box = layout_box_for_node(
        &layout, default_row);
    const LayoutNodeBox *author_row_box = layout_box_for_node(
        &layout, author_row);
    const LayoutNodeBox *middle_box = layout_box_for_node(
        &layout, middle_cell);
    const LayoutNodeBox *top_box = layout_box_for_node(&layout, top_cell);
    const LayoutNodeBox *bottom_box = layout_box_for_node(
        &layout, bottom_cell);
    const LayoutNodeBox *author_bottom_box = layout_box_for_node(
        &layout, author_bottom);
    const LayoutNodeBox *author_top_box = layout_box_for_node(
        &layout, author_top);
    const LayoutNodeBox *legacy_top_box = layout_box_for_node(
        &layout, legacy_top);
    const LayoutNodeBox *legacy_override_box = layout_box_for_node(
        &layout, legacy_override);
    const LayoutNodeBox *middle_marker = layout_box_for_node(
        &layout, find_id(root, "middle-marker"));
    const LayoutNodeBox *top_marker = layout_box_for_node(
        &layout, find_id(root, "top-marker"));
    const LayoutNodeBox *bottom_marker = layout_box_for_node(
        &layout, find_id(root, "bottom-marker"));
    const LayoutNodeBox *author_bottom_marker = layout_box_for_node(
        &layout, find_id(root, "author-bottom-marker"));
    const LayoutNodeBox *author_top_marker = layout_box_for_node(
        &layout, find_id(root, "author-top-marker"));
    const LayoutNodeBox *legacy_top_marker = layout_box_for_node(
        &layout, find_id(root, "legacy-top-marker"));
    const LayoutNodeBox *legacy_override_marker = layout_box_for_node(
        &layout, find_id(root, "legacy-override-marker"));
    const LayoutNodeBox *position_row_box = layout_box_for_node(
        &layout, position_row);
    const LayoutNodeBox *position_cell_box = layout_box_for_node(
        &layout, position_cell);
    const LayoutNodeBox *positioned_marker_box = layout_box_for_node(
        &layout, positioned_marker);
    const LayoutNodeBox *position_flow_marker = layout_box_for_node(
        &layout, find_id(root, "position-flow-marker"));
    const LinkRegion *positioned_link = NULL;
    for (size_t i = 0; i < layout.link_count; i++) {
        if (layout.links[i].node == positioned_marker) {
            positioned_link = &layout.links[i];
            break;
        }
    }
    CHECK(default_row_box != NULL && author_row_box != NULL
          && middle_box != NULL && top_box != NULL && bottom_box != NULL
          && author_bottom_box != NULL && author_top_box != NULL
          && legacy_top_box != NULL && legacy_override_box != NULL
          && middle_marker != NULL && top_marker != NULL
          && bottom_marker != NULL && author_bottom_marker != NULL
          && author_top_marker != NULL && legacy_top_marker != NULL
          && legacy_override_marker != NULL && position_row_box != NULL
          && position_cell_box != NULL && positioned_marker_box != NULL
          && position_flow_marker != NULL && positioned_link != NULL);
    CHECK(default_row_box->height == 60 && author_row_box->height == 60
          && middle_box->height == 60 && top_box->height == 60
          && bottom_box->height == 60 && author_bottom_box->height == 60
          && author_top_box->height == 60 && legacy_top_box->height == 60
          && legacy_override_box->height == 60
          && position_row_box->height == 60
          && position_cell_box->height == 60);
    CHECK(middle_marker->y == default_row_box->y + 20
          && top_marker->y == default_row_box->y
          && bottom_marker->y == default_row_box->y + 40
          && author_bottom_marker->y == author_row_box->y + 40
          && author_top_marker->y == author_row_box->y
          && legacy_top_marker->y == author_row_box->y
          && legacy_override_marker->y == author_row_box->y + 40
          && position_flow_marker->y == position_row_box->y + 40
          && positioned_marker_box->positioned_ancestor_distance != 0
          && positioned_marker_box->y == position_row_box->y
          && positioned_link->y == positioned_marker_box->y);
    layout_destroy(&layout);
    stylesheet_destroy(&sheet);
    document_destroy(&document);
    CHECK(budget->current == 0);
    return 0;
}
static int fallback_command_baseline(const LayoutDocument *layout,
                                     const char *wanted)
{
    size_t wanted_length = strlen(wanted);
    for (size_t i = 0; i < layout->count; i++) {
        const DrawCommand *command = &layout->commands[i];
        if (command->type == DRAW_TEXT
            && command->text_length == wanted_length
            && memcmp(command->text, wanted, wanted_length) == 0) {
            return command->y + 6 * (command->scale > 0 ? command->scale : 1);
        }
    }
    return -1;
}
static uint32_t command_color(const LayoutDocument *layout,
                              const char *wanted)
{
    size_t wanted_length = strlen(wanted);
    for (size_t i = 0; i < layout->count; i++) {
        const DrawCommand *command = &layout->commands[i];
        if (command->type == DRAW_TEXT
            && command->text_length == wanted_length
            && memcmp(command->text, wanted, wanted_length) == 0) {
            return command->color;
        }
    }
    return UINT32_MAX;
}
static uint16_t command_opacity(const LayoutDocument *layout,
                                const char *wanted)
{
    size_t wanted_length = strlen(wanted);
    for (size_t i = 0; i < layout->count; i++) {
        const DrawCommand *command = &layout->commands[i];
        if (command->type == DRAW_TEXT
            && command->text_length == wanted_length
            && memcmp(command->text, wanted, wanted_length) == 0) {
            return command->opacity_scale;
        }
    }
    return UINT16_MAX;
}
static uint16_t fill_opacity(const LayoutDocument *layout, uint32_t color)
{
    for (size_t i = 0; i < layout->count; i++) {
        const DrawCommand *command = &layout->commands[i];
        if (command->type == DRAW_FILL_RECT && command->color == color) {
            return command->opacity_scale;
        }
    }
    return UINT16_MAX;
}
static bool deep_layout_append(char *html, size_t capacity, size_t *length,
                               const char *text)
{
    size_t text_length = strlen(text);
    if (*length > capacity || text_length > capacity - *length) return false;
    memcpy(html + *length, text, text_length);
    *length += text_length;
    html[*length] = '\0';
    return true;
}
static uint64_t deep_layout_signature(const LayoutDocument *layout,
                                      size_t *text_bytes)
{
    uint64_t hash = UINT64_C(1469598103934665603);
    *text_bytes = 0;
    for (size_t i = 0; i < layout->count; i++) {
        const DrawCommand *command = &layout->commands[i];
        const uint32_t values[] = {
            (uint32_t) command->type, (uint32_t) command->x,
            (uint32_t) command->y, (uint32_t) command->width,
            (uint32_t) command->height, command->color,
            command->text_length
        };
        for (size_t value = 0;
             value < sizeof(values) / sizeof(values[0]); value++) {
            hash ^= values[value];
            hash *= UINT64_C(1099511628211);
        }
        if (command->type != DRAW_TEXT || command->text == NULL) continue;
        *text_bytes += command->text_length;
        for (size_t at = 0; at < command->text_length; at++) {
            hash ^= (unsigned char) command->text[at];
            hash *= UINT64_C(1099511628211);
        }
    }
    hash ^= layout->node_box_count;
    hash *= UINT64_C(1099511628211);
    hash ^= layout->link_count;
    return hash;
}
static bool test_deep_layout_case(Budget *budget, DeepLayoutMode mode,
                                  size_t depth, bool require_target_box)
{
    size_t capacity = depth * 64u + 512u;
    char *html = malloc(capacity + 1u);
    if (html == NULL) return false;
    size_t length = 0;
    bool ok = deep_layout_append(
        html, capacity, &length, "<!doctype html><html><body>");
    if (ok && mode == DEEP_LAYOUT_FLEX_INTRINSIC) {
        ok = deep_layout_append(
            html, capacity, &length,
            "<div style='display:flex'><div style='width:max-content'>");
    }
    const char *open = mode == DEEP_LAYOUT_BLOCK ? "<div>x" : "<span>x";
    const char *target_open = mode == DEEP_LAYOUT_BLOCK
        ? "<div id='deep-target'>x" : "<span id='deep-target'>x";
    const char *close = mode == DEEP_LAYOUT_BLOCK ? "</div>" : "</span>";
    for (size_t i = 0; ok && i < depth; i++) {
        ok = deep_layout_append(
            html, capacity, &length, i + 1u == depth ? target_open : open);
    }
    if (ok) {
        ok = deep_layout_append(
            html, capacity, &length,
            "<script>not-visible</script>"
            "<span style='display:none'>not-visible</span>"
            "<a href='/deep'>tail</a>");
    }
    for (size_t i = 0; ok && i < depth; i++) {
        ok = deep_layout_append(html, capacity, &length, close);
    }
    if (ok && mode == DEEP_LAYOUT_FLEX_INTRINSIC) {
        ok = deep_layout_append(html, capacity, &length, "</div></div>");
    }
    if (ok) {
        ok = deep_layout_append(html, capacity, &length, "</body></html>");
    }
    uint64_t signatures[2] = {0};
    size_t command_counts[2] = {0}, node_box_counts[2] = {0};
    for (size_t run = 0; ok && run < 2; run++) {
        size_t baseline = budget->current;
        PocDocument document = {0};
        Stylesheet stylesheet = {0};
        LayoutDocument layout = {0};
        ok = document_parse(&document, budget, html, length, 17)
            && stylesheet_build(&stylesheet, budget, &document, 480)
            && layout_build(&layout, budget, &document, &stylesheet,
                            NULL, NULL, 480);
        size_t text_bytes = 0;
        if (ok) {
            signatures[run] = deep_layout_signature(&layout, &text_bytes);
            command_counts[run] = layout.count;
            node_box_counts[run] = layout.node_box_count;
            bool retained_deep_link = false;
            for (size_t i = 0; i < layout.link_count; i++) {
                if (layout.links[i].url_length == 5
                    && memcmp(layout.links[i].url, "/deep", 5) == 0) {
                    retained_deep_link = true;
                    break;
                }
            }
            lxb_dom_node_t *target = find_id(
                lxb_dom_interface_node(document.html), "deep-target");
            ok = text_bytes == depth + 4u && retained_deep_link
                && (!require_target_box
                    || layout_box_for_node(&layout, target) != NULL);
        }
        for (size_t i = 0; ok && i < layout.node_box_count; i++) {
            for (size_t j = i + 1; j < layout.node_box_count; j++) {
                if (layout.node_boxes[i].node == layout.node_boxes[j].node) {
                    ok = false;
                    break;
                }
            }
        }
        layout_destroy(&layout);
        stylesheet_destroy(&stylesheet);
        document_destroy(&document);
        if (budget->current != baseline) ok = false;
    }
    free(html);
    return ok && signatures[0] == signatures[1]
        && command_counts[0] == command_counts[1]
        && node_box_counts[0] == node_box_counts[1];
}
static int test_overflow_wrap_correctness(Budget *budget)
{
    size_t baseline = budget->current;
    static const char page[] =
        "<!doctype html><style>body{margin:0;padding:0}"
        ".wrap{display:block;font-family:monospace}"
        "#break-line{width:140px;word-wrap:BrEaK-WoRd}"
        "#overlong,#nowrap{width:40px;word-wrap:break-word}"
        "#nowrap{white-space:nowrap}"
        ".minimum{display:flex;width:80px}"
        ".minimum>.target{display:block;flex:1 1 0}"
        ".minimum>.fixed{display:block;flex:0 0 40px;width:40px;min-width:0}"
        "#any-item{overflow-wrap:anywhere}"
        "#break-item{overflow-wrap:break-word}"
        "#invalid{overflow-wrap:anywhere;word-wrap:not-anywhere}"
        "#invalid-only{overflow-wrap:not-anywhere}"
        "#normal{word-wrap:normal}"
        "</style><body>"
        "<div id=break-line class=wrap>AAAA <span id=inherited>"
        "BBBBBBBBBB</span></div>"
        "<div id=overlong class=wrap>CCCCCCCCCCCC</div>"
        "<div id=nowrap class=wrap>DDDDDDDDDDDD</div>"
        "<div class=minimum><span id=any-item class=target>EEEEE</span>"
        "<span class=fixed></span></div>"
        "<div class=minimum><span id=break-item class=target>FFFFF</span>"
        "<span class=fixed></span></div>"
        "<div id=invalid></div><div id=invalid-only></div>"
        "<div id=normal></div></body>";
    PocDocument document;
    Stylesheet sheet;
    LayoutDocument layout;
    CHECK(sizeof(OverflowWrap) == 1
          && document_parse(&document, budget, page, strlen(page), 19)
          && stylesheet_build(&sheet, budget, &document, 480)
          && layout_build(&layout, budget, &document, &sheet,
                          NULL, NULL, 480));

    lxb_dom_node_t *root = lxb_dom_interface_node(document.html);
    lxb_dom_node_t *break_line = find_id(root, "break-line");
    lxb_dom_node_t *inherited = find_id(root, "inherited");
    lxb_dom_node_t *overlong = find_id(root, "overlong");
    lxb_dom_node_t *nowrap = find_id(root, "nowrap");
    lxb_dom_node_t *any_item = find_id(root, "any-item");
    lxb_dom_node_t *break_item = find_id(root, "break-item");
    lxb_dom_node_t *invalid = find_id(root, "invalid");
    lxb_dom_node_t *invalid_only = find_id(root, "invalid-only");
    lxb_dom_node_t *normal = find_id(root, "normal");
    CHECK(break_line != NULL && inherited != NULL && overlong != NULL
          && nowrap != NULL && any_item != NULL && break_item != NULL
          && invalid != NULL && invalid_only != NULL && normal != NULL);

    ComputedStyle break_line_style = style_for_node(
        &sheet, break_line, NULL);
    ComputedStyle inherited_style = style_for_node(
        &sheet, inherited, &break_line_style);
    ComputedStyle invalid_style = style_for_node(&sheet, invalid, NULL);
    ComputedStyle invalid_only_style = style_for_node(
        &sheet, invalid_only, NULL);
    ComputedStyle normal_style = style_for_node(&sheet, normal, NULL);
    CHECK(computed_style_overflow_wrap(&break_line_style)
              == OVERFLOW_WRAP_BREAK_WORD
          && computed_style_overflow_wrap(&inherited_style)
              == OVERFLOW_WRAP_BREAK_WORD
          && computed_style_overflow_wrap(&invalid_style)
              == OVERFLOW_WRAP_ANYWHERE
          && computed_style_overflow_wrap(&invalid_only_style)
              == OVERFLOW_WRAP_NORMAL
          && computed_style_overflow_wrap(&normal_style)
              == OVERFLOW_WRAP_NORMAL);

    const LayoutNodeBox *break_line_box = layout_box_for_node(
        &layout, break_line);
    const LayoutNodeBox *overlong_box = layout_box_for_node(
        &layout, overlong);
    const LayoutNodeBox *nowrap_box = layout_box_for_node(&layout, nowrap);
    const LayoutNodeBox *any_item_box = layout_box_for_node(
        &layout, any_item);
    const LayoutNodeBox *break_item_box = layout_box_for_node(
        &layout, break_item);
    const DrawCommand *prefix = text_command(&layout, "AAAA");
    const DrawCommand *next_word = text_command(&layout, "BBBBBBBBBB");
    const DrawCommand *nowrap_word = text_command(
        &layout, "DDDDDDDDDDDD");
    CHECK(break_line_box != NULL && overlong_box != NULL
          && nowrap_box != NULL && any_item_box != NULL
          && break_item_box != NULL && prefix != NULL && next_word != NULL
          && next_word->x == break_line_box->x
          && next_word->y > prefix->y
          && next_word->x + next_word->width
             <= break_line_box->x + break_line_box->width
          && nowrap_word != NULL
          && nowrap_word->x + nowrap_word->width
             > nowrap_box->x + nowrap_box->width);

    size_t overlong_pieces = 0;
    bool overlong_contained = true;
    for (size_t i = 0; i < layout.count; i++) {
        const DrawCommand *command = &layout.commands[i];
        if (command->type != DRAW_TEXT || command->text_length == 0) continue;
        bool all_c = true;
        for (size_t j = 0; j < command->text_length; j++) {
            if (command->text[j] != 'C') {
                all_c = false;
                break;
            }
        }
        if (!all_c) continue;
        overlong_pieces++;
        overlong_contained = overlong_contained
            && command->x >= overlong_box->x
            && command->x + command->width
               <= overlong_box->x + overlong_box->width;
    }
    CHECK(overlong_pieces > 1 && overlong_contained
          && any_item_box->width == 40
          && break_item_box->width > any_item_box->width);

    layout_destroy(&layout);
    stylesheet_destroy(&sheet);
    document_destroy(&document);
    CHECK(budget->current == baseline);
    return 0;
}

static int test_calculated_unitless_line_height(Budget *budget)
{
    size_t baseline = budget->current;
    static const char page[] =
        "<!doctype html><style>"
        "#large{font-size:36px;--lh-a:2.5;--lh-b:2.25;"
        "line-height:calc(var(--lh-a)/var(--lh-b))}"
        "#small{font-size:12px;--lh:calc(1/.75);line-height:var(--lh)}"
        "</style><div id=large>LARGE</div><div id=small>SMALL</div>";
    PocDocument document = {0};
    Stylesheet sheet = {0};
    CHECK(document_parse(&document, budget, page, sizeof(page) - 1, 17)
          && stylesheet_build(&sheet, budget, &document, 480));
    lxb_dom_node_t *root = lxb_dom_interface_node(document.html);
    lxb_dom_node_t *large_node = find_id(root, "large");
    lxb_dom_node_t *small_node = find_id(root, "small");
    ComputedStyle large = style_for_node(&sheet, large_node, NULL);
    ComputedStyle small = style_for_node(&sheet, small_node, NULL);
    CHECK(large_node != NULL && small_node != NULL
          && large.font_size == 36 && large.line_height == 40
          && small.font_size == 12 && small.line_height == 16);
    stylesheet_destroy(&sheet);
    document_destroy(&document);
    CHECK(budget->current == baseline);
    return 0;
}

static int test_modern_mobile_css_interaction_and_presentation(Budget *budget)
{
    size_t baseline = budget->current;
    static const char page[] =
        "<!doctype html><style>html,body{margin:0;padding:0}"
        "#policy{user-select:none;touch-action:pan-x;text-wrap:balance;"
        "text-size-adjust:120%}"
        "#child{font-size:20px}"
        "#transform{display:block;width:40px;height:20px;"
        "background:#123456;translate:10px 5px;rotate:90deg;scale:2;"
        "isolation:isolate;border-start-start-radius:7px;"
        "border-start-end-radius:9px;border-end-start-radius:11px;"
        "border-end-end-radius:13px}"
        "#transform-list{transform:translate(10px,5px) scale(2) "
        "rotate(.25turn)}"
        "#transform-matrix{transform:matrix(0,2,-2,0,7,9)}"
        "#transform-rejected{transform:translate(1px) rotate(45deg)}"
        "#rtl{direction:rtl;border-start-start-radius:17px}"
        ".copy{display:block;width:120px;font-size:12px;line-height:14px}"
        "#balanced{text-wrap:balance}#pretty{text-wrap:pretty}"
        "textarea{display:block;width:100px;height:40px;resize:both}"
        "@supports (translate:10px 20px) and (rotate:90deg) and "
        "(scale:2){#support{width:37px}}"
        "@supports (rotate:45deg){#unsupported{display:none}}"
        "</style><div id=policy><span id=child>CHILD</span></div>"
        "<div id=transform>TURN</div><div id=transform-list></div>"
        "<div id=transform-matrix></div><div id=transform-rejected></div>"
        "<div id=rtl></div>"
        "<p id=normal class=copy>one two three four five six seven eight"
        " nine ten</p>"
        "<p id=balanced class=copy>one two three four five six seven eight"
        " nine ten</p>"
        "<p id=pretty class=copy>one two three four five six seven eight"
        " nine ten</p>"
        "<textarea id=editor>EDIT</textarea><i id=support></i>"
        "<i id=unsupported></i>";
    PocDocument document = {0};
    Stylesheet sheet = {0};
    LayoutDocument layout = {0};
    CHECK(document_parse(&document, budget, page, strlen(page), 93)
          && stylesheet_build(&sheet, budget, &document, 480));
    lxb_dom_node_t *root = lxb_dom_interface_node(document.html);
    lxb_dom_node_t *policy_node = find_id(root, "policy");
    lxb_dom_node_t *child_node = find_id(root, "child");
    lxb_dom_node_t *transform_node = find_id(root, "transform");
    lxb_dom_node_t *transform_list_node = find_id(root, "transform-list");
    lxb_dom_node_t *transform_matrix_node = find_id(root, "transform-matrix");
    lxb_dom_node_t *transform_rejected_node = find_id(root, "transform-rejected");
    lxb_dom_node_t *rtl_node = find_id(root, "rtl");
    lxb_dom_node_t *editor_node = find_id(root, "editor");
    lxb_dom_node_t *support_node = find_id(root, "support");
    lxb_dom_node_t *unsupported_node = find_id(root, "unsupported");
    ComputedStyle policy = style_for_node(&sheet, policy_node, NULL);
    ComputedStyle child = style_for_node(&sheet, child_node, &policy);
    ComputedStyle transform = style_for_node(
        &sheet, transform_node, NULL);
    ComputedStyle transform_list = style_for_node(
        &sheet, transform_list_node, NULL);
    ComputedStyle transform_matrix = style_for_node(
        &sheet, transform_matrix_node, NULL);
    ComputedStyle transform_rejected = style_for_node(
        &sheet, transform_rejected_node, NULL);
    ComputedStyle rtl = style_for_node(&sheet, rtl_node, NULL);
    ComputedStyle support = style_for_node(&sheet, support_node, NULL);
    ComputedStyle unsupported = style_for_node(
        &sheet, unsupported_node, NULL);
    CHECK((sheet.modern_property_mask & STYLE_MODERN_USER_SELECT) != 0
          && (sheet.modern_property_mask & STYLE_MODERN_TOUCH_ACTION) != 0
          && (sheet.modern_property_mask & STYLE_MODERN_TEXT_SIZE_ADJUST) != 0
          && (sheet.modern_property_mask & STYLE_MODERN_RESIZE) != 0
          && (sheet.modern_property_mask & STYLE_MODERN_TEXT_WRAP) != 0
          && (sheet.modern_property_mask & STYLE_MODERN_TRANSLATE) != 0
          && (sheet.modern_property_mask & STYLE_MODERN_ROTATE) != 0
          && (sheet.modern_property_mask & STYLE_MODERN_SCALE) != 0
          && (sheet.modern_property_mask & STYLE_MODERN_ISOLATION) != 0
          && (sheet.modern_property_mask & STYLE_MODERN_LOGICAL_RADIUS) != 0);
    CHECK(computed_style_user_select(&policy) == STYLE_USER_SELECT_NONE
          && computed_style_user_select(&child) == STYLE_USER_SELECT_AUTO
          && policy.touch_action == STYLE_TOUCH_ACTION_PAN_X
          && computed_style_text_wrap(&policy) == STYLE_TEXT_WRAP_BALANCE
          && computed_style_font_size_fixed(&child) == 24 * 64);
    CHECK(transform.has_transform && transform.transform_x == 10
          && transform.transform_y == 5
          && !transform.transform_x_percent
          && !transform.transform_y_percent
          && transform.transform_scale_q6 == 128
          && transform.individual_rotate_quadrants == 1
          && computed_style_isolation_isolate(&transform));
    CHECK(transform_list.has_transform
          && transform_list.transform_x == 10
          && transform_list.transform_y == 5
          && transform_list.transform_scale_q6 == 128
          && transform_list.individual_rotate_quadrants == 1
          && transform_matrix.has_transform
          && transform_matrix.transform_x == 7
          && transform_matrix.transform_y == 9
          && transform_matrix.transform_scale_q6 == 128
          && transform_matrix.individual_rotate_quadrants == 1
          && !transform_rejected.has_transform);
    CHECK(style_border_radius_corner(transform.border_radius, 0) == 7
          && style_border_radius_corner(transform.border_radius, 1) == 9
          && style_border_radius_corner(transform.border_radius, 2) == 13
          && style_border_radius_corner(transform.border_radius, 3) == 11
          && style_border_radius_corner(rtl.border_radius, 1) == 17);
    CHECK(support.width == 37 && unsupported.display != DISPLAY_NONE
          && stylesheet_supports_matches(
                 &sheet, "(text-wrap:balance) and (isolation:isolate)",
                 strlen("(text-wrap:balance) and (isolation:isolate)"))
          && !stylesheet_supports_matches(
                 &sheet, "(rotate:45deg)", strlen("(rotate:45deg)")));
    CHECK(layout_build(&layout, budget, &document, &sheet,
                       NULL, NULL, 480));
    const LayoutNodeBox *transform_box = layout_box_for_node(
        &layout, transform_node);
    const LayoutNodeBox *normal_box = layout_box_for_node(
        &layout, find_id(root, "normal"));
    const LayoutNodeBox *balanced_box = layout_box_for_node(
        &layout, find_id(root, "balanced"));
    const LayoutNodeBox *pretty_box = layout_box_for_node(
        &layout, find_id(root, "pretty"));
    const LayoutNodeBox *editor_box = layout_box_for_node(
        &layout, editor_node);
    CHECK(transform_box != NULL && transform_box->width == 40
          && transform_box->height == 80
          && transform_box->cssom_geometry_authoritative
          && normal_box != NULL && balanced_box != NULL
          && pretty_box != NULL && editor_box != NULL);
    bool rotated_text = false;
    for (size_t i = transform_box->command_start;
         i < transform_box->command_end && i < layout.count; i++) {
        if (layout.commands[i].type == DRAW_TEXT
            && draw_command_rotation_quadrants(&layout.commands[i]) == 1u) {
            rotated_text = true;
        }
    }
    bool resize_handle = false;
    for (size_t i = 0; i < layout.control_count; i++) {
        if (layout.controls[i].node == editor_node
            && layout.controls[i].type == CONTROL_RESIZE) {
            resize_handle = layout.controls[i].width > 0
                && layout.controls[i].height > 0;
        }
    }
    CHECK(rotated_text && resize_handle);

    int normal_last_width = 0, balanced_last_width = 0;
    int normal_last_y = INT_MIN, balanced_last_y = INT_MIN;
    for (size_t i = 0; i < layout.count; i++) {
        const DrawCommand *command = &layout.commands[i];
        if (command->type != DRAW_TEXT
            || command->y < normal_box->y
            || command->y >= normal_box->y + normal_box->height) continue;
        if (command->y > normal_last_y) {
            normal_last_y = command->y;
            normal_last_width = 0;
        }
        if (command->y == normal_last_y) {
            int right = command->x + command->width - normal_box->x;
            if (right > normal_last_width) normal_last_width = right;
        }
    }
    for (size_t i = 0; i < layout.count; i++) {
        const DrawCommand *command = &layout.commands[i];
        if (command->type != DRAW_TEXT
            || command->y < balanced_box->y
            || command->y >= balanced_box->y + balanced_box->height) {
            continue;
        }
        if (command->y > balanced_last_y) {
            balanced_last_y = command->y;
            balanced_last_width = 0;
        }
        if (command->y == balanced_last_y) {
            int right = command->x + command->width - balanced_box->x;
            if (right > balanced_last_width) balanced_last_width = right;
        }
    }
    CHECK(normal_last_y != INT_MIN && balanced_last_y != INT_MIN
          && balanced_last_width >= normal_last_width);

    layout_destroy(&layout);
    CHECK(document_control_resize_set(
              &document, editor_node, 180, 70)
          && layout_build(&layout, budget, &document, &sheet,
                          NULL, NULL, 480));
    editor_box = layout_box_for_node(&layout, editor_node);
    CHECK(editor_box != NULL && editor_box->width == 180
          && editor_box->height >= 70);
    layout_destroy(&layout);
    stylesheet_destroy(&sheet);
    document_destroy(&document);
    CHECK(budget->current == baseline);
    return 0;
}
