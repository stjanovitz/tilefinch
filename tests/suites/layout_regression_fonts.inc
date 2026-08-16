    puts("test: intrinsic max-content and collapsed visual affordances");
    static const char collapsed_page[] =
        "<!doctype html><style>body{margin:0;padding:0}"
        "#intrinsic-host{width:300px}"
        "#max-content{display:flex;width:max-content;padding:0 16px}"
        "#max-content>span{display:block;width:17px;height:10px}"
        "#flattened-max{display:inline-flex;width:max-content;gap:3px}"
        "#flattened-contents{display:contents}"
        "#flattened-a{display:block;width:31px;height:10px}"
        "#flattened-b{display:block;width:41px;height:10px}"
        "#flattened-c{display:block;width:51px;height:10px}"
        "#paired-intrinsic{float:left}"
        "#empty-clip{position:absolute;clip:rect(1px,1px,1px,1px);"

        "width:20px;height:20px;background:#ff0000}"
        "#zero-scale{display:block;transform:scale(0) translate(-7px);"
        "width:20px;height:20px;background:#00ff00}"
        "#border-none{display:block;width:20px;height:10px;"
        "border-bottom:none}</style><body>"
        "<div id=intrinsic-host><div id=max-content><span id=max-child>"
        "</span></div><div id=flattened-max><div id=flattened-contents>"
        "<span id=flattened-a></span><span id=flattened-b></span>"
        "<span id=flattened-c></span></div></div></div>"
        "<div id=empty-clip></div>"
        "<div id=zero-scale></div><div id=border-none></div>"
        "<div id=paired-intrinsic>paired intrinsic words</div>"
        "</body>";
    PocDocument collapsed_document;
    Stylesheet collapsed_sheet;
    LayoutDocument collapsed_layout;
    CHECK(document_parse(&collapsed_document, &budget, collapsed_page,
                         strlen(collapsed_page), 23)
          && stylesheet_build(&collapsed_sheet, &budget,
                              &collapsed_document, 480));
    lxb_dom_node_t *collapsed_root = lxb_dom_interface_node(
        collapsed_document.html);
    lxb_dom_node_t *max_content = find_id(collapsed_root, "max-content");
    lxb_dom_node_t *max_child = find_id(collapsed_root, "max-child");
    lxb_dom_node_t *flattened_max = find_id(collapsed_root, "flattened-max");
    lxb_dom_node_t *flattened_a = find_id(collapsed_root, "flattened-a");
    lxb_dom_node_t *flattened_b = find_id(collapsed_root, "flattened-b");
    lxb_dom_node_t *flattened_c = find_id(collapsed_root, "flattened-c");
    lxb_dom_node_t *empty_clip = find_id(collapsed_root, "empty-clip");
    lxb_dom_node_t *zero_scale = find_id(collapsed_root, "zero-scale");
    lxb_dom_node_t *border_none = find_id(collapsed_root, "border-none");
    CHECK(max_content != NULL && max_child != NULL && flattened_max != NULL
          && flattened_a != NULL && flattened_b != NULL
          && flattened_c != NULL && empty_clip != NULL
          && zero_scale != NULL && border_none != NULL);
    ComputedStyle max_content_style = style_for_node(
        &collapsed_sheet, max_content, NULL);
    ComputedStyle empty_clip_style = style_for_node(
        &collapsed_sheet, empty_clip, NULL);
    ComputedStyle zero_scale_style = style_for_node(
        &collapsed_sheet, zero_scale, NULL);
    ComputedStyle border_none_style = style_for_node(
        &collapsed_sheet, border_none, NULL);
    CHECK(max_content_style.has_width && max_content_style.width_max_content
          && empty_clip_style.clip_rect_empty && empty_clip_style.hidden
          && zero_scale_style.transform_scale_q6 == 0
          && zero_scale_style.hidden
          && border_none_style.border.bottom == 0);
    CHECK(layout_build(&collapsed_layout, &budget, &collapsed_document,
                       &collapsed_sheet, NULL, NULL, 480));
    const LayoutNodeBox *max_content_box = layout_box_for_node(
        &collapsed_layout, max_content);
    const LayoutNodeBox *max_child_box = layout_box_for_node(
        &collapsed_layout, max_child);
    const LayoutNodeBox *flattened_max_box = layout_box_for_node(
        &collapsed_layout, flattened_max);
    const LayoutNodeBox *flattened_a_box = layout_box_for_node(
        &collapsed_layout, flattened_a);
    const LayoutNodeBox *flattened_b_box = layout_box_for_node(
        &collapsed_layout, flattened_b);
    const LayoutNodeBox *flattened_c_box = layout_box_for_node(
        &collapsed_layout, flattened_c);
    CHECK(max_content_box != NULL && max_child_box != NULL
          && max_content_box->width == 49 && max_child_box->width == 17
          && max_child_box->x == max_content_box->x + 16
          && flattened_max_box != NULL && flattened_a_box != NULL
          && flattened_b_box != NULL && flattened_c_box != NULL
          && flattened_max_box->width == 129
          && flattened_b_box->x == flattened_a_box->x + 34
          && flattened_c_box->x == flattened_b_box->x + 44
          && layout_box_for_node(&collapsed_layout, empty_clip) == NULL
          && layout_box_for_node(&collapsed_layout, zero_scale) == NULL
          && collapsed_layout.performance
                 .intrinsic_paired_text_measurements > 0);
    layout_destroy(&collapsed_layout);
    stylesheet_destroy(&collapsed_sheet);
    document_destroy(&collapsed_document);
    CHECK(budget.current == 0);

    puts("test: positioned containing boxes, percentage insets, opposing-edge sizing");
    static const char positioned_page[] =
        "<!doctype html><style>body{margin:0;padding:0}#before{height:30px}"
        "#host{position:relative;margin-left:20px;width:200px;height:100px;"
        "padding:10px;border:2px solid #111}#shell{display:block}"
        "#absolute{position:absolute;left:7px;top:11px;width:30px;height:20px;background:#f00}"
        "#nested{position:absolute;left:4px;top:3px;width:8px;height:6px;background:#00f}"
        "#corner{position:absolute;right:9px;bottom:13px;width:20px;height:10px;background:#ff0}"
        "#percent{position:absolute;left:10%;top:20%;width:20px;height:10px;background:#0ff}"
        "#stretched{position:absolute;inset:5% 10% 15% 20%;background:#f0f}"
        "#fixed{position:fixed;left:17px;top:19px;width:40px;height:20px;background:#00ff00}"
        "#fixed-clip{position:fixed;left:200px;top:2px;width:10px;"
        "height:10px;overflow:hidden;background:#0000ff}"
        "#fixed-overflow{display:block;width:60px;height:10px;"
        "background:#ff0000}"
        "#tail{height:400px}</style><body><div id=before></div>"
        "<div id=host><div id=shell><a id=absolute href=/positioned>ABS"
        "<span id=nested></span></a><div id=corner></div><div id=percent></div>"
        "<div id=stretched></div></div></div>"
        "<div id=fixed>FIXED</div><div id=fixed-clip>"
        "<span id=fixed-overflow></span></div><div id=tail></div></body>";
    PocDocument positioned_document;
    Stylesheet positioned_sheet;
    LayoutDocument positioned_layout;
    CHECK(document_parse(&positioned_document, &budget, positioned_page,
                         strlen(positioned_page), 17)
          && stylesheet_build(&positioned_sheet, &budget,
                              &positioned_document, 240)
          && layout_build(&positioned_layout, &budget, &positioned_document,
                          &positioned_sheet, NULL, NULL, 240));
    lxb_dom_node_t *positioned_root = lxb_dom_interface_node(
        positioned_document.html);
    lxb_dom_node_t *positioned_host = find_id(positioned_root, "host");
    lxb_dom_node_t *positioned_absolute = find_id(positioned_root,
                                                   "absolute");
    lxb_dom_node_t *positioned_nested = find_id(positioned_root, "nested");
    lxb_dom_node_t *positioned_corner = find_id(positioned_root, "corner");
    lxb_dom_node_t *positioned_percent = find_id(positioned_root, "percent");
    lxb_dom_node_t *positioned_stretched = find_id(positioned_root,
                                                    "stretched");
    lxb_dom_node_t *positioned_fixed = find_id(positioned_root, "fixed");
    lxb_dom_node_t *positioned_fixed_clip = find_id(positioned_root,
                                                     "fixed-clip");
    const LayoutNodeBox *positioned_host_box = layout_box_for_node(
        &positioned_layout, positioned_host);
    const LayoutNodeBox *positioned_absolute_box = layout_box_for_node(
        &positioned_layout, positioned_absolute);
    const LayoutNodeBox *positioned_nested_box = layout_box_for_node(
        &positioned_layout, positioned_nested);
    const LayoutNodeBox *positioned_corner_box = layout_box_for_node(
        &positioned_layout, positioned_corner);
    const LayoutNodeBox *positioned_percent_box = layout_box_for_node(
        &positioned_layout, positioned_percent);
    const LayoutNodeBox *positioned_stretched_box = layout_box_for_node(
        &positioned_layout, positioned_stretched);
    const LayoutNodeBox *positioned_fixed_box = layout_box_for_node(
        &positioned_layout, positioned_fixed);
    const LinkRegion *positioned_link = NULL;
    for (size_t i = 0; i < positioned_layout.link_count; i++) {
        if (positioned_layout.links[i].node == positioned_absolute) {
            positioned_link = &positioned_layout.links[i];
        }
    }
    CHECK(positioned_host_box != NULL && positioned_absolute_box != NULL
          && positioned_nested_box != NULL && positioned_corner_box != NULL
          && positioned_percent_box != NULL
          && positioned_stretched_box != NULL
          && positioned_fixed_box != NULL && positioned_fixed_clip != NULL
          && positioned_link != NULL
          && positioned_host_box->x == 20 && positioned_host_box->y == 30
          && positioned_absolute_box->x == 29
          && positioned_absolute_box->y == 43
          && positioned_nested_box->x == 33
          && positioned_nested_box->y == 46
          && positioned_corner_box->x == 213
          && positioned_corner_box->y == 129
          && positioned_percent_box->x == 44
          && positioned_percent_box->y == 56
          && positioned_stretched_box->x == 66
          && positioned_stretched_box->y == 38
          && positioned_stretched_box->width == 154
          && positioned_stretched_box->height == 96
          && positioned_link->x == positioned_absolute_box->x
          && positioned_link->y == positioned_absolute_box->y
          && positioned_fixed_box->x == 17
          && positioned_layout.fixed_count == 2);
    TileCache positioned_cache;
    uint16_t *positioned_frame = budget_malloc(
        &budget, 240u * 120u * sizeof(*positioned_frame));
    CHECK(positioned_frame != NULL
          && tile_cache_init(&positioned_cache, &budget,
                             &positioned_layout, 2)
          && tile_cache_set_frame(&positioned_cache, positioned_frame,
                                  240u * 120u)
          && tile_cache_render_frame(&positioned_cache, 80, 240, 120, NULL));
    CHECK(positioned_frame[37u * 240u + 18u]
             == test_rgb565(0x00ff00)
          && positioned_frame[5u * 240u + 205u]
             == test_rgb565(0xff0000)
          && positioned_frame[5u * 240u + 225u]
             == positioned_frame[5u * 240u + 239u]);
    tile_cache_destroy(&positioned_cache);
    budget_free(&budget, positioned_frame);
    layout_destroy(&positioned_layout);
    stylesheet_destroy(&positioned_sheet);
    document_destroy(&positioned_document);
    CHECK(budget.current == 0);

    puts("test: font shorthand cascade, inheritance, media, and layout");
    static const char font_shorthand_page[] =
        "<!doctype html><style>body{margin:0;padding:0}"
        "#font-parent{font-style:normal;font-weight:normal;font-size:9px;"
        "line-height:9px;font-family:monospace;"
        "font:italic 700 condensed 20px/1.5 'Mobile Sans',sans-serif}"
        "#font-reset{font-style:italic;font-weight:bold;line-height:24px;"
        "font-family:monospace;font:12px serif}"
        "#font-malformed{font-size:18px;font-family:monospace;"
        "font-weight:bold;font:italic 700 /1.2 serif}"
        "#font-cascade{font:italic 700 18px/2 monospace;font-size:10px}"
        "#font-important{font:12px serif;"
        "font:italic 700 18px/2 monospace!important;font-size:9px}"
        "@media screen and (max-width:500px){#font-media{"
        "font:oblique 12deg 600 semi-condensed 16px/125% monospace}}"
        "@media screen and (min-width:700px){#font-media{font:30px serif}}"
        "#font-variable{--mobile-font:italic 600 15px/1.2 monospace;"
        "font:var(--mobile-font)}"
        "#font-production{font:normal 500 14px/20px Benton Sans}"
        "#font-rounded{font:21px/1.1904761905 sans-serif}"
        "#font-metric{font-family:'Unavailable Face',Arial,sans-serif}"
        "#font-system{font-family:-apple-system,BlinkMacSystemFont,"
        "'Segoe UI',sans-serif}"
        "#font-humanist{font-family:Verdana,Geneva,sans-serif}"
        "#font-points{font-size:11pt;margin-left:1pc;padding-left:1in}"
        "#font-fraction-parent{font-size:10pt}"
        "#font-fraction-child{font-size:75%}"
        "#font-layout-large{font:700 24px/1 sans-serif}"
        "#font-layout-semi{font:600 24px/1 sans-serif}"
        "#font-layout-small{font:12px/1 serif}"
        "#font-layout-tight{font:40px/20px sans-serif}"
        "#font-layout-after-tight{font:12px/12px sans-serif}"
        "h2{font:inherit}"
        "#line-inherit-parent{display:flex;font-size:24px;"
        "line-height:1.375;padding:12px 0}"
        "#line-inherit-parent h2{font-size:inherit;line-height:inherit;"
        "margin:0;padding:0}</style><body>"
        "<div id=font-parent>PARENT<span id=font-child>CHILD</span></div>"
        "<div id=font-reset>RESET</div>"
        "<div id=font-malformed>MALFORMED</div>"
        "<div id=font-cascade>CASCADE</div>"
        "<div id=font-important>IMPORTANT</div>"
        "<div id=font-media>MEDIA</div>"
        "<div id=font-variable>VARIABLE</div>"
        "<div id=font-production>PRODUCTION</div>"
        "<div id=font-rounded>ROUNDED</div>"
        "<div id=font-metric>METRIC</div>"
        "<div id=font-system>SYSTEM</div>"
        "<div id=font-humanist>HUMANIST</div>"
        "<div id=font-points>POINTS</div>"
        "<div id=font-fraction-parent>FRACTION-PARENT"
        "<span id=font-fraction-child>FRACTION-CHILD</span></div>"
        "<div id=font-layout-large>LARGETEXT</div>"
        "<div id=font-layout-semi>SEMITEXT</div>"
        "<div id=font-layout-small>SMALLTEXT</div>"
        "<div id=font-layout-tight>TIGHT</div>"
        "<div id=font-layout-after-tight>AFTERTIGHT</div>"
        "<div id=line-inherit-parent><h2 id=line-inherit-child>"
        "INHERITED-LINE</h2></div></body>";
    PocDocument font_shorthand_document;
    Stylesheet font_shorthand_sheet;
    LayoutDocument font_shorthand_layout;
    CHECK(document_parse(&font_shorthand_document, &budget,
                         font_shorthand_page, strlen(font_shorthand_page), 23)
          && stylesheet_build(&font_shorthand_sheet, &budget,
                              &font_shorthand_document, 480));
    lxb_dom_node_t *font_root = lxb_dom_interface_node(
        font_shorthand_document.html);
    lxb_dom_node_t *font_body = document_body_node(&font_shorthand_document);
    lxb_dom_node_t *font_html = font_body == NULL ? NULL : font_body->parent;
    lxb_dom_node_t *font_parent = find_id(font_root, "font-parent");
    lxb_dom_node_t *font_child = find_id(font_root, "font-child");
    lxb_dom_node_t *font_reset = find_id(font_root, "font-reset");
    lxb_dom_node_t *font_malformed = find_id(font_root, "font-malformed");
    lxb_dom_node_t *font_cascade = find_id(font_root, "font-cascade");
    lxb_dom_node_t *font_important = find_id(font_root, "font-important");
    lxb_dom_node_t *font_media = find_id(font_root, "font-media");
    lxb_dom_node_t *font_variable = find_id(font_root, "font-variable");
    lxb_dom_node_t *font_production = find_id(font_root, "font-production");
    lxb_dom_node_t *font_rounded = find_id(font_root, "font-rounded");
    lxb_dom_node_t *font_metric = find_id(font_root, "font-metric");
    lxb_dom_node_t *font_system = find_id(font_root, "font-system");
    lxb_dom_node_t *font_humanist = find_id(font_root, "font-humanist");
    lxb_dom_node_t *font_points = find_id(font_root, "font-points");
    lxb_dom_node_t *font_fraction_parent = find_id(
        font_root, "font-fraction-parent");
    lxb_dom_node_t *font_fraction_child = find_id(
        font_root, "font-fraction-child");
    lxb_dom_node_t *font_layout_tight = find_id(
        font_root, "font-layout-tight");
    lxb_dom_node_t *font_layout_after_tight = find_id(
        font_root, "font-layout-after-tight");
    lxb_dom_node_t *line_inherit_parent = find_id(
        font_root, "line-inherit-parent");
    lxb_dom_node_t *line_inherit_child = find_id(
        font_root, "line-inherit-child");
    CHECK(font_html != NULL && font_parent != NULL && font_child != NULL
          && font_reset != NULL
          && font_malformed != NULL && font_cascade != NULL
          && font_important != NULL && font_media != NULL
          && font_variable != NULL && font_production != NULL
          && font_rounded != NULL
          && font_metric != NULL && font_system != NULL
          && font_humanist != NULL
          && font_points != NULL && font_fraction_parent != NULL
          && font_fraction_child != NULL && font_layout_tight != NULL
          && font_layout_after_tight != NULL
          && line_inherit_parent != NULL && line_inherit_child != NULL);
    ComputedStyle font_parent_style = style_for_node(
        &font_shorthand_sheet, font_parent, NULL);
    ComputedStyle font_child_style = style_for_node(
        &font_shorthand_sheet, font_child, &font_parent_style);
    ComputedStyle font_reset_style = style_for_node(
        &font_shorthand_sheet, font_reset, NULL);
    ComputedStyle font_malformed_style = style_for_node(
        &font_shorthand_sheet, font_malformed, NULL);
    ComputedStyle font_cascade_style = style_for_node(
        &font_shorthand_sheet, font_cascade, NULL);
    ComputedStyle font_important_style = style_for_node(
        &font_shorthand_sheet, font_important, NULL);
    ComputedStyle font_media_style = style_for_node(
        &font_shorthand_sheet, font_media, NULL);
    ComputedStyle font_variable_style = style_for_node(
        &font_shorthand_sheet, font_variable, NULL);
    ComputedStyle font_production_style = style_for_node(
        &font_shorthand_sheet, font_production, NULL);
    ComputedStyle font_rounded_style = style_for_node(
        &font_shorthand_sheet, font_rounded, NULL);
    ComputedStyle font_metric_style = style_for_node(
        &font_shorthand_sheet, font_metric, NULL);
    ComputedStyle font_system_style = style_for_node(
        &font_shorthand_sheet, font_system, NULL);
    ComputedStyle font_humanist_style = style_for_node(
        &font_shorthand_sheet, font_humanist, NULL);
    ComputedStyle font_points_style = style_for_node(
        &font_shorthand_sheet, font_points, NULL);
    ComputedStyle font_fraction_parent_style = style_for_node(
        &font_shorthand_sheet, font_fraction_parent, NULL);
    ComputedStyle font_fraction_child_style = style_for_node(
        &font_shorthand_sheet, font_fraction_child,
        &font_fraction_parent_style);
    ComputedStyle font_html_style = style_for_node(
        &font_shorthand_sheet, font_html, NULL);
    ComputedStyle line_inherit_parent_style = style_for_node(
        &font_shorthand_sheet, line_inherit_parent, NULL);
    ComputedStyle line_inherit_child_style = style_for_node(
        &font_shorthand_sheet, line_inherit_child,
        &line_inherit_parent_style);
    CHECK(font_html_style.font_size == 16
          && font_html_style.root_font_size == 16);
    CHECK(font_parent_style.font_size == 20
          && font_parent_style.line_height == 30
          && font_parent_style.font_family == FONT_SANS
          && font_parent_style.font_weight == 700
          && font_parent_style.font_italic && font_parent_style.font_bold);
    CHECK(font_child_style.font_size == 20
          && font_child_style.line_height == 30
          && font_child_style.font_family == FONT_SANS
          && font_child_style.font_weight == 700
          && font_child_style.font_italic && font_child_style.font_bold);
    CHECK(font_reset_style.font_size == 12
          && font_reset_style.line_height == 0
          && font_reset_style.font_family == FONT_SERIF
          && font_reset_style.font_weight == 400
          && !font_reset_style.font_italic && !font_reset_style.font_bold);
    CHECK(font_malformed_style.font_size == 18
          && font_malformed_style.font_family == FONT_MONOSPACE
          && font_malformed_style.font_weight == 700
          && font_malformed_style.font_bold
          && !font_malformed_style.font_italic);
    CHECK(font_cascade_style.font_size == 10
          && font_cascade_style.line_height == 20
          && font_cascade_style.font_family == FONT_MONOSPACE
          && font_cascade_style.font_weight == 700
          && font_cascade_style.font_italic && font_cascade_style.font_bold);
    CHECK(font_important_style.font_size == 18
          && font_important_style.line_height == 36
          && font_important_style.font_family == FONT_MONOSPACE
          && font_important_style.font_weight == 700
          && font_important_style.font_italic
          && font_important_style.font_bold);
    CHECK(font_media_style.font_size == 16
          && font_media_style.line_height == 20
          && font_media_style.font_family == FONT_MONOSPACE
          && font_media_style.font_weight == 600
          && font_media_style.font_italic && font_media_style.font_bold);
    CHECK(font_variable_style.font_size == 15
          && font_variable_style.line_height == 18
          && font_variable_style.font_family == FONT_MONOSPACE
          && font_variable_style.font_weight == 600
          && font_variable_style.font_italic
          && font_variable_style.font_bold);
    CHECK(font_production_style.font_size == 14
          && font_production_style.line_height == 20
          && font_production_style.font_family == FONT_SANS
          && font_production_style.font_weight == 500
          && !font_production_style.font_italic
          && !font_production_style.font_bold);
    CHECK(font_rounded_style.font_size == 21
          && font_rounded_style.line_height == 25);
    CHECK(font_metric_style.font_family == FONT_METRIC_SANS);
    CHECK(font_system_style.font_family == FONT_METRIC_SANS);
    CHECK(font_humanist_style.font_family == FONT_HUMANIST_SANS);
    CHECK(computed_style_font_size_fixed(&font_points_style) == 939
          && font_points_style.margin.left == 16
          && font_points_style.padding.left == 96);
    CHECK(computed_style_font_size_fixed(&font_fraction_parent_style) == 853
          && computed_style_font_size_fixed(&font_fraction_child_style)
                 == 640
          && font_fraction_child_style.root_font_size == 16
          && font_fraction_child_style.root_font_size_fraction == 0
          && line_inherit_parent_style.line_height == 33
          && line_inherit_child_style.line_height == 33);
    CHECK(layout_build(&font_shorthand_layout, &budget,
                       &font_shorthand_document, &font_shorthand_sheet,
                       NULL, NULL, 480));
    const LayoutNodeBox *font_layout_tight_box = layout_box_for_node(
        &font_shorthand_layout, font_layout_tight);
    const LayoutNodeBox *font_layout_after_tight_box = layout_box_for_node(
        &font_shorthand_layout, font_layout_after_tight);
    const LayoutNodeBox *line_inherit_parent_box = layout_box_for_node(
        &font_shorthand_layout, line_inherit_parent);
    const DrawCommand *font_metric_command = text_command(
        &font_shorthand_layout, "METRIC");
    const DrawCommand *font_fraction_command = text_command(
        &font_shorthand_layout, "FRACTION-PARENT");
    CHECK(font_layout_tight_box != NULL
          && font_layout_after_tight_box != NULL
          && line_inherit_parent_box != NULL
          && font_metric_command != NULL
          && font_fraction_command != NULL
          && font_metric_command->font_size == 16
          && draw_command_text_font_size_fixed(font_fraction_command) == 853
          && font_layout_tight_box->height == 20
          && font_layout_after_tight_box->y
                 == font_layout_tight_box->y + 20
          && line_inherit_parent_box->height == 57);
    const DrawCommand *large_font_command = NULL;
    const DrawCommand *semi_font_command = NULL;
    const DrawCommand *small_font_command = NULL;
    for (size_t i = 0; i < font_shorthand_layout.count; i++) {
        const DrawCommand *command = &font_shorthand_layout.commands[i];
        if (command->type != DRAW_TEXT) continue;
        if (command->text_length == strlen("LARGETEXT")
            && memcmp(command->text, "LARGETEXT",
                      command->text_length) == 0) {
            large_font_command = command;
        } else if (command->text_length == strlen("SEMITEXT")
                   && memcmp(command->text, "SEMITEXT",
                             command->text_length) == 0) {
            semi_font_command = command;
        } else if (command->text_length == strlen("SMALLTEXT")
                   && memcmp(command->text, "SMALLTEXT",
                             command->text_length) == 0) {
            small_font_command = command;
        }
    }
    CHECK(large_font_command != NULL && semi_font_command != NULL
          && small_font_command != NULL
          && large_font_command->font_size == 24
          && large_font_command->font_family == FONT_SANS
          && large_font_command->font_weight == 70
          && semi_font_command->font_size == 24
          && semi_font_command->font_family == FONT_SANS
          && semi_font_command->font_weight == 60
          && small_font_command->font_size == 12
          && small_font_command->font_family == FONT_SERIF
          && small_font_command->font_weight == 40
          && large_font_command->height > small_font_command->height);
    layout_destroy(&font_shorthand_layout);
    stylesheet_destroy(&font_shorthand_sheet);
    document_destroy(&font_shorthand_document);
    CHECK(budget.current == 0);

    puts("test: root-relative rem font sizes and late stylesheets");
    static const char rem_font_page[] =
        "<!doctype html><style>html{font-size:62.5%}body{margin:0}"
        "#rem-parent{font-size:14px}"
        "#rem-long{font-size:1.8rem;line-height:1}"
        "#rem-short{font:normal 1.8rem/1 sans-serif}"
        "#em-control{font-size:1.8em;line-height:1}</style><body>"
        "<div id=rem-parent><div id=rem-long>REM-LONG</div>"
        "<div id=rem-short>REM-SHORT</div>"
        "<div id=em-control>EM-CONTROL</div></div></body>";
    PocDocument rem_font_document;
    Stylesheet rem_font_sheet;
    LayoutDocument rem_font_layout;
    CHECK(sizeof(ComputedStyle) == 376
          && document_parse(&rem_font_document, &budget, rem_font_page,
                            strlen(rem_font_page), 29)
          && stylesheet_build(&rem_font_sheet, &budget,
                              &rem_font_document, 480));
    lxb_dom_node_t *rem_body = document_body_node(&rem_font_document);
    lxb_dom_node_t *rem_html = rem_body == NULL ? NULL : rem_body->parent;
    lxb_dom_node_t *rem_parent = find_id(
        lxb_dom_interface_node(rem_font_document.html), "rem-parent");
    lxb_dom_node_t *rem_long = find_id(
        lxb_dom_interface_node(rem_font_document.html), "rem-long");
    lxb_dom_node_t *rem_short = find_id(
        lxb_dom_interface_node(rem_font_document.html), "rem-short");
    lxb_dom_node_t *em_control = find_id(
        lxb_dom_interface_node(rem_font_document.html), "em-control");
    CHECK(rem_html != NULL && rem_parent != NULL && rem_long != NULL
          && rem_short != NULL && em_control != NULL);
    ComputedStyle rem_html_style = style_for_node(
        &rem_font_sheet, rem_html, NULL);
    ComputedStyle rem_body_style = style_for_node(
        &rem_font_sheet, rem_body, &rem_html_style);
    ComputedStyle rem_parent_style = style_for_node(
        &rem_font_sheet, rem_parent, &rem_body_style);
    ComputedStyle rem_long_style = style_for_node(
        &rem_font_sheet, rem_long, &rem_parent_style);
    ComputedStyle rem_short_style = style_for_node(
        &rem_font_sheet, rem_short, &rem_parent_style);
    ComputedStyle em_control_style = style_for_node(
        &rem_font_sheet, em_control, &rem_parent_style);
    CHECK(rem_html_style.font_size == 10
          && rem_html_style.root_font_size == 10
          && computed_style_font_size_fixed(&rem_long_style) == 18 * 64
          && computed_style_font_size_fixed(&rem_short_style) == 18 * 64
          && computed_style_font_size_fixed(&em_control_style) == 1613
          && layout_build(&rem_font_layout, &budget,
                          &rem_font_document, &rem_font_sheet,
                          NULL, NULL, 480));
    const DrawCommand *rem_long_command = text_command(
        &rem_font_layout, "REM-LONG");
    const DrawCommand *rem_short_command = text_command(
        &rem_font_layout, "REM-SHORT");
    const DrawCommand *em_control_command = text_command(
        &rem_font_layout, "EM-CONTROL");
    CHECK(rem_long_command != NULL && rem_short_command != NULL
          && em_control_command != NULL
          && draw_command_text_font_size_fixed(rem_long_command) == 18 * 64
          && draw_command_text_font_size_fixed(rem_short_command) == 18 * 64
          && draw_command_text_font_size_fixed(em_control_command) == 1613);
    layout_destroy(&rem_font_layout);
    static const char late_root_font[] = "html{font-size:75%}";
    CHECK(stylesheet_add_css(&rem_font_sheet, late_root_font,
                             strlen(late_root_font)));
    rem_html_style = style_for_node(&rem_font_sheet, rem_html, NULL);
    rem_body_style = style_for_node(
        &rem_font_sheet, rem_body, &rem_html_style);
    rem_parent_style = style_for_node(
        &rem_font_sheet, rem_parent, &rem_body_style);
    rem_long_style = style_for_node(
        &rem_font_sheet, rem_long, &rem_parent_style);
    rem_short_style = style_for_node(
        &rem_font_sheet, rem_short, &rem_parent_style);
    em_control_style = style_for_node(
        &rem_font_sheet, em_control, &rem_parent_style);
    CHECK(rem_html_style.font_size == 12
          && rem_html_style.root_font_size == 12
          && computed_style_font_size_fixed(&rem_long_style) == 1382
          && computed_style_font_size_fixed(&rem_short_style) == 1382
          && computed_style_font_size_fixed(&em_control_style) == 1613
          && layout_build(&rem_font_layout, &budget,
                          &rem_font_document, &rem_font_sheet,
                          NULL, NULL, 480));
    rem_long_command = text_command(&rem_font_layout, "REM-LONG");
    rem_short_command = text_command(&rem_font_layout, "REM-SHORT");
    em_control_command = text_command(&rem_font_layout, "EM-CONTROL");
    CHECK(rem_long_command != NULL && rem_short_command != NULL
          && em_control_command != NULL
          && draw_command_text_font_size_fixed(rem_long_command) == 1382
          && draw_command_text_font_size_fixed(rem_short_command) == 1382
          && draw_command_text_font_size_fixed(em_control_command) == 1613);
    layout_destroy(&rem_font_layout);
    stylesheet_destroy(&rem_font_sheet);
    document_destroy(&rem_font_document);
    CHECK(budget.current == 0);

    puts("test: multi-layer background shorthand keeps the base image layer");
    /* A representative signup banner has two comma-separated url layers with
       the fill color in the final layer.  The engine paints one background-image
       slot, so it keeps the bottommost (last) url layer -- the wide scene the
       author positions "-970px" -- not the small "left top" overlay, and it
       reads that layer's own position, repeat, and the final color.  The
       media rule (single-quoted url, lone negative-length position) must win
       at this viewport so the position resolves to -970px, not the base
       rule's unparseable "center right". */
    static const char multilayer_bg_page[] =
        "<!doctype html><style>body{margin:0}"
        ".infobar.listingsignupbar{background:"
        "url(\"../listingsignupbar-orbit-2.png\") left top no-repeat,"
        "url('../listingsignupbar-orbit-1.png') center right no-repeat "
        "#fefbda!important}"
        "@media screen and (max-width:1170px){.infobar.listingsignupbar{"
        "background:url(\"../listingsignupbar-orbit-2.png\") left top no-repeat,"
        "url('../listingsignupbar-orbit-1.png') -970px no-repeat "
        "#fefbda!important}}</style><body>"
        "<section id=signupbar class=\"infobar listingsignupbar\">"
        "SIGNUP</section></body>";
    PocDocument multilayer_bg_document;
    Stylesheet multilayer_bg_sheet;
    CHECK(document_parse(&multilayer_bg_document, &budget, multilayer_bg_page,
                         strlen(multilayer_bg_page), 30)
          && stylesheet_build(&multilayer_bg_sheet, &budget,
                              &multilayer_bg_document, 480));
    lxb_dom_node_t *signupbar = find_id(
        lxb_dom_interface_node(multilayer_bg_document.html), "signupbar");
    CHECK(signupbar != NULL);
    ComputedStyle signupbar_style = style_for_node(
        &multilayer_bg_sheet, signupbar, NULL);
    CHECK(signupbar_style.has_background
          && signupbar_style.background == 0xfefbda
          && signupbar_style.background_image != NULL
          && strcmp(signupbar_style.background_image,
                    "../listingsignupbar-orbit-1.png") == 0
          && signupbar_style.background_position_x == -970
          && signupbar_style.background_position_y == 0
          && (signupbar_style.background_size_flags
              & STYLE_BACKGROUND_POSITION_PIXELS)
          && (signupbar_style.background_size_flags
              & STYLE_BACKGROUND_NO_REPEAT_X)
          && (signupbar_style.background_size_flags
              & STYLE_BACKGROUND_NO_REPEAT_Y)
          && signupbar_style.background_fit == 0);
    stylesheet_destroy(&multilayer_bg_sheet);
    document_destroy(&multilayer_bg_document);
    CHECK(budget.current == 0);

    puts("test: unparseable declaration falls back within its own block");
    /* An unparseable `background` falls back to a background the SAME
       declaration block already set -- old.reddit's "BECOME A REDDITOR"
       button ships exactly that chain in one block -- but must NOT reach
       across rules.  "Unparseable" here means "this engine could not parse
       it", which is a larger set than "invalid CSS": mastodon's media pills
       use the valid relative-colour form `rgb(from #000 r g b/65%)`, and
       letting that reach across rules resurrected a light UA button
       background and repainted the dark "Hide" pill opaque white. */
    static const char invalid_fallback_page[] =
        "<!doctype html><style>body{margin:0}"
        ".primary-button{background:#ff9770;"
        "background:linear-gradient(to bottom,#ff9770,#ff7d4d)}"
        ".exotic{background:#336699;"
        "background:radial-gradient(circle,#fff,#000)}"
        ".ghosted{background:#123456;background:var(--not-defined)}"
        /* Cross-rule: the unparseable value must still clear, exactly as it
           did before the in-block fallback existed, so the cascade under it
           is unaffected.  This is the mastodon media-pill shape. */
        ".pill{background:#c0c0c0}"
        ".pill.pill-skin{background:rgb(from #000 r g b/65%)}"
        ".framed{border:2px solid #112233}"
        ".framed{border:var(--not-defined)}"
        ".edged{border:3px solid #334455}"
        ".edged{border-width:var(--not-defined)}"
        ".tracked{grid-template-columns:repeat(3,80px)}"
        ".tracked{grid-template-columns:var(--not-defined)}"
        "</style><body>"
        "<a id=fallback-primary class=primary-button>BECOME A REDDITOR</a>"
        "<a id=fallback-exotic class=exotic>E</a>"
        "<a id=fallback-ghosted class=ghosted>G</a>"
        "<a id=fallback-pill class=\"pill pill-skin\">Hide</a>"
        "<a id=fallback-framed class=framed>F</a>"
        "<a id=fallback-edged class=edged>E</a>"
        "<div id=fallback-tracked class=tracked><i>T</i></div></body>";
    PocDocument invalid_fallback_document;
    Stylesheet invalid_fallback_sheet;
    CHECK(document_parse(&invalid_fallback_document, &budget,
                         invalid_fallback_page,
                         strlen(invalid_fallback_page), 30)
          && stylesheet_build(&invalid_fallback_sheet, &budget,
                              &invalid_fallback_document, 480));
    lxb_dom_node_t *invalid_fallback_root =
        lxb_dom_interface_node(invalid_fallback_document.html);
    /* Both static linear and centred radial gradients now take precedence
       over their same-block solid fallbacks. */
    ComputedStyle fallback_primary_style = style_for_node(
        &invalid_fallback_sheet,
        find_id(invalid_fallback_root, "fallback-primary"), NULL);
    const StyleGradient *fallback_primary_gradient =
        stylesheet_background_gradient(
            &invalid_fallback_sheet, &fallback_primary_style);
    CHECK(fallback_primary_style.background_image_kind
              == STYLE_BACKGROUND_IMAGE_GRADIENT
          && fallback_primary_gradient != NULL
          && fallback_primary_gradient->stop_count == 2
          && fallback_primary_gradient->angle == 180
          && fallback_primary_gradient->stop_argb[0]
             == 0xffff9770u
          && fallback_primary_gradient->stop_argb[1]
             == 0xffff7d4du);
    ComputedStyle fallback_exotic_style = style_for_node(
        &invalid_fallback_sheet,
        find_id(invalid_fallback_root, "fallback-exotic"), NULL);
    const StyleGradient *fallback_exotic_gradient =
        stylesheet_background_gradient(
            &invalid_fallback_sheet, &fallback_exotic_style);
    CHECK(fallback_exotic_style.background_image_kind
              == STYLE_BACKGROUND_IMAGE_GRADIENT
          && fallback_exotic_gradient != NULL
          && (fallback_exotic_gradient->angle
              & STYLE_GRADIENT_RADIAL) != 0
          && fallback_exotic_gradient->stop_count == 2);
    ComputedStyle fallback_ghosted_style = style_for_node(
        &invalid_fallback_sheet,
        find_id(invalid_fallback_root, "fallback-ghosted"), NULL);
    /* Substitution happens after the declaration has won the cascade.  An
       undefined var() therefore resets the property to its initial value;
       it cannot uncover either an earlier declaration in the same block or
       one from a lower-priority rule. */
    CHECK(!fallback_ghosted_style.has_background);
    ComputedStyle fallback_pill_style = style_for_node(
        &invalid_fallback_sheet,
        find_id(invalid_fallback_root, "fallback-pill"), NULL);
    CHECK(!fallback_pill_style.has_background);
    ComputedStyle fallback_framed_style = style_for_node(
        &invalid_fallback_sheet,
        find_id(invalid_fallback_root, "fallback-framed"), NULL);
    CHECK(fallback_framed_style.border.top == 2
          && fallback_framed_style.border.left == 2
          && stylesheet_border_color(
                 &invalid_fallback_sheet, &fallback_framed_style,
                 STYLE_BORDER_TOP, NULL) == 0x112233);
    ComputedStyle fallback_edged_style = style_for_node(
        &invalid_fallback_sheet,
        find_id(invalid_fallback_root, "fallback-edged"), NULL);
    CHECK(fallback_edged_style.border.top == 3
          && fallback_edged_style.border.right == 3
          && fallback_edged_style.border.bottom == 3
          && fallback_edged_style.border.left == 3
          && stylesheet_border_color(
                 &invalid_fallback_sheet, &fallback_edged_style,
                 STYLE_BORDER_TOP, NULL) == 0x334455);
    ComputedStyle fallback_tracked_style = style_for_node(
        &invalid_fallback_sheet,
        find_id(invalid_fallback_root, "fallback-tracked"), NULL);
    CHECK(computed_style_grid_column_count(&fallback_tracked_style) == 0);
    stylesheet_destroy(&invalid_fallback_sheet);
    document_destroy(&invalid_fallback_document);
    CHECK(budget.current == 0);

    puts("test: UA button is an atomic inline box with positioned icon");
    static const char atomic_button_page[] =
        "<!doctype html><style>body{margin:0}.search-button{position:relative;"
        "width:40px;height:40px;padding:0;text-indent:-5000px}"
        ".search-button svg{position:absolute;left:12px;top:12px;"
        "width:16px;height:16px}</style><body>"
        "<button id=atomic-button class=search-button>SEARCH"
        "<svg id=atomic-icon viewBox='0 0 16 16' "
        "xmlns='http://www.w3.org/2000/svg'>"
        "<path fill='#123456' d='M0 0h16v16H0z'/></svg></button>"
        "<span id=atomic-tail>TAIL</span></body>";
    PocDocument atomic_button_document;
    Stylesheet atomic_button_sheet;
    ImageResources atomic_button_images = {0};
    LayoutDocument atomic_button_layout;
    CHECK(document_parse(&atomic_button_document, &budget,
                         atomic_button_page, strlen(atomic_button_page), 31)
          && stylesheet_build(&atomic_button_sheet, &budget,
                              &atomic_button_document, 480));
    lxb_dom_node_t *ua_atomic_button = find_id(
        lxb_dom_interface_node(atomic_button_document.html), "atomic-button");
    lxb_dom_node_t *ua_atomic_icon = find_id(
        lxb_dom_interface_node(atomic_button_document.html), "atomic-icon");
    CHECK(ua_atomic_button != NULL && ua_atomic_icon != NULL);
    ComputedStyle atomic_button_style = style_for_node(
        &atomic_button_sheet, ua_atomic_button, NULL);
    CHECK(atomic_button_style.display == DISPLAY_INLINE_BLOCK
          && images_load_external(
              &atomic_button_document, &atomic_button_sheet,
              &atomic_button_images, &budget,
              "https://atomic-button.test/", "https://atomic-button.test/",
              NULL, 8, 64 * 1024, 32 * 1024, 64 * 1024, 1000,
              NULL, NULL)
          && layout_build(&atomic_button_layout, &budget,
                          &atomic_button_document, &atomic_button_sheet,
                          NULL, &atomic_button_images, 480));
    const LayoutNodeBox *atomic_button_box = layout_box_for_node(
        &atomic_button_layout, ua_atomic_button);
    const LayoutNodeBox *atomic_icon_box = layout_box_for_node(
        &atomic_button_layout, ua_atomic_icon);
    const ImageResource *atomic_icon_resource = images_find_node(
        &atomic_button_images, ua_atomic_icon);
    bool atomic_icon_drawn = false;
    for (size_t i = 0; i < atomic_button_layout.count; i++) {
        const DrawCommand *command = &atomic_button_layout.commands[i];
        if (command->type == DRAW_IMAGE
            && command->image == atomic_icon_resource
            && atomic_icon_box != NULL
            && command->x == atomic_icon_box->x
            && command->y == atomic_icon_box->y
            && command->width == 16 && command->height == 16) {
            atomic_icon_drawn = true;
            break;
        }
    }
    if (atomic_button_box == NULL || atomic_icon_box == NULL
        || atomic_icon_resource == NULL
        || !image_resource_available(atomic_icon_resource)
        || atomic_button_box->width != 40 || atomic_button_box->height != 40
        || atomic_icon_box->x != atomic_button_box->x
                                  + atomic_button_style.border.left + 12
        || atomic_icon_box->y != atomic_button_box->y
                                  + atomic_button_style.border.top + 12
        || command_x(&atomic_button_layout, "SEARCH") >= -1000
        || command_x(&atomic_button_layout, "TAIL") < 40
        || command_x(&atomic_button_layout, "TAIL") >= 100
        || atomic_button_layout.control_count != 1 || !atomic_icon_drawn) {
        fprintf(stderr,
                "atomic button=%d,%d %dx%d icon=%d,%d %dx%d search=%d "
                "tail=%d controls=%zu drawn=%d resource=%d\n",
                atomic_button_box == NULL ? -1 : atomic_button_box->x,
                atomic_button_box == NULL ? -1 : atomic_button_box->y,
                atomic_button_box == NULL ? -1 : atomic_button_box->width,
                atomic_button_box == NULL ? -1 : atomic_button_box->height,
                atomic_icon_box == NULL ? -1 : atomic_icon_box->x,
                atomic_icon_box == NULL ? -1 : atomic_icon_box->y,
                atomic_icon_box == NULL ? -1 : atomic_icon_box->width,
                atomic_icon_box == NULL ? -1 : atomic_icon_box->height,
                command_x(&atomic_button_layout, "SEARCH"),
                command_x(&atomic_button_layout, "TAIL"),
                atomic_button_layout.control_count, atomic_icon_drawn,
                atomic_icon_resource != NULL
                    && image_resource_available(atomic_icon_resource));
    }
    CHECK(atomic_button_box != NULL && atomic_icon_box != NULL
          && atomic_icon_resource != NULL
          && image_resource_available(atomic_icon_resource)
          && atomic_button_box->width == 40
          && atomic_button_box->height == 40
          && atomic_icon_box->x == atomic_button_box->x
                                  + atomic_button_style.border.left + 12
          && atomic_icon_box->y == atomic_button_box->y
                                  + atomic_button_style.border.top + 12
          && command_x(&atomic_button_layout, "SEARCH") < -1000
          && command_x(&atomic_button_layout, "TAIL") >= 40
          && command_x(&atomic_button_layout, "TAIL") < 100
          && atomic_button_layout.control_count == 1
          && atomic_icon_drawn);
    layout_destroy(&atomic_button_layout);
    images_destroy(&atomic_button_images);
    stylesheet_destroy(&atomic_button_sheet);
    document_destroy(&atomic_button_document);
    CHECK(budget.current == 0);

    puts("test: bounded TrueType fonts");
    FontSet fonts;
    CHECK(font_set_load(&fonts, &budget, TILEFINCH_TEST_SANS_FONT,
                        TILEFINCH_TEST_SERIF_FONT, TILEFINCH_TEST_SANS_ITALIC_FONT,
                        TILEFINCH_TEST_SANS_BOLD_FONT,
                        TILEFINCH_TEST_SERIF_BOLD_FONT,
                        TILEFINCH_TEST_METRIC_SANS_FONT,
                        TILEFINCH_TEST_METRIC_SANS_BOLD_FONT,
                        2 * MIB));
    const FontFace *sans = font_set_face(&fonts, FONT_SANS);
    CHECK(sans != NULL);
    FontFamily web_humanist = FONT_SANS;
    WebFontSet metric_web_fonts = {0};
    CHECK(font_family_web(1, FONT_HUMANIST_SANS, &web_humanist)
          && font_family_is_web(web_humanist)
          && font_family_web_slot(web_humanist) == 1
          && font_family_web_fallback(web_humanist)
                 == FONT_HUMANIST_SANS
          && font_context_metric_family(
                 &metric_web_fonts, web_humanist, sans)
                 == FONT_HUMANIST_SANS
          && font_context_metric_family(
                 &metric_web_fonts, web_humanist,
                 &metric_web_fonts.families[1].regular)
                 == FONT_SANS);
    CHECK(font_set_face_style(&fonts, FONT_SANS, true)
          == &fonts.sans_italic);
    CHECK(font_set_face_variant(&fonts, FONT_SANS, false, true)
          == &fonts.sans_bold);
    CHECK(font_set_face_variant(&fonts, FONT_SERIF, false, true)
          == &fonts.serif_bold);
    CHECK(font_set_face(&fonts, FONT_METRIC_SANS) == &fonts.metric_sans
          && font_set_face_variant(&fonts, FONT_METRIC_SANS, false, true)
                 == &fonts.metric_sans_bold);
    CHECK(font_set_face(&fonts, FONT_HUMANIST_SANS) == sans
          && font_set_face_style(&fonts, FONT_HUMANIST_SANS, true)
                 == &fonts.sans_italic
          && font_set_face_variant(
                 &fonts, FONT_HUMANIST_SANS, false, true)
                 == &fonts.sans_bold);
    CHECK(font_text_width(sans, "WWW", 3, 16, false)
          > font_text_width(sans, "iii", 3, 16, false));
    int native_bar_advance = font_text_width_at_size_fixed(
        sans, "|", 1, 12 * 64, false);
    int humanist_bar_advance = font_text_width_for_family_at_size_fixed(
        sans, FONT_HUMANIST_SANS, "|", 1, 12 * 64, false, false);
    CHECK(native_bar_advance > 0
          && humanist_bar_advance == 349
          && humanist_bar_advance > native_bar_advance
          && font_glyph_advance_for_family_at_size_fixed(
                 FONT_HUMANIST_SANS, '|', 12 * 64,
                 false, false, native_bar_advance)
                 == humanist_bar_advance
          && font_text_width_for_family_at_size_fixed(
                 sans, FONT_SANS, "|", 1, 12 * 64, false, false)
                 == native_bar_advance);
    CHECK(font_metric_height(sans, 12) == 14
          && font_metric_height(sans, 16) == 19
          && font_line_height(sans, 12) == 15
          && font_line_height(sans, 16) == 20);
    int sans_ascent = font_ascent(sans, 16);
    int sans_metric_height = font_metric_height(sans, 16);
    CHECK(sans_ascent > 0 && sans_metric_height > 0
          && font_line_baseline(sans, 16, sans_metric_height)
                 == sans_ascent
          && font_line_baseline(sans, 16, sans_metric_height - 7)
                 == sans_ascent - 4
          && font_line_baseline(sans, 16, sans_metric_height + 7)
                 == sans_ascent + 3
          && font_metric_height(NULL, 16) == -1
          && font_line_baseline(NULL, 16, sans_metric_height) == -1);
    FontGlyph glyph;
    CHECK(font_glyph_load(sans, 'A', 18, false, &glyph));
    CHECK(glyph.pixels != NULL && glyph.width > 0 && glyph.height > 0
          && glyph.advance > 0);
    font_glyph_destroy(sans, &glyph);
    /* The packaged PSP face is Latin-only. Precomposed Hangul must use the
       bounded procedural fallback rather than its .notdef rectangle. */
    static const char sample_hangul[] = "\xed\x85\x8c\xec\x8a\xa4\xed\x8a\xb8";
    CHECK(font_text_width(
              sans, sample_hangul, sizeof(sample_hangul) - 1,
              16, false) == 16 * 3);
    CHECK(font_glyph_load(sans, 0xcea3u, 16, false, &glyph));
    size_t hangul_ink = 0;
    for (int pixel = 0; pixel < glyph.width * glyph.height; pixel++) {
        if (glyph.pixels[pixel] != 0) hangul_ink++;
    }
    CHECK(glyph.width == 16 && glyph.height == 16
          && glyph.advance == 16 && hangul_ink > 20);
    font_glyph_destroy(sans, &glyph);
    /* Common Japanese/Chinese and emoji use a generated 16-pixel bitmap
       subset only when the selected face has no authored glyph. */
    static const char japanese_fallback[] =
        "\xe6\x97\xa5\xe6\x9c\xac\xe8\xaa\x9e";
    static const char chinese_fallback[] =
        "\xe4\xb8\xad\xe5\x9b\xbd";
    static const char emoji_fallback[] = "\xf0\x9f\x98\x80";
    CHECK(font_text_width(
              sans, japanese_fallback, sizeof(japanese_fallback) - 1,
              16, false) == 16 * 3);
    CHECK(font_text_width(
              sans, chinese_fallback, sizeof(chinese_fallback) - 1,
              16, false) == 16 * 2);
    CHECK(font_text_width(
              sans, emoji_fallback, sizeof(emoji_fallback) - 1,
              16, false) == 16);
    const unsigned fallback_probes[] = {
        0x3042u, /* Japanese hiragana A */
        0x65e5u, /* Japanese/Chinese sun or day */
        0x4e2du, /* Chinese middle */
        0x1f600u /* grinning face */
    };
    for (size_t probe = 0;
         probe < sizeof(fallback_probes) / sizeof(fallback_probes[0]);
         probe++) {
        CHECK(font_glyph_load(
            sans, fallback_probes[probe], 16, false, &glyph));
        size_t fallback_ink = 0;
        for (int pixel = 0; pixel < glyph.width * glyph.height; pixel++) {
            if (glyph.pixels[pixel] != 0) fallback_ink++;
        }
        CHECK(glyph.width == 16 && glyph.height == 16
              && glyph.advance == 16 && fallback_ink > 8);
        font_glyph_destroy(sans, &glyph);
    }
    /* Keep emoji presentation selectors and joiners from becoming visible
       tofu or consuming an extra em. Complex sequences remain monochrome. */
    CHECK(font_glyph_load(sans, 0xfe0fu, 16, false, &glyph)
          && glyph.pixels == NULL && glyph.width == 0 && glyph.height == 0
          && glyph.advance_fixed == 0);
    font_glyph_destroy(sans, &glyph);
    CHECK(font_glyph_load(sans, 0x200du, 16, false, &glyph)
          && glyph.pixels == NULL && glyph.width == 0 && glyph.height == 0
          && glyph.advance_fixed == 0);
    font_glyph_destroy(sans, &glyph);

    puts("test: fractional advances survive adjacent inline run boundaries");
    static const char fractional_page[] =
        "<!doctype html><style>html,body{margin:0;padding:0}"
        "body{font-family:Arial,sans-serif;font-size:17px;line-height:20px}"
        "</style><body><span>i</span><span>i</span><span>i</span>"
        "<div><span style='display:inline-block'>b</span><span>c</span></div>"
        "<div><span>b</span><span>c</span></div></body>";
    PocDocument fractional_document;
    Stylesheet fractional_sheet;
    LayoutDocument fractional_layout;
    CHECK(document_parse(&fractional_document, &budget,
                         fractional_page, strlen(fractional_page), 19)
          && stylesheet_build(&fractional_sheet, &budget,
                              &fractional_document, 480)
          && layout_build(&fractional_layout, &budget,
                          &fractional_document, &fractional_sheet,
                          &fonts, NULL, 480));
    const DrawCommand *fractional_runs[3] = {0};
    size_t fractional_count = 0;
    const DrawCommand *atomic_letters[4] = {0};
    size_t atomic_letter_count = 0;
    for (size_t i = 0;
         i < fractional_layout.count; i++) {
        const DrawCommand *command = &fractional_layout.commands[i];
        if (fractional_count < 3 && command->type == DRAW_TEXT
            && command->text_length == 1
            && command->text != NULL && command->text[0] == 'i') {
            fractional_runs[fractional_count++] = command;
        }
        if (atomic_letter_count < 4 && command->type == DRAW_TEXT
            && command->text_length == 1 && command->text != NULL
            && (command->text[0] == 'b' || command->text[0] == 'c')) {
            atomic_letters[atomic_letter_count++] = command;
        }
    }
    const FontFace *metric_sans = font_set_face(
        &fonts, FONT_METRIC_SANS);
    int fractional_advance = font_text_width_fixed(
        metric_sans, "i", 1, 17, false);
    DrawCommand packed_fraction = {.type = DRAW_TEXT};
    draw_command_set_text_x_fixed(&packed_fraction, 123 * 64 + 17);
    draw_command_set_text_font_size_fixed(&packed_fraction, 13 * 64 + 21);
    CHECK(fractional_count == 3 && fractional_advance > 0
          && fractional_advance % 64 != 0
          && draw_command_text_x_fixed(fractional_runs[1])
                 == draw_command_text_x_fixed(fractional_runs[0])
                    + fractional_advance
          && draw_command_text_x_fixed(fractional_runs[2])
                 == draw_command_text_x_fixed(fractional_runs[0])
                    + fractional_advance * 2
          && (fractional_runs[1]->radius
              & LAYOUT_TEXT_X_FRACTION_MASK)
                 == (draw_command_text_x_fixed(fractional_runs[1]) & 63)
          && atomic_letter_count == 4
          && draw_command_text_x_fixed(atomic_letters[1])
                 - draw_command_text_x_fixed(atomic_letters[0])
             == draw_command_text_x_fixed(atomic_letters[3])
                    - draw_command_text_x_fixed(atomic_letters[2])
          && draw_command_text_x_fixed(&packed_fraction) == 123 * 64 + 17
          && draw_command_text_font_size_fixed(&packed_fraction)
                 == 13 * 64 + 21);
    layout_destroy(&fractional_layout);
    stylesheet_destroy(&fractional_sheet);
    document_destroy(&fractional_document);

    puts("test: fractional normal line heights accumulate before rounding");
    static const char fractional_line_page[] =
        "<!doctype html><style>html,body{margin:0;padding:0}"
        "body{font-family:Arial,sans-serif;font-size:11pt}"
        "div{width:1px}</style><body><div>W W W</div></body>";
    PocDocument fractional_line_document;
    Stylesheet fractional_line_sheet;
    LayoutDocument fractional_line_layout;
    CHECK(document_parse(&fractional_line_document, &budget,
                         fractional_line_page,
                         strlen(fractional_line_page), 23)
          && stylesheet_build(&fractional_line_sheet, &budget,
                              &fractional_line_document, 480)
          && layout_build(&fractional_line_layout, &budget,
                          &fractional_line_document,
                          &fractional_line_sheet, &fonts, NULL, 480));
    const DrawCommand *fractional_line_runs[3] = {0};
    size_t fractional_line_count = 0;
    for (size_t i = 0; i < fractional_line_layout.count
         && fractional_line_count < 3; i++) {
        const DrawCommand *command = &fractional_line_layout.commands[i];
        if (command->type == DRAW_TEXT && command->text_length == 1
            && command->text != NULL && command->text[0] == 'W') {
            fractional_line_runs[fractional_line_count++] = command;
        }
    }
    int accumulated_line_height = font_line_height_fixed_at_size(
        metric_sans, 939);
    CHECK(fractional_line_count == 3 && accumulated_line_height > 0
          && accumulated_line_height % 64 != 0
          && fractional_line_runs[1]->y - fractional_line_runs[0]->y
                 == accumulated_line_height / 64
          && fractional_line_runs[2]->y - fractional_line_runs[0]->y
                 == accumulated_line_height * 2 / 64
          && fractional_line_runs[2]->y - fractional_line_runs[0]->y
                 < 2 * ((accumulated_line_height + 63) / 64));
    layout_destroy(&fractional_line_layout);
    stylesheet_destroy(&fractional_line_sheet);
    document_destroy(&fractional_line_document);

    puts("test: atomic inlines honor fractional line boundaries");
    static const char atomic_boundary_page[] =
        "<!doctype html><style>html,body{margin:0;padding:0}"
        "body{font-family:Arial,sans-serif;font-size:11pt}"
        ".host{width:160px}.atomic-child{display:inline-block}"
        ".child{width:160px}</style><body>"
        "<div id=block-host class=host>x<div id=block-child class=child>"
        "This is some text that is wider than the host but has no words "
        "wider than the host.</div>z</div>"
        "<div id=atomic-host class=host>x <div id=atomic-child "
        "class='child atomic-child'>This is some text that is wider than "
        "the host but has no words wider than the host.</div> z</div>"
        "</body>";
    PocDocument atomic_boundary_document;
    Stylesheet atomic_boundary_sheet;
    LayoutDocument atomic_boundary_layout;
    CHECK(document_parse(
              &atomic_boundary_document, &budget, atomic_boundary_page,
              strlen(atomic_boundary_page), 24)
          && stylesheet_build(
              &atomic_boundary_sheet, &budget,
              &atomic_boundary_document, 480)
          && layout_build(
              &atomic_boundary_layout, &budget,
              &atomic_boundary_document, &atomic_boundary_sheet,
              &fonts, NULL, 480));
    lxb_dom_node_t *atomic_boundary_root =
        lxb_dom_interface_node(atomic_boundary_document.html);
    const LayoutNodeBox *block_host_box = layout_box_for_node(
        &atomic_boundary_layout,
        find_id(atomic_boundary_root, "block-host"));
    const LayoutNodeBox *block_child_box = layout_box_for_node(
        &atomic_boundary_layout,
        find_id(atomic_boundary_root, "block-child"));
    const LayoutNodeBox *atomic_host_box = layout_box_for_node(
        &atomic_boundary_layout,
        find_id(atomic_boundary_root, "atomic-host"));
    const LayoutNodeBox *atomic_child_box = layout_box_for_node(
        &atomic_boundary_layout,
        find_id(atomic_boundary_root, "atomic-child"));
    CHECK(block_host_box != NULL && block_child_box != NULL
          && atomic_host_box != NULL && atomic_child_box != NULL
          && atomic_child_box->y - atomic_host_box->y
                 == block_child_box->y - block_host_box->y);
    layout_destroy(&atomic_boundary_layout);
    stylesheet_destroy(&atomic_boundary_sheet);
    document_destroy(&atomic_boundary_document);

    puts("test: mixed inline font sizes share a text baseline");
    static const char inline_baseline_page[] =
        "<!doctype html><style>html,body{margin:0;padding:0}"
        "#large-inline{font-size:24px}#small-inline{font-size:12px}"
        "</style><body><span id=large-inline>BIG</span>"
        "<a id=small-inline href=/small>small</a></body>";
    PocDocument inline_baseline_document;
    Stylesheet inline_baseline_sheet;
    LayoutDocument inline_baseline_layout;
    CHECK(document_parse(
              &inline_baseline_document, &budget, inline_baseline_page,
              strlen(inline_baseline_page), 24)
          && stylesheet_build(
              &inline_baseline_sheet, &budget, &inline_baseline_document, 480)
          && layout_build(
              &inline_baseline_layout, &budget, &inline_baseline_document,
              &inline_baseline_sheet, &fonts, NULL, 480));
    const DrawCommand *large_inline_command = NULL;
    const DrawCommand *small_inline_command = NULL;
    const LinkRegion *small_inline_link = NULL;
    for (size_t i = 0; i < inline_baseline_layout.count; i++) {
        const DrawCommand *command = &inline_baseline_layout.commands[i];
        if (command->type != DRAW_TEXT || command->text == NULL) continue;
        if (command->text_length == 3
            && memcmp(command->text, "BIG", 3) == 0) {
            large_inline_command = command;
        } else if (command->text_length == 5
                   && memcmp(command->text, "small", 5) == 0) {
            small_inline_command = command;
        }
    }
    for (size_t i = 0; i < inline_baseline_layout.link_count; i++) {
        const LinkRegion *link = &inline_baseline_layout.links[i];
        if (link->url_length == 6
            && memcmp(link->url, "/small", 6) == 0) {
            small_inline_link = link;
            break;
        }
    }
    CHECK(large_inline_command != NULL && small_inline_command != NULL);
    int large_inline_baseline = large_inline_command->y
        + font_line_baseline_at_size(
              metric_sans,
              draw_command_text_font_size_fixed(large_inline_command),
              large_inline_command->height);
    int small_inline_baseline = small_inline_command->y
        + font_line_baseline_at_size(
              metric_sans,
              draw_command_text_font_size_fixed(small_inline_command),
              small_inline_command->height);
    CHECK(large_inline_baseline == small_inline_baseline
          && small_inline_link != NULL
          && small_inline_link->y == small_inline_command->y
          && small_inline_command->y > large_inline_command->y);
    layout_destroy(&inline_baseline_layout);
    stylesheet_destroy(&inline_baseline_sheet);
    document_destroy(&inline_baseline_document);

    puts("test: preformatted spaces and segment breaks are preserved");
    static const char preformatted_page[] =
        "<!doctype html><style>html,body{margin:0;padding:0}"
        "#preformatted{white-space:pre;font-size:16px;line-height:20px;"
        "margin:0}</style><body><pre id=preformatted>"
        " A\n  B\n\nC</pre></body>";
    PocDocument preformatted_document;
    Stylesheet preformatted_sheet;
    LayoutDocument preformatted_layout;
    CHECK(document_parse(
              &preformatted_document, &budget, preformatted_page,
              strlen(preformatted_page), 24)
          && stylesheet_build(
              &preformatted_sheet, &budget, &preformatted_document, 480)
          && layout_build(
              &preformatted_layout, &budget, &preformatted_document,
              &preformatted_sheet, &fonts, NULL, 480));
    const DrawCommand *preformatted_a = NULL;
    const DrawCommand *preformatted_b = NULL;
    const DrawCommand *preformatted_c = NULL;
    for (size_t i = 0; i < preformatted_layout.count; i++) {
        const DrawCommand *command = &preformatted_layout.commands[i];
        if (command->type != DRAW_TEXT || command->text_length != 1
            || command->text == NULL) continue;
        if (command->text[0] == 'A') preformatted_a = command;
        else if (command->text[0] == 'B') preformatted_b = command;
        else if (command->text[0] == 'C') preformatted_c = command;
    }
    CHECK(preformatted_a != NULL && preformatted_b != NULL
          && preformatted_c != NULL
          && preformatted_b->x > preformatted_a->x
          && preformatted_b->y - preformatted_a->y == 20
          && preformatted_c->y - preformatted_a->y == 60);
    layout_destroy(&preformatted_layout);
    stylesheet_destroy(&preformatted_sheet);
    document_destroy(&preformatted_document);

    puts("test: text-flow units, breaks, and indentation stay coherent");
    static const char text_flow_page[] =
        "<!doctype html><style>html,body{margin:0;padding:0}"
        "#ch{display:block;width:4ch;height:1px;font-size:20px}"
        "#indent{display:block;width:100px;text-indent:50%}"
        "#indent-atomic{display:block;width:100px;text-indent:50%}"
        "#indent-atomic span{display:inline-block;width:10px;height:10px}"
        "#keep{word-break:keep-all}#legacy{word-break:break-word}"
        "#font-basis{letter-spacing:1em;font:20px/1 sans-serif}"
        "#percent-margin{display:block;width:10px;height:1px;"
        "margin:0 0 0 25%}"
        "#cluster{display:block;width:0;overflow-wrap:anywhere;"
        "font-size:20px;line-height:24px}"
        "#breaks{font-size:16px;line-height:20px;margin:0}"
        "#breaks br{font-size:80px;line-height:100px}"
        "</style><body><div id=ch></div><div id=indent>I</div>"
        "<div id=indent-atomic><span id=indent-atomic-child></span></div>"
        "<div id=keep></div><div id=legacy></div>"
        "<div id=font-basis></div>"
        "<div id=percent-margin></div>"
        "<div id=cluster>\xe0\xa4\xb7\xe0\xa4\xbf"
        "\xe0\xa4\xb7\xe0\xa4\xbf</div>"
        "<div id=auto-dir dir=auto>\xd9\x81</div>"
        "<p id=breaks>A<br>B</p></body>";
    PocDocument text_flow_document;
    Stylesheet text_flow_sheet;
    LayoutDocument text_flow_layout;
    CHECK(document_parse(
              &text_flow_document, &budget, text_flow_page,
              strlen(text_flow_page), 24)
          && stylesheet_build(
              &text_flow_sheet, &budget, &text_flow_document, 480)
          && layout_build(
              &text_flow_layout, &budget, &text_flow_document,
              &text_flow_sheet, &fonts, NULL, 480));
    lxb_dom_node_t *text_flow_root =
        lxb_dom_interface_node(text_flow_document.html);
    lxb_dom_node_t *ch_node = find_id(text_flow_root, "ch");
    lxb_dom_node_t *indent_node = find_id(text_flow_root, "indent");
    lxb_dom_node_t *indent_atomic_child =
        find_id(text_flow_root, "indent-atomic-child");
    lxb_dom_node_t *keep_node = find_id(text_flow_root, "keep");
    lxb_dom_node_t *legacy_node = find_id(text_flow_root, "legacy");
    lxb_dom_node_t *font_basis_node =
        find_id(text_flow_root, "font-basis");
    lxb_dom_node_t *auto_dir_node =
        find_id(text_flow_root, "auto-dir");
    lxb_dom_node_t *percent_margin_node =
        find_id(text_flow_root, "percent-margin");
    const LayoutNodeBox *ch_box =
        layout_box_for_node(&text_flow_layout, ch_node);
    const LayoutNodeBox *indent_atomic_box =
        layout_box_for_node(&text_flow_layout, indent_atomic_child);
    const LayoutNodeBox *percent_margin_box =
        layout_box_for_node(&text_flow_layout, percent_margin_node);
    ComputedStyle indent_style =
        style_for_node(&text_flow_sheet, indent_node, NULL);
    ComputedStyle keep_style =
        style_for_node(&text_flow_sheet, keep_node, NULL);
    ComputedStyle legacy_style =
        style_for_node(&text_flow_sheet, legacy_node, NULL);
    ComputedStyle font_basis_style =
        style_for_node(&text_flow_sheet, font_basis_node, NULL);
    ComputedStyle auto_dir_style =
        style_for_node(&text_flow_sheet, auto_dir_node, NULL);
    int resolved_indent = 0;
    const FontFace *sans_face = font_context_face_variant(
        &fonts, NULL, FONT_SANS, false, false);
    int zero_advance_fixed = font_text_width_for_family_at_size_fixed(
        sans_face, FONT_SANS, "0", 1, 20 * 64, false, false);
    int expected_ch_width = zero_advance_fixed > 0
        ? ((zero_advance_fixed + 63) / 64) * 4 : 40;
    const DrawCommand *break_a = NULL;
    const DrawCommand *break_b = NULL;
    const DrawCommand *indent_i = NULL;
    const DrawCommand *cluster_first = NULL;
    const DrawCommand *cluster_second = NULL;
    for (size_t i = 0; i < text_flow_layout.count; i++) {
        const DrawCommand *command = &text_flow_layout.commands[i];
        if (command->type != DRAW_TEXT || command->text == NULL) continue;
        if (command->text_length == 1) {
            if (command->text[0] == 'A') break_a = command;
            else if (command->text[0] == 'B') break_b = command;
            else if (command->text[0] == 'I') indent_i = command;
        } else if (command->text_length == 6
                   && memcmp(
                       command->text, "\xe0\xa4\xb7\xe0\xa4\xbf", 6)
                          == 0) {
            if (cluster_first == NULL) cluster_first = command;
            else cluster_second = command;
        }
    }
    CHECK(ch_box != NULL && ch_box->width == expected_ch_width);
    CHECK(style_length_resolve(
              &text_flow_sheet, indent_style.text_indent,
              100, &resolved_indent)
          && resolved_indent == 50);
    CHECK(indent_i != NULL && indent_i->x == 50);
    CHECK(indent_atomic_box != NULL && indent_atomic_box->x == 50);
    CHECK(keep_style.word_break_mode == WORD_BREAK_KEEP_ALL
          && legacy_style.word_break_mode == WORD_BREAK_LEGACY);
    CHECK(font_basis_style.font_size == 20
          && font_basis_style.letter_spacing == 20);
    CHECK(percent_margin_box != NULL && percent_margin_box->x == 120);
    CHECK(computed_style_direction_rtl(&auto_dir_style));
    CHECK(cluster_first != NULL && cluster_second != NULL
          && cluster_second->y > cluster_first->y);
    CHECK(break_a != NULL && break_b != NULL
          && break_b->y - break_a->y == 20);
    layout_destroy(&text_flow_layout);
    stylesheet_destroy(&text_flow_sheet);
    document_destroy(&text_flow_document);

    puts("test: synthetic italic shear is fractionally continuous");
    static const char italic_raster_page[] =
        "<!doctype html><style>html,body{margin:0;background:#000}"
        "#bar{display:block;color:#fff;font:italic 32px serif;"
        "line-height:40px}</style><span id=bar>|</span>";
    PocDocument italic_raster_document;
    Stylesheet italic_raster_sheet;
    LayoutDocument italic_raster_layout;
    TileCache italic_raster_cache;
    uint16_t *italic_raster_frame = NULL;
    CHECK(document_parse(
              &italic_raster_document, &budget, italic_raster_page,
              strlen(italic_raster_page), 17)
          && stylesheet_build(
                 &italic_raster_sheet, &budget,
                 &italic_raster_document, 96)
          && layout_build(
                 &italic_raster_layout, &budget,
                 &italic_raster_document, &italic_raster_sheet,
                 &fonts, NULL, 96)
          && tile_cache_init(
                 &italic_raster_cache, &budget,
                 &italic_raster_layout, 4));
    italic_raster_frame = budget_calloc(
        &budget, 96u * 64u, sizeof(*italic_raster_frame));
    CHECK(italic_raster_frame != NULL
          && tile_cache_set_frame(
                 &italic_raster_cache, italic_raster_frame, 96u * 64u)
          && tile_cache_render_frame(
                 &italic_raster_cache, 0, 96, 64, NULL));
    int previous_centroid = 0;
    int first_centroid = 0;
    int last_centroid = 0;
    bool have_previous_centroid = false;
    bool have_first_centroid = false;
    size_t continuous_pairs = 0;
    int maximum_centroid_step = 0;
    for (int y = 10; y < 34; y++) {
        uint64_t weighted_x = 0;
        uint64_t coverage = 0;
        for (int x = 0; x < 24; x++) {
            uint16_t pixel = italic_raster_frame[(size_t) y * 96u + x];
            unsigned weight = ((pixel >> 11) & 31u) * 2u
                + ((pixel >> 5) & 63u) + (pixel & 31u) * 2u;
            coverage += weight;
            weighted_x += (uint64_t) weight * (unsigned) x * 256u;
        }
        if (coverage == 0) {
            have_previous_centroid = false;
            continue;
        }
        int centroid = (int) (weighted_x / coverage);
        if (!have_first_centroid) {
            first_centroid = centroid;
            have_first_centroid = true;
        }
        last_centroid = centroid;
        if (have_previous_centroid) {
            int step = centroid - previous_centroid;
            if (step < 0) step = -step;
            if (step > maximum_centroid_step) maximum_centroid_step = step;
            continuous_pairs++;
        }
        previous_centroid = centroid;
        have_previous_centroid = true;
    }
    /* Integer-only shear held several rows and then jumped a full pixel
       (256 units). Fractional coverage keeps the vertical stroke continuous
       across the same middle-glyph rows. */
    CHECK(continuous_pairs >= 8u && maximum_centroid_step < 192
          && have_first_centroid && first_centroid > last_centroid);
    tile_cache_destroy(&italic_raster_cache);
    budget_free(&budget, italic_raster_frame);
    layout_destroy(&italic_raster_layout);
    stylesheet_destroy(&italic_raster_sheet);
    document_destroy(&italic_raster_document);

    font_set_destroy(&fonts);
    CHECK(budget.current == 0);

    FontSet missing_primary_fonts;
    CHECK(font_set_load(
              &missing_primary_fonts, &budget,
              "/definitely/missing/sans.ttf", TILEFINCH_TEST_SERIF_FONT,
              NULL, NULL, NULL, NULL, NULL, 2 * MIB)
          && !missing_primary_fonts.sans.loaded
          && missing_primary_fonts.serif.loaded);
    font_set_destroy(&missing_primary_fonts);
    CHECK(budget.current == 0);

    return 0;
}
