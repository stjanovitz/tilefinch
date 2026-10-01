static int command_y(const LayoutDocument *layout, const char *wanted)
{
    size_t wanted_length = strlen(wanted);
    for (size_t i = 0; i < layout->count; i++) {
        const DrawCommand *command = &layout->commands[i];
        if (command->type == DRAW_TEXT && command->text_length == wanted_length
            && memcmp(command->text, wanted, wanted_length) == 0) {
            return command->y;
        }
    }
    return -1;
}
static int test_block_boundary_whitespace(Budget *budget)
{
    static const char page[] =
        "<!doctype html><style>"
        "body{margin:0;padding:0}"
        "#globalheader,#main,#whitespace,#indented,#inline,#tight{display:block}"
        "#globalnav{position:fixed;top:0;height:48px}"
        "#curtain{position:absolute;top:0;height:48px}"
        "#placeholder{display:none}"
        "#main{padding-top:48px}"
        "#hero,#before,#after,#inner,#indented-after{display:block;height:10px}"
        "#single-break,#double-break{display:block}"
        "#heading-parent{font-family:monospace}"
        "</style><body>"
        "<header id=globalheader>\n"
        "  <nav id=globalnav></nav>\n"
        "  <div id=curtain></div>\n"
        "  <div id=placeholder></div>\n"
        "</header>"
        "<main id=main><div id=hero>HERO</div></main>"
        "<div id=before></div>"
        "<div id=whitespace>\n  \t\n</div>"
        "<div id=after></div>"
        "<div id=indented>\n  <div id=inner></div>\n</div>"
        "<div id=indented-after></div>"
        "<div id=inline><span>A</span> \n <span>B</span></div>"
        "<div id=tight><span>C</span><span>D</span></div>"
        "<div id=single-break>SINGLE-A<br>SINGLE-B</div>"
        "<div id=double-break>DOUBLE-A<br><br>DOUBLE-B</div>"
        "<div id=heading-parent><h1 id=inherited-heading>HEADING</h1>"
        "<b id=ua-bold>BOLD</b><strong id=ua-strong>STRONG</strong></div>"
        "</body>";
    PocDocument document;
    Stylesheet sheet;
    LayoutDocument layout;
    CHECK(document_parse(&document, budget, page, strlen(page), 19)
          && stylesheet_build(&sheet, budget, &document, 480)
          && layout_build(&layout, budget, &document, &sheet,
                          NULL, NULL, 480));
    lxb_dom_node_t *root = lxb_dom_interface_node(document.html);
    lxb_dom_node_t *globalheader = find_id(root, "globalheader");
    lxb_dom_node_t *main = find_id(root, "main");
    lxb_dom_node_t *hero = find_id(root, "hero");
    lxb_dom_node_t *before = find_id(root, "before");
    lxb_dom_node_t *whitespace = find_id(root, "whitespace");
    lxb_dom_node_t *after = find_id(root, "after");
    lxb_dom_node_t *indented = find_id(root, "indented");
    lxb_dom_node_t *inner = find_id(root, "inner");
    lxb_dom_node_t *indented_after = find_id(root, "indented-after");
    const LayoutNodeBox *globalheader_box = layout_box_for_node(
        &layout, globalheader);
    const LayoutNodeBox *main_box = layout_box_for_node(&layout, main);
    const LayoutNodeBox *hero_box = layout_box_for_node(&layout, hero);
    const LayoutNodeBox *before_box = layout_box_for_node(&layout, before);
    const LayoutNodeBox *whitespace_box = layout_box_for_node(
        &layout, whitespace);
    const LayoutNodeBox *after_box = layout_box_for_node(&layout, after);
    const LayoutNodeBox *indented_box = layout_box_for_node(
        &layout, indented);
    const LayoutNodeBox *inner_box = layout_box_for_node(&layout, inner);
    const LayoutNodeBox *indented_after_box = layout_box_for_node(
        &layout, indented_after);
    const DrawCommand *a = text_command(&layout, "A");
    const DrawCommand *b = text_command(&layout, "B");
    const DrawCommand *c = text_command(&layout, "C");
    const DrawCommand *d = text_command(&layout, "D");
    const DrawCommand *single_a = text_command(&layout, "SINGLE-A");
    const DrawCommand *single_b = text_command(&layout, "SINGLE-B");
    const DrawCommand *double_a = text_command(&layout, "DOUBLE-A");
    const DrawCommand *double_b = text_command(&layout, "DOUBLE-B");
    lxb_dom_node_t *heading_parent = find_id(root, "heading-parent");
    lxb_dom_node_t *inherited_heading = find_id(root, "inherited-heading");
    lxb_dom_node_t *ua_bold = find_id(root, "ua-bold");
    lxb_dom_node_t *ua_strong = find_id(root, "ua-strong");
    ComputedStyle heading_parent_style = style_for_node(
        &sheet, heading_parent, NULL);
    ComputedStyle inherited_heading_style = style_for_node(
        &sheet, inherited_heading, &heading_parent_style);
    ComputedStyle ua_bold_style = style_for_node(
        &sheet, ua_bold, &heading_parent_style);
    ComputedStyle ua_strong_style = style_for_node(
        &sheet, ua_strong, &heading_parent_style);
    CHECK(globalheader_box != NULL && main_box != NULL && hero_box != NULL
          && globalheader_box->height == 0
          && main_box->y == globalheader_box->y
          && hero_box->y == main_box->y + 48
          && command_y(&layout, "HERO") == hero_box->y);
    CHECK(before_box != NULL && whitespace_box != NULL && after_box != NULL
          && whitespace_box->y == before_box->y + before_box->height
          && whitespace_box->height == 0
          && after_box->y == whitespace_box->y);
    CHECK(indented_box != NULL && inner_box != NULL
          && indented_after_box != NULL
          && inner_box->y == indented_box->y
          && indented_box->height == inner_box->height
          && indented_after_box->y
             == indented_box->y + indented_box->height);
    CHECK(a != NULL && b != NULL && c != NULL && d != NULL
          && b->x > a->x + a->width
          && d->x == c->x + c->width);
    CHECK(single_a != NULL && single_b != NULL
          && double_a != NULL && double_b != NULL
          && single_b->y > single_a->y
          && double_b->y - double_a->y
             == 2 * (single_b->y - single_a->y));
    CHECK(heading_parent != NULL && inherited_heading != NULL
          && heading_parent_style.font_family == FONT_MONOSPACE
          && inherited_heading_style.font_family == FONT_MONOSPACE
          && inherited_heading_style.font_weight == 700
          && inherited_heading_style.font_bold
          && ua_bold_style.font_weight == 700 && ua_bold_style.font_bold
          && ua_strong_style.font_weight == 700 && ua_strong_style.font_bold);
    layout_destroy(&layout);
    stylesheet_destroy(&sheet);
    document_destroy(&document);
    CHECK(budget->current == 0);

    return 0;
}
static int test_ua_body_spacing(Budget *budget)
{
    static const struct {
        const char *html;
        int expected_x;
        int expected_y;
    } cases[] = {
        {"<!doctype html><body>UA-SPACING</body>", 8, 8},
        {"<!doctype html><style>body{margin:0}</style>"
         "<body>UA-SPACING</body>", 0, 0},
        {"<!doctype html><style>body{padding:4px}</style>"
         "<body>UA-SPACING</body>", 12, 12}
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        PocDocument document = {0};
        Stylesheet sheet = {0};
        LayoutDocument layout = {0};
        CHECK(document_parse(&document, budget, cases[i].html,
                             strlen(cases[i].html), 19)
              && stylesheet_build(&sheet, budget, &document, 480)
              && layout_build(&layout, budget, &document, &sheet,
                              NULL, NULL, 480));
        const DrawCommand *text = text_command(&layout, "UA-SPACING");
        CHECK(text != NULL && text->x == cases[i].expected_x
              && text->y == cases[i].expected_y);
        if (i == 0) {
            CHECK(text->color == 0x000000
                  && layout.page_background == 0xffffff);
        }
        layout_destroy(&layout);
        stylesheet_destroy(&sheet);
        document_destroy(&document);
        CHECK(budget->current == 0);
    }

    static const char ua_block_margin_page[] =
        "<!doctype html><style>body{margin:0}"
        "#ua-p-large{font-size:20px}</style><body>"
        "<p id=ua-p>UA-P</p><p id=ua-p-large>UA-P-LARGE</p>"
        "<h1 id=ua-h1>UA-H1</h1>"
        "<h2 id=ua-h2>UA-H2</h2><h3 id=ua-h3>UA-H3</h3>"
        "<h4 id=ua-h4>UA-H4</h4><h5 id=ua-h5>UA-H5</h5>"
        "<h6 id=ua-h6>UA-H6</h6></body>";
    static const struct {
        const char *id;
        const char *text;
        int margin_top;
        int margin_bottom;
    } ua_blocks[] = {
        {"ua-p", "UA-P", 16, 16},
        {"ua-p-large", "UA-P-LARGE", 20, 20},
        {"ua-h1", "UA-H1", 5, 5},
        {"ua-h2", "UA-H2", 4, 4},
        {"ua-h3", "UA-H3", 4, 4},
        {"ua-h4", "UA-H4", 0, 0},
        {"ua-h5", "UA-H5", 0, 0},
        {"ua-h6", "UA-H6", 0, 0}
    };
    PocDocument ua_block_margin_document = {0};
    Stylesheet ua_block_margin_sheet = {0};
    LayoutDocument ua_block_margin_layout = {0};
    CHECK(document_parse(&ua_block_margin_document, budget,
                         ua_block_margin_page,
                         strlen(ua_block_margin_page), 19)
          && stylesheet_build(&ua_block_margin_sheet, budget,
                              &ua_block_margin_document, 480)
          && layout_build(&ua_block_margin_layout, budget,
                          &ua_block_margin_document,
                          &ua_block_margin_sheet, NULL, NULL, 480));
    lxb_dom_node_t *ua_block_margin_root = lxb_dom_interface_node(
        ua_block_margin_document.html);
    lxb_dom_node_t *ua_block_margin_body = document_body_node(
        &ua_block_margin_document);
    lxb_dom_node_t *ua_block_margin_html = ua_block_margin_body == NULL
        ? NULL : ua_block_margin_body->parent;
    CHECK(ua_block_margin_html != NULL);
    ComputedStyle ua_block_margin_html_style = style_for_node(
        &ua_block_margin_sheet, ua_block_margin_html, NULL);
    ComputedStyle ua_block_margin_body_style = style_for_node(
        &ua_block_margin_sheet, ua_block_margin_body,
        &ua_block_margin_html_style);
    for (size_t i = 0; i < sizeof(ua_blocks) / sizeof(ua_blocks[0]); i++) {
        lxb_dom_node_t *node = find_id(
            ua_block_margin_root, ua_blocks[i].id);
        CHECK(node != NULL);
        ComputedStyle style = style_for_node(
            &ua_block_margin_sheet, node, &ua_block_margin_body_style);
        const DrawCommand *text = text_command(
            &ua_block_margin_layout, ua_blocks[i].text);
        CHECK(style.margin.top == ua_blocks[i].margin_top
              && style.margin.right == 0
              && style.margin.bottom == ua_blocks[i].margin_bottom
              && style.margin.left == 0
              && text != NULL && text->x == 0);
    }
    layout_destroy(&ua_block_margin_layout);
    stylesheet_destroy(&ua_block_margin_sheet);
    document_destroy(&ua_block_margin_document);
    CHECK(budget->current == 0);

    static const char zero_margin_page[] =
        "<!doctype html><style>#physical{border-top-width:4px;"
        "border-right-width:3px;border-bottom-width:2px;"
        "border-left-width:1px;border-top-color:#123456}"
        "#author-form{margin:0}</style>"
        "<body><form id=ua-form></form><form id=author-form></form>"
        "<ul><li id=ua-item>ITEM</li></ul>"
        "<div id=physical></div></body>";
    PocDocument zero_margin_document = {0};
    Stylesheet zero_margin_sheet = {0};
    CHECK(document_parse(&zero_margin_document, budget, zero_margin_page,
                         strlen(zero_margin_page), 19)
          && stylesheet_build(&zero_margin_sheet, budget,
                              &zero_margin_document, 480));
    lxb_dom_node_t *zero_margin_root = lxb_dom_interface_node(
        zero_margin_document.html);
    ComputedStyle form_style = style_for_node(
        &zero_margin_sheet, find_id(zero_margin_root, "ua-form"), NULL);
    ComputedStyle author_form_style = style_for_node(
        &zero_margin_sheet, find_id(zero_margin_root, "author-form"), NULL);
    ComputedStyle item_style = style_for_node(
        &zero_margin_sheet, find_id(zero_margin_root, "ua-item"), NULL);
    ComputedStyle physical_style = style_for_node(
        &zero_margin_sheet, find_id(zero_margin_root, "physical"), NULL);
    CHECK(form_style.margin.top == 0 && form_style.margin.right == 0
          && form_style.margin.bottom == 0
          && form_style.margin.left == 0
          && author_form_style.margin.top == 0
          && author_form_style.margin.right == 0
          && author_form_style.margin.bottom == 0
          && author_form_style.margin.left == 0
          && item_style.margin.top == 0 && item_style.margin.right == 0
          && item_style.margin.bottom == 0 && item_style.margin.left == 0
          && physical_style.border.top == 4
          && physical_style.border.right == 3
          && physical_style.border.bottom == 2
          && physical_style.border.left == 1
          && stylesheet_border_color(
                 &zero_margin_sheet, &physical_style,
                 STYLE_BORDER_TOP, NULL) == 0x123456);
    stylesheet_destroy(&zero_margin_sheet);
    document_destroy(&zero_margin_document);
    CHECK(budget->current == 0);

    static const char quirks_form_page[] =
        "<style>body{font-size:16px}#large-form{font-size:20px}"
        "#author-form{margin:0}</style><body>"
        "<form id=ua-form></form><form id=large-form></form>"
        "<form id=author-form></form></body>";
    PocDocument quirks_form_document = {0};
    Stylesheet quirks_form_sheet = {0};
    CHECK(document_parse(&quirks_form_document, budget, quirks_form_page,
                         strlen(quirks_form_page), 19)
          && quirks_form_document.html->dom_document.compat_mode
                 == LXB_DOM_DOCUMENT_CMODE_QUIRKS
          && stylesheet_build(&quirks_form_sheet, budget,
                              &quirks_form_document, 480));
    lxb_dom_node_t *quirks_form_root = lxb_dom_interface_node(
        quirks_form_document.html);
    ComputedStyle quirks_body_style = style_for_node(
        &quirks_form_sheet, document_body_node(&quirks_form_document), NULL);
    ComputedStyle quirks_form_style = style_for_node(
        &quirks_form_sheet, find_id(quirks_form_root, "ua-form"),
        &quirks_body_style);
    ComputedStyle large_form_style = style_for_node(
        &quirks_form_sheet, find_id(quirks_form_root, "large-form"),
        &quirks_body_style);
    ComputedStyle quirks_author_form_style = style_for_node(
        &quirks_form_sheet, find_id(quirks_form_root, "author-form"),
        &quirks_body_style);
    CHECK(quirks_form_style.font_size == 16
          && quirks_form_style.margin.top == 0
          && quirks_form_style.margin.right == 0
          && quirks_form_style.margin.bottom == 16
          && quirks_form_style.margin.left == 0
          && large_form_style.font_size == 20
          && large_form_style.margin.bottom == 20
          && quirks_author_form_style.margin.top == 0
          && quirks_author_form_style.margin.right == 0
          && quirks_author_form_style.margin.bottom == 0
          && quirks_author_form_style.margin.left == 0);
    stylesheet_destroy(&quirks_form_sheet);
    document_destroy(&quirks_form_document);
    CHECK(budget->current == 0);
    return 0;
}

static int test_scroll_scene_coordinate_containment(Budget *budget)
{
    static const char page[] =
        "<!doctype html><style>body{margin:0}.scene{height:1048575px;"
        "animation:scroll-scene 2s linear}.tail{height:20px}"
        "@keyframes scroll-scene{from{transform:translateY(100vh);opacity:0}"
        "to{transform:none;opacity:1}}</style><body>"
        "<div class=scene></div><p id=tail class=tail>TAIL-AFTER-SCENE</p>"
        "</body>";
    PocDocument document = {0};
    Stylesheet sheet = {0};
    LayoutDocument layout = {0};
    CHECK(document_parse(&document, budget, page, sizeof(page) - 1u, 31u)
          && stylesheet_build(&sheet, budget, &document, 480)
          && layout_build(&layout, budget, &document, &sheet,
                          NULL, NULL, 480));
    const DrawCommand *tail = text_command(&layout, "TAIL-AFTER-SCENE");
    CHECK(tail != NULL && tail->y > 0 && tail->y < 272 * 16
          && layout.height < 272 * 20
          && layout.performance.coordinate_containments == 1u
          && layout.performance.first_coordinate_clamp_y == 0);
    layout_destroy(&layout);
    stylesheet_destroy(&sheet);
    document_destroy(&document);
    CHECK(budget->current == 0);

    static const char root_page[] =
        "<!doctype html><style>body{margin:0;height:1048575px}</style>"
        "<body><p>ROOT-CONTENT</p></body>";
    PocDocument root_document = {0};
    Stylesheet root_sheet = {0};
    LayoutDocument root_layout = {0};
    CHECK(document_parse(&root_document, budget, root_page,
                         sizeof(root_page) - 1u, 32u));
    CHECK(stylesheet_build(&root_sheet, budget, &root_document, 480));
    CHECK(layout_build(&root_layout, budget, &root_document, &root_sheet,
                       NULL, NULL, 480));
    CHECK(root_layout.height < 272 * 65);
    CHECK(root_layout.performance.coordinate_clamps == 1u);
    CHECK(root_layout.performance.coordinate_containments == 1u);
    layout_destroy(&root_layout);
    stylesheet_destroy(&root_sheet);
    document_destroy(&root_document);
    CHECK(budget->current == 0);

    /* Utility frameworks commonly express negative spacing as a product
       whose scalar has a unary sign.  Treating that sign as calc()'s binary
       subtraction operator used to turn this ordinary heading margin into
       INT_MAX, saturate its parent, and make long server-rendered pages look
       like an unsupported scroll scene. */
    static const char negative_scalar_page[] =
        "<!doctype html><style>:root{--spacing:4px}body{margin:0}"
        ".hero h1{height:48px;padding-bottom:8px;"
        "margin-block-end:calc(var(--spacing) * -2)}"
        ".section{height:240px}</style><body>"
        "<div class=hero><h1 id=calc-heading>REAL HEADING</h1>"
        "<p>Introductory server-rendered content.</p></div>"
        "<div class=section>A</div><div class=section>B</div>"
        "<div class=section>C</div><div class=section>D</div>"
        "<div class=section>E</div><div class=section>F</div>"
        "<p id=calc-tail>REAL PAGE TAIL</p></body>";
    PocDocument negative_scalar_document = {0};
    Stylesheet negative_scalar_sheet = {0};
    LayoutDocument negative_scalar_layout = {0};
    CHECK(document_parse(&negative_scalar_document, budget,
                         negative_scalar_page,
                         sizeof(negative_scalar_page) - 1u, 33u)
          && stylesheet_build(&negative_scalar_sheet, budget,
                              &negative_scalar_document, 480)
          && layout_build(&negative_scalar_layout, budget,
                          &negative_scalar_document, &negative_scalar_sheet,
                          NULL, NULL, 480));
    lxb_dom_node_t *negative_scalar_root = lxb_dom_interface_node(
        negative_scalar_document.html);
    lxb_dom_node_t *negative_scalar_heading = find_id(
        negative_scalar_root, "calc-heading");
    ComputedStyle negative_scalar_heading_style = style_for_node(
        &negative_scalar_sheet, negative_scalar_heading, NULL);
    lxb_dom_node_t *negative_scalar_tail = find_id(
        negative_scalar_root, "calc-tail");
    const LayoutNodeBox *negative_scalar_tail_box = layout_box_for_node(
        &negative_scalar_layout, negative_scalar_tail);
    CHECK(negative_scalar_heading != NULL
          && negative_scalar_heading_style.margin.bottom == -8
          && style_parse_length(
                 &negative_scalar_sheet, "calc(10px - -2px)",
                 sizeof("calc(10px - -2px)") - 1u,
                 INT_MIN, NULL) == 12
          && style_parse_length(
                 &negative_scalar_sheet, "calc(2px * -3 + 10px)",
                 sizeof("calc(2px * -3 + 10px)") - 1u,
                 INT_MIN, NULL) == 4
          && style_parse_length(
                 &negative_scalar_sheet, "calc(1e-2px + 1px)",
                 sizeof("calc(1e-2px + 1px)") - 1u,
                 INT_MIN, NULL) == 1
          && style_parse_length(
                 &negative_scalar_sheet, "calc(8px + broken)",
                 sizeof("calc(8px + broken)") - 1u,
                 INT_MIN, NULL) == INT_MIN
          && negative_scalar_tail_box != NULL
          && negative_scalar_tail_box->y > 1200
          && negative_scalar_layout.height > 1400
          && negative_scalar_layout.height < 2000
          && negative_scalar_layout.performance.coordinate_containments == 0u);
    layout_destroy(&negative_scalar_layout);
    stylesheet_destroy(&negative_scalar_sheet);
    document_destroy(&negative_scalar_document);
    CHECK(budget->current == 0);

    /* A sticky child is constrained by its finite containing block.  Once
       that block has scrolled away, later page content must not be covered by
       a stale scroll-scene frame. */
    static const char bounded_sticky_page[] =
        "<!doctype html><style>body{margin:0}.before{height:40px}"
        ".scene{position:relative;top:60px;height:300px;background:#112233}"
        ".sticky{position:sticky;top:0;height:40px;background:#aa5500}"
        ".scene-fixed{position:fixed;left:0;top:0;width:40px;height:40px;"
        "background:#cc3300}"
        ".after{height:400px;background:#224466}</style><body>"
        "<div class=before></div><section class=scene>"
        "<div class=sticky>FINITE STICKY<div class=scene-fixed></div></div>"
        "</section>"
        "<div class=after>CONTENT AFTER SCENE</div></body>";
    PocDocument bounded_sticky_document = {0};
    Stylesheet bounded_sticky_sheet = {0};
    LayoutDocument bounded_sticky_layout = {0};
    TileCache bounded_sticky_cache = {0};
    uint16_t *bounded_sticky_frame = NULL;
    CHECK(document_parse(&bounded_sticky_document, budget,
                         bounded_sticky_page,
                         sizeof(bounded_sticky_page) - 1u, 34u)
          && stylesheet_build(&bounded_sticky_sheet, budget,
                              &bounded_sticky_document, 480)
          && layout_build(&bounded_sticky_layout, budget,
                          &bounded_sticky_document, &bounded_sticky_sheet,
                          NULL, NULL, 480));
    bounded_sticky_frame = budget_malloc(
        budget, 480u * 272u * sizeof(*bounded_sticky_frame));
    CHECK(bounded_sticky_layout.sticky_count == 1u
          && bounded_sticky_layout.fixed_count == 1u
          && bounded_sticky_layout.height >= 700
          && bounded_sticky_layout.sticky_ranges[0].origin_y == 100
          && bounded_sticky_layout.sticky_ranges[0].bottom_y == 140
          && bounded_sticky_layout.sticky_ranges[0].maximum_offset == 260
          && bounded_sticky_layout.fixed_ranges[0].scroll_end == 400
          && bounded_sticky_frame != NULL
          && tile_cache_init(&bounded_sticky_cache, budget,
                             &bounded_sticky_layout, 4)
          && tile_cache_set_frame(&bounded_sticky_cache,
                                  bounded_sticky_frame, 480u * 272u)
          && tile_cache_render_frame(&bounded_sticky_cache, 0,
                                     480, 272, NULL)
          && bounded_sticky_frame[70u * 480u + 10u]
                 == test_rgb565(0xcc3300)
          && tile_cache_render_frame(&bounded_sticky_cache, 350,
                                     480, 272, NULL)
          && bounded_sticky_frame[70u * 480u + 10u]
                 == test_rgb565(0xcc3300)
          && tile_cache_render_frame(&bounded_sticky_cache, 420,
                                     480, 272, NULL)
          && bounded_sticky_frame[70u * 480u + 10u]
                 == test_rgb565(0x224466));
    tile_cache_destroy(&bounded_sticky_cache);
    budget_free(budget, bounded_sticky_frame);
    layout_destroy(&bounded_sticky_layout);
    stylesheet_destroy(&bounded_sticky_sheet);
    document_destroy(&bounded_sticky_document);
    CHECK(budget->current == 0);
    return 0;
}
static int test_inline_replaced_box_model(Budget *budget)
{
    static const char page[] =
        "<!doctype html><style>body{margin:0;padding:0}"
        "#decorated{width:4px;height:2px;padding:1px;"
        "border:2px solid #aabbcc;margin:0 5px 0 3px;"
        "background:#112233}</style><body>"
        "<img id=decorated>TAIL</body>";
    PocDocument document = {0};
    Stylesheet sheet = {0};
    LayoutDocument layout = {0};
    CHECK(document_parse(&document, budget, page, strlen(page), 19)
          && stylesheet_build(&sheet, budget, &document, 480));
    lxb_dom_node_t *image_node = find_id(
        lxb_dom_interface_node(document.html), "decorated");
    static unsigned char rgba[] = {
        255, 0, 0, 255, 0, 255, 0, 255,
        0, 0, 255, 255, 255, 255, 255, 255
    };
    ImageResource item = {
        .node = image_node, .pixels = rgba, .width = 2, .height = 2
    };
    ImageResources images = {.items = &item, .count = 1};
    CHECK(image_node != NULL
          && layout_build(&layout, budget, &document, &sheet,
                          NULL, &images, 480));
    const DrawCommand *painted_image = NULL;
    bool background = false, top = false, bottom = false;
    bool left = false, right = false;
    for (size_t i = 0; i < layout.count; i++) {
        const DrawCommand *command = &layout.commands[i];
        if (command->type == DRAW_IMAGE && command->image == &item) {
            painted_image = command;
        } else if (command->type == DRAW_FILL_RECT
                   && command->color == 0x112233) {
            background = command->x == 5 && command->y == 2
                         && command->width == 6 && command->height == 4;
        } else if (command->type == DRAW_FILL_RECT
                   && command->color == 0xaabbcc) {
            top |= command->x == 3 && command->y == 0
                   && command->width == 10 && command->height == 2;
            bottom |= command->x == 3 && command->y == 6
                      && command->width == 10 && command->height == 2;
            left |= command->x == 3 && command->y == 0
                    && command->width == 2 && command->height == 8;
            right |= command->x == 11 && command->y == 0
                     && command->width == 2 && command->height == 8;
        }
    }
    CHECK(painted_image != NULL && painted_image->x == 6
          && painted_image->y == 3 && painted_image->width == 4
          && painted_image->height == 2 && background
          && top && bottom && left && right
          && command_x(&layout, "TAIL") == 18);
    layout_destroy(&layout);
    stylesheet_destroy(&sheet);
    document_destroy(&document);
    CHECK(budget->current == 0);
    return 0;
}

static int test_css_auto_overrides_image_dimension_hint(Budget *budget)
{
    static const char page[] =
        "<!doctype html><style>body{margin:0;padding:0}"
        "img{width:80px;height:auto}#block{display:block}</style><body>"
        "<img id=inline width=80 height=80>"
        "<img id=block width=80 height=80></body>";
    PocDocument document = {0};
    Stylesheet sheet = {0};
    LayoutDocument layout = {0};
    CHECK(document_parse(&document, budget, page, strlen(page), 19)
          && stylesheet_build(&sheet, budget, &document, 480));
    lxb_dom_node_t *root = lxb_dom_interface_node(document.html);
    lxb_dom_node_t *inline_node = find_id(root, "inline");
    lxb_dom_node_t *block_node = find_id(root, "block");
    static unsigned char pixel = 0xff;
    ImageResource items[] = {
        {.node = inline_node, .pixels = &pixel, .width = 121, .height = 88},
        {.node = block_node, .pixels = &pixel, .width = 150, .height = 48}
    };
    ImageResources images = {.items = items, .count = 2};
    CHECK(inline_node != NULL && block_node != NULL
          && layout_build(&layout, budget, &document, &sheet,
                          NULL, &images, 480));
    const DrawCommand *inline_image = NULL;
    const DrawCommand *block_image = NULL;
    for (size_t i = 0; i < layout.count; i++) {
        const DrawCommand *command = &layout.commands[i];
        if (command->type != DRAW_IMAGE) continue;
        if (command->image == &items[0]) inline_image = command;
        else if (command->image == &items[1]) block_image = command;
    }
    CHECK(inline_image != NULL && inline_image->width == 80
          && inline_image->height == 58
          && block_image != NULL && block_image->width == 80
          && block_image->height == 25);
    layout_destroy(&layout);
    stylesheet_destroy(&sheet);
    document_destroy(&document);
    CHECK(budget->current == 0);
    return 0;
}

static int test_responsive_replaced_sizing(Budget *budget)
{
    static const char page[] =
        "<!doctype html><style>body{margin:0;padding:0}"
        "#positioned{display:block;width:80px;height:40px;"
        "object-fit:contain;object-position:right bottom}"
        "#height-only{display:block;height:40px;object-fit:scale-down}"
        "#natural{display:block;width:80px;height:40px;object-fit:none;"
        "object-position:bottom 1px right 2px}"
        "#bounded{display:block;width:120px;max-width:60px}"
        "#minimum{display:block;width:20px;min-width:50px}"
        "#poster{display:block;width:100px;aspect-ratio:1/1;"
        "object-fit:contain;object-position:top left}"
        "#frame{display:block;width:100px;aspect-ratio:1/1;border:0}"
        "#flex-host{display:flex;width:80px}"
        "#flexed{width:120px;max-width:60px;min-width:0;flex:0 1 auto}"
        "#grid-host{display:grid;width:60px;grid-template-columns:60px}"
        "#gridded{display:block;width:100%;aspect-ratio:2/1;min-width:0}"
        "</style><body><img id=positioned><img id=height-only>"
        "<img id=natural><img id=bounded><img id=minimum>"
        "<video id=poster poster=poster.png></video>"
        "<iframe id=frame></iframe>"
        "<div id=flex-host><img id=flexed></div>"
        "<div id=grid-host><img id=gridded></div></body>";
    PocDocument document = {0};
    Stylesheet sheet = {0};
    LayoutDocument layout = {0};
    CHECK(document_parse(&document, budget, page, strlen(page), 19)
          && stylesheet_build(&sheet, budget, &document, 160));
    lxb_dom_node_t *root = lxb_dom_interface_node(document.html);
    lxb_dom_node_t *positioned = find_id(root, "positioned");
    lxb_dom_node_t *height_only = find_id(root, "height-only");
    lxb_dom_node_t *natural = find_id(root, "natural");
    lxb_dom_node_t *bounded = find_id(root, "bounded");
    lxb_dom_node_t *minimum = find_id(root, "minimum");
    lxb_dom_node_t *poster = find_id(root, "poster");
    lxb_dom_node_t *frame = find_id(root, "frame");
    lxb_dom_node_t *flexed = find_id(root, "flexed");
    lxb_dom_node_t *gridded = find_id(root, "gridded");
    static unsigned char pixels[20 * 10 * 4];
    ImageResource items[] = {
        {.node=positioned, .pixels=pixels, .width=20, .height=10},
        {.node=height_only, .pixels=pixels, .width=20, .height=10},
        {.node=natural, .pixels=pixels, .width=20, .height=10},
        {.node=bounded, .pixels=pixels, .width=20, .height=10},
        {.node=minimum, .pixels=pixels, .width=20, .height=10},
        {.node=poster, .pixels=pixels, .width=20, .height=50},
        {.node=flexed, .pixels=pixels, .width=20, .height=10},
        {.node=gridded, .pixels=pixels, .width=20, .height=10}
    };
    ImageResources images = {
        .items = items, .count = sizeof(items) / sizeof(items[0])
    };
    CHECK(positioned != NULL && height_only != NULL && natural != NULL
          && bounded != NULL && minimum != NULL
          && poster != NULL && frame != NULL
          && flexed != NULL && gridded != NULL
          && layout_build(&layout, budget, &document, &sheet,
                          NULL, &images, 160));
    const DrawCommand *positioned_command = NULL;
    const DrawCommand *height_command = NULL;
    const DrawCommand *natural_command = NULL;
    const DrawCommand *bounded_command = NULL;
    const DrawCommand *minimum_command = NULL;
    const DrawCommand *poster_command = NULL;
    const DrawCommand *flexed_command = NULL;
    const DrawCommand *gridded_command = NULL;
    for (size_t i = 0; i < layout.count; i++) {
        const DrawCommand *command = &layout.commands[i];
        if (command->type != DRAW_IMAGE) continue;
        if (command->image == &items[0]) positioned_command = command;
        else if (command->image == &items[1]) height_command = command;
        else if (command->image == &items[2]) natural_command = command;
        else if (command->image == &items[3]) bounded_command = command;
        else if (command->image == &items[4]) minimum_command = command;
        else if (command->image == &items[5]) poster_command = command;
        else if (command->image == &items[6]) flexed_command = command;
        else if (command->image == &items[7]) gridded_command = command;
    }
    const LayoutNodeBox *frame_box = layout_box_for_node(&layout, frame);
    ComputedStyle positioned_style = style_for_node(
        &sheet, positioned, NULL);
    CHECK(positioned_command != NULL
          && positioned_command->width == 80
          && positioned_command->height == 40
          && positioned_command->image_fit == LAYOUT_IMAGE_FIT_CONTAIN
          && style_object_position_percent(
                 (uint16_t) draw_command_image_offset_x(
                     positioned_command)) == 100
          && style_object_position_percent(
                 (uint16_t) draw_command_image_offset_y(
                     positioned_command)) == 100
          && style_object_position_percent(
                 positioned_style.object_position_x) == 100
          && style_object_position_percent(
                 positioned_style.object_position_y) == 100
          && height_command != NULL
          && height_command->width == 80
          && height_command->height == 40
          && height_command->image_fit == LAYOUT_IMAGE_FIT_SCALE_DOWN
          && natural_command != NULL
          && natural_command->image_fit == LAYOUT_IMAGE_FIT_NONE
          && style_object_position_percent(
                 (uint16_t) draw_command_image_offset_x(
                     natural_command)) == 100
          && style_object_position_offset(
                 (uint16_t) draw_command_image_offset_x(
                     natural_command)) == -2
          && style_object_position_percent(
                 (uint16_t) draw_command_image_offset_y(
                     natural_command)) == 100
          && style_object_position_offset(
                 (uint16_t) draw_command_image_offset_y(
                     natural_command)) == -1
          && bounded_command != NULL
          && bounded_command->width == 60
          && bounded_command->height == 30
          && minimum_command != NULL
          && minimum_command->width == 50
          && minimum_command->height == 25
          && poster_command != NULL
          && poster_command->width == 100
          && poster_command->height == 100
          && poster_command->image_fit == LAYOUT_IMAGE_FIT_CONTAIN
          && frame_box != NULL && frame_box->width == 100
          && frame_box->height == 100
          && flexed_command != NULL
          && flexed_command->width == 60
          && flexed_command->height == 30
          && gridded_command != NULL
          && gridded_command->width == 60
          && gridded_command->height == 30);
    layout_destroy(&layout);
    stylesheet_destroy(&sheet);
    document_destroy(&document);
    CHECK(budget->current == 0);
    return 0;
}

static int test_bottom_positioned_asset_uses_final_containing_height(
    Budget *budget)
{
    static const char page[] =
        "<!doctype html><style>html,body{margin:0;padding:0}"
        "#hero{position:relative;min-height:100px;width:100px}"
        "#asset{position:absolute;inset:0;height:100%}"
        "#asset{height:auto;top:auto;bottom:0}"
        "#picture{display:block;width:auto;height:auto}"
        "</style><body><div id=hero><div id=asset>"
        "<img id=picture></div></div></body>";
    PocDocument document = {0};
    Stylesheet sheet = {0};
    LayoutDocument layout = {0};
    CHECK(document_parse(&document, budget, page, strlen(page), 19)
          && stylesheet_build(&sheet, budget, &document, 160));
    lxb_dom_node_t *root = lxb_dom_interface_node(document.html);
    lxb_dom_node_t *hero = find_id(root, "hero");
    lxb_dom_node_t *asset = find_id(root, "asset");
    lxb_dom_node_t *picture = find_id(root, "picture");
    static unsigned char pixels[20 * 10 * 4];
    ImageResource item = {
        .node = picture, .pixels = pixels, .width = 20, .height = 10
    };
    ImageResources images = {.items = &item, .count = 1};
    CHECK(hero != NULL && asset != NULL && picture != NULL
          && layout_build(&layout, budget, &document, &sheet,
                          NULL, &images, 160));
    const LayoutNodeBox *hero_box = layout_box_for_node(&layout, hero);
    const LayoutNodeBox *asset_box = layout_box_for_node(&layout, asset);
    const DrawCommand *picture_command = NULL;
    for (size_t i = 0; i < layout.count; i++) {
        if (layout.commands[i].type == DRAW_IMAGE
            && layout.commands[i].image == &item) {
            picture_command = &layout.commands[i];
            break;
        }
    }
    CHECK(hero_box != NULL && asset_box != NULL && picture_command != NULL
          && hero_box->height == 100
          && asset_box->height == 10
          && asset_box->y == hero_box->y + 90
          && picture_command->y == asset_box->y);
    layout_destroy(&layout);
    stylesheet_destroy(&sheet);
    document_destroy(&document);
    CHECK(budget->current == 0);
    return 0;
}

static int test_declared_failed_image_geometry_converges(Budget *budget)
{
    static const char page[] =
        "<!doctype html><style>body{margin:0;padding:0}</style><body>"
        "<img id=declared src=/missing.png width=80 height=40 "
        "alt='A deliberately wider fallback'><span id=tail>TAIL</span>"
        "</body>";
    PocDocument document = {0};
    Stylesheet sheet = {0};
    LayoutDocument preview = {0};
    LayoutDocument authoritative = {0};
    ViewportContext viewport = {0};
    CHECK(document_parse(&document, budget, page, strlen(page), 19)
          && stylesheet_build(&sheet, budget, &document, 160)
          && viewport_context_init(&viewport, 160, 272, 160, 272));
    bool truncated = true;
    CHECK(layout_build_context_preview(
              &preview, budget, &document, &sheet, NULL, NULL,
              &viewport, NULL, 200, &truncated)
          && !truncated
          && layout_build_context(
              &authoritative, budget, &document, &sheet, NULL, NULL,
              &viewport));
    lxb_dom_node_t *root = lxb_dom_interface_node(document.html);
    lxb_dom_node_t *image = find_id(root, "declared");
    lxb_dom_node_t *tail = find_id(root, "tail");
    const LayoutNodeBox *preview_image =
        layout_box_for_node(&preview, image);
    const LayoutNodeBox *final_image =
        layout_box_for_node(&authoritative, image);
    const LayoutNodeBox *preview_tail =
        layout_box_for_node(&preview, tail);
    const LayoutNodeBox *final_tail =
        layout_box_for_node(&authoritative, tail);
    bool preview_alt = false, final_alt = false;
    size_t alt_length = 0;
    const char *alt_text = document_attribute(image, "alt", &alt_length);
    for (size_t i = 0; i < preview.count; i++) {
        const DrawCommand *command = &preview.commands[i];
        if (command->type == DRAW_TEXT
            && command->text == alt_text && command->text_length != 0
            && command->text_length <= alt_length) preview_alt = true;
    }
    for (size_t i = 0; i < authoritative.count; i++) {
        const DrawCommand *command = &authoritative.commands[i];
        if (command->type == DRAW_TEXT
            && command->text == alt_text && command->text_length != 0
            && command->text_length <= alt_length) final_alt = true;
    }
    CHECK(preview_image != NULL && final_image != NULL
          && preview_image->x == final_image->x
          && preview_image->y == final_image->y
          && preview_image->width == 80 && final_image->width == 80
          && preview_image->height == 40 && final_image->height == 40
          && preview_tail != NULL && final_tail != NULL
          && preview_tail->x == final_tail->x
          && preview_tail->y == final_tail->y
          && preview_alt && final_alt);
    layout_destroy(&authoritative);
    layout_destroy(&preview);
    stylesheet_destroy(&sheet);
    document_destroy(&document);
    CHECK(budget->current == 0);
    return 0;
}

static int test_width_only_failed_image_in_flex_shows_alt(Budget *budget)
{
    static const char page[] =
        "<!doctype html><style>body{margin:0}brand-shell,brand-shell a{"
        "display:flex}img{width:108px;height:auto}</style><body>"
        "<brand-shell id=brand><a><img id=logo src=/missing.png "
        "alt='Example brand'></a></brand-shell></body>";
    PocDocument document = {0};
    Stylesheet sheet = {0};
    LayoutDocument layout = {0};
    CHECK(document_parse(&document, budget, page, sizeof(page) - 1, 20)
          && stylesheet_build(&sheet, budget, &document, 160)
          && layout_build(&layout, budget, &document, &sheet,
                          NULL, NULL, 160));
    lxb_dom_node_t *root = lxb_dom_interface_node(document.html);
    lxb_dom_node_t *brand = find_id(root, "brand");
    lxb_dom_node_t *logo = find_id(root, "logo");
    const LayoutNodeBox *brand_box = layout_box_for_node(&layout, brand);
    const LayoutNodeBox *logo_box = layout_box_for_node(&layout, logo);
    bool found_alt_first = false;
    bool found_alt_second = false;
    for (size_t i = 0; i < layout.count; i++) {
        const DrawCommand *command = &layout.commands[i];
        if (command->type != DRAW_TEXT) continue;
        if (command->text_length == 7
            && memcmp(command->text, "Example", 7) == 0) {
            found_alt_first = true;
        } else if (command->text_length == 5
                   && memcmp(command->text, "brand", 5) == 0) {
            found_alt_second = true;
        }
    }
    CHECK(brand_box != NULL);
    CHECK(logo_box != NULL);
    CHECK(brand_box->height > 0);
    CHECK(logo_box->height > 0);
    CHECK(found_alt_first && found_alt_second);
    layout_destroy(&layout);
    stylesheet_destroy(&sheet);
    document_destroy(&document);
    CHECK(budget->current == 0);
    return 0;
}

static bool layout_command_equivalent(const DrawCommand *left,
                                      const DrawCommand *right)
{
    if (left == NULL || right == NULL
        || left->type != right->type
        || left->x != right->x || left->y != right->y
        || left->width != right->width || left->height != right->height
        || left->color != right->color
        || left->text_length != right->text_length
        || left->z_index != right->z_index
        || left->opacity_scale != right->opacity_scale) return false;
    return left->text_length == 0
        || (left->text != NULL && right->text != NULL
            && memcmp(left->text, right->text, left->text_length) == 0);
}

static int test_resumable_authoritative_layout(Budget *budget)
{
    static const char page[] =
        "<!doctype html><style>html,body{margin:0;padding:0}"
        ".flex{display:flex;gap:3px}.flex>*{width:30px;height:12px}"
        ".grid{display:grid;grid-template-columns:40px 1fr;gap:2px}"
        "table{border-spacing:2px}td{padding:1px}</style><body>"
        "<main><div class=flex><span>ONE</span><span>TWO</span></div>"
        "<div class=grid><b>GRID</b><i>ITEM</i></div>"
        "<table><tr><td>A</td><td>B</td></tr></table>"
        "<p>TAIL WORDS</p></main></body>";
    PocDocument document = {0};
    Stylesheet sheet = {0};
    LayoutDocument monolithic = {0};
    LayoutDocument resumed = {0};
    ViewportContext viewport = {0};
    CHECK(document_parse(&document, budget, page, sizeof(page) - 1, 23)
          && stylesheet_build(&sheet, budget, &document, 240)
          && viewport_context_init(&viewport, 240, 272, 240, 272));
    bool truncated = true;
    CHECK(layout_build_context_preview(
              &monolithic, budget, &document, &sheet, NULL, NULL,
              &viewport, NULL, INT_MAX, &truncated)
          && !truncated);
    size_t warm_baseline = budget->current;

    LayoutBuildJob *job = layout_build_job_begin(
        budget, &document, &sheet, NULL, NULL, &viewport, NULL);
    CHECK(job != NULL);
    LayoutBuildStatus status = LAYOUT_BUILD_PENDING;
    size_t pumps = 0;
    while (status == LAYOUT_BUILD_PENDING && pumps < 16) {
        status = layout_build_job_pump(job);
        pumps++;
    }
    CHECK(status == LAYOUT_BUILD_COMPLETE && pumps == 8
          && layout_build_job_take(job, &resumed));
    layout_build_job_destroy(job);
    CHECK(resumed.width == monolithic.width
          && resumed.height == monolithic.height
          && resumed.scroll_width == monolithic.scroll_width
          && resumed.count == monolithic.count
          && resumed.link_count == monolithic.link_count
          && resumed.control_count == monolithic.control_count
          && resumed.node_box_count == monolithic.node_box_count
          && resumed.paint_order_count == monolithic.paint_order_count
          && resumed.text_fingerprint == monolithic.text_fingerprint
          && resumed.performance.resumable_phases == 8
          && resumed.performance.resumable_passes == 1);
    for (size_t i = 0; i < resumed.count; i++) {
        CHECK(layout_command_equivalent(
            &resumed.commands[i], &monolithic.commands[i]));
    }
    layout_destroy(&resumed);
    CHECK(budget->current == warm_baseline);

    job = layout_build_job_begin(
        budget, &document, &sheet, NULL, NULL, &viewport, NULL);
    CHECK(job != NULL
          && layout_build_job_pump(job) == LAYOUT_BUILD_PENDING
          && layout_build_job_pump(job) == LAYOUT_BUILD_PENDING);
    layout_build_job_cancel(job);
    CHECK(layout_build_job_pump(job) == LAYOUT_BUILD_CANCELLED);
    layout_build_job_destroy(job);
    CHECK(budget->current == warm_baseline);

    /* Preview and tiny synchronous builds share cancellation ownership with
       the explicit continuation; a refused preview must never look complete. */
    tilefinch_test_faults()->cancel_next_layout_cooperate = true;
    truncated = false;
    CHECK(!layout_build_context_preview(
        &resumed, budget, &document, &sheet, NULL, NULL, &viewport,
        NULL, INT_MAX, &truncated));
    CHECK(resumed.performance.cancelled
          && !tilefinch_test_faults()->cancel_next_layout_cooperate
          && resumed.commands == NULL && budget->current == warm_baseline);
    layout_destroy(&resumed);
    for (size_t failure = 0; failure < 4; failure++) {
        budget_inject_failure_after(budget, failure);
        (void) layout_build_context_preview(
            &resumed, budget, &document, &sheet, NULL, NULL, &viewport,
            NULL, 16, &truncated);
        budget_clear_failure_injection(budget);
        layout_destroy(&resumed);
        CHECK(budget->current == warm_baseline);
    }

    layout_destroy(&monolithic);
    stylesheet_destroy(&sheet);
    document_destroy(&document);
    CHECK(budget->current == 0);
    return 0;
}

static int test_table_column_and_rowspan_construction(Budget *budget)
{
    extern size_t table_cell_row_span(lxb_dom_node_t *cell);
    static const char page[] =
        "<!doctype html><style>body{margin:0;padding:0}"
        "table{width:300px}td{padding:0;height:20px}"
        "caption{caption-side:bottom;height:12px}</style><body>"
        "<table id=grid><caption id=caption>CAPTION</caption>"
        "<colgroup><col style='width:80px'><col>"
        "<col style='width:60px'></colgroup><tbody>"
        "<tr id=first><td id=span rowspan=2>A</td>"
        "<td id=b>B</td><td id=c>C</td></tr>"
        "<tr id=second><td id=wide colspan=2>WIDE</td></tr>"
        "</tbody></table></body>";
    PocDocument document = {0};
    Stylesheet sheet = {0};
    LayoutDocument layout = {0};
    CHECK(document_parse(&document, budget, page, strlen(page), 19)
          && stylesheet_build(&sheet, budget, &document, 320)
          && layout_build(&layout, budget, &document, &sheet,
                          NULL, NULL, 320));
    lxb_dom_node_t *root = lxb_dom_interface_node(document.html);
    lxb_dom_node_t *grid = find_id(root, "grid");
    lxb_dom_node_t *first = find_id(root, "first");
    lxb_dom_node_t *second = find_id(root, "second");
    lxb_dom_node_t *span = find_id(root, "span");
    lxb_dom_node_t *b = find_id(root, "b");
    lxb_dom_node_t *c = find_id(root, "c");
    lxb_dom_node_t *wide = find_id(root, "wide");
    lxb_dom_node_t *caption = find_id(root, "caption");
    const LayoutNodeBox *grid_box = layout_box_for_node(&layout, grid);
    const LayoutNodeBox *first_box = layout_box_for_node(&layout, first);
    const LayoutNodeBox *second_box = layout_box_for_node(&layout, second);
    const LayoutNodeBox *span_box = layout_box_for_node(&layout, span);
    const LayoutNodeBox *b_box = layout_box_for_node(&layout, b);
    const LayoutNodeBox *c_box = layout_box_for_node(&layout, c);
    const LayoutNodeBox *wide_box = layout_box_for_node(&layout, wide);
    const LayoutNodeBox *caption_box = layout_box_for_node(&layout, caption);
    CHECK(grid_box != NULL && grid_box->width == 300
          && first_box != NULL && second_box != NULL
          && span_box != NULL && span_box->width >= 80
          && b_box != NULL && b_box->x == span_box->x + span_box->width
          && c_box != NULL && c_box->width >= 60
          && wide_box != NULL && wide_box->x == b_box->x
          && wide_box->width == b_box->width + c_box->width
          && second_box->y >= first_box->y + first_box->height
          && caption_box != NULL
          && caption_box->y >= second_box->y + second_box->height
          && table_cell_row_span(span) == 2);
    layout_destroy(&layout);
    stylesheet_destroy(&sheet);
    document_destroy(&document);
    CHECK(budget->current == 0);
    return 0;
}

static int test_flex_percentage_image_minimum(Budget *budget)
{
    static const char page[] =
        "<!doctype html><style>body{margin:0;padding:0}.card{display:flex;"
        "width:432px;gap:15px}.copy{flex:1 1 70%}.art{flex:0 0 80px;"
        "margin:0}.art img{display:block;width:100%;height:auto}</style>"
        "<body><div class=card><div class=copy>COPY</div>"
        "<figure id=art class=art><img id=card-image></figure></div></body>";
    PocDocument document = {0};
    Stylesheet sheet = {0};
    LayoutDocument layout = {0};
    CHECK(document_parse(&document, budget, page, strlen(page), 19)
          && stylesheet_build(&sheet, budget, &document, 480));
    lxb_dom_node_t *root = lxb_dom_interface_node(document.html);
    lxb_dom_node_t *art = find_id(root, "art");
    lxb_dom_node_t *image_node = find_id(root, "card-image");
    static unsigned char rgba[] = {255, 0, 0, 255};
    ImageResource item = {
        .node = image_node, .pixels = rgba, .width = 160, .height = 160
    };
    ImageResources images = {.items = &item, .count = 1};
    CHECK(art != NULL && image_node != NULL
          && layout_build(&layout, budget, &document, &sheet,
                          NULL, &images, 480));
    const LayoutNodeBox *art_box = layout_box_for_node(&layout, art);
    const LayoutNodeBox *image_box = layout_box_for_node(
        &layout, image_node);
    const DrawCommand *painted_image = NULL;
    for (size_t i = 0; i < layout.count; i++) {
        if (layout.commands[i].type == DRAW_IMAGE
            && layout.commands[i].image == &item) {
            painted_image = &layout.commands[i];
            break;
        }
    }
    CHECK(art_box != NULL && art_box->width == 80
          && image_box != NULL && image_box->width == 80
          && painted_image != NULL && painted_image->width == 80
          && painted_image->height == 80);
    layout_destroy(&layout);
    stylesheet_destroy(&sheet);
    document_destroy(&document);
    CHECK(budget->current == 0);
    return 0;
}

static int test_flex_definite_replaced_auto_minimum(Budget *budget)
{
    static const char page[] =
        "<!doctype html><style>body{margin:0}button{display:flex;"
        "box-sizing:border-box;width:42px;height:42px;padding:8px 16px;"
        "border:0}svg{display:block;width:24px;height:24px}</style>"
        "<button id=button><svg id=icon viewBox='0 0 24 24'>"
        "<path d='M2 12h20'/></svg></button>";
    PocDocument document = {0};
    Stylesheet sheet = {0};
    LayoutDocument layout = {0};
    CHECK(document_parse(&document, budget, page, strlen(page), 19)
          && stylesheet_build(&sheet, budget, &document, 480)
          && layout_build(&layout, budget, &document, &sheet,
                          NULL, NULL, 480));
    lxb_dom_node_t *root = lxb_dom_interface_node(document.html);
    const LayoutNodeBox *button = layout_box_for_node(
        &layout, find_id(root, "button"));
    const LayoutNodeBox *icon = layout_box_for_node(
        &layout, find_id(root, "icon"));
    CHECK(button != NULL && button->width == 42
          && icon != NULL && icon->width == 24 && icon->height == 24);
    layout_destroy(&layout);
    stylesheet_destroy(&sheet);
    document_destroy(&document);
    CHECK(budget->current == 0);
    return 0;
}
static int test_flex_percentage_width_intrinsic(Budget *budget)
{
    static const char page[] =
        "<!doctype html><style>body{margin:0;padding:0}"
        "#outer,#options{display:flex}#outer{width:450px}"
        "#label{margin-right:10px}#second{margin-left:10px}"
        ".option button{display:block;box-sizing:border-box;width:100%;"
        "min-width:80px;height:10px;padding:0;border:0}</style><body>"
        "<div id=outer><div id=label>Label</div><div id=options class=options>"
        "<div id=first class=option><button></button></div>"
        "<div id=second class=option><button></button></div>"
        "</div></div></body>";
    PocDocument document = {0};
    Stylesheet sheet = {0};
    LayoutDocument layout = {0};
    CHECK(document_parse(&document, budget, page, strlen(page), 19)
          && stylesheet_build(&sheet, budget, &document, 480)
          && layout_build(&layout, budget, &document, &sheet,
                          NULL, NULL, 480));
    lxb_dom_node_t *root = lxb_dom_interface_node(document.html);
    const LayoutNodeBox *options = layout_box_for_node(
        &layout, find_id(root, "options"));
    const LayoutNodeBox *first = layout_box_for_node(
        &layout, find_id(root, "first"));
    const LayoutNodeBox *second = layout_box_for_node(
        &layout, find_id(root, "second"));
    CHECK(options != NULL && options->width == 170
          && first != NULL && first->width == 80
          && second != NULL && second->width == 80
          && second->x == first->x + first->width + 10);
    layout_destroy(&layout);
    stylesheet_destroy(&sheet);
    document_destroy(&document);
    CHECK(budget->current == 0);
    return 0;
}
static int test_generated_block_pseudo_flow(Budget *budget)
{
    static const char page[] =
        "<!doctype html><style>body{margin:0;padding:0}"
        "#link{font-size:10px;line-height:10px}"
        "#link::before{content:'';display:block;padding-top:12px}"
        "#after{height:1px}</style><body>"
        "<a id=link>TEXT</a><div id=after></div></body>";
    PocDocument document = {0};
    Stylesheet sheet = {0};
    LayoutDocument layout = {0};
    CHECK(document_parse(&document, budget, page, strlen(page), 19)
          && stylesheet_build(&sheet, budget, &document, 480)
          && layout_build(&layout, budget, &document, &sheet,
                          NULL, NULL, 480));
    const DrawCommand *text = text_command(&layout, "TEXT");
    const LayoutNodeBox *after = layout_box_for_node(
        &layout, find_id(lxb_dom_interface_node(document.html), "after"));
    CHECK(text != NULL && text->y == 12
          && after != NULL && after->y == 22);
    layout_destroy(&layout);
    stylesheet_destroy(&sheet);
    document_destroy(&document);
    CHECK(budget->current == 0);
    return 0;
}

static int test_generated_content_counters_and_markers(Budget *budget)
{
    static const char page[] =
        "<!doctype html><style>body{margin:0;padding:0;font:10px/10px sans-serif}"
        "#multi:before{content:no-open-quote 'P' 'A' 'S' 'S'}"
        "#count{counter-reset:item}"
        "#count span{counter-increment:item}"
        "#count span::before{content:counter(item) '.' attr(data-label)}"
        "ol{list-style-type:lower-alpha;list-style-position:inside}"
        "</style><body><div id=multi></div><div id=count>"
        "<span data-label=A></span><span data-label=B></span></div>"
        "<ol start=3><li>THREE</li><li value=8>EIGHT</li>"
        "<li>NINE</li></ol></body>";
    PocDocument document = {0};
    Stylesheet sheet = {0};
    LayoutDocument layout = {0};
    CHECK(document_parse(&document, budget, page, strlen(page), 19)
          && stylesheet_build(&sheet, budget, &document, 480)
          && layout_build(&layout, budget, &document, &sheet,
                          NULL, NULL, 480));
    lxb_dom_node_t *root = lxb_dom_interface_node(document.html);
    const LayoutNodeBox *multi_box = layout_box_for_node(
        &layout, find_id(root, "multi"));
    const LayoutNodeBox *count_box = layout_box_for_node(
        &layout, find_id(root, "count"));
    CHECK(text_command(&layout, "PASS") != NULL
          && text_command(&layout, "1.A") != NULL
          && text_command(&layout, "2.B") != NULL
          && text_command(&layout, "c.") != NULL
          && text_command(&layout, "h.") != NULL
          && text_command(&layout, "i.") != NULL
          && multi_box != NULL && count_box != NULL
          && multi_box->height > 0
          && count_box->y >= multi_box->y + multi_box->height);
    layout_destroy(&layout);
    stylesheet_destroy(&sheet);
    document_destroy(&document);
    CHECK(budget->current == 0);
    return 0;
}

static int test_overflow_index_uses_nearby_bands(Budget *budget)
{
    enum { PAIRS = 1000 };
    LayoutDocument layout = {0};
    layout.budget = budget;
    layout.height = PAIRS * 100;
    layout.count = layout.paint_order_count = PAIRS * 2;
    layout.node_box_count = PAIRS;
    layout.commands = budget_calloc(budget, layout.count, sizeof(*layout.commands));
    layout.paint_order = budget_calloc(budget, layout.count, sizeof(*layout.paint_order));
    layout.node_boxes = budget_calloc(budget, PAIRS, sizeof(*layout.node_boxes));
    layout.fixed_ranges = budget_calloc(budget, 1, sizeof(*layout.fixed_ranges));
    LayoutContext *context = budget_calloc(budget, 1, sizeof(*context));
    CHECK(layout.commands && layout.paint_order && layout.node_boxes
          && layout.fixed_ranges && context);
    context->layout = &layout;
    for (size_t i = 0; i < PAIRS; i++) {
        DrawCommand *overflow = &layout.commands[2 * i];
        overflow->type = DRAW_FILL_RECT;
        overflow->y = (int) i * 100;
        overflow->width = overflow->height = 10;
        layout.commands[2 * i + 1] = *overflow;
        layout.commands[2 * i + 1].x = i < 3 ? 0 : 20;
        layout.commands[2 * i + 1].z_index = 1;
        layout.paint_order[2 * i] = (uint32_t) (2 * i);
        layout.paint_order[2 * i + 1] = (uint32_t) (2 * i + 1);
        layout.node_boxes[i].clips_x = true;
        layout.node_boxes[i].scroll_command_start = (uint32_t) (2 * i);
        layout.node_boxes[i].scroll_command_end = (uint32_t) (2 * i + 1);
    }
    /* Exercise local, globally indexed and fixed/unindexed overflow. */
    layout.commands[2].height = 4096;
    layout.fixed_count = 1;
    layout.fixed_ranges[0].command_start = 4;
    layout.fixed_ranges[0].command_end = 5;
    CHECK(build_spatial_index(&layout, context));
    CHECK(layout.late_positioned_order_count == 3);
    for (size_t i = 0; i < 3; i++) {
        CHECK(layout.late_positioned_orders[i] == 2 * i + 1);
    }
    CHECK(layout.layout_work_units < 100000);
    /* Late-positioned commands retain their ordinary document-y index.
       Hoisting a large context must not force a whole-document walk on
       every small viewport repaint. All odd records are now red overlays. */
    uint32_t *late = budget_realloc(budget, layout.late_positioned_orders,
        PAIRS * sizeof(*late));
    CHECK(late != NULL);
    layout.late_positioned_orders = late;
    layout.late_positioned_order_count = PAIRS;
    for (size_t i = 0; i < PAIRS; i++) {
        size_t index = 2 * i + 1;
        late[i] = (uint32_t) index;
        layout.command_flags[index] |= LAYOUT_COMMAND_LATE_POSITIONED;
        layout.commands[index].color = 0xff0000;
        layout.commands[index].opacity_scale = 256;
    }
    TileCache cache = {0};
    uint16_t frame[64 * 64];
    CHECK(tile_cache_init(&cache, budget, &layout, 1)
        && tile_cache_set_frame(&cache, frame, 64 * 64)
        && tile_cache_render_frame(&cache, 0, 64, 64, NULL));
    CHECK(frame[5 * 64 + 5] == 0xf800);
    CHECK(cache.command_candidates < 128);
    tile_cache_destroy(&cache);
    budget_free(budget, context);
    layout_destroy(&layout);
    CHECK(budget->current == 0);
    return 0;
}

typedef struct {
    LayoutReuseCache *reuse;
    uint64_t now;
    bool cancelled;
} CounterReuseCancel;

static uint64_t counter_reuse_cancel_clock(void *opaque)
{
    CounterReuseCancel *probe = opaque;
    probe->now += UINT64_C(8000000);
    return probe->now;
}

static bool counter_reuse_cancel_poll(void *opaque, const char *phase, size_t work)
{
    (void) phase;
    (void) work;
    CounterReuseCancel *probe = opaque;
    if (probe->reuse->counter_entries == NULL) probe->cancelled = true;
    return !probe->cancelled;
}

static int test_generated_counter_lookup_work(Budget *budget)
{
    char html[8192];
    static const char prefix[] = "<!doctype html><style>body{counter-reset:n}"
        "span{counter-increment:n}span::before{content:counter(n) ':'}"
        "</style><body>";
    size_t length = sizeof(prefix) - 1u;
    memcpy(html, prefix, length);
    static const char label[] = "<span>x</span>";
    for (unsigned i = 0; i < 96; i++) {
        CHECK(sizeof(label) - 1u < sizeof(html) - length);
        memcpy(html + length, label, sizeof(label) - 1u);
        length += sizeof(label) - 1u;
    }
    PocDocument document = {0};
    Stylesheet sheet = {0};
    LayoutDocument layout = {0};
    ViewportContext viewport;
    CHECK(document_parse(&document, budget, html, length, 19)
        && viewport_context_init(&viewport, 480, 272, 480, 272)
        && stylesheet_build_context(&sheet, budget, &document, &viewport));
    LayoutReuseCache *reuse = layout_reuse_cache_create(budget);
    CHECK(reuse != NULL && layout_build_context_reuse(&layout, budget,
        &document, &sheet, NULL, NULL, &viewport, reuse));
    LayoutReuseStats stats;
    layout_reuse_cache_stats(reuse, &stats);
    printf("counter lookup: entries=%zu visits=%zu\n", reuse->counter_count,
        stats.counter_lookup_visits);
    CHECK(text_command(&layout, "96:") != NULL && stats.counter_lookup_visits != 0
        && stats.counter_lookup_visits <= 8u * reuse->counter_count);
    uint64_t fingerprint = layout.text_fingerprint;
    size_t visits = stats.counter_lookup_visits;
    layout_destroy(&layout);
    layout_reuse_cache_begin_font_publication(reuse);
    CHECK(layout_build_context_reuse(&layout, budget, &document, &sheet,
        NULL, NULL, &viewport, reuse));
    layout_reuse_cache_end_font_publication(reuse);
    layout_reuse_cache_stats(reuse, &stats);
    CHECK(layout.text_fingerprint == fingerprint
        && stats.counter_lookup_visits - visits <= 8u * reuse->counter_count);
    layout_destroy(&layout);
    /* Reordered flow must wrap the hint without changing DOM-order counter
       values. Moving the first label last forces a backward lookup. */
    static const char reordered[] = "body{display:flex;flex-wrap:wrap}"
        "span:first-child{order:1}";
    CHECK(stylesheet_add_css(&sheet, reordered, sizeof(reordered) - 1u)
        && layout_build_context_reuse(&layout, budget, &document, &sheet,
            NULL, NULL, &viewport, reuse));
    const DrawCommand *first = text_command(&layout, "1:");
    const DrawCommand *last = text_command(&layout, "96:");
    CHECK(first != NULL && last != NULL && first > last);
    layout_destroy(&layout);
    layout_reuse_cache_destroy(reuse);
    stylesheet_destroy(&sheet);
    document_destroy(&document);
    CHECK(budget->current == 0);
    return 0;
}

static int test_generated_counter_resource_reuse(Budget *budget)
{
    static const char html[] =
        "<!doctype html><style>body{counter-reset:n}"
        "span{counter-increment:n}span::before{content:counter(n) ':'}"
        ".gone{display:none}</style><body>"
        "<span>first</span><div><span>second</span></div><span>third</span>";
    PocDocument document = {0};
    Stylesheet sheet = {0};
    LayoutDocument layout = {0};
    ViewportContext viewport;
    CHECK(document_parse(&document, budget, html, sizeof(html) - 1, 19)
        && viewport_context_init(&viewport, 480, 272, 480, 272)
        && stylesheet_build_context(&sheet, budget, &document, &viewport));
    LayoutReuseCache *reuse = layout_reuse_cache_create(budget);
    CHECK(reuse != NULL && layout_build_context_reuse(&layout, budget,
        &document, &sheet, NULL, NULL, &viewport, reuse));
    CHECK(text_command(&layout, "3:") != NULL);
    uint64_t fingerprint = layout.text_fingerprint;
    LayoutReuseStats initial, after;
    layout_reuse_cache_stats(reuse, &initial);
    CHECK(initial.counter_index_builds == 1 && initial.counter_index_hits == 0);
    layout_destroy(&layout);
    layout_reuse_cache_begin_font_publication(reuse);
    CHECK(layout_build_context_reuse(&layout, budget, &document, &sheet,
        NULL, NULL, &viewport, reuse));
    layout_reuse_cache_end_font_publication(reuse);
    layout_reuse_cache_stats(reuse, &after);
    CHECK(after.counter_index_builds == 1 && after.counter_index_hits == 1
        && after.retained_bytes == initial.retained_bytes
        && layout.text_fingerprint == fingerprint);
    layout_destroy(&layout);
    ImageResources images = {0};
    layout_reuse_cache_update_images(reuse, &images);
    CHECK(layout_build_context_reuse(&layout, budget, &document, &sheet,
        NULL, &images, &viewport, reuse));
    layout_reuse_cache_stats(reuse, &after);
    CHECK(after.counter_index_hits == 2 && layout.text_fingerprint == fingerprint);
    layout_destroy(&layout);

    size_t before_cancel = budget->current;
    size_t counter_bytes = reuse->counter_capacity * sizeof(LayoutCounterEntry);
    CounterReuseCancel cancel = {.reuse = reuse};
    TilefinchPlatformServices services = {.context = &cancel,
        .monotonic_time_ns = counter_reuse_cancel_clock,
        .cooperate = counter_reuse_cancel_poll};
    tilefinch_platform_set_services(&services);
    bool built = layout_build_context_reuse(&layout, budget, &document, &sheet,
        NULL, &images, &viewport, reuse);
    tilefinch_platform_set_services(NULL);
    CHECK(!built && cancel.cancelled && layout.performance.cancelled);
    layout_destroy(&layout);
    CHECK(reuse->counter_entries == NULL
        && budget->current <= before_cancel - counter_bytes);
    CHECK(layout_build_context_reuse(&layout, budget, &document, &sheet,
        NULL, &images, &viewport, reuse)
        && layout.text_fingerprint == fingerprint);
    layout_destroy(&layout);
    CHECK(budget->current == before_cancel);

    /* A scoped display mutation changes the ordered prefix, not only the
       changed node's own value. Rebuild before numbering later siblings. */
    lxb_dom_node_t *group = document_body_node(&document)->first_child->next;
    CHECK(group != NULL);
    BudgetAllocationOwner owner = document_allocation_owner_enter(&document);
    CHECK(lxb_dom_element_set_attribute(lxb_dom_interface_element(group),
        (const lxb_char_t *) "class", 5, (const lxb_char_t *) "gone", 4) != NULL);
    document_allocation_owner_leave(&document, owner);
    layout_reuse_cache_invalidate_node_scoped(reuse, group, false);
    CHECK(layout_build_context_reuse(&layout, budget, &document, &sheet,
        NULL, &images, &viewport, reuse));
    layout_reuse_cache_stats(reuse, &after);
    CHECK(after.counter_index_builds == 3 && text_command(&layout, "3:") == NULL
        && text_command(&layout, "2:") != NULL);
    layout_destroy(&layout);
    static const char reset[] = "body{counter-reset:n 10}";
    CHECK(stylesheet_add_css(&sheet, reset, sizeof(reset) - 1));
    CHECK(layout_build_context_reuse(&layout, budget, &document, &sheet,
        NULL, &images, &viewport, reuse));
    CHECK(text_command(&layout, "12:") != NULL);
    layout_destroy(&layout);
    /* Container-query conditions can change when font/image geometry does.
       Such sheets deliberately rebuild instead of retaining this proof. */
    static const char query[] = "@container (min-width:1px){span{counter-increment:n 2}}";
    CHECK(stylesheet_add_css(&sheet, query, sizeof(query) - 1)
        && stylesheet_has_container_queries(&sheet));
    CHECK(layout_build_context_reuse(&layout, budget, &document, &sheet,
        NULL, &images, &viewport, reuse));
    layout_reuse_cache_stats(reuse, &initial);
    layout_destroy(&layout);
    CHECK(layout_build_context_reuse(&layout, budget, &document, &sheet,
        NULL, &images, &viewport, reuse));
    layout_reuse_cache_stats(reuse, &after);
    CHECK(after.counter_index_builds == initial.counter_index_builds + 1
        && after.counter_index_hits == initial.counter_index_hits);
    layout_destroy(&layout);
    layout_reuse_cache_destroy(reuse);
    stylesheet_destroy(&sheet);
    document_destroy(&document);
    CHECK(budget->current == 0);
    return 0;
}

static int test_generated_counter_analysis_is_shared(Budget *budget)
{
    char page[48000];
    size_t used = (size_t) snprintf(page, sizeof(page),
        "<!doctype html><style>body{counter-reset:item}"
        "span{counter-increment:item}span::before{content:counter(item) ':'}"
        "</style><body>");
    for (unsigned i = 0; i < 180; i++) {
        used += (size_t) snprintf(page + used, sizeof(page) - used,
                                 "<span>entry</span>");
    }
    used += (size_t) snprintf(page + used, sizeof(page) - used,
        "<div style='counter-reset:item 10'><span>nested</span></div>"
        "<div style='display:none'><span>not counted</span></div>"
        "<span>outer</span>");
    PocDocument document = {0};
    Stylesheet sheet = {0};
    LayoutDocument layout = {0};
    CHECK(document_parse(&document, budget, page, used, 19)
          && stylesheet_build(&sheet, budget, &document, 480));
    uint64_t before = sheet.rule_index_queries;
    CHECK(layout_build(&layout, budget, &document, &sheet, NULL, NULL, 480));
    CHECK(text_command(&layout, "1:") != NULL
          && text_command(&layout, "180:") != NULL
          && text_command(&layout, "11:") != NULL
          && text_command(&layout, "181:") != NULL);
    /* Each counter must not resolve the styles of its entire DOM prefix.
       Count work instead of wall time so this holds on host and Allegrex. */
    CHECK(sheet.rule_index_queries - before < 10000);
    /* Sequential labels must reuse the analyzed prefix, not replay it for
       every measurement and emission. Includes nested-scope restoration. */
    CHECK(layout.layout_work_units < 10000u);
    layout_destroy(&layout);
    size_t parsed_count = document.element_count;
    BudgetAllocationOwner owner = document_allocation_owner_enter(&document);
    for (unsigned i = 0; i < 32; ++i) {
        lxb_dom_element_t *element = lxb_dom_document_create_element(
            &document.html->dom_document, (const lxb_char_t *) "span", 4, NULL);
        CHECK(element != NULL
              && lxb_dom_node_append_child(document_body_node(&document),
                   lxb_dom_interface_node(element)) == LXB_DOM_EXCEPTION_OK);
    }
    document_allocation_owner_leave(&document, owner);
    CHECK(document.element_count == parsed_count);
    CHECK(layout_build(&layout, budget, &document, &sheet, NULL, NULL, 480));
    CHECK(text_command(&layout, "213:") != NULL);
    layout_destroy(&layout);
    stylesheet_destroy(&sheet);
    document_destroy(&document);
    CHECK(budget->current == 0);
    used = (size_t) snprintf(page, sizeof(page),
        "<!doctype html><style>body{counter-reset:a 0 b 100}"
        "span{counter-increment:a b}"
        "span::before{counter-increment:a 10;content:counter(a) ':' counter(b)}"
        "</style><body><span>first</span><span>second</span>");
    CHECK(document_parse(&document, budget, page, used, 19)
          && stylesheet_build(&sheet, budget, &document, 480)
          && layout_build(&layout, budget, &document, &sheet, NULL, NULL, 480));
    /* Alternating names cannot share state; pseudo increments must not
       persist into either the next measurement or the next sibling. */
    CHECK(text_command(&layout, "11:101") != NULL
          && text_command(&layout, "12:102") != NULL);
    layout_destroy(&layout);
    stylesheet_destroy(&sheet);
    document_destroy(&document);
    CHECK(budget->current == 0);
    used = (size_t) snprintf(page, sizeof(page),
        "<!doctype html><style>body{counter-reset:item}"
        "span::before{content:counter(item) ':'}</style><body>");
    for (unsigned i = 0; i < 4300; i++)
        used += (size_t) snprintf(page + used, sizeof(page) - used, "<i></i>");
    for (unsigned i = 0; i < 96; i++)
        used += (size_t) snprintf(page + used, sizeof(page) - used, "<span>tail</span>");
    CHECK(used < sizeof(page)
          && document_parse(&document, budget, page, used, 19)
          && stylesheet_build(&sheet, budget, &document, 480)
          && layout_build(&layout, budget, &document, &sheet, NULL, NULL, 480));
    CHECK(text_command(&layout, "0:") != NULL);
    /* Unindexed tail labels must not replay 4096 counter operations each.
       This measures structural work, independently of machine speed. */
    CHECK(layout.layout_work_units < 50000u);
    layout_destroy(&layout);
    stylesheet_destroy(&sheet);
    document_destroy(&document);
    CHECK(budget->current == 0);
    return 0;
}

static int test_font_relative_math_bases(Budget *budget)
{
    static const char page[] =
        "<!doctype html><style>html{font-size:10px}"
        "body{margin:0;padding:0;font-size:14px}"
        "#sum{font-size:calc(2rem + 10%)}"
        "#minimum{font-size:min(3em,4rem)}"
        "#maximum{font-size:max(1.5em,2rem)}"
        "#clamped{font-size:clamp(1rem,2em,3rem)}"
        "#parent{font-size:20px}#child{font-size:calc(1em + 1rem)}"
        "#shorthand{font:normal calc(1rem + 50%) sans-serif}"
        "#em-parent{font:20px/1 sans-serif}"
        "#em-inherit{height:3em;padding:1em}"
        "#em-cross{padding:1em}#em-cross{font-size:22px}"
        "#em-same{padding:1em;font-size:18px}"
        "</style><body id=font-body><span id=sum>SUM</span>"
        "<span id=minimum>MIN</span><span id=maximum>MAX</span>"
        "<span id=clamped>CLAMP</span><div id=parent>"
        "<span id=child>CHILD</span></div>"
        "<span id=shorthand>SHORT</span>"
        "<div id=em-parent><span id=em-inherit>I</span></div>"
        "<span id=em-cross>C</span><span id=em-same>S</span></body>";
    PocDocument document = {0};
    Stylesheet sheet = {0};
    CHECK(document_parse(&document, budget, page, strlen(page), 23)
          && stylesheet_build(&sheet, budget, &document, 480));
    lxb_dom_node_t *document_root = lxb_dom_interface_node(document.html);
    lxb_dom_node_t *body = find_id(document_root, "font-body");
    lxb_dom_node_t *root = body == NULL ? NULL : body->parent;
    lxb_dom_node_t *parent = find_id(document_root, "parent");
    CHECK(root != NULL && body != NULL && parent != NULL);
    ComputedStyle root_style = style_for_node(&sheet, root, NULL);
    ComputedStyle body_style = style_for_node(&sheet, body, &root_style);
    ComputedStyle sum_style = style_for_node(
        &sheet, find_id(document_root, "sum"), &body_style);
    ComputedStyle minimum_style = style_for_node(
        &sheet, find_id(document_root, "minimum"), &body_style);
    ComputedStyle maximum_style = style_for_node(
        &sheet, find_id(document_root, "maximum"), &body_style);
    ComputedStyle clamped_style = style_for_node(
        &sheet, find_id(document_root, "clamped"), &body_style);
    ComputedStyle parent_style = style_for_node(
        &sheet, parent, &body_style);
    ComputedStyle child_style = style_for_node(
        &sheet, find_id(document_root, "child"), &parent_style);
    ComputedStyle shorthand_style = style_for_node(
        &sheet, find_id(document_root, "shorthand"), &body_style);
    lxb_dom_node_t *em_parent = find_id(document_root, "em-parent");
    ComputedStyle em_parent_style = style_for_node(
        &sheet, em_parent, &body_style);
    ComputedStyle em_inherit_style = style_for_node(
        &sheet, find_id(document_root, "em-inherit"), &em_parent_style);
    ComputedStyle em_cross_style = style_for_node(
        &sheet, find_id(document_root, "em-cross"), &body_style);
    ComputedStyle em_same_style = style_for_node(
        &sheet, find_id(document_root, "em-same"), &body_style);
    CHECK(root_style.font_size == 10 && root_style.root_font_size == 10
          && body_style.font_size == 14 && body_style.root_font_size == 10
          && sum_style.font_size == 21
          && minimum_style.font_size == 40
          && maximum_style.font_size == 21
          && clamped_style.font_size == 28
          && child_style.font_size == 30
          && shorthand_style.font_size == 17
          && em_parent_style.font_size == 20
          && em_inherit_style.height == 60
          && em_inherit_style.padding.top == 20
          && em_inherit_style.padding.right == 20
          && em_cross_style.font_size == 22
          && em_cross_style.padding.top == 22
          && em_same_style.font_size == 18
          && em_same_style.padding.top == 18);
    stylesheet_destroy(&sheet);
    document_destroy(&document);
    CHECK(budget->current == 0);
    return 0;
}
static int test_width_fit_content(Budget *budget)
{
    /* A responsive article skin narrows tables with
       .article-output table{width:fit-content}. Retain the keyword so
       layout and CSSOM can distinguish it from unconstrained max-content. */
    static const char page[] =
        "<!doctype html><style>body{margin:0;padding:0}"
        "#fits{width:fit-content}</style>"
        "<body><div id=fits>short</div></body>";
    PocDocument document = {0};
    Stylesheet sheet = {0};
    CHECK(document_parse(&document, budget, page, strlen(page), 23)
          && stylesheet_build(&sheet, budget, &document, 480));
    lxb_dom_node_t *document_root = lxb_dom_interface_node(document.html);
    lxb_dom_node_t *fits = find_id(document_root, "fits");
    CHECK(fits != NULL);
    ComputedStyle fits_style = style_for_node(&sheet, fits, NULL);
    CHECK(fits_style.width_max_content && fits_style.has_width
          && computed_style_width_fit_content(&fits_style)
          && !fits_style.width_percent);
    stylesheet_destroy(&sheet);
    document_destroy(&document);
    CHECK(budget->current == 0);
    return 0;
}

static int test_intrinsic_keywords_line_clamp_and_content_visibility(
    Budget *budget)
{
    static const char page[] =
        "<!doctype html><style>body{margin:0;padding:0;width:120px}"
        ".probe{padding:0;border:0;margin:0}"
        "#min{width:min-content}#max{width:max-content}"
        "#fit{width:fit-content}#guard{width:8px;min-width:min-content}"
        "#flex{display:flex;width:120px}#basis-min{flex:0 0 min-content}"
        "#basis-max{flex-grow:0;flex-shrink:0;flex-basis:max-content}"
        "#clamp{display:-webkit-box;-webkit-box-orient:vertical;width:44px;"
        "line-height:10px;overflow:hidden;-webkit-line-clamp:2}"
        "#clamp{text-overflow:ellipsis}"
        "#legacy{display:-webkit-box;width:100px;height:40px;"
        "-webkit-box-orient:horizontal;-webkit-box-direction:reverse;"
        "-webkit-box-pack:justify;-webkit-box-align:center;"
        "-webkit-box-lines:multiple}"
        "#legacy-a{-webkit-box-flex:1;-webkit-box-ordinal-group:2}"
        "#prefixed{display:-ms-flexbox;-webkit-flex-direction:column;"
        "-webkit-justify-content:center;-webkit-align-items:flex-end;"
        "-webkit-box-sizing:border-box;-webkit-transform:translateX(2px)}"
        "#text-fill{color:#112233;-webkit-text-fill-color:#abcdef}"
        "#hidden{content-visibility:hidden;width:40px;height:20px;"
        "background:#123456}#intrinsic{content-visibility:hidden;"
        "contain-intrinsic-size:auto 33px;width:30px}"
        "#auto{content-visibility:auto}"
        "</style><body>"
        "<div id=min class=probe>alpha beta</div>"
        "<div id=max class=probe>alpha beta gamma delta</div>"
        "<div id=fit class=probe>alpha beta gamma delta</div>"
        "<div id=guard class=probe>unbreakable</div>"
        "<div id=flex><span id=basis-min>alpha beta</span>"
        "<span id=basis-max>alpha beta</span></div>"
        "<div id=clamp class=probe>one two three four five six seven</div>"
        "<div id=legacy><span id=legacy-a>A</span><span>B</span></div>"
        "<div id=prefixed><span>PREFIXED</span></div>"
        "<span id=text-fill>FILL</span>"
        "<div id=hidden><span>HIDDEN-CONTENT</span></div>"
        "<div id=intrinsic><span>INTRINSIC-CONTENT</span></div>"
        "<div id=auto><span>AUTO-CONTENT</span></div>"
        "</body>";
    PocDocument document = {0};
    Stylesheet sheet = {0};
    LayoutDocument layout = {0};
    CHECK(document_parse(&document, budget, page, strlen(page), 31)
          && stylesheet_build(&sheet, budget, &document, 120)
          && layout_build(&layout, budget, &document, &sheet,
                          NULL, NULL, 120));
    lxb_dom_node_t *root = lxb_dom_interface_node(document.html);
    lxb_dom_node_t *min = find_id(root, "min");
    lxb_dom_node_t *max = find_id(root, "max");
    lxb_dom_node_t *fit = find_id(root, "fit");
    lxb_dom_node_t *guard = find_id(root, "guard");
    lxb_dom_node_t *basis_min = find_id(root, "basis-min");
    lxb_dom_node_t *basis_max = find_id(root, "basis-max");
    lxb_dom_node_t *clamp = find_id(root, "clamp");
    lxb_dom_node_t *hidden = find_id(root, "hidden");
    lxb_dom_node_t *intrinsic = find_id(root, "intrinsic");
    lxb_dom_node_t *legacy = find_id(root, "legacy");
    lxb_dom_node_t *legacy_a = find_id(root, "legacy-a");
    lxb_dom_node_t *prefixed = find_id(root, "prefixed");
    lxb_dom_node_t *text_fill = find_id(root, "text-fill");
    lxb_dom_node_t *auto_node = find_id(root, "auto");
    const LayoutNodeBox *min_box = layout_box_for_node(&layout, min);
    const LayoutNodeBox *max_box = layout_box_for_node(&layout, max);
    const LayoutNodeBox *fit_box = layout_box_for_node(&layout, fit);
    const LayoutNodeBox *guard_box = layout_box_for_node(&layout, guard);
    const LayoutNodeBox *basis_min_box = layout_box_for_node(
        &layout, basis_min);
    const LayoutNodeBox *basis_max_box = layout_box_for_node(
        &layout, basis_max);
    const LayoutNodeBox *clamp_box = layout_box_for_node(&layout, clamp);
    const LayoutNodeBox *hidden_box = layout_box_for_node(&layout, hidden);
    const LayoutNodeBox *intrinsic_box = layout_box_for_node(
        &layout, intrinsic);
    ComputedStyle min_style = style_for_node(&sheet, min, NULL);
    ComputedStyle max_style = style_for_node(&sheet, max, NULL);
    ComputedStyle fit_style = style_for_node(&sheet, fit, NULL);
    ComputedStyle clamp_style = style_for_node(&sheet, clamp, NULL);
    ComputedStyle basis_min_style = style_for_node(&sheet, basis_min, NULL);
    ComputedStyle basis_max_style = style_for_node(&sheet, basis_max, NULL);
    ComputedStyle hidden_style = style_for_node(&sheet, hidden, NULL);
    ComputedStyle legacy_style = style_for_node(&sheet, legacy, NULL);
    ComputedStyle legacy_a_style = style_for_node(&sheet, legacy_a, NULL);
    ComputedStyle prefixed_style = style_for_node(&sheet, prefixed, NULL);
    ComputedStyle text_fill_style = style_for_node(&sheet, text_fill, NULL);
    ComputedStyle auto_style = style_for_node(&sheet, auto_node, NULL);
    bool hidden_text = false, auto_text = false, ellipsis = false;
    int clamp_first_y = INT_MIN, clamp_second_y = INT_MIN;
    for (size_t i = 0; i < layout.count; i++) {
        const DrawCommand *command = &layout.commands[i];
        if (command->type != DRAW_TEXT || command->text == NULL) continue;
        if (command->text_length == 14
            && memcmp(command->text, "HIDDEN-CONTENT", 14) == 0) {
            hidden_text = true;
        }
        if (command->text_length == 12
            && memcmp(command->text, "AUTO-CONTENT", 12) == 0) {
            auto_text = true;
        }
        if (command->text_length == 3
            && memcmp(command->text, "\xe2\x80\xa6", 3) == 0) {
            ellipsis = true;
        }
        bool clamp_text = (command->text_length == 3
                           && (memcmp(command->text, "one", 3) == 0
                               || memcmp(command->text, "two", 3) == 0
                               || memcmp(command->text, "six", 3) == 0))
            || (command->text_length == 4
                && (memcmp(command->text, "four", 4) == 0
                    || memcmp(command->text, "five", 4) == 0))
            || (command->text_length == 5
                && memcmp(command->text, "three", 5) == 0)
            || (command->text_length == 3
                && memcmp(command->text, "\xe2\x80\xa6", 3) == 0);
        if (clamp_text) {
            if (clamp_first_y == INT_MIN) clamp_first_y = command->y;
            else if (command->y != clamp_first_y
                     && clamp_second_y == INT_MIN) {
                clamp_second_y = command->y;
            } else if (clamp_second_y != INT_MIN) {
                CHECK(command->y == clamp_first_y
                      || command->y == clamp_second_y);
            }
        }
    }
    CHECK(computed_style_width_min_content(&min_style)
          && computed_style_width_max_content(&max_style)
          && computed_style_width_fit_content(&fit_style)
          && basis_min_style.flex_basis == STYLE_LENGTH_MIN_CONTENT
          && basis_max_style.flex_basis == STYLE_LENGTH_MAX_CONTENT
          && clamp_style.line_clamp == 2
          && computed_style_text_overflow_ellipsis(&clamp_style)
          && clamp_style.display == DISPLAY_FLEX
          && clamp_style.flex_direction == FLEX_COLUMN
          && legacy_style.display == DISPLAY_FLEX
          && legacy_style.flex_direction == FLEX_ROW_REVERSE
          && legacy_style.justify_content == JUSTIFY_SPACE_BETWEEN
          && legacy_style.align_items == ALIGN_CENTER
          && legacy_style.flex_wrap
          && legacy_a_style.flex_grow == 1000
          && legacy_a_style.order == 1
          && prefixed_style.display == DISPLAY_FLEX
          && prefixed_style.flex_direction == FLEX_COLUMN
          && prefixed_style.justify_content == JUSTIFY_CENTER
          && prefixed_style.align_items == ALIGN_END
          && prefixed_style.box_sizing_border_box
          && prefixed_style.transform_x == 2
          && text_fill_style.color == 0xabcdef
          && hidden_style.content_visibility
                 == STYLE_CONTENT_VISIBILITY_HIDDEN
          && auto_style.content_visibility == STYLE_CONTENT_VISIBILITY_AUTO
          && min_box != NULL && max_box != NULL && fit_box != NULL
          && guard_box != NULL && clamp_box != NULL && hidden_box != NULL
          && basis_min_box != NULL && basis_max_box != NULL
          && min_box->width < fit_box->width
          && fit_box->width <= 120
          && max_box->width > fit_box->width
          && guard_box->width > 8
          && basis_min_box->width < basis_max_box->width
          && clamp_first_y != INT_MIN && clamp_second_y != INT_MIN
          && ellipsis && !hidden_text && auto_text
          && hidden_box->width == 40 && hidden_box->height == 20
          && intrinsic_box != NULL && intrinsic_box->width == 30
          && intrinsic_box->height == 33);
    layout_destroy(&layout);
    stylesheet_destroy(&sheet);
    document_destroy(&document);
    CHECK(budget->current == 0);
    return 0;
}

static int test_flex_flow_shorthand(Budget *budget)
{
    /* flex-flow can be the only flex declaration used by linearized-infobox
       rules (tbody{flex-flow:column nowrap}); before the shorthand
       parsed, every infobox row stacked at y=0 in a phantom row layout. */
    static const char page[] =
        "<!doctype html><style>body{margin:0;padding:0}"
        "#col{display:flex;flex-flow:column nowrap;width:200px}"
        "#col div{height:20px}"
        "#wrapper{display:flex;flex-flow:row wrap-reverse;width:200px}"
        "</style><body><div id=col><div>A</div><div>B</div><div>C</div>"
        "</div><div id=wrapper></div></body>";
    PocDocument document = {0};
    Stylesheet sheet = {0};
    CHECK(document_parse(&document, budget, page, strlen(page), 23)
          && stylesheet_build(&sheet, budget, &document, 480));
    lxb_dom_node_t *document_root = lxb_dom_interface_node(document.html);
    lxb_dom_node_t *column = find_id(document_root, "col");
    lxb_dom_node_t *wrapper = find_id(document_root, "wrapper");
    CHECK(column != NULL && wrapper != NULL);
    ComputedStyle column_style = style_for_node(&sheet, column, NULL);
    ComputedStyle wrapper_style = style_for_node(&sheet, wrapper, NULL);
    CHECK(column_style.display == DISPLAY_FLEX
          && column_style.flex_direction == FLEX_COLUMN
          && !column_style.flex_wrap
          && wrapper_style.flex_direction == FLEX_ROW
          && wrapper_style.flex_wrap
          && wrapper_style.flex_wrap_reverse);
    stylesheet_destroy(&sheet);
    document_destroy(&document);
    CHECK(budget->current == 0);
    return 0;
}

static int test_root_font_size_cascade_keywords(Budget *budget)
{
    /* Mirrors a production root cascade: a reset hits html with
       font-size:100% and font:inherit, then the theme rule sets the
       root size through a var()-carrying (deferred-reparse) declaration
       block.  rem sizes must resolve against the themed root, and every
       CSS-wide keyword on font-size must land on its defined behavior. */
    static const char page[] =
        "<!doctype html><style>"
        "html,body,div,span,p{font-size:100%;font:inherit;margin:0;padding:0}"
        "html{color:var(--color-text-primary);font-size:15px;font-weight:500}"
        "#remsized{font-size:2rem}"
        "#parent{font-size:20px}"
        "#kw-inherit{font-size:inherit}"
        "#kw-unset{font-size:unset}"
        "#kw-initial{font-size:initial}"
        "#kw-revert{font-size:revert}"
        "</style><body id=cascade-body><span id=remsized>REM</span>"
        "<div id=parent><span id=kw-inherit>A</span>"
        "<span id=kw-unset>B</span><span id=kw-initial>C</span>"
        "<span id=kw-revert>D</span></div></body>";
    PocDocument document = {0};
    Stylesheet sheet = {0};
    CHECK(document_parse(&document, budget, page, strlen(page), 23)
          && stylesheet_build(&sheet, budget, &document, 480));
    lxb_dom_node_t *document_root = lxb_dom_interface_node(document.html);
    lxb_dom_node_t *body = find_id(document_root, "cascade-body");
    lxb_dom_node_t *root = body == NULL ? NULL : body->parent;
    lxb_dom_node_t *parent = find_id(document_root, "parent");
    CHECK(root != NULL && body != NULL && parent != NULL);
    ComputedStyle root_style = style_for_node(&sheet, root, NULL);
    ComputedStyle body_style = style_for_node(&sheet, body, &root_style);
    ComputedStyle rem_style = style_for_node(
        &sheet, find_id(document_root, "remsized"), &body_style);
    ComputedStyle parent_style = style_for_node(&sheet, parent, &body_style);
    ComputedStyle inherit_style = style_for_node(
        &sheet, find_id(document_root, "kw-inherit"), &parent_style);
    ComputedStyle unset_style = style_for_node(
        &sheet, find_id(document_root, "kw-unset"), &parent_style);
    ComputedStyle initial_style = style_for_node(
        &sheet, find_id(document_root, "kw-initial"), &parent_style);
    ComputedStyle revert_style = style_for_node(
        &sheet, find_id(document_root, "kw-revert"), &parent_style);
    CHECK(root_style.font_size == 15 && root_style.root_font_size == 15
          && root_style.font_weight == 500
          && body_style.font_size == 15
          && rem_style.font_size == 30
          && parent_style.font_size == 20
          && inherit_style.font_size == 20
          && unset_style.font_size == 20
          && initial_style.font_size == 16
          && revert_style.font_size == 16);
    stylesheet_destroy(&sheet);
    document_destroy(&document);
    CHECK(budget->current == 0);
    return 0;
}

static int test_underlined_internal_whitespace(Budget *budget)
{
    static const char page[] =
        "<!doctype html><style>body{margin:0;padding:0}"
        "#copyright{text-decoration:underline;font-size:16px}"
        "</style><body><a id=copyright href=/copyright>"
        "CROWN COPYRIGHT</a></body>";
    PocDocument document = {0};
    Stylesheet sheet = {0};
    LayoutDocument layout = {0};
    CHECK(document_parse(&document, budget, page, strlen(page), 19)
          && stylesheet_build(&sheet, budget, &document, 480)
          && layout_build(&layout, budget, &document, &sheet,
                          NULL, NULL, 480));
    const DrawCommand *crown = text_command(&layout, "CROWN");
    const DrawCommand *copyright = text_command(&layout, "COPYRIGHT");
    lxb_dom_node_t *link_node = find_id(
        lxb_dom_interface_node(document.html), "copyright");
    const LinkRegion *crown_link = NULL;
    for (size_t i = 0; i < layout.link_count; i++) {
        const LinkRegion *link = &layout.links[i];
        if (link->node == link_node && crown != NULL
            && link->x == crown->x && link->y == crown->y) {
            crown_link = link;
            break;
        }
    }
    CHECK(crown != NULL && copyright != NULL && crown_link != NULL
          && (crown->image_fit & LAYOUT_TEXT_DECORATION_UNDERLINE) != 0
          && crown->x + crown->width == copyright->x
          && crown_link->x + crown_link->width == copyright->x);
    layout_destroy(&layout);
    stylesheet_destroy(&sheet);
    document_destroy(&document);
    CHECK(budget->current == 0);
    return 0;
}
static int test_text_underline_offset_raster(Budget *budget)
{
    static const char page[] =
        "<!doctype html><style>html,body{margin:0;padding:0;"
        "background:#fff}#spacer{height:160px}#auto,#offset{"
        "position:absolute;left:10px;font-size:16px;line-height:16px;"
        "color:#000;text-decoration:underline}#auto{top:20px}"
        "#offset{top:111px;text-underline-offset:4px}</style><body>"
        "<div id=spacer></div><span id=auto>AUTOLN</span>"
        "<span id=offset>OFFSTX</span></body>";
    PocDocument document = {0};
    Stylesheet sheet = {0};
    LayoutDocument layout = {0};
    TileCache cache = {0};
    uint16_t *frame = NULL;
    CHECK(document_parse(&document, budget, page, strlen(page), 13)
          && stylesheet_build(&sheet, budget, &document, 120)
          && layout_build(&layout, budget, &document, &sheet,
                          NULL, NULL, 120));
    const DrawCommand *automatic = text_command(&layout, "AUTOLN");
    const DrawCommand *offset = text_command(&layout, "OFFSTX");
    int pixels = 0;
    CHECK(automatic != NULL && offset != NULL
          && automatic->x == offset->x
          && automatic->width == offset->width
          && automatic->height == offset->height
          && offset->y - automatic->y == 91
          && automatic->image_fit == LAYOUT_TEXT_DECORATION_UNDERLINE
          && !draw_command_text_underline_offset(automatic, NULL)
          && draw_command_text_underline_offset(offset, &pixels)
          && pixels == 4);
    enum { FRAME_WIDTH = 120, FRAME_HEIGHT = 150 };
    frame = budget_malloc(
        budget, FRAME_WIDTH * FRAME_HEIGHT * sizeof(*frame));
    CHECK(frame != NULL && tile_cache_init(&cache, budget, &layout, 4)
          && tile_cache_set_frame(
                 &cache, frame, FRAME_WIDTH * FRAME_HEIGHT)
          && tile_cache_render_frame(
                 &cache, 0, FRAME_WIDTH, FRAME_HEIGHT, NULL));
    int automatic_row = automatic->y + automatic->height - 1;
    int offset_zero_row = offset->y + offset->height - 1;
    int offset_row = offset_zero_row + pixels;
    int sample_x = offset->x + offset->width / 2;
    if (!(automatic_row >= 0 && automatic_row < FRAME_HEIGHT
          && offset_zero_row >= 0 && offset_zero_row < FRAME_HEIGHT
          && offset_row >= LAYOUT_SPATIAL_BAND_HEIGHT
          && offset_row < FRAME_HEIGHT
          && sample_x >= 0 && sample_x < FRAME_WIDTH
          && frame[(size_t) automatic_row * FRAME_WIDTH + sample_x] == 0
          && frame[(size_t) offset_zero_row * FRAME_WIDTH + sample_x] != 0
          && frame[(size_t) offset_row * FRAME_WIDTH + sample_x] == 0)) {
        fprintf(stderr,
                "underline raster auto=%d zero=%d offset=%d x=%d "
                "colors=%04x/%04x/%04x command=%d,%d %dx%d\n",
                automatic_row, offset_zero_row, offset_row, sample_x,
                automatic_row >= 0 && automatic_row < FRAME_HEIGHT
                    ? frame[(size_t) automatic_row * FRAME_WIDTH + sample_x]
                    : 0xffff,
                offset_zero_row >= 0 && offset_zero_row < FRAME_HEIGHT
                    ? frame[(size_t) offset_zero_row * FRAME_WIDTH + sample_x]
                    : 0xffff,
                offset_row >= 0 && offset_row < FRAME_HEIGHT
                    ? frame[(size_t) offset_row * FRAME_WIDTH + sample_x]
                    : 0xffff,
                offset->x, offset->y, offset->width, offset->height);
    }
    CHECK(automatic_row >= 0 && automatic_row < FRAME_HEIGHT
          && offset_zero_row >= 0 && offset_zero_row < FRAME_HEIGHT
          && offset_row >= LAYOUT_SPATIAL_BAND_HEIGHT
          && offset_row < FRAME_HEIGHT
          && sample_x >= 0 && sample_x < FRAME_WIDTH
          && frame[(size_t) automatic_row * FRAME_WIDTH + sample_x] == 0
          && frame[(size_t) offset_zero_row * FRAME_WIDTH + sample_x] != 0
          && frame[(size_t) offset_row * FRAME_WIDTH + sample_x] == 0);
    tile_cache_destroy(&cache);
    budget_free(budget, frame);
    layout_destroy(&layout);
    stylesheet_destroy(&sheet);
    document_destroy(&document);
    CHECK(budget->current == 0);
    return 0;
}

static int test_flex_overlay_used_size_and_paint_order(Budget *budget)
{
    static const char page[] =
        "<!doctype html><style>html,body{margin:0;padding:0}"
        "#hero{display:flex;position:relative;width:160px;padding-top:20px}"
        "#asset{position:absolute;width:100%;height:100%;background:#0000ff}"
        "#content{flex:1;min-height:80px;background:#ff0000;z-index:3}"
        "#percent{font-size:32px;line-height:125%}"
        "#em{font-size:32px;line-height:1.25em}</style><body>"
        "<div id=hero><div id=asset></div><div id=content></div></div>"
        "<p id=percent>PERCENT</p><p id=em>EM</p></body>";
    PocDocument document = {0};
    Stylesheet sheet = {0};
    LayoutDocument layout = {0};
    CHECK(document_parse(&document, budget, page, strlen(page), 31)
          && stylesheet_build(&sheet, budget, &document, 160)
          && layout_build(&layout, budget, &document, &sheet,
                          NULL, NULL, 160));
    lxb_dom_node_t *root = lxb_dom_interface_node(document.html);
    lxb_dom_node_t *hero = find_id(root, "hero");
    lxb_dom_node_t *asset = find_id(root, "asset");
    lxb_dom_node_t *content = find_id(root, "content");
    const LayoutNodeBox *hero_box = layout_box_for_node(&layout, hero);
    const LayoutNodeBox *asset_box = layout_box_for_node(&layout, asset);
    const LayoutNodeBox *content_box = layout_box_for_node(&layout, content);
    ComputedStyle percent = style_for_node(
        &sheet, find_id(root, "percent"), NULL);
    ComputedStyle em = style_for_node(&sheet, find_id(root, "em"), NULL);
    CHECK(hero_box != NULL && asset_box != NULL && content_box != NULL
          && hero_box->height == 100
          && asset_box->x == hero_box->x && asset_box->y == hero_box->y
          && asset_box->width == 160 && asset_box->height == 100
          && content_box->height == 80
          && percent.line_height == 40 && em.line_height == 40);
    size_t blue_order = SIZE_MAX, red_order = SIZE_MAX;
    for (size_t order = 0; order < layout.paint_order_count; order++) {
        const DrawCommand *command =
            &layout.commands[layout.paint_order[order]];
        if (command->type != DRAW_FILL_RECT) continue;
        if (command->color == 0x0000ff) blue_order = order;
        if (command->color == 0xff0000) red_order = order;
    }
    CHECK(blue_order != SIZE_MAX && red_order != SIZE_MAX
          && blue_order < red_order);
    layout_destroy(&layout);
    stylesheet_destroy(&sheet);
    document_destroy(&document);
    CHECK(budget->current == 0);
    return 0;
}

static int test_mobile_overflow_intrinsic_minimums(Budget *budget)
{
    static const char page[] =
        "<!doctype html><style>html,body{margin:0;padding:0}"
        ".flex{display:flex;width:100px}.item{white-space:nowrap}"
        ".fixed{flex:0 0 40px}"
        "#flex-clip{overflow:clip}#flex-hidden{overflow:hidden}"
        ".grid{display:grid;width:100px;grid-template-columns:auto 40px}"
        "#grid-zero{min-width:0}#grid-hidden{overflow:hidden}"
        "#grid-clip{overflow:clip}"
        "#outer{display:block;width:100px;overflow:auto}"
        "#inner-clip{display:block;width:100px;overflow:hidden}"
        "#inner-wide{display:block;width:300px;height:8px}"
        ".compact-row{display:flex;width:100px;justify-content:flex-end}"
        "#compact-wrapper{display:inline-flex;min-width:0}"
        "#compact-select{box-sizing:border-box;width:1px;"
        "padding-left:34px;padding-right:28px;border:1px solid}"
        ".wrapped-wide{display:flex;flex-wrap:wrap;width:100px}"
        ".wrapped-wide>span{display:block;box-sizing:content-box;"
        "width:100%;padding:0 12px;height:8px}"
        ".audio-row{display:flex;width:100px}.audio-row audio{display:block}"
        "</style><body>"
        "<div class=flex><span id=flex-clip class=item>UNBREAKABLETOKEN</span><i class=fixed></i></div>"
        "<div class=flex><span id=flex-hidden class=item>UNBREAKABLETOKEN</span><i class=fixed></i></div>"
        "<div class=grid><span id=grid-zero class=item>UNBREAKABLETOKEN</span><i></i></div>"
        "<div class=grid><span id=grid-hidden class=item>UNBREAKABLETOKEN</span><i></i></div>"
        "<div class=grid><span id=grid-clip class=item>UNBREAKABLETOKEN</span><i></i></div>"
        "<div id=outer><div id=inner-clip><div id=inner-wide></div></div></div>"
        "<div class=compact-row><div id=compact-wrapper>"
        "<select id=compact-select><option>English</option></select>"
        "</div></div>"
        "<div class=wrapped-wide><span id=wrapped-wide></span></div>"
        "<div class=audio-row><audio id=audio-without-controls></audio>"
        "<i style='display:block;width:20px'></i></div>"
        "</body>";
    PocDocument document = {0};
    Stylesheet sheet = {0};
    LayoutDocument layout = {0};
    CHECK(document_parse(&document, budget, page, strlen(page), 31)
          && stylesheet_build(&sheet, budget, &document, 100)
          && layout_build(&layout, budget, &document, &sheet,
                          NULL, NULL, 100));
    lxb_dom_node_t *root = lxb_dom_interface_node(document.html);
    const LayoutNodeBox *flex_clip = layout_box_for_node(
        &layout, find_id(root, "flex-clip"));
    const LayoutNodeBox *flex_hidden = layout_box_for_node(
        &layout, find_id(root, "flex-hidden"));
    const LayoutNodeBox *grid_zero = layout_box_for_node(
        &layout, find_id(root, "grid-zero"));
    const LayoutNodeBox *grid_hidden = layout_box_for_node(
        &layout, find_id(root, "grid-hidden"));
    const LayoutNodeBox *grid_clip = layout_box_for_node(
        &layout, find_id(root, "grid-clip"));
    const LayoutNodeBox *outer = layout_box_for_node(
        &layout, find_id(root, "outer"));
    const LayoutNodeBox *inner_clip = layout_box_for_node(
        &layout, find_id(root, "inner-clip"));
    const LayoutNodeBox *compact_wrapper = layout_box_for_node(
        &layout, find_id(root, "compact-wrapper"));
    const LayoutNodeBox *compact_select = layout_box_for_node(
        &layout, find_id(root, "compact-select"));
    const LayoutNodeBox *wrapped_wide = layout_box_for_node(
        &layout, find_id(root, "wrapped-wide"));
    const LayoutNodeBox *audio_without_controls = layout_box_for_node(
        &layout, find_id(root, "audio-without-controls"));
    CHECK(flex_clip != NULL && flex_hidden != NULL
          && grid_zero != NULL && grid_hidden != NULL && grid_clip != NULL
          && outer != NULL && inner_clip != NULL
          && compact_wrapper != NULL && compact_select != NULL
          && wrapped_wide != NULL && audio_without_controls != NULL);
    if (!(flex_clip->width > 60 && flex_hidden->width <= 60
          && grid_zero->width <= 60 && grid_hidden->width <= 60
          && grid_clip->width > 60
          && outer->content_width == outer->client_width
          && inner_clip->content_width > inner_clip->client_width
          && compact_select->width >= 64
          && compact_wrapper->width >= compact_select->width
          && wrapped_wide->width <= 100
          && audio_without_controls->width <= 8)) {
        fprintf(stderr,
                "mobile minima flex=%d/%d grid=%d/%d/%d "
                "overflow=%d/%d inner=%d/%d compact=%d/%d wrap=%d audio=%d\n",
                flex_clip->width, flex_hidden->width,
                grid_zero->width, grid_hidden->width, grid_clip->width,
                outer->content_width, outer->client_width,
                inner_clip->content_width, inner_clip->client_width,
                compact_wrapper->width, compact_select->width,
                wrapped_wide->width, audio_without_controls->width);
    }
    CHECK(flex_clip->width > 60 && flex_hidden->width <= 60
          && grid_zero->width <= 60 && grid_hidden->width <= 60
          && grid_clip->width > 60
          && outer->content_width == outer->client_width
          && inner_clip->content_width > inner_clip->client_width
          && compact_select->width >= 64
          && compact_wrapper->width >= compact_select->width
          && wrapped_wide->width <= 100
          && audio_without_controls->width <= 8);
    layout_destroy(&layout);
    stylesheet_destroy(&sheet);
    document_destroy(&document);
    CHECK(budget->current == 0);
    return 0;
}

static int test_retained_matches_reuse(Budget *budget)
{
    /* More elements than the computed-style reuse cache holds, so a rebuild
       reaches the retained matched-rule table for most of them. */
    static char html[96 * 1024];
    static const char prefix[] = "<!doctype html><style>"
        ".a{color:#112233}.b{color:#445566}div>span{font-weight:bold}"
        "span:last-child{color:#778899}[data-k=v]{color:#aabbcc}"
        "span.b::before{content:'*'}.q span{color:#00ff00}";
    size_t length = sizeof(prefix) - 1u;
    memcpy(html, prefix, length);
    /* The table is only allocated for stylesheets large enough to make
       candidate matching the dominant cost; pad past that threshold. */
    for (unsigned i = 0; i < 64; i++) {
        char rule[32];
        int written = snprintf(rule, sizeof(rule), ".pad%u{margin:0}", i);
        CHECK(written > 0 && (size_t) written < sizeof(html) - length);
        memcpy(html + length, rule, (size_t) written);
        length += (size_t) written;
    }
    static const char open[] = "</style><body><div>";
    CHECK(sizeof(open) - 1u < sizeof(html) - length);
    memcpy(html + length, open, sizeof(open) - 1u);
    length += sizeof(open) - 1u;
    for (unsigned i = 0; i < 1400; i++) {
        char item[64];
        int written = snprintf(item, sizeof(item),
            "<span class=%s%s>t%04u</span>", i % 7 == 0 ? "b" : "a",
            i % 11 == 0 ? " data-k=v" : "", i);
        CHECK(written > 0 && (size_t) written < sizeof(html) - length);
        memcpy(html + length, item, (size_t) written);
        length += (size_t) written;
    }
    static const char suffix[] = "</div>";
    CHECK(sizeof(suffix) - 1u < sizeof(html) - length);
    memcpy(html + length, suffix, sizeof(suffix) - 1u);
    length += sizeof(suffix) - 1u;
    PocDocument document = {0};
    Stylesheet sheet = {0};
    LayoutDocument layout = {0};
    ViewportContext viewport;
    CHECK(document_parse(&document, budget, html, length, 19)
        && viewport_context_init(&viewport, 480, 272, 480, 272)
        && stylesheet_build_context(&sheet, budget, &document, &viewport));
    LayoutReuseCache *reuse = layout_reuse_cache_create(budget);
    CHECK(reuse != NULL);
    layout_reuse_cache_enable_retained_matches(reuse);
    CHECK(layout_build_context_reuse(&layout, budget, &document, &sheet,
        NULL, NULL, &viewport, reuse));
    LayoutReuseStats stats, again;
    layout_reuse_cache_stats(reuse, &stats);
    printf("retained matches: stores=%zu hits=%zu misses=%zu\n",
           stats.matched_stores, stats.matched_hits, stats.matched_misses);
    /* Within one build the bidi pre-scan and the flow both resolve these
       spans; only the stores are meaningful here. */
    CHECK(stats.matched_stores >= 1400);
    uint64_t fingerprint = layout.text_fingerprint;
    const DrawCommand *first = text_command(&layout, "t0001");
    const DrawCommand *last = text_command(&layout, "t1399");
    const DrawCommand *keyed = text_command(&layout, "t0011");
    CHECK(first != NULL && first->color == 0x112233
        && last != NULL && last->color == 0x778899
        && keyed != NULL && keyed->color == 0xaabbcc);
    layout_destroy(&layout);

    /* Replayed lists reproduce the identical layout. */
    CHECK(layout_build_context_reuse(&layout, budget, &document, &sheet,
        NULL, NULL, &viewport, reuse));
    layout_reuse_cache_stats(reuse, &again);
    CHECK(again.matched_hits > stats.matched_hits
        && layout.text_fingerprint == fingerprint);
    first = text_command(&layout, "t0001");
    last = text_command(&layout, "t1399");
    keyed = text_command(&layout, "t0011");
    CHECK(first != NULL && first->color == 0x112233
        && last != NULL && last->color == 0x778899
        && keyed != NULL && keyed->color == 0xaabbcc);
    layout_destroy(&layout);

    /* A journaled class change drops the changed element's list only. */
    lxb_dom_node_t *group = document_body_node(&document)->first_child;
    lxb_dom_node_t *span = group == NULL || group->first_child == NULL
        ? NULL : group->first_child->next;
    CHECK(span != NULL);
    BudgetAllocationOwner owner = document_allocation_owner_enter(&document);
    CHECK(lxb_dom_element_set_attribute(lxb_dom_interface_element(span),
        (const lxb_char_t *) "class", 5, (const lxb_char_t *) "b", 1) != NULL);
    document_allocation_owner_leave(&document, owner);
    layout_reuse_cache_invalidate_node_scoped(reuse, span, false);
    CHECK(layout_build_context_reuse(&layout, budget, &document, &sheet,
        NULL, NULL, &viewport, reuse));
    first = text_command(&layout, "t0001");
    const DrawCommand *third = text_command(&layout, "t0002");
    last = text_command(&layout, "t1399");
    CHECK(first != NULL && first->color == 0x445566
        && third != NULL && third->color == 0x112233
        && last != NULL && last->color == 0x778899);
    layout_destroy(&layout);

    /* A class change whose tokens no rule depends on drops only the changed
       element's own lists (element plus its two pseudo slots). */
    lxb_dom_node_t *third_span = span->next;
    CHECK(third_span != NULL);
    uint32_t tokens[8];
    size_t token_count = 0;
    CHECK(stylesheet_attribute_change_tokens("class", 5, "a", 1, "zz", 2,
                                             tokens, 8, &token_count)
        && token_count == 2);
    owner = document_allocation_owner_enter(&document);
    CHECK(lxb_dom_element_set_attribute(lxb_dom_interface_element(third_span),
        (const lxb_char_t *) "class", 5, (const lxb_char_t *) "zz", 2) != NULL);
    document_allocation_owner_leave(&document, owner);
    layout_reuse_cache_stats(reuse, &stats);
    layout_reuse_cache_invalidate_attribute(
        reuse, third_span, tokens, token_count, false, false);
    layout_reuse_cache_flush_invalidations(reuse);
    layout_reuse_cache_stats(reuse, &again);
    CHECK(again.matched_token_invalidations == stats.matched_token_invalidations + 1
        && again.matched_token_fallbacks == stats.matched_token_fallbacks
        && again.matched_token_dropped >= stats.matched_token_dropped + 1
        && again.matched_token_dropped <= stats.matched_token_dropped + 3);
    CHECK(layout_build_context_reuse(&layout, budget, &document, &sheet,
        NULL, NULL, &viewport, reuse));
    third = text_command(&layout, "t0002");
    const DrawCommand *fourth = text_command(&layout, "t0003");
    CHECK(third != NULL && third->color == 0x000000
        && fourth != NULL && fourth->color == 0x112233);
    layout_destroy(&layout);

    /* A class change that a relational rule depends on drops every list
       that rule's rightmost compound could select. */
    CHECK(stylesheet_attribute_change_tokens("class", 5, NULL, 0, "q", 1,
                                             tokens, 8, &token_count)
        && token_count == 1);
    owner = document_allocation_owner_enter(&document);
    CHECK(lxb_dom_element_set_attribute(lxb_dom_interface_element(group),
        (const lxb_char_t *) "class", 5, (const lxb_char_t *) "q", 1) != NULL);
    document_allocation_owner_leave(&document, owner);
    layout_reuse_cache_stats(reuse, &stats);
    layout_reuse_cache_invalidate_attribute(
        reuse, group, tokens, token_count, false, false);
    layout_reuse_cache_flush_invalidations(reuse);
    layout_reuse_cache_stats(reuse, &again);
    CHECK(again.matched_token_invalidations == stats.matched_token_invalidations + 1
        && again.matched_token_fallbacks == stats.matched_token_fallbacks
        && again.matched_token_dropped >= stats.matched_token_dropped + 1400);
    CHECK(layout_build_context_reuse(&layout, budget, &document, &sheet,
        NULL, NULL, &viewport, reuse));
    fourth = text_command(&layout, "t0003");
    last = text_command(&layout, "t1399");
    CHECK(fourth != NULL && fourth->color == 0x00ff00
        && last != NULL && last->color == 0x00ff00);
    layout_destroy(&layout);

    /* A full reset discards every list. */
    layout_reuse_cache_stats(reuse, &stats);
    layout_reuse_cache_reset(reuse);
    CHECK(layout_build_context_reuse(&layout, budget, &document, &sheet,
        NULL, NULL, &viewport, reuse));
    layout_reuse_cache_stats(reuse, &again);
    /* Every element had to be recorded again. */
    CHECK(again.matched_stores - stats.matched_stores >= 1400);
    layout_destroy(&layout);
    layout_reuse_cache_destroy(reuse);
    stylesheet_destroy(&sheet);
    document_destroy(&document);
    CHECK(budget->current == 0);
    return 0;
}

static bool commands_equal(const LayoutDocument *a, const LayoutDocument *b)
{
    if (a->count != b->count) {
        printf("command count %zu vs %zu\n", a->count, b->count);
        return false;
    }
    for (size_t i = 0; i < a->count; i++) {
        const DrawCommand *x = &a->commands[i], *y = &b->commands[i];
        if (x->type != y->type || x->x != y->x || x->y != y->y
            || x->width != y->width || x->height != y->height
            || x->color != y->color || x->text_length != y->text_length) {
            printf("command %zu differs: type=%u/%u rect=%d,%d,%d,%d/%d,%d,%d,%d "
                   "color=%06x/%06x\n", i, x->type, y->type, x->x, x->y,
                   x->width, x->height, y->x, y->y, y->width, y->height,
                   x->color, y->color);
            return false;
        }
    }
    return true;
}

static lxb_dom_node_t *insert_style_element(PocDocument *document,
                                            lxb_dom_node_t *parent,
                                            lxb_dom_node_t *before,
                                            const char *css)
{
    static const char style_tag[] = "style";
    BudgetAllocationOwner previous = document_allocation_owner_enter(document);
    lxb_dom_element_t *element = lxb_dom_document_create_element(
        &document->html->dom_document, (const lxb_char_t *) style_tag,
        sizeof(style_tag) - 1u, NULL);
    lxb_dom_text_t *text = element == NULL ? NULL
        : lxb_dom_document_create_text_node(
              &document->html->dom_document, (const lxb_char_t *) css,
              strlen(css));
    lxb_dom_node_t *node = NULL;
    if (element != NULL && text != NULL
        && lxb_dom_node_append_child(lxb_dom_interface_node(element),
                                     lxb_dom_interface_node(text))
               == LXB_DOM_EXCEPTION_OK) {
        node = lxb_dom_interface_node(element);
        if (before != NULL) {
            lxb_dom_node_insert_before(before, node);
        } else if (lxb_dom_node_append_child(parent, node)
                   != LXB_DOM_EXCEPTION_OK) {
            node = NULL;
        }
    }
    document_allocation_owner_leave(document, previous);
    return node;
}

static int checked_toggle_layout(Budget *budget, const char *extra_css,
                                 bool expect_reset)
{
    static char html[8192];
    size_t length = 0;
    int written = snprintf(html, sizeof(html),
        "<!doctype html><style>p{color:#111111}.drawer{display:none}"
        ".toggle:checked ~ .drawer{display:block;color:#123456}"
        "input:not(:checked) + label{color:#222222}"
        ".flag:has(.inner){color:#654321}%s", extra_css);
    CHECK(written > 0 && (size_t) written < sizeof(html));
    length = (size_t) written;
    /* Enough rules for the retained matched-rule table to be allocated. */
    for (unsigned i = 0; i < 64; i++) {
        written = snprintf(html + length, sizeof(html) - length,
                           ".pad%u{margin:0}", i);
        CHECK(written > 0 && (size_t) written < sizeof(html) - length);
        length += (size_t) written;
    }
    static const char rest[] =
        "</style><body><input type=checkbox class=toggle id=t>"
        "<label for=t>label</label><p class=drawer>drawer</p>"
        "<div class=flag><span class=inner>inner</span></div>"
        "<p>other</p></body>";
    CHECK(sizeof(rest) - 1u < sizeof(html) - length);
    memcpy(html + length, rest, sizeof(rest) - 1u);
    length += sizeof(rest) - 1u;
    PocDocument document = {0};
    Stylesheet sheet = {0};
    ViewportContext viewport;
    CHECK(document_parse(&document, budget, html, length, 19)
        && viewport_context_init(&viewport, 480, 272, 480, 272)
        && stylesheet_build_context(&sheet, budget, &document, &viewport));
    LayoutReuseCache *reuse = layout_reuse_cache_create(budget);
    CHECK(reuse != NULL);
    layout_reuse_cache_enable_retained_matches(reuse);
    LayoutDocument retained = {0}, fresh = {0};
    CHECK(layout_build_context_reuse(&retained, budget, &document, &sheet,
        NULL, NULL, &viewport, reuse));
    CHECK(text_command(&retained, "drawer") == NULL);
    const DrawCommand *label = text_command(&retained, "label");
    CHECK(label != NULL && label->color == 0x222222);
    LayoutReuseStats first;
    layout_reuse_cache_stats(reuse, &first);
    CHECK(first.matched_misses > 0);
    layout_destroy(&retained);
    lxb_dom_node_t *toggle = find_id(document_body_node(&document), "t");
    CHECK(toggle != NULL);
    for (int round = 0; round < 2; round++) {
        if (round == 0) {
            CHECK(lxb_dom_element_set_attribute(
                lxb_dom_interface_element(toggle),
                (const lxb_char_t *) "checked", 7,
                (const lxb_char_t *) "", 0) != NULL);
        } else {
            CHECK(lxb_dom_element_remove_attribute(
                lxb_dom_interface_element(toggle),
                (const lxb_char_t *) "checked", 7) == LXB_STATUS_OK);
        }
        LayoutReuseStats before;
        layout_reuse_cache_stats(reuse, &before);
        layout_reuse_cache_invalidate_checked(reuse, &toggle, 1);
        CHECK(layout_build_context_reuse(&retained, budget, &document,
            &sheet, NULL, NULL, &viewport, reuse));
        CHECK(layout_build_context(&fresh, budget, &document, &sheet,
            NULL, NULL, &viewport));
        CHECK(commands_equal(&retained, &fresh));
        const DrawCommand *drawer = text_command(&retained, "drawer");
        label = text_command(&retained, "label");
        CHECK(round == 0 ? drawer != NULL && drawer->color == 0x123456
                           && label != NULL && label->color != 0x222222
                         : drawer == NULL
                           && label != NULL && label->color == 0x222222);
        LayoutReuseStats after;
        layout_reuse_cache_stats(reuse, &after);
        if (expect_reset) {
            CHECK(after.full_resets == before.full_resets + 1);
        } else {
            /* Everything but the control and the elements a checked-state
               rule can select (label, drawer) replays its matched-rule
               list; three nodes' element and pseudo lookups miss. */
            CHECK(after.full_resets == before.full_resets
                  && after.matched_hits > before.matched_hits
                  && (after.matched_misses - before.matched_misses) * 2u
                         < first.matched_misses);
        }
        layout_destroy(&retained);
        layout_destroy(&fresh);
    }
    layout_reuse_cache_destroy(reuse);
    stylesheet_destroy(&sheet);
    document_destroy(&document);
    CHECK(budget->current == 0);
    return 0;
}

/* Wikipedia's mobile menu drawer: a fixed, scrollable panel nested in a
   header item that the header's flex row centers vertically, whose rows are
   li restyled to display:block around a centering flex anchor. The fixed
   panel keeps its position when its ancestor item is centered, and so must
   the overflow clips inside it (they moved alone, clipping the top half of
   every row's label and icon); a block li has no marker line either. */
static int test_fixed_drawer_rows(Budget *budget)
{
    static const char html[] =
        "<!doctype html><style>body{margin:0}"
        ".header{display:flex;align-items:center;height:80px}"
        "#left{position:fixed;top:0;left:0;bottom:0;min-width:200px;"
        "max-width:80%;overflow-y:auto;background:#eee}"
        "ul{margin:0;padding:0}li.row{display:block;background:#fff}"
        "a{display:flex;align-items:center;height:60px;padding:0 10px}"
        ".icon{display:inline-block;width:20px;height:20px;"
        "background:#333}"
        ".label{overflow:hidden;white-space:nowrap;font-size:14px}"
        "</style><body><div class=header><div class=nav><span>menu</span>"
        "<div id=left><ul>\n"
        "  <li class=row>\n    <a href=#h><span class=icon></span>\n"
        "    <span class=label id=home>Home</span></a>\n  </li>\n"
        "</ul></div></div><span>title</span></div>"
        "<ul><li>listed</li></ul></body>";
    PocDocument document = {0};
    Stylesheet sheet = {0};
    ViewportContext viewport;
    CHECK(document_parse(&document, budget, html, sizeof(html) - 1u, 19)
        && viewport_context_init(&viewport, 480, 272, 480, 272)
        && stylesheet_build_context(&sheet, budget, &document, &viewport));
    LayoutDocument layout = {0};
    CHECK(layout_build_context(&layout, budget, &document, &sheet,
        NULL, NULL, &viewport));
    /* One marker, for the ordinary list item only. */
    size_t bullets = 0;
    for (size_t i = 0; i < layout.count; i++) {
        const DrawCommand *command = &layout.commands[i];
        if (command->type == DRAW_TEXT && command->text_length == 3
            && memcmp(command->text, "\xe2\x80\xa2", 3) == 0) bullets++;
    }
    CHECK(bullets == 1);
    const DrawCommand *home = text_command(&layout, "Home");
    lxb_dom_node_t *label = find_id(document_body_node(&document), "home");
    const LayoutNodeBox *box = label == NULL
        ? NULL : layout_box_for_node(&layout, label);
    const LayoutNodeBox *row = label == NULL || label->parent == NULL
        ? NULL : layout_box_for_node(&layout, label->parent);
    CHECK(home != NULL && box != NULL && box->clips_y && row != NULL);
    /* The label's clip still covers its text, and both sit centered in
       the row (fixed content keeps its static position in the display
       list; the fixed layer places it on screen). */
    CHECK(box->y <= home->y
          && home->y + home->height <= box->y + box->height);
    CHECK(row->height == 60 && home->y > row->y + 10
          && home->y + home->height < row->y + 50);
    layout_destroy(&layout);
    stylesheet_destroy(&sheet);
    document_destroy(&document);
    CHECK(budget->current == 0);
    return 0;
}

/* The retained-list scan behind a checkbox toggle yields to the frontend;
   a cancel there drops every list (always correct) instead of holding
   input, and the next layout still matches a fresh build. */
static bool refuse_style_invalidate(void *context, const char *phase,
                                    size_t units)
{
    (void) units;
    if (strcmp(phase, "style-invalidate") != 0) return true;
    (*(size_t *) context)++;
    return false;
}

static int test_checked_invalidation_cancel_drops_lists(Budget *budget)
{
    static char html[8192];
    int used = snprintf(html, sizeof(html),
        "<!doctype html><style>p{color:#111111}.drawer{display:none}"
        ".toggle:checked ~ .drawer{display:block;color:#123456}");
    for (unsigned i = 0; used > 0 && i < 64; i++) {
        used += snprintf(html + used, sizeof(html) - (size_t) used,
                         ".pad%u{margin:0}", i);
    }
    CHECK(used > 0 && (size_t) used < sizeof(html) - 256u);
    used += snprintf(html + used, sizeof(html) - (size_t) used,
        "</style><body><input type=checkbox class=toggle id=t>"
        "<p class=drawer>drawer</p><p>other</p></body>");
    PocDocument document = {0};
    Stylesheet sheet = {0};
    ViewportContext viewport;
    CHECK(document_parse(&document, budget, html, (size_t) used, 19)
        && viewport_context_init(&viewport, 480, 272, 480, 272)
        && stylesheet_build_context(&sheet, budget, &document, &viewport));
    LayoutReuseCache *reuse = layout_reuse_cache_create(budget);
    CHECK(reuse != NULL);
    layout_reuse_cache_enable_retained_matches(reuse);
    LayoutDocument retained = {0}, fresh = {0};
    CHECK(layout_build_context_reuse(&retained, budget, &document, &sheet,
        NULL, NULL, &viewport, reuse));
    layout_destroy(&retained);
    LayoutReuseStats before;
    layout_reuse_cache_stats(reuse, &before);
    CHECK(before.matched_stores > 0);
    lxb_dom_node_t *toggle = find_id(document_body_node(&document), "t");
    CHECK(toggle != NULL && lxb_dom_element_set_attribute(
        lxb_dom_interface_element(toggle), (const lxb_char_t *) "checked", 7,
        (const lxb_char_t *) "", 0) != NULL);
    size_t refusals = 0;
    TilefinchPlatformServices services = {
        .context = &refusals, .cooperate = refuse_style_invalidate
    };
    tilefinch_platform_set_services(&services);
    layout_reuse_cache_invalidate_checked(reuse, &toggle, 1);
    tilefinch_platform_set_services(NULL);
    LayoutReuseStats after;
    layout_reuse_cache_stats(reuse, &after);
    CHECK(refusals == 1 && after.full_resets == before.full_resets
          && reuse->matches != NULL && reuse->matches->occupied == 0);
    CHECK(layout_build_context_reuse(&retained, budget, &document, &sheet,
        NULL, NULL, &viewport, reuse));
    CHECK(layout_build_context(&fresh, budget, &document, &sheet,
        NULL, NULL, &viewport));
    CHECK(commands_equal(&retained, &fresh));
    const DrawCommand *drawer = text_command(&retained, "drawer");
    CHECK(drawer != NULL && drawer->color == 0x123456);
    layout_destroy(&retained);
    layout_destroy(&fresh);
    layout_reuse_cache_destroy(reuse);
    stylesheet_destroy(&sheet);
    document_destroy(&document);
    CHECK(budget->current == 0);
    return 0;
}

/* Toggling a checkbox re-answers only rules that can observe checkedness,
   however far a sibling combinator carries it; an unrelated :has() rule
   does not force a reset, and one that can see the state does. */
static int test_checked_toggle_retains_matches(Budget *budget)
{
    CHECK(checked_toggle_layout(budget, "", false) == 0);
    CHECK(checked_toggle_layout(
        budget, "div:has(input:checked) p{color:#010101}", true) == 0);
    return 0;
}

/* The :has() classification behind scoped invalidation: where each anchor
   lies relative to the subject, which region its argument reads, and when
   the argument can read outside that region. */
static int test_has_plan_classification(Budget *budget)
{
    static const struct {
        const char *selector;
        StyleHasRelation relation;
        uint8_t region;
        bool escapes;
        bool anchor_keyed;
    } cases[] = {
        {".a:has(+ .x)", STYLE_HAS_RELATION_SELF,
         STYLE_HAS_REGION_SIBLINGS, false, true},
        {".a:has(~ .x .y)", STYLE_HAS_RELATION_SELF,
         STYLE_HAS_REGION_SIBLINGS, false, true},
        {".a:has(> .x)", STYLE_HAS_RELATION_SELF,
         STYLE_HAS_REGION_DESCENDANTS, false, true},
        {".a:has(.x) .b", STYLE_HAS_RELATION_ANCESTOR,
         STYLE_HAS_REGION_DESCENDANTS, false, true},
        {".b:where(.a:has(> [hidden]) *)", STYLE_HAS_RELATION_ANCESTOR,
         STYLE_HAS_REGION_DESCENDANTS, false, true},
        {".b:where(.a:where(:has(.x)) *)", STYLE_HAS_RELATION_ANCESTOR,
         STYLE_HAS_REGION_DESCENDANTS, false, true},
        {".b:not(:has(ul:not(:empty)))", STYLE_HAS_RELATION_SELF,
         STYLE_HAS_REGION_DESCENDANTS, false, true},
        {":is(:has(.x))", STYLE_HAS_RELATION_SELF,
         STYLE_HAS_REGION_DESCENDANTS, false, false},
        {".b:where(:has(.x:is(:last-child *)))", STYLE_HAS_RELATION_SELF,
         STYLE_HAS_REGION_DESCENDANTS, true, true},
        {".b:has(button:disabled)", STYLE_HAS_RELATION_SELF,
         STYLE_HAS_REGION_DESCENDANTS, true, true},
        {".b:has(~ :nth-child(2 of .x))", STYLE_HAS_RELATION_SELF,
         STYLE_HAS_REGION_SIBLINGS, true, true},
        {".b:has(> :nth-child(1 of p, h1))", STYLE_HAS_RELATION_SELF,
         STYLE_HAS_REGION_DESCENDANTS, false, true},
        {".b:has(.x:first-child + :is(p, ul:has(> li)))",
         STYLE_HAS_RELATION_SELF, STYLE_HAS_REGION_DESCENDANTS, false, true},
        {".a:has(.x) ~ .b", STYLE_HAS_RELATION_OTHER, 0, false, true},
        {".b:is(h3:has(button) + * *)", STYLE_HAS_RELATION_OTHER, 0, false,
         true},
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        char html[256];
        int written = snprintf(html, sizeof(html),
            "<!doctype html><style>%s{color:red}</style><p>x</p>",
            cases[i].selector);
        CHECK(written > 0 && (size_t) written < sizeof(html));
        PocDocument document = {0};
        Stylesheet sheet = {0};
        ViewportContext viewport;
        CHECK(document_parse(&document, budget, html, (size_t) written, 19)
            && viewport_context_init(&viewport, 480, 272, 480, 272)
            && stylesheet_build_context(&sheet, budget, &document,
                                        &viewport));
        StyleHasPlanSummary plan[4];
        bool bounded = true;
        size_t count = style_has_plan_summarize(&sheet, plan, 4, &bounded);
        if (count != 1 || bounded || plan[0].relation != cases[i].relation
            || (cases[i].relation != STYLE_HAS_RELATION_OTHER
                && (plan[0].region != cases[i].region
                    || plan[0].escapes != cases[i].escapes
                    || plan[0].anchor_keyed != cases[i].anchor_keyed))) {
            printf("has plan %s: count=%zu bounded=%d relation=%d region=%u "
                   "escapes=%d anchor-keyed=%d\n", cases[i].selector, count,
                   (int) bounded, count == 0 ? -1 : (int) plan[0].relation,
                   count == 0 ? 0u : (unsigned) plan[0].region,
                   count == 0 ? -1 : (int) plan[0].escapes,
                   count == 0 ? -1 : (int) plan[0].anchor_keyed);
            CHECK(false);
        }
        stylesheet_destroy(&sheet);
        document_destroy(&document);
    }
    CHECK(budget->current == 0);
    return 0;
}

/* Tree changes that no :has() argument can end on, or that no anchor can
   reach, are not relational: the bridge journals them as such. */
static int test_has_tree_change_classification(Budget *budget)
{
    static const char html[] =
        "<!doctype html><style>.a:has(.x){color:red}"
        ".b:has(~ .y){color:blue}.c:has(> .z:empty){color:green}"
        ".k:has(> *){color:gray}.w:has(.q:first-child .x){color:olive}"
        ".s:has(~ .y .x){color:navy}</style>"
        "<div class=w><div id=P2><div class=q><span class=x>x</span></div>"
        "</div></div>"
        "<div><div class=s>s</div><div class=y><div id=deep>d</div></div>"
        "</div>"
        "<div class=a><div id=inA>a</div></div>"
        "<div><div class=b>b</div><div id=afterB>c</div></div>"
        "<div class=c><span class=z id=Z>t</span></div>"
        "<div class=k id=K></div><div id=plain>p</div>";
    PocDocument document = {0};
    Stylesheet sheet = {0};
    ViewportContext viewport;
    CHECK(document_parse(&document, budget, html, sizeof(html) - 1u, 19)
        && viewport_context_init(&viewport, 480, 272, 480, 272)
        && stylesheet_build_context(&sheet, budget, &document, &viewport));
    lxb_dom_node_t *body = document_body_node(&document);
    static const struct {
        const char *parent;
        const char *classes;
        bool after_parent;
        bool expected;
        bool first;
    } cases[] = {
        {"plain", "q", false, false, false}, /* no feature, no anchor */
        {"inA", "q", false, false, false},   /* under an anchor, no feature */
        {"inA", "x", false, true, false},    /* the argument's element */
        {"afterB", "y", true, true, false},  /* a later sibling of .b */
        {"K", "q", false, true, false},      /* keyless, anchor parent */
        /* .q stops being :first-child: its descendant .x moves .w. */
        {"P2", "z", false, true, true},
        /* The anchor is an earlier sibling of an ancestor. */
        {"deep", "x", false, true, false},
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        lxb_dom_node_t *target = find_id(body, cases[i].parent);
        CHECK(target != NULL);
        lxb_dom_node_t *parent = cases[i].after_parent ? target->parent
                                                       : target;
        BudgetAllocationOwner previous =
            document_allocation_owner_enter(&document);
        lxb_dom_element_t *element = lxb_dom_document_create_element(
            &document.html->dom_document, (const lxb_char_t *) "span", 4,
            NULL);
        CHECK(element != NULL && lxb_dom_element_set_attribute(
            element, (const lxb_char_t *) "class", 5,
            (const lxb_char_t *) cases[i].classes,
            strlen(cases[i].classes)) != NULL);
        lxb_dom_node_t *node = lxb_dom_interface_node(element);
        if (cases[i].first && parent->first_child != NULL)
            lxb_dom_node_insert_before(parent->first_child, node);
        else
            CHECK(lxb_dom_node_append_child(parent, node)
                  == LXB_DOM_EXCEPTION_OK);
        bool relational =
            stylesheet_tree_change_may_affect_has(&sheet, node, parent);
        lxb_dom_node_remove(node);
        lxb_dom_node_destroy_deep(node);
        document_allocation_owner_leave(&document, previous);
        if (relational != cases[i].expected) {
            printf("tree change %zu under %s: relational=%d\n", i,
                   cases[i].parent, (int) relational);
            CHECK(false);
        }
    }
    /* Text: only a parent an :empty argument can end on. */
    lxb_dom_node_t *z = find_id(body, "Z");
    lxb_dom_node_t *plain = find_id(body, "plain");
    CHECK(z != NULL && plain != NULL && z->first_child != NULL
          && plain->first_child != NULL);
    CHECK(stylesheet_tree_change_may_affect_has(&sheet, z->first_child, z));
    CHECK(!stylesheet_tree_change_may_affect_has(&sheet, plain->first_child,
                                                 plain));
    stylesheet_destroy(&sheet);
    document_destroy(&document);
    CHECK(budget->current == 0);
    return 0;
}

static uint32_t has_fuzz_next(uint32_t *state)
{
    *state = *state * 1103515245u + 12345u;
    return (*state >> 16) & 0x7fffu;
}

static size_t has_fuzz_elements(lxb_dom_node_t *root, lxb_dom_node_t **out,
                                size_t capacity)
{
    size_t count = 0;
    lxb_dom_node_t *node = root->first_child;
    while (node != NULL && count < capacity) {
        if (node->type == LXB_DOM_NODE_TYPE_ELEMENT) out[count++] = node;
        if (node->first_child != NULL) {
            node = node->first_child;
            continue;
        }
        while (node != root && node->next == NULL) node = node->parent;
        node = node == root ? NULL : node->next;
    }
    return count;
}

typedef struct {
    const char *css;
    const char *const *classes;
    size_t class_count;
    uint32_t seed;
    unsigned rounds;
    /* Check every tree or text change classified as not relational
       against the actual :has() answers of every surviving element. */
    bool oracle;
} HasFuzzConfig;

typedef struct {
    char text[160];
    size_t length;
} HasFuzzSnippet;

/* Every `:has(...)` of the (test's own, simply written) sheet, prefixed
   with the simple selectors before it in its compound, or before the
   :is()/:where()/:not() holding it: the answer a rule actually reads, at
   the elements it reads it at. */
static size_t has_fuzz_snippets(const char *css, HasFuzzSnippet *out,
                                size_t capacity)
{
    size_t count = 0, length = strlen(css);
    for (size_t at = 0; at + 5 <= length && count < capacity; at++) {
        if (strncmp(css + at, ":has(", 5) != 0) continue;
        size_t close = at + 4, depth = 0;
        for (; close < length; close++) {
            if (css[close] == '(') depth++;
            else if (css[close] == ')' && --depth == 0) break;
        }
        size_t begin = at;
        while (begin > 0 && strchr(" >+~,(}", css[begin - 1]) == NULL)
            begin--;
        size_t prefix_end = begin;
        if (begin > 0 && css[begin - 1] == '(') {
            /* Inside :is/:where/:not: the enclosing compound's prefix. */
            prefix_end = begin - 1;
            while (prefix_end > 0 && css[prefix_end - 1] != ':')
                prefix_end--;
            prefix_end = prefix_end == 0 ? 0 : prefix_end - 1;
            begin = prefix_end;
            while (begin > 0 && strchr(" >+~,(}", css[begin - 1]) == NULL)
                begin--;
        } else {
            prefix_end = at;
        }
        size_t prefix = prefix_end - begin;
        if (close >= length
            || prefix + close + 1 - at >= sizeof(out[count].text)) continue;
        memcpy(out[count].text, css + begin, prefix);
        memcpy(out[count].text + prefix, css + at, close + 1 - at);
        out[count].length = prefix + close + 1 - at;
        out[count].text[out[count].length] = '\0';
        count++;
    }
    return count;
}

static void has_fuzz_answers(lxb_dom_node_t *const *elements, size_t count,
                             const HasFuzzSnippet *snippets,
                             size_t snippet_count, uint8_t *answers)
{
    for (size_t e = 0; e < count; e++) {
        for (size_t k = 0; k < snippet_count; k++) {
            answers[e * snippet_count + k] = (uint8_t) style_selector_matches(
                elements[e], snippets[k].text, snippets[k].length);
        }
    }
}

static const char *has_fuzz_class(const HasFuzzConfig *config,
                                  uint32_t *state)
{
    return config->classes[has_fuzz_next(state) % config->class_count];
}

static lxb_dom_node_t *has_fuzz_create(const HasFuzzConfig *config,
                                       PocDocument *document,
                                       uint32_t *state, unsigned serial,
                                       bool nested)
{
    char classes[64], label[16];
    snprintf(classes, sizeof(classes), "%s %s",
             has_fuzz_class(config, state), has_fuzz_class(config, state));
    snprintf(label, sizeof(label), "n%u", serial);
    BudgetAllocationOwner previous = document_allocation_owner_enter(document);
    lxb_dom_element_t *element = lxb_dom_document_create_element(
        &document->html->dom_document, (const lxb_char_t *) "div", 3, NULL);
    lxb_dom_text_t *text = element == NULL ? NULL
        : lxb_dom_document_create_text_node(
              &document->html->dom_document, (const lxb_char_t *) label,
              strlen(label));
    lxb_dom_node_t *node = NULL;
    if (text != NULL
        && lxb_dom_element_set_attribute(
               element, (const lxb_char_t *) "class", 5,
               (const lxb_char_t *) classes, strlen(classes)) != NULL
        && lxb_dom_node_append_child(lxb_dom_interface_node(element),
                                     lxb_dom_interface_node(text))
               == LXB_DOM_EXCEPTION_OK)
        node = lxb_dom_interface_node(element);
    document_allocation_owner_leave(document, previous);
    /* Sometimes a small subtree, as frameworks insert. */
    if (node != NULL && nested && has_fuzz_next(state) % 3u == 0) {
        lxb_dom_node_t *child = has_fuzz_create(config, document, state,
                                                serial + 1000u, false);
        if (child != NULL) {
            previous = document_allocation_owner_enter(document);
            (void) lxb_dom_node_append_child(node, child);
            document_allocation_owner_leave(document, previous);
        }
    }
    return node;
}

static const char has_fuzz_css[] =
    ".s1:has(+ .x){color:#100001}.s2:has(~ .x){color:#100002}"
    ".s3:has(> .x){color:#100003}.s4:has(.x){color:#100004}"
    ".a5:has(.x) .b5{color:#100005}.a6:has(> .y) > .b6{color:#100006}"
    ".s7:not(:has(.y)){color:#100007}.s8:is(:has(.x), .zz){color:#100008}"
    ".s9:where(:has(.x:is(.m *))){color:#100009}"
    ".a10:has(.y) ~ .b10{color:#10000a}.s11:has(+ .p + .x){color:#10000b}"
    ".s12:has(> .x:first-child){color:#10000c}"
    ".s13:has(~ .q .x){color:#10000d}.s14:has([data-on]){color:#10000e}"
    "div:has(> .x) > .t16{color:#100010}"
    ".s17:has(~ .x:nth-child(odd)){color:#100011}"
    ".s18:has(> .y:empty){color:#100012}"
    ".s19:has(.p:not(:empty)) .b5{color:#100013}"
    ".s11:has(.q:first-child .x){color:#100014}"
    ".s4:has(.y) .s1{--tone:#200000;border-left:1px solid var(--tone)}";

/* Random class, attribute, text and tree changes, each followed by exactly
   the invalidation (and :has() classification) the navigation journal
   issues, must leave the reused cache rendering what a fresh build
   renders, without a single full reset. */
static int has_fuzz_run(Budget *budget, const HasFuzzConfig *config)
{
    static char html[16384];
    uint32_t state = config->seed;
    int written = snprintf(html, sizeof(html),
                           "<!doctype html><style>%s", config->css);
    CHECK(written > 0 && (size_t) written < sizeof(html));
    size_t length = (size_t) written;
    for (unsigned i = 0; i < 64; i++) {
        written = snprintf(html + length, sizeof(html) - length,
                           ".pad%u{margin:0}", i);
        CHECK(written > 0 && (size_t) written < sizeof(html) - length);
        length += (size_t) written;
    }
    written = snprintf(html + length, sizeof(html) - length,
                       "</style><body>");
    length += (size_t) written;
    /* A fixed random tree of 60 elements, up to four deep. */
    unsigned depth = 0;
    for (unsigned i = 0; i < 60; i++) {
        if (depth > 0 && has_fuzz_next(&state) % 3u == 0) {
            memcpy(html + length, "</div>", 6);
            length += 6;
            depth--;
        }
        written = snprintf(html + length, sizeof(html) - length,
            "<div class='%s %s'>e%u", has_fuzz_class(config, &state),
            has_fuzz_class(config, &state), i);
        CHECK(written > 0 && (size_t) written < sizeof(html) - length);
        length += (size_t) written;
        if (depth < 4 && has_fuzz_next(&state) % 2u == 0) {
            depth++;
        } else {
            memcpy(html + length, "</div>", 6);
            length += 6;
        }
    }
    PocDocument document = {0};
    Stylesheet sheet = {0};
    ViewportContext viewport;
    CHECK(document_parse(&document, budget, html, length, 19)
        && viewport_context_init(&viewport, 480, 272, 480, 272)
        && stylesheet_build_context(&sheet, budget, &document, &viewport));
    LayoutReuseCache *reuse = layout_reuse_cache_create(budget);
    CHECK(reuse != NULL);
    layout_reuse_cache_enable_retained_matches(reuse);
    LayoutDocument retained = {0}, fresh = {0};
    CHECK(layout_build_context_reuse(&retained, budget, &document, &sheet,
        NULL, NULL, &viewport, reuse));
    layout_destroy(&retained);
    lxb_dom_node_t *body = document_body_node(&document);
    CHECK(body != NULL);
    LayoutReuseStats start;
    layout_reuse_cache_stats(reuse, &start);
    unsigned serial = 0;
    static HasFuzzSnippet snippets[32];
    size_t snippet_count = config->oracle
        ? has_fuzz_snippets(config->css, snippets, 32) : 0;
    static uint8_t answers_before[256 * 32], answers_after[256 * 32];
    for (unsigned round = 0; round < config->rounds; round++) {
        lxb_dom_node_t *elements[256];
        size_t count = has_fuzz_elements(body, elements, 256);
        if (count == 0) {
            /* A removal took everything: start over under the body. */
            lxb_dom_node_t *created = has_fuzz_create(
                config, &document, &state, serial++, true);
            CHECK(created != NULL && lxb_dom_node_append_child(body, created)
                                         == LXB_DOM_EXCEPTION_OK);
            layout_reuse_cache_invalidate_tree(reuse, created, true);
            count = has_fuzz_elements(body, elements, 256);
        }
        lxb_dom_node_t *node = elements[has_fuzz_next(&state) % count];
        unsigned op = has_fuzz_next(&state) % 7u;
        if (count < 30) op = 3;
        bool classified = false, relational = true;
        if (snippet_count != 0 && op >= 3)
            has_fuzz_answers(elements, count, snippets, snippet_count,
                             answers_before);
        if (op == 0 || op == 1) {
            /* Toggle one class token. */
            const char *token = has_fuzz_class(config, &state);
            size_t old_length = 0;
            const lxb_char_t *old = lxb_dom_element_get_attribute(
                lxb_dom_interface_element(node), (const lxb_char_t *) "class",
                5, &old_length);
            char before[256], after[256];
            snprintf(before, sizeof(before), "%.*s", (int) old_length,
                     old == NULL ? "" : (const char *) old);
            char padded[260], needle[40];
            snprintf(padded, sizeof(padded), " %s ", before);
            snprintf(needle, sizeof(needle), " %s ", token);
            char *found = strstr(padded, needle);
            if (found != NULL) {
                memmove(found + 1, found + strlen(needle),
                        strlen(found + strlen(needle)) + 1);
                snprintf(after, sizeof(after), "%s", padded + 1);
            } else {
                snprintf(after, sizeof(after), "%s %s", before, token);
            }
            uint32_t tokens[8];
            size_t token_count = 0;
            bool exact = stylesheet_attribute_change_tokens(
                "class", 5, before, strlen(before), after, strlen(after),
                tokens, 8, &token_count);
            bool relational = stylesheet_attribute_change_may_affect_has(
                &sheet, "class", 5, before, strlen(before), after,
                strlen(after));
            CHECK(lxb_dom_element_set_attribute(
                lxb_dom_interface_element(node), (const lxb_char_t *) "class",
                5, (const lxb_char_t *) after, strlen(after)) != NULL);
            layout_reuse_cache_invalidate_attribute(
                reuse, node, exact ? tokens : NULL, token_count, false,
                relational);
        } else if (op == 2) {
            /* Toggle an attribute only a :has() argument reads. */
            bool present = lxb_dom_element_has_attribute(
                lxb_dom_interface_element(node),
                (const lxb_char_t *) "data-on", 7);
            bool relational = stylesheet_attribute_change_may_affect_has(
                &sheet, "data-on", 7, present ? "" : NULL, 0,
                present ? NULL : "", 0);
            if (present) {
                CHECK(lxb_dom_element_remove_attribute(
                    lxb_dom_interface_element(node),
                    (const lxb_char_t *) "data-on", 7) == LXB_STATUS_OK);
            } else {
                CHECK(lxb_dom_element_set_attribute(
                    lxb_dom_interface_element(node),
                    (const lxb_char_t *) "data-on", 7,
                    (const lxb_char_t *) "", 0) != NULL);
            }
            if (relational) {
                layout_reuse_cache_invalidate_relational(reuse, node,
                                                         "data-on", false);
            } else {
                layout_reuse_cache_invalidate_node_scoped(reuse, node, false);
            }
        } else if (op == 3) {
            /* Insert a new element before a random child, or append. */
            lxb_dom_node_t *created = has_fuzz_create(
                config, &document, &state, serial++, true);
            CHECK(created != NULL);
            if (has_fuzz_next(&state) % 2u == 0) {
                lxb_dom_node_insert_before(node, created);
            } else {
                CHECK(lxb_dom_node_append_child(node, created)
                      == LXB_DOM_EXCEPTION_OK);
            }
            /* Classified after insertion, as the bridge does. */
            classified = true;
            uint64_t entries = UINT64_MAX;
            uint32_t serial = 0;
            relational = stylesheet_tree_change_has_entries(
                &sheet, created, created->parent, &entries, &serial);
            /* The journal notes only the entries the probe saw reached
               (half the time, to keep the unfiltered path covered). */
            if (has_fuzz_next(&state) % 2u == 0)
                layout_reuse_cache_set_structure_filter(reuse, entries,
                                                        serial);
            layout_reuse_cache_invalidate_tree(reuse, created, relational);
            layout_reuse_cache_set_structure_filter(reuse, UINT64_MAX, 0);
        } else if (op == 6) {
            /* Move the element under another one outside its subtree;
               the bridge journals a move as relational. */
            lxb_dom_node_t *target = elements[has_fuzz_next(&state) % count];
            bool inside = false;
            for (lxb_dom_node_t *at = target; at != NULL && !inside;
                 at = at->parent)
                inside = at == node;
            if (inside) continue;
            lxb_dom_node_t *old_parent = node->parent;
            /* The bridge probes both positions and the record keeps the
               union (half the time, to keep the unfiltered path). */
            uint64_t before_entries = UINT64_MAX, after_entries = UINT64_MAX;
            uint32_t before_serial = 0, after_serial = 0;
            bool before_relational = stylesheet_tree_change_has_entries(
                &sheet, node, old_parent, &before_entries, &before_serial);
            lxb_dom_node_remove(node);
            CHECK(lxb_dom_node_append_child(target, node)
                  == LXB_DOM_EXCEPTION_OK);
            bool after_relational = stylesheet_tree_change_has_entries(
                &sheet, node, target, &after_entries, &after_serial);
            bool relational_move = before_relational || after_relational;
            if (has_fuzz_next(&state) % 2u == 0 && before_serial != 0
                && before_serial == after_serial)
                layout_reuse_cache_set_structure_filter(
                    reuse, before_entries | after_entries, after_serial);
            else
                relational_move = true;
            layout_reuse_cache_invalidate_children(reuse, old_parent, node,
                                                   relational_move);
            layout_reuse_cache_invalidate_tree(reuse, node, relational_move);
            layout_reuse_cache_set_structure_filter(reuse, UINT64_MAX, 0);
        } else if (op == 5) {
            /* Empty or refill the element's text (its :empty state). */
            lxb_dom_node_t *text = node->first_child;
            if (text == NULL || text->type != LXB_DOM_NODE_TYPE_TEXT)
                continue;
            size_t text_length = 0;
            (void) lxb_dom_node_text_content(text, &text_length);
            BudgetAllocationOwner previous =
                document_allocation_owner_enter(&document);
            lxb_status_t status = lxb_dom_node_text_content_set(
                text, (const lxb_char_t *) "e", text_length == 0 ? 1u : 0u);
            document_allocation_owner_leave(&document, previous);
            CHECK(status == LXB_STATUS_OK);
            classified = true;
            uint64_t entries = UINT64_MAX;
            uint32_t serial = 0;
            relational = stylesheet_tree_change_has_entries(
                &sheet, text, node, &entries, &serial);
            layout_reuse_cache_set_structure_filter(reuse, entries, serial);
            if (relational)
                layout_reuse_cache_invalidate_relational(reuse, text, NULL,
                                                         true);
            else
                layout_reuse_cache_invalidate_node_scoped(reuse, text, true);
            layout_reuse_cache_set_structure_filter(reuse, UINT64_MAX, 0);
        } else {
            /* Remove a subtree: classify it while attached, retire it,
               then scope its old parent. */
            lxb_dom_node_t *parent = node->parent;
            classified = true;
            uint64_t entries = UINT64_MAX;
            uint32_t serial = 0;
            relational = stylesheet_tree_change_has_entries(
                &sheet, node, parent, &entries, &serial);
            /* The removed subtree's answers go with it. */
            for (size_t e = 0; e < count; e++) {
                for (const lxb_dom_node_t *at = elements[e]; at != NULL;
                     at = at->parent) {
                    if (at == node) {
                        elements[e] = NULL;
                        break;
                    }
                }
            }
            layout_reuse_cache_retire_subtree(reuse, node);
            lxb_dom_node_remove(node);
            /* The journal names the removed child while it lives. */
            bool known = has_fuzz_next(&state) % 2u == 0;
            layout_reuse_cache_set_structure_filter(reuse, entries, serial);
            layout_reuse_cache_invalidate_children(
                reuse, parent, known ? node : NULL, relational);
            layout_reuse_cache_set_structure_filter(reuse, UINT64_MAX, 0);
            BudgetAllocationOwner previous =
                document_allocation_owner_enter(&document);
            lxb_dom_node_destroy_deep(node);
            document_allocation_owner_leave(&document, previous);
        }
        if (snippet_count != 0 && classified && !relational) {
            size_t kept = 0;
            for (size_t e = 0; e < count; e++) {
                if (elements[e] == NULL) continue;
                for (size_t k = 0; k < snippet_count; k++)
                    answers_before[kept * snippet_count + k] =
                        answers_before[e * snippet_count + k];
                elements[kept++] = elements[e];
            }
            has_fuzz_answers(elements, kept, snippets, snippet_count,
                             answers_after);
            if (memcmp(answers_before, answers_after,
                       kept * snippet_count) != 0) {
                printf("has fuzz round %u op %u: a :has() answer moved on a "
                       "change classified as not relational\n", round, op);
                for (size_t e = 0; e < kept; e++) {
                    for (size_t k = 0; k < snippet_count; k++) {
                        size_t at = e * snippet_count + k;
                        if (answers_before[at] == answers_after[at]) continue;
                        size_t class_length = 0;
                        const lxb_char_t *classes =
                            lxb_dom_element_get_attribute(
                                lxb_dom_interface_element(elements[e]),
                                (const lxb_char_t *) "class", 5,
                                &class_length);
                        printf("  '%.*s' %s: %u -> %u\n", (int) class_length,
                               classes == NULL ? "" : (const char *) classes,
                               snippets[k].text, answers_before[at],
                               answers_after[at]);
                    }
                }
                CHECK(false);
            }
        }
        layout_reuse_cache_flush_invalidations(reuse);
        CHECK(layout_build_context_reuse(&retained, budget, &document, &sheet,
            NULL, NULL, &viewport, reuse));
        CHECK(layout_build_context(&fresh, budget, &document, &sheet,
            NULL, NULL, &viewport));
        if (!commands_equal(&retained, &fresh)) {
            printf("has fuzz round %u op %u diverged\n", round, op);
            CHECK(false);
        }
        layout_destroy(&retained);
        layout_destroy(&fresh);
    }
    LayoutReuseStats end;
    layout_reuse_cache_stats(reuse, &end);
    /* Every change stayed scoped and most styles were reused. */
    if (end.full_resets != start.full_resets || end.has_fallbacks != 0
        || end.has_scoped <= start.has_scoped
        || end.style_hits - start.style_hits
               <= end.style_misses - start.style_misses) {
        printf("has fuzz resets=%zu/%zu fallbacks=%zu scoped=%zu "
               "styles=%zu/%zu dropped=%zu scans=%zu\n", start.full_resets,
               end.full_resets, end.has_fallbacks, end.has_scoped,
               end.style_hits - start.style_hits,
               end.style_misses - start.style_misses, end.has_dropped,
               end.has_scans);
        CHECK(false);
    }
    layout_reuse_cache_destroy(reuse);
    stylesheet_destroy(&sheet);
    document_destroy(&document);
    CHECK(budget->current == 0);
    return 0;
}

/* Focus moves and inline custom-property writes, each followed by exactly
   the invalidation the navigation journal issues for it, must leave the
   reused cache rendering what a fresh build renders. `scoped` asks that
   none of them resets the cache and that most styles are reused. */
typedef struct {
    const char *css;
    uint32_t seed;
    unsigned rounds;
    bool scoped;
} StateFuzzConfig;

static const char *const state_fuzz_classes[] = {
    "mk", "mk2", "mk3", "mk4", "k1", "k3", "k4", "k5", "k6", "k7", "k8",
    "v1", "v2", "v3", "v4", "d3", "hs", "x", "p", "q",
};
#define STATE_FUZZ_CLASS_COUNT \
    (sizeof(state_fuzz_classes) / sizeof(state_fuzz_classes[0]))

static const char state_fuzz_css[] =
    ".k1:where(.mk:focus-within *){color:#400001}"
    ".mk2:focus-within{background-color:#400002}"
    ".mk3:focus-within .k3{color:#400003}"
    ".k4:where(.mk:not(:focus-within) *){padding-left:3px}"
    ".mk:focus-within > .k5{color:#400005}"
    ".k6:focus{color:#400006}.mk:focus-visible .k7{color:#400007}"
    ".mk4:focus-within{--fw:#400008}.k8{color:var(--fw,#400009)}"
    ".v1{color:var(--p1,#500001)}.v2{background-color:var(--p2,#500002)}"
    ".d3{--p3:var(--p1,#500003)}"
    ".v3{border-left:1px solid var(--p3,#500004)}"
    ".hs:has(.x){--p4:#500005}.v4{color:var(--p4,#500006)}"
    ".p .q{margin-left:2px}";

/* Class and attribute dependencies beyond the subject: ancestor markers
   keyed by their subjects, sibling reach, attribute selectors off the
   subject and on it, an unkeyed subject, and custom properties declared
   through an ancestor token. */
static const char state_fuzz_reach_css[] =
    ".p .q{margin-left:2px}.k1:where(.mk *){color:#400021}"
    ".p + .q{padding-left:1px}.x ~ .v1{margin-top:1px}"
    ".x:first-child + .k6{color:#400022}"
    "[data-a] .k3{color:#400023}.k4[data-b]{color:#400024}"
    "[data-c] + .k7{color:#400025}[data-c] ~ .k8 .v2{color:#400026}"
    ".mk2 *{margin-bottom:1px}"
    ".mk3 .v2{--p2:#400027}.v2{background-color:var(--p2,#400028)}"
    "[data-a] .v3{--p3:#400029}.v3{border-left:1px solid var(--p3,#40002a)}"
    ".k5.x .v4{color:#40002b}.hs:has(.x){--p4:#500005}"
    ".v4{color:var(--p4,#500006)}"
    /* Generated content appearing and disappearing with the element's
       own, an ancestor's and a sibling's state, and through a custom
       property only the pseudo-element reads. */
    ".k6::before{content:'b6'}.mk .k7::after{content:'a7'}"
    "[data-a] .k8::before{content:'b8'}.x ~ .q::after{content:'aq'}"
    ".mk4 .v3{--c:'c3'}.v3::after{content:var(--c)}"
    /* Custom properties sibling reach sets, read by the reached
       elements' descendants. */
    ".x + .k6{--sib:#400031}.p ~ .k7{--sib:#400032}"
    "[data-c] + .k8{--sib:#400033}.k5:first-child{--sib:#400039}"
    ".x ~ .q,.x ~ .k1,.p ~ .v1,.p ~ .v2,.mk ~ .k3,.mk ~ .v4,.k4 ~ .v3,"
    "[data-a] ~ .k4,[data-b] ~ .mk2{--sib:#40003b}"
    ".q{background-color:var(--sib,#400037)}"
    ".v4{border-left:1px solid var(--sib,#400038)}"
    /* Readers that are no target, so no target's own style changes. */
    ".hs,.d3,.mk3{background-color:var(--sib,#40003a)}";

static const char *const state_fuzz_attributes[] = {
    "data-a", "data-b", "data-c", "data-z", "title",
};

static const char *state_fuzz_class(uint32_t *state)
{
    return state_fuzz_classes[has_fuzz_next(state) % STATE_FUZZ_CLASS_COUNT];
}

static int state_fuzz_run(Budget *budget, const StateFuzzConfig *config)
{
    static char html[16384];
    uint32_t state = config->seed;
    int written = snprintf(html, sizeof(html),
                           "<!doctype html><html style='--p1:#600001'>"
                           "<style>%s", config->css);
    CHECK(written > 0 && (size_t) written < sizeof(html));
    size_t length = (size_t) written;
    for (unsigned i = 0; i < 64; i++) {
        written = snprintf(html + length, sizeof(html) - length,
                           ".pad%u{margin:0}", i);
        CHECK(written > 0 && (size_t) written < sizeof(html) - length);
        length += (size_t) written;
    }
    written = snprintf(html + length, sizeof(html) - length,
                       "</style><body>");
    length += (size_t) written;
    unsigned depth = 0;
    for (unsigned i = 0; i < 60; i++) {
        if (depth > 0 && has_fuzz_next(&state) % 3u == 0) {
            memcpy(html + length, "</div>", 6);
            length += 6;
            depth--;
        }
        written = snprintf(html + length, sizeof(html) - length,
            "<div class='%s %s'>e%u", state_fuzz_class(&state),
            state_fuzz_class(&state), i);
        CHECK(written > 0 && (size_t) written < sizeof(html) - length);
        length += (size_t) written;
        if (depth < 5 && has_fuzz_next(&state) % 2u == 0) {
            depth++;
        } else {
            memcpy(html + length, "</div>", 6);
            length += 6;
        }
    }
    PocDocument document = {0};
    Stylesheet sheet = {0};
    ViewportContext viewport;
    CHECK(document_parse(&document, budget, html, length, 19)
        && viewport_context_init(&viewport, 480, 272, 480, 272)
        && stylesheet_build_context(&sheet, budget, &document, &viewport));
    LayoutReuseCache *reuse = layout_reuse_cache_create(budget);
    CHECK(reuse != NULL);
    layout_reuse_cache_enable_retained_matches(reuse);
    LayoutDocument retained = {0}, fresh = {0};
    CHECK(layout_build_context_reuse(&retained, budget, &document, &sheet,
        NULL, NULL, &viewport, reuse));
    layout_destroy(&retained);
    lxb_dom_node_t *body = document_body_node(&document);
    lxb_dom_node_t *root = body == NULL ? NULL : body->parent;
    CHECK(body != NULL && root != NULL
          && root->type == LXB_DOM_NODE_TYPE_ELEMENT);
    LayoutReuseStats start;
    layout_reuse_cache_stats(reuse, &start);
    lxb_dom_node_t *focused = NULL;
    static const char *const colors[] = {
        "#700001", "#700002", "#700003", "#700004"
    };
    for (unsigned round = 0; round < config->rounds; round++) {
        lxb_dom_node_t *elements[256];
        size_t count = has_fuzz_elements(body, elements, 256);
        CHECK(count != 0);
        lxb_dom_node_t *node = elements[has_fuzz_next(&state) % count];
        unsigned op = has_fuzz_next(&state) % 7u;
        if (getenv("STATE_FUZZ_TRACE")) { printf("round %u op %u\n", round, op); fflush(stdout); }
        if (op == 0 || op == 1) {
            /* Move focus: the old element loses the marker, the new one
               gains it (sometimes nothing takes it). */
            lxb_dom_node_t *next = has_fuzz_next(&state) % 5u == 0
                ? NULL : node;
            if (next == focused) continue;
            if (focused != NULL) {
                CHECK(lxb_dom_element_remove_attribute(
                    lxb_dom_interface_element(focused),
                    (const lxb_char_t *) "data-tilefinch-focus", 20)
                      == LXB_STATUS_OK);
                layout_reuse_cache_invalidate_focus(reuse, focused);
            }
            if (next != NULL) {
                CHECK(lxb_dom_element_set_attribute(
                    lxb_dom_interface_element(next),
                    (const lxb_char_t *) "data-tilefinch-focus", 20,
                    (const lxb_char_t *) "", 0) != NULL);
                layout_reuse_cache_invalidate_focus(reuse, next);
            }
            focused = next;
        } else if (op == 2 || op == 3) {
            /* Rewrite an inline style: custom properties (on the root
               element half the time), sometimes an ordinary property. */
            lxb_dom_node_t *target = has_fuzz_next(&state) % 2u == 0
                ? root : node;
            char style[160];
            int used = snprintf(style, sizeof(style), "--p%u:%s;",
                                1u + has_fuzz_next(&state) % 4u,
                                colors[has_fuzz_next(&state) % 4u]);
            if (has_fuzz_next(&state) % 2u == 0)
                used += snprintf(style + used, sizeof(style) - (size_t) used,
                                 "--p%u:%s;", 1u + has_fuzz_next(&state) % 4u,
                                 colors[has_fuzz_next(&state) % 4u]);
            if (has_fuzz_next(&state) % 3u == 0)
                used += snprintf(style + used, sizeof(style) - (size_t) used,
                                 "padding-top:%upx;",
                                 has_fuzz_next(&state) % 4u);
            if (has_fuzz_next(&state) % 5u == 0) used = 0;
            size_t old_length = 0;
            const lxb_char_t *old = lxb_dom_element_get_attribute(
                lxb_dom_interface_element(target),
                (const lxb_char_t *) "style", 5, &old_length);
            char before[256];
            snprintf(before, sizeof(before), "%.*s", (int) old_length,
                     old == NULL ? "" : (const char *) old);
            if (getenv("STATE_FUZZ_TRACE")) { printf(" style '%s' -> '%.*s'\n", before, used, style); fflush(stdout); }
            bool relational = stylesheet_attribute_change_may_affect_has(
                &sheet, "style", 5, before, strlen(before), style,
                (size_t) used);
            if (getenv("STATE_FUZZ_TRACE")) { printf(" rel %d\n", relational); fflush(stdout); }
            CHECK(lxb_dom_element_set_attribute(
                lxb_dom_interface_element(target),
                (const lxb_char_t *) "style", 5, (const lxb_char_t *) style,
                (size_t) used) != NULL);
            layout_reuse_cache_invalidate_inline_style(
                reuse, target, NULL, relational);
        } else if (op == 5) {
            /* Write or remove an attribute: some selectors read it off
               the subject, some on it, one never (data-z, title). The
               journal's path for a write no :has() reads. */
            const char *name = state_fuzz_attributes[
                has_fuzz_next(&state) % (sizeof(state_fuzz_attributes)
                                         / sizeof(state_fuzz_attributes[0]))];
            size_t name_length = strlen(name);
            size_t old_length = 0;
            const lxb_char_t *old = lxb_dom_element_get_attribute(
                lxb_dom_interface_element(node), (const lxb_char_t *) name,
                name_length, &old_length);
            bool present = old != NULL;
            bool relational = stylesheet_attribute_change_may_affect_has(
                &sheet, name, name_length,
                present ? (const char *) old : NULL, old_length,
                present ? NULL : "1", present ? 0 : 1);
            if (present) {
                CHECK(lxb_dom_element_remove_attribute(
                    lxb_dom_interface_element(node),
                    (const lxb_char_t *) name, name_length)
                      == LXB_STATUS_OK);
            } else {
                CHECK(lxb_dom_element_set_attribute(
                    lxb_dom_interface_element(node),
                    (const lxb_char_t *) name, name_length,
                    (const lxb_char_t *) "1", 1) != NULL);
            }
            if (relational)
                layout_reuse_cache_invalidate_relational(reuse, node, name,
                                                         false);
            else
                layout_reuse_cache_invalidate_own_attribute(reuse, node,
                                                            name);
        } else if (op == 6) {
            /* Replace the whole class list (atomic-CSS style swaps). */
            size_t old_length = 0;
            const lxb_char_t *old = lxb_dom_element_get_attribute(
                lxb_dom_interface_element(node), (const lxb_char_t *) "class",
                5, &old_length);
            char before[256], after[256];
            snprintf(before, sizeof(before), "%.*s", (int) old_length,
                     old == NULL ? "" : (const char *) old);
            size_t used = 0;
            unsigned tokens_wanted = has_fuzz_next(&state) % 5u;
            after[0] = '\0';
            for (unsigned t = 0; t < tokens_wanted; t++)
                used += (size_t) snprintf(after + used, sizeof(after) - used,
                                          "%s%s", t == 0 ? "" : " ",
                                          state_fuzz_class(&state));
            uint32_t tokens[64];
            size_t token_count = 0;
            bool exact = stylesheet_attribute_change_tokens(
                "class", 5, before, strlen(before), after, strlen(after),
                tokens, 64, &token_count);
            bool relational = stylesheet_attribute_change_may_affect_has(
                &sheet, "class", 5, before, strlen(before), after,
                strlen(after));
            CHECK(lxb_dom_element_set_attribute(
                lxb_dom_interface_element(node), (const lxb_char_t *) "class",
                5, (const lxb_char_t *) after, strlen(after)) != NULL);
            layout_reuse_cache_invalidate_attribute(
                reuse, node, exact ? tokens : NULL, token_count, false,
                relational);
        } else {
            /* Toggle one class token. */
            const char *token = state_fuzz_class(&state);
            size_t old_length = 0;
            const lxb_char_t *old = lxb_dom_element_get_attribute(
                lxb_dom_interface_element(node), (const lxb_char_t *) "class",
                5, &old_length);
            char before[256], after[256];
            snprintf(before, sizeof(before), "%.*s", (int) old_length,
                     old == NULL ? "" : (const char *) old);
            char padded[260], needle[40];
            snprintf(padded, sizeof(padded), " %s ", before);
            snprintf(needle, sizeof(needle), " %s ", token);
            char *found = strstr(padded, needle);
            if (found != NULL) {
                memmove(found + 1, found + strlen(needle),
                        strlen(found + strlen(needle)) + 1);
                snprintf(after, sizeof(after), "%s", padded + 1);
            } else {
                snprintf(after, sizeof(after), "%s %s", before, token);
            }
            uint32_t tokens[8];
            size_t token_count = 0;
            bool exact = stylesheet_attribute_change_tokens(
                "class", 5, before, strlen(before), after, strlen(after),
                tokens, 8, &token_count);
            bool relational = stylesheet_attribute_change_may_affect_has(
                &sheet, "class", 5, before, strlen(before), after,
                strlen(after));
            CHECK(lxb_dom_element_set_attribute(
                lxb_dom_interface_element(node), (const lxb_char_t *) "class",
                5, (const lxb_char_t *) after, strlen(after)) != NULL);
            layout_reuse_cache_invalidate_attribute(
                reuse, node, exact ? tokens : NULL, token_count, false,
                relational);
        }
        layout_reuse_cache_flush_invalidations(reuse);
        CHECK(layout_build_context_reuse(&retained, budget, &document, &sheet,
            NULL, NULL, &viewport, reuse));
        CHECK(layout_build_context(&fresh, budget, &document, &sheet,
            NULL, NULL, &viewport));
        if (!commands_equal(&retained, &fresh)) {
            printf("state fuzz seed %u round %u op %u diverged\n",
                   config->seed, round, op);
            CHECK(false);
        }
        layout_destroy(&retained);
        layout_destroy(&fresh);
    }
    LayoutReuseStats end;
    layout_reuse_cache_stats(reuse, &end);
    printf("state fuzz seed %u: resets=%zu styles=%zu/%zu\n", config->seed,
           end.full_resets - start.full_resets,
           end.style_hits - start.style_hits,
           end.style_misses - start.style_misses);
    if (config->scoped
        && (end.full_resets != start.full_resets
            || end.style_hits - start.style_hits
                   <= 2u * (end.style_misses - start.style_misses))) {
        CHECK(false);
    }
    layout_reuse_cache_destroy(reuse);
    stylesheet_destroy(&sheet);
    document_destroy(&document);
    CHECK(budget->current == 0);
    return 0;
}

static lxb_dom_node_t *state_find_id(lxb_dom_node_t *node, const char *id)
{
    for (; node != NULL; node = node->next) {
        if (node->type == LXB_DOM_NODE_TYPE_ELEMENT) {
            size_t length = 0;
            const char *value = document_attribute(node, "id", &length);
            if (value != NULL && length == strlen(id)
                && memcmp(value, id, length) == 0) return node;
        }
        lxb_dom_node_t *found = state_find_id(node->first_child, id);
        if (found != NULL) return found;
    }
    return NULL;
}

/* One mutation, invalidated as the navigation journal does, must leave
   the reused cache rendering what a fresh build renders. `step` applies
   the change and its invalidation. */
typedef void (*ReachStep)(LayoutReuseCache *reuse, const Stylesheet *sheet,
                          lxb_dom_node_t *root);

static int reach_case(Budget *budget, const char *label, const char *html,
                      ReachStep step)
{
    PocDocument document = {0};
    Stylesheet sheet = {0};
    ViewportContext viewport;
    CHECK(document_parse(&document, budget, html, strlen(html), 19)
        && viewport_context_init(&viewport, 480, 272, 480, 272)
        && stylesheet_build_context(&sheet, budget, &document, &viewport));
    LayoutReuseCache *reuse = layout_reuse_cache_create(budget);
    CHECK(reuse != NULL);
    layout_reuse_cache_enable_retained_matches(reuse);
    LayoutDocument retained = {0}, fresh = {0};
    CHECK(layout_build_context_reuse(&retained, budget, &document, &sheet,
        NULL, NULL, &viewport, reuse));
    layout_destroy(&retained);
    lxb_dom_node_t *body = document_body_node(&document);
    CHECK(body != NULL && body->parent != NULL
          && body->parent->type == LXB_DOM_NODE_TYPE_ELEMENT);
    /* The <html> element (document.html is the document node). */
    step(reuse, &sheet, body->parent);
    layout_reuse_cache_flush_invalidations(reuse);
    CHECK(layout_build_context_reuse(&retained, budget, &document, &sheet,
        NULL, NULL, &viewport, reuse));
    CHECK(layout_build_context(&fresh, budget, &document, &sheet,
        NULL, NULL, &viewport));
    bool equal = commands_equal(&retained, &fresh);
    if (!equal) printf("custom-property reach %s diverged\n", label);
    layout_destroy(&retained);
    layout_destroy(&fresh);
    layout_reuse_cache_destroy(reuse);
    stylesheet_destroy(&sheet);
    document_destroy(&document);
    CHECK(budget->current == 0);
    return equal ? 0 : 1;
}

static void reach_set_class(LayoutReuseCache *reuse, const Stylesheet *sheet,
                            lxb_dom_node_t *root)
{
    lxb_dom_node_t *node = state_find_id(root, "a");
    uint32_t tokens[8];
    size_t count = 0;
    bool exact = stylesheet_attribute_change_tokens(
        "class", 5, "", 0, "on", 2, tokens, 8, &count);
    bool relational = stylesheet_attribute_change_may_affect_has(
        sheet, "class", 5, "", 0, "on", 2);
    (void) lxb_dom_element_set_attribute(lxb_dom_interface_element(node),
        (const lxb_char_t *) "class", 5, (const lxb_char_t *) "on", 2);
    layout_reuse_cache_invalidate_attribute(reuse, node,
        exact ? tokens : NULL, count, false, relational);
}

static void reach_set_attribute(LayoutReuseCache *reuse,
                                const Stylesheet *sheet,
                                lxb_dom_node_t *root)
{
    lxb_dom_node_t *node = state_find_id(root, "a");
    bool relational = stylesheet_attribute_change_may_affect_has(
        sheet, "data-on", 7, NULL, 0, "1", 1);
    (void) lxb_dom_element_set_attribute(lxb_dom_interface_element(node),
        (const lxb_char_t *) "data-on", 7, (const lxb_char_t *) "1", 1);
    if (relational)
        layout_reuse_cache_invalidate_relational(reuse, node, "data-on",
                                                 false);
    else
        layout_reuse_cache_invalidate_own_attribute(reuse, node, "data-on");
}

static void reach_insert_before(LayoutReuseCache *reuse,
                                const Stylesheet *sheet,
                                lxb_dom_node_t *root)
{
    lxb_dom_node_t *target = state_find_id(root, "t");
    lxb_dom_element_t *created = lxb_dom_document_create_element(
        target->owner_document, (const lxb_char_t *) "div", 3, NULL);
    lxb_dom_node_insert_before(target, lxb_dom_interface_node(created));
    bool relational = stylesheet_tree_change_may_affect_has(
        sheet, lxb_dom_interface_node(created), target->parent);
    layout_reuse_cache_invalidate_tree(reuse,
                                       lxb_dom_interface_node(created),
                                       relational);
}

static void reach_root_inline(LayoutReuseCache *reuse,
                              const Stylesheet *sheet, lxb_dom_node_t *root)
{
    static const char style[] = "--c:'gen'";
    bool relational = stylesheet_attribute_change_may_affect_has(
        sheet, "style", 5, NULL, 0, style, sizeof(style) - 1u);
    (void) lxb_dom_element_set_attribute(lxb_dom_interface_element(root),
        (const lxb_char_t *) "style", 5, (const lxb_char_t *) style,
        sizeof(style) - 1u);
    layout_reuse_cache_invalidate_inline_style(reuse, root, NULL,
                                               relational);
}

/* A custom property a rule declares on an element another element's
   change reaches (a sibling, a keyed subject) is read by that element's
   descendants: they restyle although its own computed style does not
   change. And a pseudo-element whose content reads a custom property is
   never remembered as generating nothing. */
static int test_custom_property_widened_reach(Budget *budget)
{
    int failed = 0;
    failed |= reach_case(budget, "sibling class",
        "<!doctype html><style>.on + .target{--tone:#500011}"
        ".leaf{color:var(--tone,#500012)}</style><body><div id=a>x</div>"
        "<div class=target><p class=leaf>leaf</p></div></body>",
        reach_set_class);
    failed |= reach_case(budget, "sibling attribute",
        "<!doctype html><style>[data-on] ~ .target{--tone:#500013}"
        ".leaf{color:var(--tone,#500014)}</style><body><div id=a>x</div>"
        "<p>gap</p><div class=target><p class=leaf>leaf</p></div></body>",
        reach_set_attribute);
    failed |= reach_case(budget, "descendant class",
        "<!doctype html><style>.on .target{--tone:#500015}"
        ".leaf{color:var(--tone,#500016)}</style><body><div id=a>"
        "<div class=target><p class=leaf>leaf</p></div></div></body>",
        reach_set_class);
    failed |= reach_case(budget, "structural sibling",
        "<!doctype html><style>.target:first-child{--tone:#500017}"
        ".leaf{color:var(--tone,#500018)}</style><body><section>"
        "<div class=target id=t><p class=leaf>leaf</p></div></section>"
        "</body>",
        reach_insert_before);
    failed |= reach_case(budget, "pseudo content var",
        "<!doctype html><html style=\"--c:none\"><style>"
        ".v::before{content:var(--c)}</style>"
        "<body><p class=v>text</p></body>",
        reach_root_inline);
    CHECK(failed == 0);
    return 0;
}

/* A :has() or :focus-within answer that sets a custom property on its
   anchor restyles the anchor's descendants that read it, although the
   anchor's own computed style does not change. */
static int test_custom_property_anchor_invalidation(Budget *budget)
{
    static const char html[] =
        "<!doctype html><style>"
        ".hs:has(.x){--p4:#500005}.v4{color:var(--p4,#500006)}"
        ".fw:focus-within{--p5:#500007}.v5{color:var(--p5,#500008)}"
        ".sf:focus-within{--p6:#500009}.v6{color:var(--p6,#50000a)}"
        "</style><body><div class=hs><div class=v4>t</div>"
        "<div id=target>u</div></div><div class=fw><p class=v5>v</p>"
        "<input id=field></div>"
        /* The focused element is the subject itself (a walk drop, not a
           keyed selection); its reader descendants must follow. */
        "<div class=sf id=self tabindex=0><p class=v6>w</p></div></body>";
    PocDocument document = {0};
    Stylesheet sheet = {0};
    ViewportContext viewport;
    CHECK(document_parse(&document, budget, html, sizeof(html) - 1u, 19)
        && viewport_context_init(&viewport, 480, 272, 480, 272)
        && stylesheet_build_context(&sheet, budget, &document, &viewport));
    LayoutReuseCache *reuse = layout_reuse_cache_create(budget);
    CHECK(reuse != NULL);
    layout_reuse_cache_enable_retained_matches(reuse);
    LayoutDocument retained = {0}, fresh = {0};
    CHECK(layout_build_context_reuse(&retained, budget, &document, &sheet,
        NULL, NULL, &viewport, reuse));
    layout_destroy(&retained);
    lxb_dom_node_t *root = lxb_dom_interface_node(document.html);
    lxb_dom_node_t *target = state_find_id(root, "target");
    lxb_dom_node_t *field = state_find_id(root, "field");
    lxb_dom_node_t *self = state_find_id(root, "self");
    CHECK(target != NULL && field != NULL && self != NULL);
    for (int step = 0; step < 3; step++) {
        if (step == 0) {
            uint32_t tokens[8];
            size_t token_count = 0;
            bool exact = stylesheet_attribute_change_tokens(
                "class", 5, "", 0, "x", 1, tokens, 8, &token_count);
            bool relational = stylesheet_attribute_change_may_affect_has(
                &sheet, "class", 5, "", 0, "x", 1);
            CHECK(lxb_dom_element_set_attribute(
                lxb_dom_interface_element(target),
                (const lxb_char_t *) "class", 5, (const lxb_char_t *) "x", 1)
                  != NULL);
            layout_reuse_cache_invalidate_attribute(
                reuse, target, exact ? tokens : NULL, token_count, false,
                relational);
        } else {
            lxb_dom_node_t *focused = step == 1 ? field : self;
            if (step == 2) {
                CHECK(lxb_dom_element_remove_attribute(
                    lxb_dom_interface_element(field),
                    (const lxb_char_t *) "data-tilefinch-focus", 20)
                      == LXB_STATUS_OK);
                layout_reuse_cache_invalidate_focus(reuse, field);
            }
            CHECK(lxb_dom_element_set_attribute(
                lxb_dom_interface_element(focused),
                (const lxb_char_t *) "data-tilefinch-focus", 20,
                (const lxb_char_t *) "", 0) != NULL);
            layout_reuse_cache_invalidate_focus(reuse, focused);
        }
        layout_reuse_cache_flush_invalidations(reuse);
        CHECK(layout_build_context_reuse(&retained, budget, &document, &sheet,
            NULL, NULL, &viewport, reuse));
        CHECK(layout_build_context(&fresh, budget, &document, &sheet,
            NULL, NULL, &viewport));
        if (!commands_equal(&retained, &fresh))
            printf("custom-property anchor step %d diverged\n", step);
        CHECK(commands_equal(&retained, &fresh));
        layout_destroy(&retained);
        layout_destroy(&fresh);
    }
    layout_reuse_cache_destroy(reuse);
    stylesheet_destroy(&sheet);
    document_destroy(&document);
    CHECK(budget->current == 0);
    return 0;
}

static int test_state_invalidation_matches_fresh(Budget *budget)
{
    const StateFuzzConfig configs[] = {
        {state_fuzz_css, 20260929u, 500, true},
        {state_fuzz_css, 77u, 500, true},
        {state_fuzz_css, 4242u, 500, true},
        {state_fuzz_reach_css, 5150u, 500, true},
        {state_fuzz_reach_css, 991u, 500, true},
    };
    for (size_t i = 0; i < sizeof(configs) / sizeof(configs[0]); i++)
        CHECK(state_fuzz_run(budget, &configs[i]) == 0);
    CHECK(test_custom_property_anchor_invalidation(budget) == 0);
    CHECK(test_custom_property_widened_reach(budget) == 0);
    return 0;
}

static int test_has_scoped_invalidation_matches_fresh(Budget *budget)
{
    static const char *const broad_classes[] = {
        "s1", "s2", "s3", "s4", "a5", "b5", "a6", "b6", "s7", "s8", "s9",
        "a10", "b10", "s11", "s12", "s13", "s14", "t16", "s17", "s18", "s19",
        "x", "y", "p", "q", "m",
    };
    /* Few classes, so anchors, arguments and their positions meet often. */
    static const char *const dense_classes[] = {
        "a", "b", "x", "y", "q", "e",
    };
    static const char dense_css[] =
        ".a:has(.x){color:#300001}.a:has(> .y:empty){color:#300002}"
        ".a:has(.q:first-child .x){color:#300003}.b:has(+ .x){color:#300004}"
        ".b:has(~ .y .x){color:#300005}.a:has(.x) .b{color:#300006}"
        ".b:has(.e:not(:empty)) .q{color:#300007}"
        ".a:has(> .q + .x){color:#300008}.q:has(~ .e:last-child){color:#300009}"
        ".a:has(.q + .x){color:#30000a}.a:has(.y:last-child){color:#30000b}"
        ".a:has(.q + .y .x){color:#30000c}"
        /* Sibling tests on the subject only, and an ancestor test. */
        ".x:first-child{color:#300013}.y + .q{color:#300014}"
        ".e:nth-last-child(2){color:#300015}.b ~ .e{color:#300017}"
        ".a .y{color:#300016}";
    /* Sibling tests before a descendant combinator, keyed. */
    static char deep_css[2048];
    snprintf(deep_css, sizeof(deep_css),
             "%s.q + .e .x{color:#30000f}.b:first-child .y{color:#300010}"
             ".e ~ .y > .q{color:#300012}", dense_css);
    /* An :empty outside :has(): a parent's children restyle its siblings. */
    static char empty_css[2048];
    snprintf(empty_css, sizeof(empty_css),
             "%s.e:empty + .q{color:#30000d}.y:empty ~ .x .b{color:#30000e}"
             ".y + * .b{color:#300011}",
             deep_css);
    /* Custom properties a :has() subject passes to its descendants, read
       beside and below the subjects (and anchors on the subject itself). */
    static char custom_css[2048];
    snprintf(custom_css, sizeof(custom_css),
             "%s.a:has(.x){--h1:#300021}.b:has(+ .y){--h2:#300022}"
             ".x:has(> .q){--h3:#300025}.q{color:var(--h1,#300023)}"
             ".e{background-color:var(--h2,#300024)}"
             ".y .e{border-color:var(--h3,#300026)}"
             ".y{color:var(--h3,var(--h1,#300027))}", dense_css);
    const HasFuzzConfig configs[] = {
        {custom_css, dense_classes,
         sizeof(dense_classes) / sizeof(dense_classes[0]), 424242u, 600,
         true},
        {has_fuzz_css, broad_classes,
         sizeof(broad_classes) / sizeof(broad_classes[0]), 20260928u, 400,
         true},
        {dense_css, dense_classes,
         sizeof(dense_classes) / sizeof(dense_classes[0]), 7u, 600, true},
        {deep_css, dense_classes,
         sizeof(dense_classes) / sizeof(dense_classes[0]), 1234567u, 600,
         true},
        {dense_css, dense_classes,
         sizeof(dense_classes) / sizeof(dense_classes[0]), 31337u, 600, true},
        {empty_css, dense_classes,
         sizeof(dense_classes) / sizeof(dense_classes[0]), 99u, 600, true},
    };
    for (size_t i = 0; i < sizeof(configs) / sizeof(configs[0]); i++)
        CHECK(has_fuzz_run(budget, &configs[i]) == 0);
    return 0;
}

/* A child leaving a parent restyles the parent's subtree, not its
   parent's: siblings of the parent keep their cached styles, unless an
   :empty test outside :has() lets the parent's children reach them. */
static int test_children_invalidation_scope(Budget *budget)
{
    static const char *const sheets[] = {
        ".box>span{color:#111111}p{color:#222222}",
        ".box>span{color:#111111}p{color:#222222}"
        ".box:empty + p{color:#abcdef}",
        /* The removed child was the predecessor a sibling test reads. */
        ".box>span{color:#111111}p{color:#222222}"
        ".box>.y + * b{color:#abcdef}",
    };
    for (size_t variant = 0; variant < 3; variant++) {
        static char html[8192];
        int written = snprintf(html, sizeof(html),
                               "<!doctype html><style>%s", sheets[variant]);
        CHECK(written > 0 && (size_t) written < sizeof(html));
        size_t length = (size_t) written;
        for (unsigned i = 0; i < 64; i++) {
            written = snprintf(html + length, sizeof(html) - length,
                               ".pad%u{margin:0}", i);
            length += (size_t) written;
        }
        written = snprintf(html + length, sizeof(html) - length,
            variant == 2
                ? "</style><body><div class=box id=box><span id=only class=y>"
                  "x</span><div><b>bold</b></div></div><p>after</p>"
                : "</style><body><div class=box id=box>"
                  "<span id=only>x</span></div><p>after</p>");
        length += (size_t) written;
        for (unsigned i = 0; i < 40; i++) {
            written = snprintf(html + length, sizeof(html) - length,
                               "<p>p%u</p>", i);
            length += (size_t) written;
        }
        PocDocument document = {0};
        Stylesheet sheet = {0};
        ViewportContext viewport;
        CHECK(document_parse(&document, budget, html, length, 19)
            && viewport_context_init(&viewport, 480, 272, 480, 272)
            && stylesheet_build_context(&sheet, budget, &document,
                                        &viewport));
        LayoutReuseCache *reuse = layout_reuse_cache_create(budget);
        CHECK(reuse != NULL);
        layout_reuse_cache_enable_retained_matches(reuse);
        LayoutDocument retained = {0}, fresh = {0};
        CHECK(layout_build_context_reuse(&retained, budget, &document, &sheet,
            NULL, NULL, &viewport, reuse));
        layout_destroy(&retained);
        lxb_dom_node_t *box = find_id(document_body_node(&document), "box");
        lxb_dom_node_t *only = find_id(document_body_node(&document), "only");
        CHECK(box != NULL && only != NULL);
        LayoutReuseStats before, after;
        layout_reuse_cache_stats(reuse, &before);
        if (variant == 2) {
            const DrawCommand *bold = text_command(&retained, "bold");
            CHECK(bold == NULL || bold->color == 0xabcdef);
        }
        layout_reuse_cache_retire_subtree(reuse, only);
        lxb_dom_node_remove(only);
        /* The journal names the removed child while it lives. */
        layout_reuse_cache_invalidate_children(reuse, box, only, false);
        BudgetAllocationOwner previous =
            document_allocation_owner_enter(&document);
        lxb_dom_node_destroy_deep(only);
        document_allocation_owner_leave(&document, previous);
        layout_reuse_cache_flush_invalidations(reuse);
        CHECK(layout_build_context_reuse(&retained, budget, &document, &sheet,
            NULL, NULL, &viewport, reuse));
        CHECK(layout_build_context(&fresh, budget, &document, &sheet,
            NULL, NULL, &viewport));
        CHECK(commands_equal(&retained, &fresh));
        const DrawCommand *command = text_command(&retained, "after");
        CHECK(command != NULL
              && command->color == (variant == 1 ? 0xabcdef : 0x222222));
        if (variant == 2) {
            command = text_command(&retained, "bold");
            CHECK(command != NULL && command->color != 0xabcdef);
        }
        layout_reuse_cache_stats(reuse, &after);
        /* Only the box restyles (with its children's subtrees when the
           removed child fed a sibling test), or with :empty everything
           under body. */
        size_t misses = after.style_misses - before.style_misses;
        if (variant == 1 ? misses < 40 : misses > 3) {
            printf("children scope variant %zu: %zu misses\n", variant,
                   misses);
            CHECK(false);
        }
        layout_destroy(&retained);
        layout_destroy(&fresh);
        layout_reuse_cache_destroy(reuse);
        stylesheet_destroy(&sheet);
        document_destroy(&document);
    }
    CHECK(budget->current == 0);
    return 0;
}

/* `.a:has(x) .b`: `x` appearing under `.a` changes `.b`, a descendant of
   the anchor rather than one of the changed node's ancestors. And a
   :has() rule no key or walk can place falls back to a counted wipe. */
static int test_has_descendant_subject_and_fallback(Budget *budget)
{
    static const char *const sheets[] = {
        ".a:has(.x) .b{color:#123456}",
        ".a:has(.x) ~ *{color:#123456}",
    };
    for (size_t variant = 0; variant < 2; variant++) {
        static char html[4096];
        int written = snprintf(html, sizeof(html),
                               "<!doctype html><style>%s", sheets[variant]);
        CHECK(written > 0 && (size_t) written < sizeof(html));
        size_t length = (size_t) written;
        for (unsigned i = 0; i < 64; i++) {
            written = snprintf(html + length, sizeof(html) - length,
                               ".pad%u{margin:0}", i);
            length += (size_t) written;
        }
        static const char rest[] =
            "</style><body><div class=a><div id=slot>slot</div>"
            "<p class=b>bee</p></div><p>after</p></body>";
        memcpy(html + length, rest, sizeof(rest) - 1u);
        length += sizeof(rest) - 1u;
        PocDocument document = {0};
        Stylesheet sheet = {0};
        ViewportContext viewport;
        CHECK(document_parse(&document, budget, html, length, 19)
            && viewport_context_init(&viewport, 480, 272, 480, 272)
            && stylesheet_build_context(&sheet, budget, &document,
                                        &viewport));
        LayoutReuseCache *reuse = layout_reuse_cache_create(budget);
        CHECK(reuse != NULL);
        layout_reuse_cache_enable_retained_matches(reuse);
        LayoutDocument retained = {0}, fresh = {0};
        CHECK(layout_build_context_reuse(&retained, budget, &document, &sheet,
            NULL, NULL, &viewport, reuse));
        const char *probe = variant == 0 ? "bee" : "after";
        const DrawCommand *command = text_command(&retained, probe);
        CHECK(command != NULL && command->color != 0x123456);
        layout_destroy(&retained);
        LayoutReuseStats before, after;
        layout_reuse_cache_stats(reuse, &before);
        lxb_dom_node_t *slot = find_id(document_body_node(&document), "slot");
        CHECK(slot != NULL);
        lxb_dom_node_t *x = insert_style_element(&document, slot, NULL, "");
        CHECK(x != NULL);
        /* A <style> is not rendered; make it an ordinary .x element. */
        CHECK(lxb_dom_element_set_attribute(
            lxb_dom_interface_element(x), (const lxb_char_t *) "class", 5,
            (const lxb_char_t *) "x", 1) != NULL);
        layout_reuse_cache_invalidate_structure(reuse, x);
        layout_reuse_cache_flush_invalidations(reuse);
        CHECK(layout_build_context_reuse(&retained, budget, &document, &sheet,
            NULL, NULL, &viewport, reuse));
        CHECK(layout_build_context(&fresh, budget, &document, &sheet,
            NULL, NULL, &viewport));
        command = text_command(&retained, probe);
        CHECK(command != NULL && command->color == 0x123456);
        CHECK(commands_equal(&retained, &fresh));
        layout_reuse_cache_stats(reuse, &after);
        if (variant == 0) {
            CHECK(after.full_resets == before.full_resets
                  && after.has_fallbacks == 0 && after.has_scoped == 1
                  && after.has_dropped >= 1 && after.has_scans == 1);
        } else {
            /* Keyless and off the ancestor chain: every computed style
               goes (the retained lists the rule can select are found by
               matching it), counted, without a full reset. */
            CHECK(after.full_resets == before.full_resets
                  && after.has_fallbacks == 1 && after.has_scoped == 0);
        }
        layout_destroy(&retained);
        layout_destroy(&fresh);
        layout_reuse_cache_destroy(reuse);
        stylesheet_destroy(&sheet);
        document_destroy(&document);
    }
    CHECK(budget->current == 0);
    return 0;
}

/* A <style> inserted into the head after load must yield the same styles as
   a sheet rebuilt from the final document: its rules take their document
   position (before the body's inline styles), earlier rule indices remap,
   and retained matched-rule lists survive except where the new rules can
   select. */
static int test_stylesheet_append_matches_rebuild(Budget *budget)
{
    static char html[8192];
    static const char head_css[] =
        "<!doctype html><style>p{color:#111111}.x{color:#222222 !important}"
        "p.q{font-weight:bold}";
    static const char rest[] =
        "</style><body><div id=host>"
        "<style>.y{color:#333333}p.z{--tone:#444444;color:var(--tone)}"
        ".flag:has(.inner) ~ .after{color:#abcdef}</style>"
        "<div class=flag id=flag><span class=inner></span></div>"
        "<p class=after>relational</p>"
        "<p class=x>one</p><p class=y>two</p><p class=z>three</p>"
        "<p class=w>four</p><p class=q>five</p><span class=w>six</span></div>";
    size_t length = sizeof(head_css) - 1u;
    memcpy(html, head_css, length);
    /* Enough rules for the retained matched-rule table to be allocated. */
    for (unsigned i = 0; i < 64; i++) {
        char rule[32];
        int written = snprintf(rule, sizeof(rule), ".pad%u{margin:0}", i);
        CHECK(written > 0 && (size_t) written < sizeof(html) - length);
        memcpy(html + length, rule, (size_t) written);
        length += (size_t) written;
    }
    CHECK(sizeof(rest) - 1u < sizeof(html) - length);
    memcpy(html + length, rest, sizeof(rest) - 1u);
    length += sizeof(rest) - 1u;
    static const char late_css[] =
        "p.z{--tone:#555555;color:var(--tone)}.w{color:#666666}.x{color:#777777}"
        "span.w{color:#888888}";
    PocDocument document = {0};
    Stylesheet appended = {0};
    ViewportContext viewport;
    CHECK(document_parse(&document, budget, html, length, 19)
        && viewport_context_init(&viewport, 480, 272, 480, 272)
        && stylesheet_build_context(&appended, budget, &document, &viewport));
    CHECK(stylesheet_style_source_count(&appended) == 2);
    /* Retained lists filled by a first layout. */
    LayoutReuseCache *reuse = layout_reuse_cache_create(budget);
    CHECK(reuse != NULL);
    layout_reuse_cache_enable_retained_matches(reuse);
    LayoutDocument before = {0};
    CHECK(layout_build_context_reuse(&before, budget, &document, &appended,
        NULL, NULL, &viewport, reuse));
    LayoutReuseStats stats;
    layout_reuse_cache_stats(reuse, &stats);
    CHECK(stats.matched_stores > 0);
    layout_destroy(&before);
    /* Snapshot the old selectors so the remap can be checked exactly. */
    size_t old_count = appended.count;
    char old_selectors[128][64];
    CHECK(old_count <= 128);
    for (size_t i = 0; i < old_count; i++) {
        snprintf(old_selectors[i], sizeof(old_selectors[i]), "%s",
                 appended.rules[i].selector);
    }
    lxb_dom_node_t *head = lxb_dom_interface_node(
        lxb_html_document_head_element(document.html));
    CHECK(head != NULL);
    lxb_dom_node_t *late = insert_style_element(&document, head, NULL, late_css);
    CHECK(late != NULL);
    StylesheetAppendResult result;
    CHECK(stylesheet_append_style_elements_tracked(
        &appended, &late, 1, &document.content_security_policy, 1, &result));
    CHECK(result.remap != NULL && !result.appended_bounded_out
        && result.appended_count == 4 && result.old_count == old_count
        && stylesheet_style_source_count(&appended) == 3
        && appended.style_source_nodes[1] == late);
    for (size_t i = 0; i < old_count; i++) {
        CHECK(result.remap[i] < appended.count
            && strcmp(appended.rules[result.remap[i]].selector,
                      old_selectors[i]) == 0);
    }
    layout_reuse_cache_note_stylesheet_appended(
        reuse, &appended, result.remap, result.old_count, result.appended,
        result.appended_count, true);
    /* The :has() plan follows the remapped rule ordinals. */
    StyleHasPlanSummary plan[4];
    bool plan_bounded = true;
    CHECK(style_has_plan_summarize(&appended, plan, 4, &plan_bounded) == 1
          && !plan_bounded
          && plan[0].relation == STYLE_HAS_RELATION_OTHER
          && plan[0].subject_keyed
          && strcmp(appended.rules[plan[0].rule].selector,
                    ".flag:has(.inner) ~ .after") == 0);
    stylesheet_append_result_release(&appended, &result);
    /* Reference: a sheet built from the final document, no reuse. */
    Stylesheet rebuilt = {0};
    CHECK(stylesheet_build_context(&rebuilt, budget, &document, &viewport));
    LayoutDocument reference = {0}, retained = {0}, fresh = {0};
    CHECK(layout_build_context(&reference, budget, &document, &rebuilt,
        NULL, NULL, &viewport));
    CHECK(layout_build_context_reuse(&retained, budget, &document, &appended,
        NULL, NULL, &viewport, reuse));
    CHECK(layout_build_context(&fresh, budget, &document, &appended,
        NULL, NULL, &viewport));
    const DrawCommand *one = text_command(&reference, "one");
    const DrawCommand *three = text_command(&reference, "three");
    const DrawCommand *four = text_command(&reference, "four");
    const DrawCommand *six = text_command(&reference, "six");
    CHECK(one != NULL && one->color == 0x222222
        && three != NULL && three->color == 0x444444
        && four != NULL && four->color == 0x666666
        && six != NULL && six->color == 0x888888);
    CHECK(commands_equal(&fresh, &reference));
    CHECK(commands_equal(&retained, &reference));
    layout_reuse_cache_stats(reuse, &stats);
    CHECK(stats.stylesheet_appends == 1 && stats.full_resets == 0);
    layout_destroy(&reference);
    layout_destroy(&retained);
    layout_destroy(&fresh);
    /* The inserted head sheet moves the body's :has() rule ordinal. Losing
       its match must invalidate the remapped rule, not an unrelated rule. */
    lxb_dom_node_t *flag = find_id(document_body_node(&document), "flag");
    CHECK(flag != NULL && flag->first_child != NULL);
    lxb_dom_node_t *inner = flag->first_child;
    lxb_dom_node_remove(inner);
    layout_reuse_cache_invalidate_structure(reuse, flag);
    CHECK(layout_build_context_reuse(&retained, budget, &document, &appended,
        NULL, NULL, &viewport, reuse));
    const DrawCommand *relational = text_command(&retained, "relational");
    CHECK(relational != NULL && relational->color == 0x111111);
    layout_destroy(&retained);
    lxb_dom_node_destroy_deep(inner);
    /* A second append at the very end (after the body style) as well. */
    lxb_dom_node_t *body = document_body_node(&document);
    lxb_dom_node_t *tail = insert_style_element(
        &document, body, NULL, "p.z{color:#999999}");
    CHECK(tail != NULL && stylesheet_append_style_elements_tracked(
        &appended, &tail, 1, &document.content_security_policy, 3, &result));
    layout_reuse_cache_note_stylesheet_appended(
        reuse, &appended, result.remap, result.old_count, result.appended,
        result.appended_count, result.remap != NULL);
    stylesheet_append_result_release(&appended, &result);
    stylesheet_destroy(&rebuilt);
    CHECK(stylesheet_build_context(&rebuilt, budget, &document, &viewport));
    CHECK(layout_build_context(&reference, budget, &document, &rebuilt,
        NULL, NULL, &viewport));
    CHECK(layout_build_context_reuse(&retained, budget, &document, &appended,
        NULL, NULL, &viewport, reuse));
    three = text_command(&reference, "three");
    CHECK(three != NULL && three->color == 0x999999);
    CHECK(commands_equal(&retained, &reference));
    layout_destroy(&reference);
    layout_destroy(&retained);
    layout_reuse_cache_destroy(reuse);
    stylesheet_destroy(&rebuilt);
    stylesheet_destroy(&appended);
    document_destroy(&document);
    CHECK(budget->current == 0);
    return 0;
}
