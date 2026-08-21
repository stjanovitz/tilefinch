            && memcmp(responsive_layout.commands[i].text, "ITALIC", 6) == 0
            && responsive_layout.commands[i].font_italic) found_italic = true;
        if (responsive_layout.commands[i].type == DRAW_TEXT
            && responsive_layout.commands[i].text_length == 3
            && memcmp(responsive_layout.commands[i].text, "ADJ", 3) == 0) {
            adjacent_left = &responsive_layout.commands[i];
        }
        if (responsive_layout.commands[i].type == DRAW_TEXT
            && responsive_layout.commands[i].text_length == 5
            && memcmp(responsive_layout.commands[i].text, "ACENT", 5) == 0) {
            adjacent_right = &responsive_layout.commands[i];
        }
        if (responsive_layout.commands[i].type == DRAW_TEXT
            && responsive_layout.commands[i].text_length == 3
            && memcmp(responsive_layout.commands[i].text, "KEY", 3) == 0) {
            table_key = &responsive_layout.commands[i];
        }
        if (responsive_layout.commands[i].type == DRAW_TEXT
            && responsive_layout.commands[i].text_length == 5
            && memcmp(responsive_layout.commands[i].text, "VALUE", 5) == 0) {
            table_value = &responsive_layout.commands[i];
        }
        if (responsive_layout.commands[i].type == DRAW_TEXT
            && responsive_layout.commands[i].text_length == 4
            && memcmp(responsive_layout.commands[i].text, "BASE", 4) == 0) {
            baseline = &responsive_layout.commands[i];
        }
        if (responsive_layout.commands[i].type == DRAW_TEXT
            && responsive_layout.commands[i].text_length == 1
            && responsive_layout.commands[i].text[0] == '2') {
            superscript = &responsive_layout.commands[i];
        }
    }
    CHECK(found_italic && responsive_layout.sticky_count == 1
          && responsive_layout.sticky_ranges[0].top == 12
          && responsive_layout.sticky_ranges[0].command_end
             > responsive_layout.sticky_ranges[0].command_start);
    CHECK(responsive_layout.fixed_count == 1
          && responsive_layout.fixed_ranges[0].inset == 20
          && !responsive_layout.fixed_ranges[0].from_bottom);
    CHECK(adjacent_left != NULL && adjacent_right != NULL
          && adjacent_right->x == adjacent_left->x + adjacent_left->width);
    CHECK(table_key != NULL && table_value != NULL
          && table_value->x > table_key->x + table_key->width
          && table_value->y == table_key->y);
    CHECK(baseline != NULL && superscript != NULL
          && superscript->y + superscript->height * 3 / 4
             < baseline->y + baseline->height * 3 / 4);
    const StickyRange *sticky_range = &responsive_layout.sticky_ranges[0];
    const DrawCommand *sticky_fill = NULL;
    for (size_t i = sticky_range->command_start;
         i < sticky_range->command_end; i++) {
        if (responsive_layout.commands[i].type == DRAW_FILL_RECT
            && responsive_layout.commands[i].color == 0x445566) {
            sticky_fill = &responsive_layout.commands[i];
            break;
        }
    }
    CHECK(sticky_fill != NULL);
    TileCache sticky_cache;
    uint16_t *sticky_frame = budget_malloc(
        &budget, 480u * 272u * sizeof(*sticky_frame));
    CHECK(sticky_frame != NULL
          && tile_cache_init(&sticky_cache, &budget, &responsive_layout, 4)
          && tile_cache_set_frame(&sticky_cache, sticky_frame, 480u * 272u));
    int sticky_scroll = sticky_range->origin_y - sticky_range->top + 20;
    int sticky_max_scroll = responsive_layout.height - 272;
    if (sticky_scroll > sticky_max_scroll) sticky_scroll = sticky_max_scroll;
    CHECK(tile_cache_render_frame(&sticky_cache, sticky_scroll, 480, 272,
                                  NULL));
    int sticky_screen_y = sticky_fill->y - sticky_range->origin_y
                          + sticky_range->top;
    CHECK(sticky_screen_y >= 0 && sticky_screen_y < 272
          && sticky_fill->x >= 0 && sticky_fill->x < 480
          && sticky_frame[(size_t) sticky_screen_y * 480
                          + sticky_fill->x] == test_rgb565(0x445566));
    tile_cache_destroy(&sticky_cache);
    budget_free(&budget, sticky_frame);
    const FixedRange *fixed_range = &responsive_layout.fixed_ranges[0];
    const DrawCommand *fixed_fill = NULL;
    const DrawCommand *rounded_fill = NULL;
    for (size_t i = 0; i < responsive_layout.count; i++) {
        const DrawCommand *command = &responsive_layout.commands[i];
        if (i >= fixed_range->command_start && i < fixed_range->command_end
            && command->type == DRAW_FILL_RECT
            && command->color == 0x778899) fixed_fill = command;
        if (command->type == DRAW_FILL_RECT && command->color == 0x112233) {
            rounded_fill = command;
        }
    }
    CHECK(fixed_fill != NULL && rounded_fill != NULL
          && rounded_fill->radius == 8);
    TileCache fixed_cache;
    uint16_t *fixed_frame = budget_malloc(
        &budget, 480u * 272u * sizeof(*fixed_frame));
    CHECK(fixed_frame != NULL
          && tile_cache_init(&fixed_cache, &budget, &responsive_layout, 4)
          && tile_cache_set_frame(&fixed_cache, fixed_frame, 480u * 272u)
          && tile_cache_render_frame(&fixed_cache, 0, 480, 272, NULL));
    int fixed_x = fixed_fill->x + fixed_fill->width / 2;
    CHECK(fixed_frame[(size_t) 20 * 480 + fixed_x]
          == test_rgb565(0x778899));
    CHECK(tile_cache_render_frame(&fixed_cache, 136, 480, 272, NULL));
    CHECK(fixed_frame[(size_t) 20 * 480 + fixed_x]
          == test_rgb565(0x778899));
    CHECK(fixed_cache.fixed_cache_builds == 1
          && fixed_cache.fixed_cache_blits == 2
          && fixed_cache.fixed_cache_bytes > 0);
    int background_scroll = background_image_command->y - 40;
    if (background_scroll < 0) background_scroll = 0;
    if (background_scroll > responsive_layout.height - 272) {
        background_scroll = responsive_layout.height - 272;
    }
    CHECK(tile_cache_render_frame(&fixed_cache, background_scroll, 480, 272,
                                  NULL));
    int background_sample_x = background_image_command->x
                              + background_image_command->width / 2;
    int background_sample_y = background_image_command->y - background_scroll
                              + background_image_command->height / 2;
    CHECK(background_sample_x >= 0 && background_sample_x < 480
          && background_sample_y >= 20 && background_sample_y < 272
          && fixed_frame[(size_t) background_sample_y * 480
                         + background_sample_x] == test_rgb565(0x00ff00));
    tile_cache_destroy(&fixed_cache);
    budget_free(&budget, fixed_frame);

    puts("test: fixed percentage width uses viewport containing block");
    static const char fixed_percent_page[] =
        "<!doctype html><style>html,body{margin:0;padding:0}"
        "#host{display:flex;align-items:center;justify-content:center;"
        "width:480px}"
        "#narrow{width:8px}#bar{position:fixed;left:0;bottom:0;"
        "width:100%;height:20px;background:#2468ac}"
        "</style><body><div id=host><div id=narrow>"
        "<div id=bar></div></div></div></body>";
    PocDocument fixed_percent_document;
    Stylesheet fixed_percent_sheet;
    LayoutDocument fixed_percent_layout;
    CHECK(document_parse(&fixed_percent_document, &budget,
                         fixed_percent_page,
                         strlen(fixed_percent_page), 43)
          && stylesheet_build(&fixed_percent_sheet, &budget,
                              &fixed_percent_document, 480)
          && layout_build(&fixed_percent_layout, &budget,
                          &fixed_percent_document,
                          &fixed_percent_sheet, NULL, NULL, 480));
    const LayoutNodeBox *fixed_percent_box = layout_box_for_node(
        &fixed_percent_layout,
        find_id(lxb_dom_interface_node(fixed_percent_document.html),
                "bar"));
    CHECK(fixed_percent_box != NULL && fixed_percent_box->x == 0
          && fixed_percent_box->width == 480
          && fixed_percent_layout.fixed_count == 1);
    layout_destroy(&fixed_percent_layout);
    stylesheet_destroy(&fixed_percent_sheet);
    document_destroy(&fixed_percent_document);

    bool found_image = false, found_mask = false, found_pseudo = false;
    bool found_pseudo_mask = false;
    for (size_t i = 0; i < responsive_layout.count; i++) {
        if (responsive_layout.commands[i].type == DRAW_IMAGE
            && responsive_layout.commands[i].image == &image_items[0]
            && responsive_layout.commands[i].width == 4
            && responsive_layout.commands[i].height == 2
            && responsive_layout.commands[i].image_fit
               == LAYOUT_IMAGE_FIT_COVER) {
            found_image = true;
        }
        if (responsive_layout.commands[i].type == DRAW_IMAGE
            && responsive_layout.commands[i].image == &image_items[1]
            && responsive_layout.commands[i].color == 0x123456
            && responsive_layout.commands[i].width == 20
            && responsive_layout.commands[i].height == 20) found_mask = true;
        if (responsive_layout.commands[i].type == DRAW_FILL_RECT
            && responsive_layout.commands[i].color == 0xff0000
            && responsive_layout.commands[i].height == 1) found_pseudo = true;
        if (responsive_layout.commands[i].type == DRAW_IMAGE
            && responsive_layout.commands[i].image == &image_items[2]
            && responsive_layout.commands[i].width == 4
            && responsive_layout.commands[i].height == 4
            && responsive_layout.commands[i].color == 0x00ff00) {
            found_pseudo_mask = true;
        }
    }
    CHECK(found_image && found_mask && found_pseudo && found_pseudo_mask);
    layout_destroy(&responsive_layout);
    stylesheet_destroy(&mobile_sheet);

    Stylesheet desktop_sheet;
    CHECK(stylesheet_build(&desktop_sheet, &budget, &responsive, 800));
    ComputedStyle desktop_cards = style_for_node(&desktop_sheet, cards, NULL);
    CHECK(desktop_cards.display == DISPLAY_NONE);
    stylesheet_destroy(&desktop_sheet);
    document_destroy(&responsive);
    CHECK(budget.current == 0);

    puts("test: bounded stylesheet capability diagnostics");
    static const char diagnostic_page[] =
        "<!doctype html><style>"
        "#diagnostic-probe{color:#123456;unsupported-visual:1;"
        "text-decoration:underline;"
        "--this-custom-property-name-is-deliberately-longer-than-forty-eight-characters:red}"
        "@media (dynamic-range:high){#diagnostic-probe{display:none}}"
        "@supports (display:subgrid){#diagnostic-probe{display:none}}"
        "</style><body><div id=diagnostic-probe>DIAGNOSTIC</div></body>";
    PocDocument diagnostic_document;
    Stylesheet diagnostic_stylesheet;
    CHECK(document_parse(&diagnostic_document, &budget, diagnostic_page,
                         strlen(diagnostic_page), 17)
          && stylesheet_build(&diagnostic_stylesheet, &budget,
                              &diagnostic_document, 480));
    lxb_dom_node_t *diagnostic_probe = find_id(
        lxb_dom_interface_node(diagnostic_document.html),
        "diagnostic-probe");
    CHECK(diagnostic_probe != NULL);
    ComputedStyle diagnostic_style = style_for_node(
        &diagnostic_stylesheet, diagnostic_probe, NULL);
    CHECK(diagnostic_style.display == DISPLAY_BLOCK
          && diagnostic_style.color == 0x123456
          && computed_style_has_text_underline(&diagnostic_style)
          && diagnostic_stylesheet.diagnostic_declarations >= 3
          && diagnostic_stylesheet.diagnostic_supported_declarations >= 1
          && diagnostic_stylesheet.diagnostic_rejected_declarations >= 1
          && diagnostic_stylesheet.diagnostic_custom_property_drops >= 1
          && diagnostic_stylesheet.diagnostic_unknown_media_features >= 1
          && diagnostic_stylesheet.diagnostic_supports_false_queries >= 1
          && diagnostic_stylesheet.diagnostic_rejected_property_count >= 1);
    bool found_unsupported_visual = false;
    bool found_text_decoration = false;
    for (size_t i = 0;
         i < diagnostic_stylesheet.diagnostic_rejected_property_count; i++) {
        const StyleDiagnosticProperty *entry =
            &diagnostic_stylesheet.diagnostic_rejected_properties[i];
        if (strcmp(entry->name, "unsupported-visual") == 0) {
            found_unsupported_visual = true;
        } else if (strcmp(entry->name, "text-decoration") == 0) {
            found_text_decoration = true;
        }
    }
    CHECK(found_unsupported_visual && !found_text_decoration);
    stylesheet_destroy(&diagnostic_stylesheet);
    document_destroy(&diagnostic_document);
    CHECK(budget.current == 0);

    puts("test: display contents and anonymous flex items");
    static const char contents_page[] =
        "<!doctype html><style>body{margin:0;padding:0}"
        ".contents{display:contents;color:#2468ac;padding:40px}"
        ".nested{display:contents;letter-spacing:2px}"
        ".normal{display:block;width:120px}.normal-block{display:block;width:60px}"
        ".flex{display:flex;width:100px;gap:4px}"
        ".flex .item{display:block;flex:1 1 0;height:10px}"
        ".anonymous{display:flex;width:180px;justify-content:space-between}"
        ".grid-contents{display:grid;grid-template-columns:repeat(3,1fr);gap:2px;width:101px}"
        ".grid-contents .cell{display:block;height:5px}"
        ".column-contents{display:flex;flex-direction:column;width:40px;height:100px;justify-content:space-between}"
        ".column-contents .column-item{display:block;height:10px}"
        ".atomic-line{display:block;width:460px}.inline-block{display:inline-block;width:40px;height:14px}"
        ".inline-flex{display:inline-flex;width:60px;height:14px;gap:4px}"
        ".inline-flex>b{display:block;flex:1 1 0}"
        ".inline-grid{display:inline-grid;width:62px;grid-template-columns:repeat(2,1fr);gap:2px}"
        ".inline-grid>b{display:block;height:14px}"
        ".atomic-button{display:inline-flex;width:60px;height:20px}"
        ".auto-cross{display:flex;width:120px;align-items:center}.auto-cross>span{display:block;width:30px}"
        ".auto-cross .tall{height:30px}.auto-cross .short{height:10px}"
        ".auto-cross .self-start{height:10px;align-self:flex-start}"
        ".auto-cross .self-end{height:10px;--cross:end;align-self:var(--cross)}"
        ".self-column{display:flex;flex-direction:column;width:100px;align-items:center}"
        ".self-column>span{display:block;width:20px;height:10px}"
        ".self-column .self-column-start{align-self:start}"
        ".self-column .self-column-end{align-self:end}"
        ".fit-column{display:flex;flex-direction:column;width:120px;align-items:flex-start}"
        ".fit-column>span{display:block}.fit-column b{display:block;width:60px;height:10px}"
        ".fit-column-center{align-items:center}.fit-column-end{align-items:flex-end}"
        ".content-lines{display:flex;flex-wrap:wrap;width:84px;height:100px;gap:4px;--pack:space-between;align-content:var(--pack)}"
        ".content-lines>span{display:block;flex:0 0 40px;height:10px}"
        ".stretch-content{display:flex;flex-wrap:wrap;width:84px;height:100px;gap:4px;align-content:stretch;align-items:center}"
        ".stretch-content>span{display:block;flex:0 0 40px;height:10px}"
        ".single-content{display:flex;flex-wrap:wrap;width:84px;height:50px;align-content:flex-end}"
        ".single-content>span{display:block;width:40px;height:10px}"
        ".reverse-content{display:flex;--flow:wrap-reverse;flex-wrap:var(--flow);width:84px;height:50px;gap:4px;align-content:flex-start;align-items:flex-start}"
        ".reverse-content>span{display:block;flex:0 0 40px;height:10px}"
        ".overflow-center{display:flex;flex-wrap:wrap;width:84px;height:20px;gap:4px;align-content:center}"
        ".overflow-center>span{display:block;flex:0 0 40px;height:10px}"
        ".stretch-row{display:flex;width:100px;height:40px}"
        ".stretch-link{display:block;width:40px;background:#123456}"
        ".stretch-fixed{display:block;width:40px;height:10px;align-self:stretch;background:#654321}"
        ".auto-margin-row{display:flex;width:90px;height:40px;align-items:flex-end}"
        ".auto-margin-row>span{display:block;width:20px;height:10px}"
        ".auto-margin-top{margin-top:auto}.auto-margin-bottom{margin-bottom:auto}"
        ".auto-margin-both{margin-block:auto}"
        ".auto-margin-column{display:flex;flex-direction:column;width:100px;align-items:flex-start}"
        ".auto-margin-column>span{display:block;width:20px;height:10px}"
        ".auto-margin-left{margin-left:auto}.auto-margin-right{margin-right:auto}"
        ".auto-margin-inline{margin-inline:auto}"
        ".main-auto-row{display:flex;width:100px}.main-auto-row>span{display:block;width:20px;height:10px}"
        ".main-auto-row .main-row-end{margin-left:auto}"
        ".main-auto-column{display:flex;flex-direction:column;width:20px;height:100px}"
        ".main-auto-column>span{display:block;width:20px;height:10px}"
        ".main-auto-column .main-column-end{margin-top:auto}"
        ".main-auto-center{display:flex;flex-direction:column;width:20px;height:100px}"
        ".main-auto-center>span{display:block;width:20px;height:10px;margin-block:auto}"
        ".main-auto-wrap{display:flex;flex-wrap:wrap;width:100px}"
        ".main-auto-wrap>span{display:block;flex:0 0 40px;height:10px}"
        ".main-auto-wrap .main-wrap-end{margin-left:auto}"
        ".reverse-row{display:flex;flex-direction:row-reverse;width:100px}"
        ".reverse-row>span,.reverse-row-end>span,.reverse-row-auto>span{display:block;width:20px;height:10px}"
        ".reverse-row-end{display:flex;flex-direction:row-reverse;justify-content:flex-end;width:100px}"
        ".reverse-row-auto{display:flex;flex-direction:row-reverse;width:100px}"
        ".reverse-row-auto .reverse-auto-first{margin-left:auto}"
        ".reverse-wrap{display:flex;flex-direction:row-reverse;flex-wrap:wrap;width:100px}"
        ".reverse-wrap>span{display:block;flex:0 0 40px;height:10px}"
        ".reverse-column{display:flex;flex-direction:column-reverse;width:20px;height:100px}"
        ".reverse-column>span{display:block;width:20px;height:10px}"
        ".baseline-row{display:flex;width:160px;align-items:baseline}"
        ".baseline-row>span,.baseline-row>a{display:block;width:70px}"
        ".baseline-large{font-size:24px}.baseline-small{font-size:12px}"
        ".baseline-wrap{display:flex;flex-wrap:wrap;width:84px;gap:4px;align-items:baseline}"
        ".baseline-wrap>span{display:block;flex:0 0 40px}"
        ".order-row{display:flex;width:200px}.order-row>span{display:block;width:40px;height:10px}"
        ".order-neg{order:-1}.order-zero{order:0}.order-two{order:2}"
        ".order-wrap{display:flex;flex-wrap:wrap;width:84px;gap:4px}"
        ".order-wrap>span{display:block;flex:0 0 40px;height:10px}"
        ".order-reverse{display:flex;flex-direction:row-reverse;width:200px}"
        ".order-reverse>span{display:block;width:40px;height:10px}"
        ".order-column{display:flex;flex-direction:column;width:40px}"
        ".order-column>span{display:block;width:40px;height:10px}"
        ".order-many{display:flex;width:180px}.order-many>span{display:block;width:30px;height:10px}"
        "</style><body>"
        "<div id=normal-host class=normal><div id=normal-shell class=contents>"
        "<span id=normal-inline>NORMALINLINE</span><a id=normal-link class=contents href=/normal>NORMALLINK</a>"
        "<div id=normal-block class=normal-block>NORMALBLOCK</div>"
        "</div></div>"
        "<div id=flex-host class=flex><div id=flex-shell class=contents>"
        "<div id=flex-a class=item>FA</div><div id=flex-nested class=nested>"
        "<div id=flex-b class=item>FB</div></div></div></div>"
        "<div id=anonymous-host class=anonymous>ALPHA<span id=anonymous-middle>MIDDLE</span>OMEGA</div>"
        "<div id=link-host class=flex><a id=contents-link class=contents href=/contents>LINKTEXT</a>"
        "<span class=item>LINKTAIL</span></div>"
        "<div id=grid-host class=grid-contents><div id=grid-shell class=contents>"
        "<span id=contents-grid-a class=cell>CA</span><span id=contents-grid-b class=cell>CB</span>"
        "<span id=contents-grid-c class=cell>CC</span></div></div>"
        "<div id=column-host class=column-contents><div id=column-shell class=contents>"
        "<span id=contents-column-a class=column-item>CTA</span>"
        "<span id=contents-column-b class=column-item>CTB</span></div></div>"
        "<div id=atomic-line class=atomic-line>PRE<span id=atomic-block class=inline-block>IB</span>"
        "<span id=atomic-flex class=inline-flex><b id=atomic-flex-a>A</b><b id=atomic-flex-b>B</b></span>"
        "<span id=atomic-grid class=inline-grid><b id=atomic-grid-a>IGA</b><b id=atomic-grid-b>IGB</b></span>"
        "<a id=atomic-link class=inline-block href=/atomic>ATLINK</a>POST</div>"
        "<button id=atomic-button class=atomic-button><span>ATBUTTON</span></button>"
        "<div id=auto-cross class=auto-cross><span id=auto-tall class=tall>TALL</span>"
        "<span id=auto-short class=short><a id=auto-link href=/auto>SHORT</a></span>"
        "<span id=auto-self-start class=self-start>START</span>"
        "<span id=auto-self-end class=self-end>END</span></div>"
        "<div id=self-column class=self-column><span id=self-column-start class=self-column-start>CS</span>"
        "<span id=self-column-end class=self-column-end>CE</span></div>"
        "<div id=fit-column-start class=fit-column><span id=fit-column-start-item><b>FS</b></span></div>"
        "<div id=fit-column-center class=\"fit-column fit-column-center\"><span id=fit-column-center-item><b>FC</b></span></div>"
        "<div id=fit-column-end class=\"fit-column fit-column-end\"><span id=fit-column-end-item><b>FE</b></span></div>"
        "<div id=content-lines class=content-lines><span id=content-line-a>LA</span>"
        "<span id=content-line-b>LB</span><span id=content-line-c>LC</span>"
        "<span id=content-line-d>LD</span></div>"
        "<div id=stretch-content class=stretch-content><span id=stretch-content-a>SA</span>"
        "<span>SB</span><span id=stretch-content-c>SC</span><span>SD</span></div>"
        "<div id=single-content class=single-content><span id=single-content-item>ONE</span></div>"
        "<div id=reverse-content class=reverse-content><span id=reverse-content-a>XA</span>"
        "<span>XB</span><span id=reverse-content-c>XC</span><span>XD</span></div>"
        "<div id=overflow-center class=overflow-center><span id=overflow-center-a>OCA</span>"
        "<span>OCB</span><span id=overflow-center-c>OCC</span><span>OCD</span></div>"
        "<div id=stretch-row class=stretch-row><a id=stretch-link class=stretch-link href=/stretch>STRETCH</a>"
        "<span id=stretch-fixed class=stretch-fixed>FIXED</span></div>"
        "<div id=auto-margin-row class=auto-margin-row>"
        "<span id=auto-margin-top class=auto-margin-top>MT</span>"
        "<span id=auto-margin-bottom class=auto-margin-bottom>MB</span>"
        "<span id=auto-margin-both class=auto-margin-both>MX</span></div>"
        "<div id=auto-margin-column class=auto-margin-column>"
        "<span id=auto-margin-left class=auto-margin-left>ML</span>"
        "<span id=auto-margin-right class=auto-margin-right>MR</span>"
        "<span id=auto-margin-inline class=auto-margin-inline>MI</span></div>"
        "<div id=main-auto-row class=main-auto-row><span id=main-row-start>A</span>"
        "<span id=main-row-end class=main-row-end>B</span></div>"
        "<div id=main-auto-column class=main-auto-column><span id=main-column-start>C</span>"
        "<span id=main-column-end class=main-column-end>D</span></div>"
        "<div id=main-auto-center class=main-auto-center><span id=main-center>E</span></div>"
        "<div id=main-auto-wrap class=main-auto-wrap><span id=main-wrap-start>F</span>"
        "<span id=main-wrap-end class=main-wrap-end>G</span><span id=main-wrap-next>H</span></div>"
        "<div id=reverse-row class=reverse-row><span id=reverse-row-a>I</span><span id=reverse-row-b>J</span></div>"
        "<div id=reverse-row-end class=reverse-row-end><span id=reverse-end-a>K</span><span id=reverse-end-b>L</span></div>"
        "<div id=reverse-row-auto class=reverse-row-auto><span id=reverse-auto-first class=reverse-auto-first>M</span>"
        "<span id=reverse-auto-second>N</span></div>"
        "<div id=reverse-wrap class=reverse-wrap><span id=reverse-wrap-a>O</span>"
        "<span id=reverse-wrap-b>P</span><span id=reverse-wrap-c>Q</span></div>"
        "<div id=reverse-column class=reverse-column><span id=reverse-column-a>R</span>"
        "<span id=reverse-column-b>S</span></div>"
        "<div id=baseline-row class=baseline-row><span id=baseline-large class=baseline-large>BASELARGE</span>"
        "<a id=baseline-small class=baseline-small href=/baseline>BASESMALL</a></div>"
        "<div id=baseline-wrap class=baseline-wrap><span class=baseline-large>WRAPLARGE1</span>"
        "<span class=baseline-small>WRAPSMALL1</span><span class=baseline-small>WRAPSMALL2</span>"
        "<span class=baseline-large>WRAPLARGE2</span></div>"
        "<div id=order-row class=order-row><span class=order-two>OA</span><span class=order-neg>OB</span>"
        "<span class=order-two>OC</span><span class=order-zero>OD</span></div>"
        "<div id=order-wrap class=order-wrap><span class=order-two>WA</span><span class=order-neg>WB</span>"
        "<span class=order-two>WC</span><span class=order-zero>WD</span></div>"
        "<div id=order-reverse class=order-reverse><span class=order-two>RAA</span><span class=order-neg>RBB</span>"
        "<span class=order-two>RCC</span><span class=order-zero>RDD</span></div>"
        "<div id=order-column class=order-column><span class=order-two>CAO</span><span class=order-neg>CBO</span>"
        "<span class=order-two>CCO</span><span class=order-zero>CDO</span></div>"
        "<div id=order-many class=order-many><span style=order:5>M5</span><span style=order:1>M1</span>"
        "<span style=order:3>M3</span><span style=order:2>M2</span><span style=order:4>M4</span>"
        "<span style=order:0>M0</span></div>"
        "</body>";
    PocDocument contents_document;
    CHECK(document_parse(&contents_document, &budget, contents_page,
                         strlen(contents_page), 73));
    lxb_dom_node_t *contents_root = lxb_dom_interface_node(
        contents_document.html);
    lxb_dom_node_t *normal_host = find_id(contents_root, "normal-host");
    lxb_dom_node_t *normal_shell = find_id(contents_root, "normal-shell");
    lxb_dom_node_t *normal_link = find_id(contents_root, "normal-link");
    lxb_dom_node_t *normal_block = find_id(contents_root, "normal-block");
    lxb_dom_node_t *flex_shell = find_id(contents_root, "flex-shell");
    lxb_dom_node_t *flex_nested = find_id(contents_root, "flex-nested");
    lxb_dom_node_t *flex_a = find_id(contents_root, "flex-a");
    lxb_dom_node_t *flex_b = find_id(contents_root, "flex-b");
    lxb_dom_node_t *contents_link = find_id(contents_root, "contents-link");
    lxb_dom_node_t *grid_shell = find_id(contents_root, "grid-shell");
    lxb_dom_node_t *contents_grid_a = find_id(contents_root,
                                               "contents-grid-a");
    lxb_dom_node_t *contents_grid_b = find_id(contents_root,
                                               "contents-grid-b");
    lxb_dom_node_t *contents_grid_c = find_id(contents_root,
                                               "contents-grid-c");
    lxb_dom_node_t *column_host = find_id(contents_root, "column-host");
    lxb_dom_node_t *column_shell = find_id(contents_root, "column-shell");
    lxb_dom_node_t *contents_column_a = find_id(contents_root,
                                                 "contents-column-a");
    lxb_dom_node_t *contents_column_b = find_id(contents_root,
                                                 "contents-column-b");
    lxb_dom_node_t *atomic_block = find_id(contents_root, "atomic-block");
    lxb_dom_node_t *atomic_flex = find_id(contents_root, "atomic-flex");
    lxb_dom_node_t *atomic_flex_a = find_id(contents_root, "atomic-flex-a");
    lxb_dom_node_t *atomic_flex_b = find_id(contents_root, "atomic-flex-b");
    lxb_dom_node_t *atomic_grid = find_id(contents_root, "atomic-grid");
    lxb_dom_node_t *atomic_grid_a = find_id(contents_root, "atomic-grid-a");
    lxb_dom_node_t *atomic_grid_b = find_id(contents_root, "atomic-grid-b");
    lxb_dom_node_t *atomic_link = find_id(contents_root, "atomic-link");
    lxb_dom_node_t *atomic_button = find_id(contents_root, "atomic-button");
    lxb_dom_node_t *auto_cross = find_id(contents_root, "auto-cross");
    lxb_dom_node_t *auto_tall = find_id(contents_root, "auto-tall");
    lxb_dom_node_t *auto_short = find_id(contents_root, "auto-short");
    lxb_dom_node_t *auto_link = find_id(contents_root, "auto-link");
    lxb_dom_node_t *auto_self_start = find_id(contents_root,
                                               "auto-self-start");
    lxb_dom_node_t *auto_self_end = find_id(contents_root, "auto-self-end");
    lxb_dom_node_t *self_column = find_id(contents_root, "self-column");
    lxb_dom_node_t *self_column_start = find_id(contents_root,
                                                 "self-column-start");
    lxb_dom_node_t *self_column_end = find_id(contents_root,
                                               "self-column-end");
    lxb_dom_node_t *content_lines = find_id(contents_root, "content-lines");
    lxb_dom_node_t *content_line_a = find_id(contents_root,
                                              "content-line-a");
    lxb_dom_node_t *content_line_b = find_id(contents_root,
                                              "content-line-b");
    lxb_dom_node_t *content_line_c = find_id(contents_root,
                                              "content-line-c");
    lxb_dom_node_t *content_line_d = find_id(contents_root,
                                              "content-line-d");
    lxb_dom_node_t *stretch_content = find_id(contents_root,
                                               "stretch-content");
    lxb_dom_node_t *stretch_content_a = find_id(contents_root,
                                                 "stretch-content-a");
    lxb_dom_node_t *stretch_content_c = find_id(contents_root,
                                                 "stretch-content-c");
    lxb_dom_node_t *single_content = find_id(contents_root,
                                              "single-content");
    lxb_dom_node_t *single_content_item = find_id(contents_root,
                                                   "single-content-item");
    lxb_dom_node_t *reverse_content = find_id(contents_root,
                                               "reverse-content");
    lxb_dom_node_t *reverse_content_a = find_id(contents_root,
                                                 "reverse-content-a");
    lxb_dom_node_t *reverse_content_c = find_id(contents_root,
                                                 "reverse-content-c");
    lxb_dom_node_t *stretch_row = find_id(contents_root, "stretch-row");
    lxb_dom_node_t *stretch_link = find_id(contents_root, "stretch-link");
    lxb_dom_node_t *stretch_fixed = find_id(contents_root, "stretch-fixed");
    lxb_dom_node_t *auto_margin_row = find_id(contents_root,
                                               "auto-margin-row");
    lxb_dom_node_t *auto_margin_top = find_id(contents_root,
                                               "auto-margin-top");
    lxb_dom_node_t *auto_margin_bottom = find_id(contents_root,
                                                  "auto-margin-bottom");
    lxb_dom_node_t *auto_margin_both = find_id(contents_root,
                                                "auto-margin-both");
    lxb_dom_node_t *auto_margin_column = find_id(contents_root,
                                                  "auto-margin-column");
    lxb_dom_node_t *auto_margin_left = find_id(contents_root,
                                                "auto-margin-left");
    lxb_dom_node_t *auto_margin_right = find_id(contents_root,
                                                 "auto-margin-right");
    lxb_dom_node_t *auto_margin_inline = find_id(contents_root,
                                                  "auto-margin-inline");
    lxb_dom_node_t *main_auto_row = find_id(contents_root, "main-auto-row");
    lxb_dom_node_t *main_row_start = find_id(contents_root, "main-row-start");
    lxb_dom_node_t *main_row_end = find_id(contents_root, "main-row-end");
    lxb_dom_node_t *main_auto_column = find_id(contents_root,
                                                "main-auto-column");
    lxb_dom_node_t *main_column_start = find_id(contents_root,
                                                 "main-column-start");
    lxb_dom_node_t *main_column_end = find_id(contents_root,
                                               "main-column-end");
    lxb_dom_node_t *main_auto_center = find_id(contents_root,
                                                "main-auto-center");
    lxb_dom_node_t *main_center = find_id(contents_root, "main-center");
    lxb_dom_node_t *main_auto_wrap = find_id(contents_root,
                                              "main-auto-wrap");
    lxb_dom_node_t *main_wrap_start = find_id(contents_root,
                                               "main-wrap-start");
    lxb_dom_node_t *main_wrap_end = find_id(contents_root, "main-wrap-end");
    lxb_dom_node_t *main_wrap_next = find_id(contents_root,
                                              "main-wrap-next");
    lxb_dom_node_t *reverse_row = find_id(contents_root, "reverse-row");
    lxb_dom_node_t *reverse_row_a = find_id(contents_root, "reverse-row-a");
    lxb_dom_node_t *reverse_row_b = find_id(contents_root, "reverse-row-b");
    lxb_dom_node_t *reverse_row_end = find_id(contents_root,
                                               "reverse-row-end");
    lxb_dom_node_t *reverse_end_a = find_id(contents_root, "reverse-end-a");
    lxb_dom_node_t *reverse_end_b = find_id(contents_root, "reverse-end-b");
    lxb_dom_node_t *reverse_row_auto = find_id(contents_root,
                                                "reverse-row-auto");
    lxb_dom_node_t *reverse_auto_first = find_id(contents_root,
                                                  "reverse-auto-first");
    lxb_dom_node_t *reverse_auto_second = find_id(contents_root,
                                                   "reverse-auto-second");
    lxb_dom_node_t *reverse_wrap = find_id(contents_root, "reverse-wrap");
    lxb_dom_node_t *reverse_wrap_a = find_id(contents_root, "reverse-wrap-a");
    lxb_dom_node_t *reverse_wrap_b = find_id(contents_root, "reverse-wrap-b");
    lxb_dom_node_t *reverse_wrap_c = find_id(contents_root, "reverse-wrap-c");
    lxb_dom_node_t *reverse_column = find_id(contents_root, "reverse-column");
    lxb_dom_node_t *reverse_column_a = find_id(contents_root,
                                                "reverse-column-a");
    lxb_dom_node_t *reverse_column_b = find_id(contents_root,
                                                "reverse-column-b");
    lxb_dom_node_t *baseline_row = find_id(contents_root, "baseline-row");
    lxb_dom_node_t *baseline_large = find_id(contents_root, "baseline-large");
    lxb_dom_node_t *baseline_small = find_id(contents_root, "baseline-small");
    lxb_dom_node_t *order_row = find_id(contents_root, "order-row");
    lxb_dom_node_t *order_wrap = find_id(contents_root, "order-wrap");
    lxb_dom_node_t *order_reverse = find_id(contents_root, "order-reverse");
    lxb_dom_node_t *order_column = find_id(contents_root, "order-column");
    lxb_dom_node_t *order_many = find_id(contents_root, "order-many");
    CHECK(normal_host != NULL && normal_shell != NULL && normal_link != NULL
          && normal_block != NULL
          && flex_shell != NULL && flex_nested != NULL && flex_a != NULL
          && flex_b != NULL && contents_link != NULL && grid_shell != NULL
          && contents_grid_a != NULL && contents_grid_b != NULL
          && contents_grid_c != NULL && column_host != NULL
          && column_shell != NULL && contents_column_a != NULL
          && contents_column_b != NULL && atomic_block != NULL
          && atomic_flex != NULL && atomic_flex_a != NULL
          && atomic_flex_b != NULL && atomic_grid != NULL
          && atomic_grid_a != NULL && atomic_grid_b != NULL
          && atomic_link != NULL && atomic_button != NULL
          && auto_cross != NULL && auto_tall != NULL
          && auto_short != NULL && auto_link != NULL
          && auto_self_start != NULL && auto_self_end != NULL
          && self_column != NULL && self_column_start != NULL
          && self_column_end != NULL && content_lines != NULL
          && content_line_a != NULL && content_line_b != NULL
          && content_line_c != NULL && content_line_d != NULL
          && stretch_content != NULL && stretch_content_a != NULL
          && stretch_content_c != NULL && single_content != NULL
          && single_content_item != NULL
          && reverse_content != NULL && reverse_content_a != NULL
          && reverse_content_c != NULL
          && main_auto_row != NULL && main_row_start != NULL
          && main_row_end != NULL && main_auto_column != NULL
          && main_column_start != NULL && main_column_end != NULL
          && main_auto_center != NULL && main_center != NULL
          && main_auto_wrap != NULL && main_wrap_start != NULL
          && main_wrap_end != NULL && main_wrap_next != NULL
          && reverse_row != NULL && reverse_row_a != NULL
          && reverse_row_b != NULL && reverse_row_end != NULL
          && reverse_end_a != NULL && reverse_end_b != NULL
          && reverse_row_auto != NULL && reverse_auto_first != NULL
          && reverse_auto_second != NULL && reverse_wrap != NULL
          && reverse_wrap_a != NULL && reverse_wrap_b != NULL
          && reverse_wrap_c != NULL && reverse_column != NULL
          && reverse_column_a != NULL && reverse_column_b != NULL
          && baseline_row != NULL && baseline_large != NULL
          && baseline_small != NULL && order_row != NULL
          && order_wrap != NULL && order_reverse != NULL
          && order_column != NULL && order_many != NULL);
    Stylesheet contents_sheet;
    CHECK(stylesheet_build(&contents_sheet, &budget, &contents_document, 480));
    ComputedStyle contents_style = style_for_node(&contents_sheet,
                                                   normal_shell, NULL);
    CHECK(contents_style.display == DISPLAY_CONTENTS);
    CHECK(style_for_node(&contents_sheet, atomic_block, NULL).display
             == DISPLAY_INLINE_BLOCK
          && style_for_node(&contents_sheet, atomic_flex, NULL).display
             == DISPLAY_INLINE_FLEX
          && style_for_node(&contents_sheet, atomic_grid, NULL).display
             == DISPLAY_INLINE_GRID);
    CHECK(style_for_node(&contents_sheet, auto_self_start, NULL).align_self
             == ALIGN_SELF_START
          && style_for_node(&contents_sheet, auto_self_end, NULL).align_self
             == ALIGN_SELF_END);
    CHECK(style_for_node(&contents_sheet, content_lines, NULL).align_content
             == JUSTIFY_SPACE_BETWEEN);
    CHECK(style_for_node(&contents_sheet, stretch_content, NULL).align_content
             == JUSTIFY_STRETCH);
    CHECK(style_for_node(&contents_sheet, reverse_content, NULL).flex_wrap
          && style_for_node(&contents_sheet, reverse_content, NULL)
                 .flex_wrap_reverse);
    CHECK(style_for_node(&contents_sheet, reverse_row, NULL).flex_direction
             == FLEX_ROW_REVERSE
          && style_for_node(&contents_sheet, reverse_column, NULL).flex_direction
             == FLEX_COLUMN_REVERSE);
    CHECK(style_for_node(&contents_sheet, baseline_row, NULL).align_items
             == ALIGN_BASELINE);
    CHECK(style_for_node(&contents_sheet, stretch_row, NULL).align_items
             == ALIGN_STRETCH
          && style_for_node(&contents_sheet, stretch_link, NULL).align_self
             == ALIGN_SELF_AUTO
          && style_for_node(&contents_sheet, stretch_fixed, NULL).align_self
             == ALIGN_SELF_STRETCH);
    ComputedStyle auto_margin_both_style = style_for_node(
        &contents_sheet, auto_margin_both, NULL);
    ComputedStyle auto_margin_inline_style = style_for_node(
        &contents_sheet, auto_margin_inline, NULL);
    CHECK(auto_margin_both_style.margin_top_auto
          && auto_margin_both_style.margin_bottom_auto
          && auto_margin_inline_style.margin_left_auto
          && auto_margin_inline_style.margin_right_auto);
    LayoutDocument contents_layout;
    CHECK(layout_build(&contents_layout, &budget, &contents_document,
                       &contents_sheet, NULL, NULL, 480)
          && contents_layout.node_interaction_ranges == NULL
          && contents_layout.node_interaction_capacity == 0);
    const LayoutNodeBox *normal_host_box = layout_box_for_node(
        &contents_layout, normal_host);
    const LayoutNodeBox *normal_block_box = layout_box_for_node(
        &contents_layout, normal_block);
    const LayoutNodeBox *flex_a_box = layout_box_for_node(&contents_layout,
                                                          flex_a);
    const LayoutNodeBox *flex_b_box = layout_box_for_node(&contents_layout,
                                                          flex_b);
    const LayoutNodeBox *grid_a_box_contents = layout_box_for_node(
        &contents_layout, contents_grid_a);
    const LayoutNodeBox *grid_b_box_contents = layout_box_for_node(
        &contents_layout, contents_grid_b);
    const LayoutNodeBox *grid_c_box_contents = layout_box_for_node(
        &contents_layout, contents_grid_c);
    const LayoutNodeBox *column_host_box_contents = layout_box_for_node(
        &contents_layout, column_host);
    const LayoutNodeBox *column_a_box_contents = layout_box_for_node(
        &contents_layout, contents_column_a);
    const LayoutNodeBox *column_b_box_contents = layout_box_for_node(
        &contents_layout, contents_column_b);
    CHECK(layout_box_for_node(&contents_layout, normal_shell) == NULL
          && layout_box_for_node(&contents_layout, flex_shell) == NULL
          && layout_box_for_node(&contents_layout, flex_nested) == NULL
          && layout_box_for_node(&contents_layout, contents_link) == NULL
          && layout_box_for_node(&contents_layout, grid_shell) == NULL
          && layout_box_for_node(&contents_layout, column_shell) == NULL);
    CHECK(normal_host_box != NULL && normal_block_box != NULL
          && normal_block_box->x == normal_host_box->x
          && normal_block_box->width == 60
          && command_color(&contents_layout, "NORMALINLINE") == 0x2468ac
          && command_color(&contents_layout, "NORMALBLOCK") == 0x2468ac);
    CHECK(flex_a_box != NULL && flex_b_box != NULL
          && flex_a_box->width == 48 && flex_b_box->width == 48
          && flex_b_box->x == flex_a_box->x + 52
          && command_color(&contents_layout, "FB") == 0x2468ac);
    CHECK(command_x(&contents_layout, "ALPHA") >= 0
          && command_x(&contents_layout, "MIDDLE")
             > command_x(&contents_layout, "ALPHA")
          && command_x(&contents_layout, "OMEGA")
             > command_x(&contents_layout, "MIDDLE")
          && command_y(&contents_layout, "ALPHA")
             == command_y(&contents_layout, "MIDDLE")
          && command_y(&contents_layout, "OMEGA")
             == command_y(&contents_layout, "MIDDLE"));
    const LinkRegion *contents_link_region = NULL;
    const LinkRegion *normal_link_region = NULL;
    for (size_t i = 0; i < contents_layout.link_count; i++) {
        if (contents_layout.links[i].node == normal_link) {
            normal_link_region = &contents_layout.links[i];
        }
        if (contents_layout.links[i].node == contents_link) {
            contents_link_region = &contents_layout.links[i];
        }
    }
    CHECK(normal_link_region != NULL && contents_link_region != NULL
          && command_x(&contents_layout, "NORMALLINK") >= 0
          && command_x(&contents_layout, "LINKTEXT") >= 0);
    CHECK(grid_a_box_contents != NULL && grid_b_box_contents != NULL
          && grid_c_box_contents != NULL
          && grid_a_box_contents->width == 33
          && grid_b_box_contents->width == 32
          && grid_c_box_contents->width == 32
          && grid_b_box_contents->x == grid_a_box_contents->x + 35
          && grid_c_box_contents->x == grid_b_box_contents->x + 34);
    CHECK(column_host_box_contents != NULL && column_a_box_contents != NULL
          && column_b_box_contents != NULL
          && column_a_box_contents->y == column_host_box_contents->y
          && column_b_box_contents->y + column_b_box_contents->height
             == column_host_box_contents->y
                + column_host_box_contents->height);
    const LayoutNodeBox *atomic_block_box = layout_box_for_node(
        &contents_layout, atomic_block);
    const LayoutNodeBox *atomic_flex_box = layout_box_for_node(
        &contents_layout, atomic_flex);
    const LayoutNodeBox *atomic_flex_a_box = layout_box_for_node(
        &contents_layout, atomic_flex_a);
    const LayoutNodeBox *atomic_flex_b_box = layout_box_for_node(
        &contents_layout, atomic_flex_b);
    const LayoutNodeBox *atomic_grid_box = layout_box_for_node(
        &contents_layout, atomic_grid);
    const LayoutNodeBox *atomic_grid_a_box = layout_box_for_node(
        &contents_layout, atomic_grid_a);
    const LayoutNodeBox *atomic_grid_b_box = layout_box_for_node(
        &contents_layout, atomic_grid_b);
    CHECK(atomic_block_box != NULL && atomic_flex_box != NULL
          && atomic_flex_a_box != NULL && atomic_flex_b_box != NULL
          && atomic_grid_box != NULL && atomic_grid_a_box != NULL
          && atomic_grid_b_box != NULL
          && atomic_block_box->width == 40
          && atomic_flex_box->width == 60
          && atomic_flex_a_box->width == 28
          && atomic_flex_b_box->width == 28
          && atomic_flex_b_box->x == atomic_flex_a_box->x + 32
          && atomic_grid_box->width == 62
          && atomic_grid_a_box->width == 30
          && atomic_grid_b_box->width == 30
          && atomic_grid_b_box->x == atomic_grid_a_box->x + 32
          && command_y(&contents_layout, "PRE")
             == command_y(&contents_layout, "POST")
          && command_x(&contents_layout, "IB")
             > command_x(&contents_layout, "PRE")
          && command_x(&contents_layout, "POST")
             > command_x(&contents_layout, "ATLINK"));
    bool atomic_link_found = false;
    bool atomic_button_found = false;
    for (size_t i = 0; i < contents_layout.link_count; i++) {
        if (contents_layout.links[i].node == atomic_link) {
            atomic_link_found = true;
        }
    }
    for (size_t i = 0; i < contents_layout.control_count; i++) {
        if (contents_layout.controls[i].node == atomic_button
            && contents_layout.controls[i].type == CONTROL_BUTTON) {
            atomic_button_found = true;
        }
    }
    CHECK(atomic_link_found && atomic_button_found
          && command_x(&contents_layout, "ATBUTTON") >= 0);
    const LayoutNodeBox *auto_cross_box = layout_box_for_node(
        &contents_layout, auto_cross);
    const LayoutNodeBox *auto_tall_box = layout_box_for_node(
        &contents_layout, auto_tall);
    const LayoutNodeBox *auto_short_box = layout_box_for_node(
        &contents_layout, auto_short);
    const LayoutNodeBox *auto_self_start_box = layout_box_for_node(
        &contents_layout, auto_self_start);
    const LayoutNodeBox *auto_self_end_box = layout_box_for_node(
        &contents_layout, auto_self_end);
    const LayoutNodeBox *self_column_box = layout_box_for_node(
        &contents_layout, self_column);
    const LayoutNodeBox *self_column_start_box = layout_box_for_node(
        &contents_layout, self_column_start);
    const LayoutNodeBox *self_column_end_box = layout_box_for_node(
        &contents_layout, self_column_end);
    const LayoutNodeBox *fit_column_start_box = layout_box_for_node(
        &contents_layout, find_id(contents_root, "fit-column-start"));
    const LayoutNodeBox *fit_column_start_item_box = layout_box_for_node(
        &contents_layout, find_id(contents_root, "fit-column-start-item"));
    const LayoutNodeBox *fit_column_center_box = layout_box_for_node(
        &contents_layout, find_id(contents_root, "fit-column-center"));
    const LayoutNodeBox *fit_column_center_item_box = layout_box_for_node(
        &contents_layout, find_id(contents_root, "fit-column-center-item"));
    const LayoutNodeBox *fit_column_end_box = layout_box_for_node(
        &contents_layout, find_id(contents_root, "fit-column-end"));
    const LayoutNodeBox *fit_column_end_item_box = layout_box_for_node(
        &contents_layout, find_id(contents_root, "fit-column-end-item"));
    const LayoutNodeBox *content_lines_box = layout_box_for_node(
        &contents_layout, content_lines);
    const LayoutNodeBox *content_line_a_box = layout_box_for_node(
        &contents_layout, content_line_a);
    const LayoutNodeBox *content_line_b_box = layout_box_for_node(
        &contents_layout, content_line_b);
    const LayoutNodeBox *content_line_c_box = layout_box_for_node(
        &contents_layout, content_line_c);
    const LayoutNodeBox *content_line_d_box = layout_box_for_node(
        &contents_layout, content_line_d);
    const LayoutNodeBox *stretch_content_box = layout_box_for_node(
        &contents_layout, stretch_content);
    const LayoutNodeBox *stretch_content_a_box = layout_box_for_node(
        &contents_layout, stretch_content_a);
    const LayoutNodeBox *stretch_content_c_box = layout_box_for_node(
        &contents_layout, stretch_content_c);
    const LayoutNodeBox *single_content_box = layout_box_for_node(
        &contents_layout, single_content);
    const LayoutNodeBox *single_content_item_box = layout_box_for_node(
        &contents_layout, single_content_item);
    const LayoutNodeBox *reverse_content_box = layout_box_for_node(
        &contents_layout, reverse_content);
    const LayoutNodeBox *reverse_content_a_box = layout_box_for_node(
        &contents_layout, reverse_content_a);
    const LayoutNodeBox *reverse_content_c_box = layout_box_for_node(
        &contents_layout, reverse_content_c);
    const LayoutNodeBox *overflow_center_box = layout_box_for_node(
        &contents_layout, find_id(contents_root, "overflow-center"));
    const LayoutNodeBox *overflow_center_a_box = layout_box_for_node(
        &contents_layout, find_id(contents_root, "overflow-center-a"));
    const LayoutNodeBox *overflow_center_c_box = layout_box_for_node(
        &contents_layout, find_id(contents_root, "overflow-center-c"));
    const LayoutNodeBox *stretch_row_box = layout_box_for_node(
        &contents_layout, stretch_row);
    const LayoutNodeBox *stretch_link_box = layout_box_for_node(
        &contents_layout, stretch_link);
    const LayoutNodeBox *stretch_fixed_box = layout_box_for_node(
        &contents_layout, stretch_fixed);
    const LayoutNodeBox *auto_margin_row_box = layout_box_for_node(
        &contents_layout, auto_margin_row);
    const LayoutNodeBox *auto_margin_top_box = layout_box_for_node(
        &contents_layout, auto_margin_top);
    const LayoutNodeBox *auto_margin_bottom_box = layout_box_for_node(
        &contents_layout, auto_margin_bottom);
    const LayoutNodeBox *auto_margin_both_box = layout_box_for_node(
        &contents_layout, auto_margin_both);
    const LayoutNodeBox *auto_margin_column_box = layout_box_for_node(
        &contents_layout, auto_margin_column);
    const LayoutNodeBox *auto_margin_left_box = layout_box_for_node(
        &contents_layout, auto_margin_left);
    const LayoutNodeBox *auto_margin_right_box = layout_box_for_node(
        &contents_layout, auto_margin_right);
    const LayoutNodeBox *auto_margin_inline_box = layout_box_for_node(
        &contents_layout, auto_margin_inline);
    const LayoutNodeBox *main_auto_row_box = layout_box_for_node(
        &contents_layout, main_auto_row);
    const LayoutNodeBox *main_row_start_box = layout_box_for_node(
        &contents_layout, main_row_start);
    const LayoutNodeBox *main_row_end_box = layout_box_for_node(
        &contents_layout, main_row_end);
    const LayoutNodeBox *main_auto_column_box = layout_box_for_node(
        &contents_layout, main_auto_column);
    const LayoutNodeBox *main_column_start_box = layout_box_for_node(
        &contents_layout, main_column_start);
    const LayoutNodeBox *main_column_end_box = layout_box_for_node(
        &contents_layout, main_column_end);
    const LayoutNodeBox *main_auto_center_box = layout_box_for_node(
        &contents_layout, main_auto_center);
    const LayoutNodeBox *main_center_box = layout_box_for_node(
        &contents_layout, main_center);
    const LayoutNodeBox *main_auto_wrap_box = layout_box_for_node(
        &contents_layout, main_auto_wrap);
    const LayoutNodeBox *main_wrap_start_box = layout_box_for_node(
        &contents_layout, main_wrap_start);
    const LayoutNodeBox *main_wrap_end_box = layout_box_for_node(
        &contents_layout, main_wrap_end);
    const LayoutNodeBox *main_wrap_next_box = layout_box_for_node(
        &contents_layout, main_wrap_next);
    const LayoutNodeBox *reverse_row_box = layout_box_for_node(
        &contents_layout, reverse_row);
    const LayoutNodeBox *reverse_row_a_box = layout_box_for_node(
        &contents_layout, reverse_row_a);
    const LayoutNodeBox *reverse_row_b_box = layout_box_for_node(
        &contents_layout, reverse_row_b);
    const LayoutNodeBox *reverse_row_end_box = layout_box_for_node(
        &contents_layout, reverse_row_end);
    const LayoutNodeBox *reverse_end_a_box = layout_box_for_node(
        &contents_layout, reverse_end_a);
    const LayoutNodeBox *reverse_end_b_box = layout_box_for_node(
        &contents_layout, reverse_end_b);
    const LayoutNodeBox *reverse_row_auto_box = layout_box_for_node(
        &contents_layout, reverse_row_auto);
    const LayoutNodeBox *reverse_auto_first_box = layout_box_for_node(
        &contents_layout, reverse_auto_first);
    const LayoutNodeBox *reverse_auto_second_box = layout_box_for_node(
        &contents_layout, reverse_auto_second);
    const LayoutNodeBox *reverse_wrap_box = layout_box_for_node(
        &contents_layout, reverse_wrap);
    const LayoutNodeBox *reverse_wrap_a_box = layout_box_for_node(
        &contents_layout, reverse_wrap_a);
    const LayoutNodeBox *reverse_wrap_b_box = layout_box_for_node(
        &contents_layout, reverse_wrap_b);
    const LayoutNodeBox *reverse_wrap_c_box = layout_box_for_node(
        &contents_layout, reverse_wrap_c);
    const LayoutNodeBox *reverse_column_box = layout_box_for_node(
        &contents_layout, reverse_column);
    const LayoutNodeBox *reverse_column_a_box = layout_box_for_node(
        &contents_layout, reverse_column_a);
    const LayoutNodeBox *reverse_column_b_box = layout_box_for_node(
        &contents_layout, reverse_column_b);
    const LayoutNodeBox *baseline_row_box = layout_box_for_node(
        &contents_layout, baseline_row);
    const LayoutNodeBox *baseline_large_box = layout_box_for_node(
        &contents_layout, baseline_large);
    const LayoutNodeBox *baseline_small_box = layout_box_for_node(
        &contents_layout, baseline_small);
    const LayoutNodeBox *order_row_box = layout_box_for_node(
        &contents_layout, order_row);
    const LayoutNodeBox *order_wrap_box = layout_box_for_node(
        &contents_layout, order_wrap);
    const LayoutNodeBox *order_reverse_box = layout_box_for_node(
        &contents_layout, order_reverse);
    const LayoutNodeBox *order_column_box = layout_box_for_node(
        &contents_layout, order_column);
    const LayoutNodeBox *order_many_box = layout_box_for_node(
        &contents_layout, order_many);
    const LinkRegion *auto_link_region = NULL;
    const LinkRegion *stretch_link_region = NULL;
    const LinkRegion *baseline_link_region = NULL;
    for (size_t i = 0; i < contents_layout.link_count; i++) {
        if (contents_layout.links[i].node == auto_link) {
            auto_link_region = &contents_layout.links[i];
        }
        if (contents_layout.links[i].node == stretch_link) {
            stretch_link_region = &contents_layout.links[i];
        }
        if (contents_layout.links[i].node == baseline_small) {
            baseline_link_region = &contents_layout.links[i];
        }
    }
    CHECK(auto_cross_box != NULL && auto_tall_box != NULL
          && auto_short_box != NULL && auto_self_start_box != NULL
          && auto_self_end_box != NULL && auto_link_region != NULL
          && auto_cross_box->height == 30
          && auto_short_box->y == auto_tall_box->y + 10
          && auto_self_start_box->y == auto_tall_box->y
          && auto_self_end_box->y == auto_tall_box->y + 20
          && auto_link_region->y >= auto_short_box->y
          && auto_link_region->y
             < auto_short_box->y + auto_short_box->height);
    CHECK(self_column_box != NULL && self_column_start_box != NULL
          && self_column_end_box != NULL
          && self_column_start_box->x == self_column_box->x
          && self_column_end_box->x
             == self_column_box->x + self_column_box->width
                - self_column_end_box->width);
    CHECK(fit_column_start_box != NULL
          && fit_column_start_item_box != NULL
          && fit_column_center_box != NULL
          && fit_column_center_item_box != NULL
          && fit_column_end_box != NULL
          && fit_column_end_item_box != NULL
          && fit_column_start_item_box->width == 60
          && fit_column_start_item_box->x == fit_column_start_box->x
          && fit_column_center_item_box->width == 60
          && fit_column_center_item_box->x
             == fit_column_center_box->x + 30
          && fit_column_end_item_box->width == 60
          && fit_column_end_item_box->x == fit_column_end_box->x + 60);
    CHECK(content_lines_box != NULL && content_line_a_box != NULL
          && content_line_b_box != NULL && content_line_c_box != NULL
          && content_line_d_box != NULL
          && content_line_a_box->y == content_lines_box->y
          && content_line_b_box->y == content_line_a_box->y
          && content_line_c_box->y == content_line_d_box->y
          && content_line_c_box->y + content_line_c_box->height
             == content_lines_box->y + content_lines_box->height);
    CHECK(stretch_content_box != NULL && stretch_content_a_box != NULL
          && stretch_content_c_box != NULL
          && stretch_content_a_box->y == stretch_content_box->y + 19
          && stretch_content_c_box->y == stretch_content_box->y + 71);
    CHECK(single_content_box != NULL && single_content_item_box != NULL
          && single_content_item_box->y == single_content_box->y + 40);
    CHECK(reverse_content_box != NULL && reverse_content_a_box != NULL
          && reverse_content_c_box != NULL
          && reverse_content_a_box->y == reverse_content_box->y + 40
          && reverse_content_c_box->y == reverse_content_box->y + 26);
    CHECK(overflow_center_box != NULL
          && overflow_center_a_box != NULL
          && overflow_center_c_box != NULL
          && overflow_center_a_box->y == overflow_center_box->y - 2
          && overflow_center_c_box->y == overflow_center_box->y + 12);
    CHECK(stretch_row_box != NULL && stretch_link_box != NULL
          && stretch_fixed_box != NULL && stretch_link_region != NULL

          && stretch_row_box->height == 40
          && stretch_link_box->height == 40
          && stretch_fixed_box->height == 10
          && stretch_link_region->x == stretch_link_box->x
          && stretch_link_region->y == stretch_link_box->y
          && stretch_link_region->width == stretch_link_box->width
          && stretch_link_region->height == stretch_link_box->height);
    bool stretched_background = false;
    for (size_t i = stretch_link_box->command_start;
         i < stretch_link_box->command_end; i++) {
        if (contents_layout.commands[i].type == DRAW_FILL_RECT
            && contents_layout.commands[i].color == 0x123456
            && contents_layout.commands[i].height == 40) {
            stretched_background = true;
        }
    }
    CHECK(stretched_background);
    CHECK(auto_margin_row_box != NULL && auto_margin_top_box != NULL
          && auto_margin_bottom_box != NULL && auto_margin_both_box != NULL
          && auto_margin_top_box->y == auto_margin_row_box->y + 30
          && auto_margin_bottom_box->y == auto_margin_row_box->y
          && auto_margin_both_box->y == auto_margin_row_box->y + 15);
    CHECK(auto_margin_column_box != NULL && auto_margin_left_box != NULL
          && auto_margin_right_box != NULL && auto_margin_inline_box != NULL
          && auto_margin_left_box->x == auto_margin_column_box->x + 80
          && auto_margin_right_box->x == auto_margin_column_box->x
          && auto_margin_inline_box->x == auto_margin_column_box->x + 40);
    CHECK(main_auto_row_box != NULL && main_row_start_box != NULL
          && main_row_end_box != NULL
          && main_row_start_box->x == main_auto_row_box->x
          && main_row_end_box->x == main_auto_row_box->x + 80);
    CHECK(main_auto_column_box != NULL && main_column_start_box != NULL
          && main_column_end_box != NULL
          && main_column_start_box->y == main_auto_column_box->y
          && main_column_end_box->y == main_auto_column_box->y + 90);
    CHECK(main_auto_center_box != NULL && main_center_box != NULL
          && main_center_box->y == main_auto_center_box->y + 45);
    CHECK(main_auto_wrap_box != NULL && main_wrap_start_box != NULL
          && main_wrap_end_box != NULL && main_wrap_next_box != NULL
          && main_wrap_start_box->x == main_auto_wrap_box->x
          && main_wrap_end_box->x == main_auto_wrap_box->x + 60
          && main_wrap_next_box->x == main_auto_wrap_box->x
          && main_wrap_next_box->y > main_wrap_start_box->y);
    CHECK(reverse_row_box != NULL && reverse_row_a_box != NULL
          && reverse_row_b_box != NULL
          && reverse_row_a_box->x == reverse_row_box->x + 80
          && reverse_row_b_box->x == reverse_row_box->x + 60);
    CHECK(reverse_row_end_box != NULL && reverse_end_a_box != NULL
          && reverse_end_b_box != NULL
          && reverse_end_a_box->x == reverse_row_end_box->x + 20
          && reverse_end_b_box->x == reverse_row_end_box->x);
    CHECK(reverse_row_auto_box != NULL && reverse_auto_first_box != NULL
          && reverse_auto_second_box != NULL
          && reverse_auto_first_box->x == reverse_row_auto_box->x + 80
          && reverse_auto_second_box->x == reverse_row_auto_box->x);
    CHECK(reverse_wrap_box != NULL && reverse_wrap_a_box != NULL
          && reverse_wrap_b_box != NULL && reverse_wrap_c_box != NULL
          && reverse_wrap_a_box->x == reverse_wrap_box->x + 60
          && reverse_wrap_b_box->x == reverse_wrap_box->x + 20
          && reverse_wrap_c_box->x == reverse_wrap_box->x + 60
          && reverse_wrap_c_box->y > reverse_wrap_a_box->y);
    CHECK(reverse_column_box != NULL && reverse_column_a_box != NULL
          && reverse_column_b_box != NULL
          && reverse_column_a_box->y == reverse_column_box->y + 90
          && reverse_column_b_box->y == reverse_column_box->y + 80);
    CHECK(baseline_row_box != NULL && baseline_large_box != NULL
          && baseline_small_box != NULL && baseline_link_region != NULL
          && fallback_command_baseline(&contents_layout, "BASELARGE")
             == fallback_command_baseline(&contents_layout, "BASESMALL")
          && baseline_small_box->y > baseline_large_box->y
          && baseline_link_region->y == baseline_small_box->y
          && fallback_command_baseline(&contents_layout, "WRAPLARGE1")
             == fallback_command_baseline(&contents_layout, "WRAPSMALL1")
          && fallback_command_baseline(&contents_layout, "WRAPSMALL2")
             == fallback_command_baseline(&contents_layout, "WRAPLARGE2")
          && command_y(&contents_layout, "WRAPSMALL2")
             > command_y(&contents_layout, "WRAPSMALL1"));
    CHECK(order_row_box != NULL
          && command_x(&contents_layout, "OB") == order_row_box->x
          && command_x(&contents_layout, "OD") == order_row_box->x + 40
          && command_x(&contents_layout, "OA") == order_row_box->x + 80
          && command_x(&contents_layout, "OC") == order_row_box->x + 120);
    CHECK(order_wrap_box != NULL
          && command_x(&contents_layout, "WB") == order_wrap_box->x
          && command_x(&contents_layout, "WD") == order_wrap_box->x + 44
          && command_x(&contents_layout, "WA") == order_wrap_box->x
          && command_x(&contents_layout, "WC") == order_wrap_box->x + 44
          && command_y(&contents_layout, "WA")
             > command_y(&contents_layout, "WB"));
    CHECK(order_reverse_box != NULL
          && command_x(&contents_layout, "RBB") == order_reverse_box->x + 160
          && command_x(&contents_layout, "RDD") == order_reverse_box->x + 120
          && command_x(&contents_layout, "RAA") == order_reverse_box->x + 80
          && command_x(&contents_layout, "RCC") == order_reverse_box->x + 40);
    CHECK(order_column_box != NULL
          && command_y(&contents_layout, "CBO") == order_column_box->y
          && command_y(&contents_layout, "CDO") == order_column_box->y + 10
          && command_y(&contents_layout, "CAO") == order_column_box->y + 20
          && command_y(&contents_layout, "CCO") == order_column_box->y + 30);
    CHECK(order_many_box != NULL
          && command_x(&contents_layout, "M0") == order_many_box->x
          && command_x(&contents_layout, "M1") == order_many_box->x + 30
          && command_x(&contents_layout, "M2") == order_many_box->x + 60
          && command_x(&contents_layout, "M3") == order_many_box->x + 90
          && command_x(&contents_layout, "M4") == order_many_box->x + 120
          && command_x(&contents_layout, "M5") == order_many_box->x + 150);
    layout_destroy(&contents_layout);
    stylesheet_destroy(&contents_sheet);
    document_destroy(&contents_document);
    CHECK(budget.current == 0);
