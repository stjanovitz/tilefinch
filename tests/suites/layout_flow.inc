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
          && em_same_style.padding.top == 18
          && sizeof(ComputedStyle) == 376);
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
        "#clamp{width:44px;line-height:10px;overflow:hidden;"
        "-webkit-line-clamp:2}#clamp{text-overflow:ellipsis}"
        "#hidden{content-visibility:hidden;width:40px;height:20px;"
        "background:#123456}#auto{content-visibility:auto}"
        "</style><body>"
        "<div id=min class=probe>alpha beta</div>"
        "<div id=max class=probe>alpha beta gamma delta</div>"
        "<div id=fit class=probe>alpha beta gamma delta</div>"
        "<div id=guard class=probe>unbreakable</div>"
        "<div id=flex><span id=basis-min>alpha beta</span>"
        "<span id=basis-max>alpha beta</span></div>"
        "<div id=clamp class=probe>one two three four five six seven</div>"
        "<div id=hidden><span>HIDDEN-CONTENT</span></div>"
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
    ComputedStyle min_style = style_for_node(&sheet, min, NULL);
    ComputedStyle max_style = style_for_node(&sheet, max, NULL);
    ComputedStyle fit_style = style_for_node(&sheet, fit, NULL);
    ComputedStyle clamp_style = style_for_node(&sheet, clamp, NULL);
    ComputedStyle basis_min_style = style_for_node(&sheet, basis_min, NULL);
    ComputedStyle basis_max_style = style_for_node(&sheet, basis_max, NULL);
    ComputedStyle hidden_style = style_for_node(&sheet, hidden, NULL);
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
          && hidden_box->width == 40 && hidden_box->height == 20);
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
    CHECK(flex_clip != NULL && flex_hidden != NULL
          && grid_zero != NULL && grid_hidden != NULL && grid_clip != NULL
          && outer != NULL && inner_clip != NULL
          && compact_wrapper != NULL && compact_select != NULL);
    if (!(flex_clip->width > 60 && flex_hidden->width <= 60
          && grid_zero->width <= 60 && grid_hidden->width <= 60
          && grid_clip->width > 60
          && outer->content_width == outer->client_width
          && inner_clip->content_width > inner_clip->client_width
          && compact_select->width >= 64
          && compact_wrapper->width >= compact_select->width)) {
        fprintf(stderr,
                "mobile minima flex=%d/%d grid=%d/%d/%d "
                "overflow=%d/%d inner=%d/%d compact=%d/%d\n",
                flex_clip->width, flex_hidden->width,
                grid_zero->width, grid_hidden->width, grid_clip->width,
                outer->content_width, outer->client_width,
                inner_clip->content_width, inner_clip->client_width,
                compact_wrapper->width, compact_select->width);
    }
    CHECK(flex_clip->width > 60 && flex_hidden->width <= 60
          && grid_zero->width <= 60 && grid_hidden->width <= 60
          && grid_clip->width > 60
          && outer->content_width == outer->client_width
          && inner_clip->content_width > inner_clip->client_width
          && compact_select->width >= 64
          && compact_wrapper->width >= compact_select->width);
    layout_destroy(&layout);
    stylesheet_destroy(&sheet);
    document_destroy(&document);
    CHECK(budget->current == 0);
    return 0;
}
