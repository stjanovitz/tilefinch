static int test_stylesheet_ordered_append(Budget *budget)
{
    static const char prefix_page[] =
        "<!doctype html><style>.x{color:#111111}</style><body></body>";
    static const char suffix_page[] =
        "<!doctype html><style id=suffix>"
        ".x{color:#333333}.x::before{content:'prefix';"
        "background-image:url(icon.png)}.x{--local-tone:#123456}</style>"
        "<body><span id=target class=x>target</span></body>";
    static const char contextual_page[] =
        "<!doctype html><style id=contextual>"
        ":root{--late-tone:#abcdef}</style><body></body>";
    PocDocument prefix = {0}, suffix = {0}, contextual = {0};
    Stylesheet sheet = {0};
    CHECK(document_parse(
              &prefix, budget, prefix_page, strlen(prefix_page), 8)
          && document_parse(
              &suffix, budget, suffix_page, strlen(suffix_page), 8)
          && document_parse(
              &contextual, budget, contextual_page,
              strlen(contextual_page), 8)
          && stylesheet_build(&sheet, budget, &prefix, 480)
          && stylesheet_add_css(
              &sheet, ".x{color:#222222}",
              strlen(".x{color:#222222}")));
    lxb_dom_node_t *suffix_style = find_id(
        lxb_dom_interface_node(suffix.html), "suffix");
    lxb_dom_node_t *suffix_elements[] = {suffix_style};
    lxb_dom_node_t *target_node = find_id(
        lxb_dom_interface_node(suffix.html), "target");
    ComputedStyle prefix_target = style_for_node(
        &sheet, target_node, NULL);
    size_t prefix_rule_count = sheet.count;
    CHECK(prefix_target.color == 0x222222
          && sheet.rule_index_ready
          && sheet.selector_program_ready);
    uint64_t before = stylesheet_parse_context_signature(&sheet);
    CHECK(suffix_style != NULL
          && stylesheet_append_style_elements(
              &sheet, suffix_elements, 1, NULL)
          && stylesheet_parse_context_signature(&sheet) != before
          && sheet.rule_index_ready
          && sheet.selector_program_ready
          && sheet.selector_append_reused_rules >= prefix_rule_count
          && sheet.selector_append_compiled_rules != 0);
    ComputedStyle target = style_for_node(
        &sheet, target_node, NULL);
    CHECK(target.color == 0x333333);

    lxb_dom_node_t *contextual_style = find_id(
        lxb_dom_interface_node(contextual.html), "contextual");
    lxb_dom_node_t *contextual_elements[] = {contextual_style};
    before = stylesheet_parse_context_signature(&sheet);
    CHECK(contextual_style != NULL
          && stylesheet_append_style_elements(
              &sheet, contextual_elements, 1, NULL)
          && stylesheet_parse_context_signature(&sheet) != before);

    stylesheet_destroy(&sheet);
    document_destroy(&contextual);
    document_destroy(&suffix);
    document_destroy(&prefix);
    CHECK(budget->current == 0);
    return 0;
}

static const StyleGradient *resolved_background_gradient(
    const Stylesheet *sheet, ComputedStyle style)
{
    return stylesheet_background_gradient(sheet, &style);
}

static const StyleBoxShadow *resolved_box_shadow(
    const Stylesheet *sheet, ComputedStyle style, size_t index)
{
    return stylesheet_box_shadow(sheet, &style, index);
}

static int test_linear_gradient_parse(Budget *budget)
{
    static const char page[] =
        "<!doctype html><style>body{margin:0}"
        "#down{background:linear-gradient(#ff9770,#ff7d4d)}"
        "#right{background:linear-gradient(to right,#102030,#405060)}"
        "#corner{background:linear-gradient(to top right,#000,#fff)}"
        "#angled{background:linear-gradient(45deg,#000,#fff)}"
        "#wrapped{background:linear-gradient(430deg,#000,#fff)}"
        "#thirds{background:linear-gradient(#000,#888 25%,#fff)}"
        "#spread{background:linear-gradient(#000,#111,#222,#fff)}"
        "#alpha{background:linear-gradient(rgba(0,0,0,0),#000)}"
        /* Generated ramps beyond the retained stop cap degrade to an evenly
           sampled bounded gradient rather than dropping a readability
           overlay completely. */
        "#capped{background:#0a0b0c;background:linear-gradient("
        "#001,#002,#003,#004,#005,#006,#007,#008,#009)}"
        "#repeating{background:#0a0b0c;"
        "background:repeating-linear-gradient(#000,#fff)}"
        "#turned{background:#0a0b0c;"
        "background:linear-gradient(0.25turn,#000,#fff)}"
        "#lonely{background:#0a0b0c;background:linear-gradient(#000)}"
        "#hinted{background:#0a0b0c;"
        "background:linear-gradient(#000,50%,#fff)}"
        "#spaced{background:#0a0b0c;"
        "background:linear-gradient(#000 0 50%,#fff)}"
        "#radial{background:#0a0b0c;"
        "background:radial-gradient(circle,#000,#fff)}"
        "#interpolated{background:linear-gradient(in oklab,"
        "lab(50% 40 30),color-mix(in srgb,#2468ac 75%,transparent))}"
        "#inherited{background-image:inherit}"
        "</style><body>"
        "<i id=down></i><i id=right></i><i id=corner></i><i id=angled></i>"
        "<i id=wrapped></i><i id=thirds></i><i id=spread></i>"
        "<i id=alpha></i><i id=capped></i><i id=repeating></i>"
        "<i id=turned></i><i id=lonely></i><i id=hinted></i>"
        "<i id=spaced></i><i id=radial></i><i id=interpolated></i>"
        "<i id=inherit-parent style='background-image:linear-gradient("
        "to right,#123456,#abcdef)'><b id=inherited></b></i></body>";
    PocDocument document = {0};
    Stylesheet sheet = {0};
    CHECK(document_parse(&document, budget, page, strlen(page), 30)
          && stylesheet_build(&sheet, budget, &document, 480));
    lxb_dom_node_t *root = lxb_dom_interface_node(document.html);
    ComputedStyle percent_position = {0};
    CHECK(style_parse_background_position(
              &sheet, "50% 0%", strlen("50% 0%"), &percent_position)
          && percent_position.background_position_x == 50
          && percent_position.background_position_y == 0);

#define GRADIENT_OF(id) \
    resolved_background_gradient( \
        &sheet, style_for_node(&sheet, find_id(root, (id)), NULL))

    /* An omitted angle is `to bottom`, i.e. 180deg, and the two ends
       default to 0% and 100%. */
    ComputedStyle down = style_for_node(
        &sheet, find_id(root, "down"), NULL);
    const StyleGradient *down_gradient =
        stylesheet_background_gradient(&sheet, &down);
    CHECK(down.background_image_kind == STYLE_BACKGROUND_IMAGE_GRADIENT
          && down.background_image == NULL
          && down_gradient != NULL
          && down_gradient->angle == 180
          && down_gradient->stop_count == 2
          && down_gradient->stop_position[0] == 0
          && down_gradient->stop_position[1] == 255
          && down_gradient->stop_argb[0] == 0xffff9770u
          && down_gradient->stop_argb[1] == 0xffff7d4du);

    CHECK(GRADIENT_OF("right")->angle == 90);
    /* Corners are approximated at the 45-degree diagonals. */
    CHECK(GRADIENT_OF("corner")->angle == 45);
    CHECK(GRADIENT_OF("angled")->angle == 45);
    CHECK(GRADIENT_OF("wrapped")->angle == 70);

    const StyleGradient *thirds = GRADIENT_OF("thirds");
    CHECK(thirds != NULL && thirds->stop_count == 3
          && thirds->stop_position[0] == 0
          && thirds->stop_position[1] == 64
          && thirds->stop_position[2] == 255);

    /* Interior stops with no offset spread evenly between their
       bracketing neighbours. */
    const StyleGradient *spread = GRADIENT_OF("spread");
    CHECK(spread != NULL && spread->stop_count == 4
          && spread->stop_position[0] == 0
          && spread->stop_position[1] == 85
          && spread->stop_position[2] == 170
          && spread->stop_position[3] == 255);

    CHECK(GRADIENT_OF("alpha")->stop_argb[0] == 0x00000000u
          && GRADIENT_OF("alpha")->stop_argb[1]
             == 0xff000000u);

    ComputedStyle capped_style = style_for_node(
        &sheet, find_id(root, "capped"), NULL);
    const StyleGradient *capped =
        stylesheet_background_gradient(&sheet, &capped_style);
    CHECK(capped_style.background_image_kind == STYLE_BACKGROUND_IMAGE_GRADIENT
          && capped != NULL && capped->stop_count == STYLE_GRADIENT_STOP_LIMIT
          && capped->stop_argb[0] == 0xff000011u
          && capped->stop_argb[
                 STYLE_GRADIENT_STOP_LIMIT - 1u] == 0xff000099u);

    const StyleGradient *repeating = GRADIENT_OF("repeating");
    const StyleGradient *turned = GRADIENT_OF("turned");
    const StyleGradient *spaced = GRADIENT_OF("spaced");
    const StyleGradient *radial = GRADIENT_OF("radial");
    ComputedStyle interpolated_style = style_for_node(
        &sheet, find_id(root, "interpolated"), NULL);
    const StyleGradient *interpolated =
        stylesheet_background_gradient(&sheet, &interpolated_style);
    CHECK(repeating != NULL && turned != NULL && spaced != NULL
          && radial != NULL && interpolated != NULL
          && (repeating->angle & STYLE_GRADIENT_REPEATING) != 0
          && (turned->angle & STYLE_GRADIENT_ANGLE_MASK) == 90
          && spaced->stop_count == 3
          && spaced->stop_position[0] == 0
          && spaced->stop_position[1] == 128
          && spaced->stop_position[2] == 255
          && (radial->angle & STYLE_GRADIENT_RADIAL) != 0
          && (radial->angle
              & STYLE_GRADIENT_RADIAL_CIRCLE) != 0
          && interpolated_style.background_image_kind
             == STYLE_BACKGROUND_IMAGE_GRADIENT
          && interpolated->stop_count == 2
          && (interpolated->stop_argb[1] >> 24) == 191);
    ComputedStyle inherit_parent = style_for_node(
        &sheet, find_id(root, "inherit-parent"), NULL);
    ComputedStyle inherited = style_for_node(
        &sheet, find_id(root, "inherited"), &inherit_parent);
    const StyleGradient *inherited_gradient =
        stylesheet_background_gradient(&sheet, &inherited);
    CHECK(inherited.background_image_kind
              == STYLE_BACKGROUND_IMAGE_GRADIENT
          && inherited_gradient != NULL
          && inherited_gradient->angle == 90
          && inherited_gradient->stop_argb[0] == 0xff123456u
          && inherited_gradient->stop_argb[1] == 0xffabcdefu);
    uint32_t modern_color = 0;
    uint8_t modern_alpha = 0;
    static const char modern_color_value[] =
        "light-dark(color-mix(in oklab, red 25%, blue), #000)";
    CHECK(style_color_parse(modern_color_value,
                            sizeof(modern_color_value) - 1,
                            &modern_color, &modern_alpha)
          && modern_alpha == 255
          && modern_color != 0xff0000 && modern_color != 0x0000ff);

    static const char *const rejected[] = {"lonely", "hinted"};
    for (size_t i = 0; i < sizeof(rejected) / sizeof(rejected[0]); i++) {
        ComputedStyle style = style_for_node(
            &sheet, find_id(root, rejected[i]), NULL);
        if (style.background_image_kind != STYLE_BACKGROUND_IMAGE_NONE
            || !style.has_background || style.background != 0x0a0b0c) {
            fprintf(stderr, "gradient %s not rejected: kind=%u bg=%06x\n",
                    rejected[i], style.background_image_kind,
                    style.background);
        }
        CHECK(style.background_image_kind == STYLE_BACKGROUND_IMAGE_NONE
              && style.has_background && style.background == 0x0a0b0c);
    }
#undef GRADIENT_OF

    stylesheet_destroy(&sheet);
    document_destroy(&document);
    CHECK(budget->current == 0);
    return 0;
}

static int test_compact_visual_css_extensions(Budget *budget)
{
    static const char page[] =
        "<!doctype html><style>html,body{margin:0;padding:0}"
        "#ellipsis{display:block;width:42px;white-space:nowrap;"
        "overflow:hidden;text-overflow:ellipsis}"
        "#outline{display:block;width:20px;height:12px;"
        "outline:2px dashed #123456;outline-offset:3px}"
        "#clip{display:block;width:20px;height:20px;background:#f00;"
        "clip-path:circle()}"
        "#filtered{display:block;width:12px;height:12px;background:#f00;"
        "filter:grayscale(1)}"
        "#layers{display:block;width:20px;height:12px;"
        "background:linear-gradient(rgba(255,0,0,.5),rgba(255,0,0,.5)),"
        "linear-gradient(#0000ff,#0000ff)}"
        "table{border-collapse:collapse}td{border:4px solid #000}"
        "</style><body>"
        "<div id=ellipsis>one very long line</div>"
        "<div id=outline></div><div id=clip></div>"
        "<div id=filtered></div><div id=layers></div>"
        "<table id=table><tr><td>a</td><td>b</td></tr></table>"
        "</body>";
    PocDocument document = {0};
    Stylesheet sheet = {0};
    LayoutDocument layout = {0};
    CHECK(document_parse(&document, budget, page, strlen(page), 24)
          && stylesheet_build(&sheet, budget, &document, 160)
          && layout_build(&layout, budget, &document, &sheet,
                          NULL, NULL, 160));
    lxb_dom_node_t *root = lxb_dom_interface_node(document.html);
    lxb_dom_node_t *ellipsis = find_id(root, "ellipsis");
    lxb_dom_node_t *outline = find_id(root, "outline");
    lxb_dom_node_t *clip = find_id(root, "clip");
    lxb_dom_node_t *filtered = find_id(root, "filtered");
    lxb_dom_node_t *layers = find_id(root, "layers");
    lxb_dom_node_t *table = find_id(root, "table");
    ComputedStyle ellipsis_style =
        style_for_node(&sheet, ellipsis, NULL);
    ComputedStyle outline_style =
        style_for_node(&sheet, outline, NULL);
    ComputedStyle clip_style = style_for_node(&sheet, clip, NULL);
    ComputedStyle filter_style = style_for_node(&sheet, filtered, NULL);
    ComputedStyle layer_style = style_for_node(&sheet, layers, NULL);
    ComputedStyle table_style = style_for_node(&sheet, table, NULL);
    CHECK(computed_style_text_overflow_ellipsis(&ellipsis_style)
          && computed_style_overflow_x_hidden(&ellipsis_style)
          && computed_style_outline_width(&outline_style) == 2
          && computed_style_outline_style(&outline_style)
                 == STYLE_OUTLINE_DASHED
          && computed_style_outline_offset(&outline_style) == 3
          && outline_style.outline_color == 0x123456
          && computed_style_clip_path_type(&clip_style)
                 == STYLE_CLIP_PATH_CIRCLE
          && computed_style_filter_code(&filter_style)
                 == STYLE_FILTER_GRAYSCALE
          && layer_style.background_image_kind
                 == STYLE_BACKGROUND_IMAGE_GRADIENT
          && computed_style_background_overlay_gradient(&layer_style) != 0
          && stylesheet_background_overlay_gradient(
                 &sheet,
                 computed_style_background_overlay_gradient(
                     &layer_style)) != NULL
          && table_style.table_border_collapse);
    const LayoutNodeBox *ellipsis_box =
        layout_box_for_node(&layout, ellipsis);
    const LayoutNodeBox *outline_box = layout_box_for_node(&layout, outline);
    const LayoutNodeBox *clip_box = layout_box_for_node(&layout, clip);
    const LayoutNodeBox *filter_box =
        layout_box_for_node(&layout, filtered);
    bool marker = false, dashed_outline = false, filtered_fill = false;
    CHECK(ellipsis_box != NULL && outline_box != NULL && clip_box != NULL
          && filter_box != NULL && clip_box->clips_x && clip_box->clips_y
          && layout_node_box_clip_radius(clip_box) == 10);
    for (size_t i = ellipsis_box->command_start;
         i < ellipsis_box->command_end; i++) {
        const DrawCommand *command = &layout.commands[i];
        marker = marker || (command->type == DRAW_TEXT
            && command->text_length == 3
            && memcmp(command->text, "\xe2\x80\xa6", 3) == 0);
    }
    for (size_t i = outline_box->command_start;
         i < outline_box->command_end; i++) {
        const DrawCommand *command = &layout.commands[i];
        dashed_outline = dashed_outline
            || (command->type == DRAW_STROKE_RECT
                && command->image_fit == LAYOUT_STROKE_DASHED
                && command->scale == 2);
    }
    for (size_t i = filter_box->command_start;
         i < filter_box->command_end; i++) {
        const DrawCommand *command = &layout.commands[i];
        filtered_fill = filtered_fill
            || (command->type == DRAW_FILL_RECT
                && draw_command_filter_code(command)
                       == STYLE_FILTER_GRAYSCALE);
    }
    CHECK(marker && dashed_outline && filtered_fill);
    layout_destroy(&layout);
    stylesheet_destroy(&sheet);
    document_destroy(&document);
    CHECK(budget->current == 0);
    return 0;
}

static int test_unresolved_mask_keeps_transparent_geometry(Budget *budget)
{
    static const char page[] =
        "<!doctype html><style>html,body{margin:0;padding:0;background:#fff}"
        "#icon{display:block;width:20px;height:20px;background:#54595d;"
        "mask-image:url(https://example.test/icon.svg)}"
        "</style><body><span id=icon></span></body>";
    PocDocument document = {0};
    Stylesheet sheet = {0};
    LayoutDocument unresolved = {0};
    LayoutDocument resolved = {0};
    CHECK(document_parse(&document, budget, page, strlen(page), 17)
          && stylesheet_build(&sheet, budget, &document, 64)
          && layout_build(&unresolved, budget, &document, &sheet,
                          NULL, NULL, 64));
    lxb_dom_node_t *icon = find_id(
        lxb_dom_interface_node(document.html), "icon");
    const LayoutNodeBox *unresolved_box =
        layout_box_for_node(&unresolved, icon);
    bool unresolved_fill = false;
    for (size_t i = 0; i < unresolved.count; i++) {
        const DrawCommand *command = &unresolved.commands[i];
        if (command->type == DRAW_FILL_RECT
            && command->color == 0x54595d) {
            unresolved_fill = true;
            break;
        }
    }
    CHECK(icon != NULL && unresolved_box != NULL
          && unresolved_box->width == 20 && unresolved_box->height == 20
          && !unresolved_fill
          && unresolved.unresolved_external_visuals);

    static unsigned char mask_pixels[] = {
        255, 255, 255, 255, 255, 255, 255, 255,
        255, 255, 255, 255, 255, 255, 255, 255
    };
    ImageResource mask = {
        .node = icon, .pixels = mask_pixels, .width = 2, .height = 2,
        .is_mask = true
    };
    ImageResources images = {.items = &mask, .count = 1};
    CHECK(layout_build(&resolved, budget, &document, &sheet,
                       NULL, &images, 64));
    const LayoutNodeBox *resolved_box = layout_box_for_node(&resolved, icon);
    bool resolved_mask = false;
    for (size_t i = 0; i < resolved.count; i++) {
        const DrawCommand *command = &resolved.commands[i];
        if (command->type == DRAW_IMAGE && command->image == &mask
            && command->color == 0x54595d) {
            resolved_mask = true;
            break;
        }
    }
    CHECK(resolved_box != NULL && resolved_mask
          && !resolved.unresolved_external_visuals
          && resolved_box->x == unresolved_box->x
          && resolved_box->y == unresolved_box->y
          && resolved_box->width == unresolved_box->width
          && resolved_box->height == unresolved_box->height);
    layout_destroy(&resolved);
    layout_destroy(&unresolved);
    stylesheet_destroy(&sheet);
    document_destroy(&document);
    CHECK(budget->current == 0);
    return 0;
}

static int test_retained_paint_stack_addresses_are_stable(Budget *budget)
{
    size_t baseline = budget->current;
    Stylesheet sheet = {.budget = budget};
    StylePaintStack first = {0};
    first.components = STYLE_PAINT_COMPONENT_TRANSFORM_ORIGIN;
    first.transform_origin_x = 17;
    uint8_t first_id = 0;
    CHECK(style_intern_paint_stack(&sheet, &first, &first_id)
              == STYLE_PAINT_INTERN_RETAINED
          && first_id == 1);
    const StylePaintStack *retained =
        stylesheet_paint_stack(&sheet, first_id);
    CHECK(retained != NULL && retained->transform_origin_x == 17);

    for (uint16_t value = 18; value <= 20; value++) {
        StylePaintStack next = first;
        next.transform_origin_x = value;
        uint8_t id = 0;
        CHECK(style_intern_paint_stack(&sheet, &next, &id)
                  == STYLE_PAINT_INTERN_RETAINED
              && id != 0);
    }
    /* Prevent an old contiguous four-entry allocation from extending in
       place, then cross its growth boundary. Layout may still hold `retained`
       while lazy pseudo-style resolution performs exactly this operation. */
    void *barrier = budget_malloc(budget, 4096);
    CHECK(barrier != NULL);
    StylePaintStack fifth = first;
    fifth.transform_origin_x = 21;
    uint8_t fifth_id = 0;
    CHECK(style_intern_paint_stack(&sheet, &fifth, &fifth_id)
              == STYLE_PAINT_INTERN_RETAINED
          && fifth_id == 5
          && stylesheet_paint_stack(&sheet, first_id) == retained
          && retained->transform_origin_x == 17);

    budget_free(budget, barrier);
    stylesheet_destroy(&sheet);
    CHECK(budget->current == baseline);
    return 0;
}

static int test_bounded_shared_paint_stack(Budget *budget)
{
    static const char page[] =
        "<!doctype html><style>"
        "html{font-size:10px}"
        "#probe{background:linear-gradient(#100,#200),"
        "linear-gradient(#300,#400),linear-gradient(#500,#600)}"
        "#probe{background-origin:content-box,padding-box,border-box;"
        "background-clip:padding-box,border-box,content-box;"
        "mask-image:linear-gradient(#000,#fff),url(mask.svg);"
        "mask-size:50% 25%,10px 20px;"
        "mask-position:right 20% bottom 70%,5px 6px;"
        "mask-repeat:no-repeat,repeat-x}"
        "#box-defaults{background-clip:content-box,padding-box}"
        "#mask-replacement{mask-image:url(a.svg),url(b.svg);"
        "mask-image:none}"
        "#mask-shorthand{--icon:url(icon.svg);"
        "mask:var(--icon) center/100% 50% no-repeat}"
        "#pseudo-mask::before{content:'';display:block;width:16px;"
        "height:16px;mask:var(--default) center/100% 100% no-repeat}"
        "#webkit-mask{-webkit-mask:url(webkit.svg) 25% 75%/contain "
        "repeat-x}"
        "#background-position{background-image:linear-gradient(#111,#222);"
        "background-size:contain;"
        "background-position:bottom 1px right 2px}"
        "#sprite-position{background-image:linear-gradient(#333,#444);"
        "background-position:-2rem -37.8rem}"
        "#layered-sprite-position{background-image:linear-gradient(#555,#666),"
        "linear-gradient(#777,#888);background-position:-4px -8px,"
        "-12px -16px}"
        "table{border-spacing:12px 7px}</style>"
        "<body><div id=probe></div><div id=box-defaults></div>"
        "<div id=mask-replacement></div>"
        "<div id=mask-shorthand></div><div id=pseudo-mask "
        "style='--icon:url(https://cdn.example/"
        "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
        "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
        "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
        "aaaaaaaaaaaaaaaa.svg);"
        "--default:var(--icon)'></div>"
        "<div id=webkit-mask></div>"
        "<div id=background-position></div>"
        "<div id=sprite-position></div>"
        "<div id=layered-sprite-position></div>"
        "<table id=table><tr><td>x</td></tr>"
        "</table></body>";
    PocDocument document = {0};
    Stylesheet sheet = {0};
    CHECK(document_parse(&document, budget, page, strlen(page), 24)
          && stylesheet_build(&sheet, budget, &document, 480));
    lxb_dom_node_t *root = lxb_dom_interface_node(document.html);
    ComputedStyle probe = style_for_node(
        &sheet, find_id(root, "probe"), NULL);
    const StylePaintStack *paint = stylesheet_paint_stack(
        &sheet, computed_style_paint_stack_id(&probe));
    CHECK(paint != NULL
          && paint->background_count == STYLE_PAINT_LAYER_LIMIT
          && paint->backgrounds[0].kind == STYLE_PAINT_IMAGE_GRADIENT
          && paint->backgrounds[1].kind == STYLE_PAINT_IMAGE_GRADIENT
          && paint->backgrounds[2].kind == STYLE_PAINT_IMAGE_GRADIENT
          && paint->backgrounds[0].origin == STYLE_PAINT_BOX_CONTENT
          && paint->backgrounds[1].origin == STYLE_PAINT_BOX_PADDING
          && paint->backgrounds[2].origin == STYLE_PAINT_BOX_BORDER
          && paint->backgrounds[0].clip == STYLE_PAINT_BOX_PADDING
          && paint->backgrounds[1].clip == STYLE_PAINT_BOX_BORDER
          && paint->backgrounds[2].clip == STYLE_PAINT_BOX_CONTENT
          && paint->mask_count == 2
          && paint->masks[0].kind == STYLE_PAINT_IMAGE_GRADIENT
          && paint->masks[1].kind == STYLE_PAINT_IMAGE_URL
          && paint->masks[0].width == 50
          && paint->masks[0].height == 25
          && (paint->masks[0].flags
              & (STYLE_BACKGROUND_SIZE_EXPLICIT
                 | STYLE_BACKGROUND_WIDTH_PERCENT
                 | STYLE_BACKGROUND_HEIGHT_PERCENT))
             == (STYLE_BACKGROUND_SIZE_EXPLICIT
                 | STYLE_BACKGROUND_WIDTH_PERCENT
                 | STYLE_BACKGROUND_HEIGHT_PERCENT)
          && paint->masks[0].position_x == 20
          && paint->masks[0].position_y == 70
          && paint->masks[0].position_edges
             == (STYLE_PAINT_POSITION_FROM_RIGHT
                 | STYLE_PAINT_POSITION_FROM_BOTTOM)
          && (paint->masks[0].flags
              & (STYLE_BACKGROUND_NO_REPEAT_X
                 | STYLE_BACKGROUND_NO_REPEAT_Y))
             == (STYLE_BACKGROUND_NO_REPEAT_X
                 | STYLE_BACKGROUND_NO_REPEAT_Y)
          && paint->masks[1].width == 10
          && paint->masks[1].height == 20
          && (paint->masks[1].flags
              & STYLE_BACKGROUND_POSITION_PIXELS) != 0
          && (paint->masks[1].flags
              & STYLE_BACKGROUND_NO_REPEAT_Y) != 0
          && sheet.paint_storage != NULL
          && sheet.paint_storage->count <= STYLE_PAINT_STACK_LIMIT);
    ComputedStyle box_defaults = style_for_node(
        &sheet, find_id(root, "box-defaults"), NULL);
    const StylePaintStack *box_paint = stylesheet_paint_stack(
        &sheet, computed_style_paint_stack_id(&box_defaults));
    CHECK(box_paint != NULL && box_paint->background_count == 2
          && box_paint->backgrounds[0].origin
             == STYLE_PAINT_BOX_PADDING
          && box_paint->backgrounds[1].origin
             == STYLE_PAINT_BOX_PADDING
          && box_paint->backgrounds[0].clip
             == STYLE_PAINT_BOX_CONTENT
          && box_paint->backgrounds[1].clip
             == STYLE_PAINT_BOX_PADDING);
    ComputedStyle mask_replacement = style_for_node(
        &sheet, find_id(root, "mask-replacement"), NULL);
    const StylePaintStack *replacement_paint = stylesheet_paint_stack(
        &sheet, computed_style_paint_stack_id(&mask_replacement));
    CHECK(replacement_paint != NULL
          && replacement_paint->mask_count == 1
          && replacement_paint->masks[0].kind
             == STYLE_PAINT_IMAGE_NONE);
    ComputedStyle shorthand = style_for_node(
        &sheet, find_id(root, "mask-shorthand"), NULL);
    const StylePaintStack *shorthand_paint = stylesheet_paint_stack(
        &sheet, computed_style_paint_stack_id(&shorthand));
    CHECK(shorthand_paint != NULL
          && shorthand_paint->mask_count == 1
          && shorthand_paint->masks[0].kind == STYLE_PAINT_IMAGE_URL
          && strcmp(shorthand_paint->masks[0].image, "icon.svg") == 0
          && shorthand_paint->masks[0].position_x == 50
          && shorthand_paint->masks[0].position_y == 50
          && shorthand_paint->masks[0].width == 100
          && shorthand_paint->masks[0].height == 50
          && (shorthand_paint->masks[0].flags
              & (STYLE_BACKGROUND_SIZE_EXPLICIT
                 | STYLE_BACKGROUND_WIDTH_PERCENT
                 | STYLE_BACKGROUND_HEIGHT_PERCENT
                 | STYLE_BACKGROUND_NO_REPEAT_X
                 | STYLE_BACKGROUND_NO_REPEAT_Y))
             == (STYLE_BACKGROUND_SIZE_EXPLICIT
                 | STYLE_BACKGROUND_WIDTH_PERCENT
                 | STYLE_BACKGROUND_HEIGHT_PERCENT
                 | STYLE_BACKGROUND_NO_REPEAT_X
                 | STYLE_BACKGROUND_NO_REPEAT_Y));
    lxb_dom_node_t *pseudo_node = find_id(root, "pseudo-mask");
    ComputedStyle pseudo_parent = style_for_node(
        &sheet, pseudo_node, NULL);
    ComputedStyle pseudo = style_for_pseudo(
        &sheet, pseudo_node, PSEUDO_BEFORE, &pseudo_parent);
    const StylePaintStack *pseudo_paint = stylesheet_paint_stack(
        &sheet, computed_style_paint_stack_id(&pseudo));
    CHECK(pseudo_paint != NULL && pseudo_paint->mask_count == 1
          && pseudo_paint->masks[0].kind == STYLE_PAINT_IMAGE_URL
          && strncmp(pseudo_paint->masks[0].image,
                     "https://cdn.example/", 20) == 0
          && strlen(pseudo_paint->masks[0].image) > 220
          && strcmp(pseudo_paint->masks[0].image
                        + strlen(pseudo_paint->masks[0].image) - 4,
                    ".svg") == 0);
    ComputedStyle webkit_mask = style_for_node(
        &sheet, find_id(root, "webkit-mask"), NULL);
    const StylePaintStack *webkit_paint = stylesheet_paint_stack(
        &sheet, computed_style_paint_stack_id(&webkit_mask));
    CHECK(webkit_paint != NULL && webkit_paint->mask_count == 1
          && webkit_paint->masks[0].kind == STYLE_PAINT_IMAGE_URL
          && webkit_paint->masks[0].position_x == 25
          && webkit_paint->masks[0].position_y == 75
          && webkit_paint->masks[0].fit == LAYOUT_IMAGE_FIT_CONTAIN
          && (webkit_paint->masks[0].flags
              & STYLE_BACKGROUND_NO_REPEAT_Y) != 0);
    ComputedStyle positioned = style_for_node(
        &sheet, find_id(root, "background-position"), NULL);
    const StylePaintStack *positioned_paint = stylesheet_paint_stack(
        &sheet, computed_style_paint_stack_id(&positioned));
    CHECK(positioned_paint != NULL
          && positioned_paint->background_count == 1
          && positioned_paint->backgrounds[0].fit
             == LAYOUT_IMAGE_FIT_CONTAIN
          && positioned_paint->backgrounds[0].position_x == 2
          && positioned_paint->backgrounds[0].position_y == 1
          && positioned_paint->backgrounds[0].position_edges
             == (STYLE_PAINT_POSITION_FROM_RIGHT
                 | STYLE_PAINT_POSITION_FROM_BOTTOM
                 | STYLE_PAINT_POSITION_X_PIXELS
                 | STYLE_PAINT_POSITION_Y_PIXELS));
    ComputedStyle sprite_positioned = style_for_node(
        &sheet, find_id(root, "sprite-position"), NULL);
    CHECK(sprite_positioned.background_position_x == -32
          && sprite_positioned.background_position_y == -605
          && (sprite_positioned.background_size_flags
              & STYLE_BACKGROUND_POSITION_PIXELS) != 0);
    ComputedStyle layered_sprite = style_for_node(
        &sheet, find_id(root, "layered-sprite-position"), NULL);
    const StylePaintStack *layered_sprite_paint = stylesheet_paint_stack(
        &sheet, computed_style_paint_stack_id(&layered_sprite));
    CHECK(layered_sprite_paint != NULL
          && layered_sprite_paint->background_count == 2
          && layered_sprite_paint->backgrounds[0].position_x == -4
          && layered_sprite_paint->backgrounds[0].position_y == -8
          && layered_sprite_paint->backgrounds[1].position_x == -12
          && layered_sprite_paint->backgrounds[1].position_y == -16
          && (layered_sprite_paint->backgrounds[1].position_edges
              & (STYLE_PAINT_POSITION_X_PIXELS
                 | STYLE_PAINT_POSITION_Y_PIXELS))
             == (STYLE_PAINT_POSITION_X_PIXELS
                 | STYLE_PAINT_POSITION_Y_PIXELS));
    ComputedStyle table = style_for_node(
        &sheet, find_id(root, "table"), NULL);
    const StylePaintStack *table_paint = stylesheet_paint_stack(
        &sheet, computed_style_paint_stack_id(&table));
    CHECK(table_paint != NULL
          && (table_paint->components
              & STYLE_PAINT_COMPONENT_TABLE_SPACING) != 0
          && table_paint->table_spacing_x == 12
          && table_paint->table_spacing_y == 7
          && sizeof(ComputedStyle) == 376);

    /* Exhausting an optional-quality table must not reject a later
       stylesheet.  Fill every representable one-byte ID, verify that the
       outcome is distinguishable from OOM, then parse declarations through
       every retained-paint property family which previously could fail the
       whole page. */
    for (unsigned candidate = 1;
         sheet.paint_storage->count < STYLE_PAINT_STACK_LIMIT
             && candidate < UINT16_MAX;
         candidate++) {
        StylePaintStack unique = {0};
        unique.components = STYLE_PAINT_COMPONENT_TRANSFORM_ORIGIN;
        unique.transform_origin_x = (uint16_t) candidate;
        unique.transform_origin_y = (uint16_t) (candidate ^ 0x5a5au);
        uint8_t id = 0;
        StylePaintInternResult result =
            style_intern_paint_stack(&sheet, &unique, &id);
        CHECK(result == STYLE_PAINT_INTERN_RETAINED
              && id != 0);
    }
    CHECK(sheet.paint_storage->count == STYLE_PAINT_STACK_LIMIT);
    StylePaintStack overflow = {0};
    overflow.components = STYLE_PAINT_COMPONENT_TEXT_SHADOW;
    overflow.text_shadow_count = 1;
    overflow.text_shadows[0].argb = UINT32_C(0xff123456);
    uint8_t overflow_id = 99;
    CHECK(style_intern_paint_stack(&sheet, &overflow, &overflow_id)
              == STYLE_PAINT_INTERN_LIMIT
          && overflow_id == 0
          && stylesheet_add_css(
                 &sheet,
                 "#late{transform-origin:37px 19px;"
                 "text-shadow:1px 2px #123456;"
                 "border-spacing:12px 7px;"
                 "border-radius:1px 2px 3px 4px;"
                 "background:linear-gradient(#000,#fff) right 2px "
                 "bottom 3px/contain no-repeat content-box padding-box;"
                 "mask-image:linear-gradient(#fff,#000);"
                 "mask-position:4px 5px;mask-size:6px 7px}",
                 strlen("#late{transform-origin:37px 19px;"
                        "text-shadow:1px 2px #123456;"
                        "border-spacing:12px 7px;"
                        "border-radius:1px 2px 3px 4px;"
                        "background:linear-gradient(#000,#fff) right 2px "
                        "bottom 3px/contain no-repeat content-box padding-box;"
                        "mask-image:linear-gradient(#fff,#000);"
                        "mask-position:4px 5px;mask-size:6px 7px}")));

    stylesheet_destroy(&sheet);
    document_destroy(&document);
    CHECK(budget->current == 0);

    Budget failure_budget;
    budget_init(&failure_budget, 64u * 1024u);
    budget_inject_failure_after(&failure_budget, 0);
    Stylesheet failure_sheet = {.budget = &failure_budget};
    StylePaintStack first = {0};
    first.components = STYLE_PAINT_COMPONENT_TRANSFORM_ORIGIN;
    uint8_t failed_id = 99;
    CHECK(style_intern_paint_stack(&failure_sheet, &first, &failed_id)
              == STYLE_PAINT_INTERN_OOM
          && failed_id == 0);
    stylesheet_destroy(&failure_sheet);
    CHECK(failure_budget.current == 0);
    return 0;
}

static int test_linear_gradient_raster(Budget *budget)
{
    enum { FRAME_WIDTH = 64, FRAME_HEIGHT = 32 };
    static const char page[] =
        "<!doctype html><style>html,body{margin:0;padding:0;background:#fff}"
        "#ramp{position:absolute;left:0;top:0;width:32px;height:32px;"
        "background:linear-gradient(to right,#ff0000,#0000ff)}"
        "#radial-ramp{position:absolute;left:32px;top:0;width:32px;height:32px;"
        "background:radial-gradient(circle,#000,#fff)}"
        "</style><body><div id=ramp></div><div id=radial-ramp></div></body>";
    PocDocument document = {0};
    Stylesheet sheet = {0};
    LayoutDocument layout = {0};
    TileCache cache = {0};
    uint16_t *frame = NULL;
    CHECK(document_parse(&document, budget, page, strlen(page), 10)
          && stylesheet_build(&sheet, budget, &document, FRAME_WIDTH)
          && layout_build(&layout, budget, &document, &sheet,
                          NULL, NULL, FRAME_WIDTH));
    /* The ramp is interned once into the bounded per-document table and the
       fill command carries its slot. */
    size_t slot = SIZE_MAX;
    const DrawCommand *ramp_command = NULL;
    for (size_t i = 0; i < layout.count; i++) {
        size_t candidate = 0;
        if (draw_command_fill_gradient(&layout.commands[i], &candidate)) {
            ramp_command = &layout.commands[i];
            slot = candidate;
            break;
        }
    }
    CHECK(ramp_command != NULL && slot == 0 && layout.gradient_count == 2
          && layout.gradients[0].angle == 90
          && layout.gradients[0].stop_count == 2);

    frame = budget_malloc(
        budget, FRAME_WIDTH * FRAME_HEIGHT * sizeof(*frame));
    CHECK(frame != NULL && tile_cache_init(&cache, budget, &layout, 4)
          && tile_cache_set_frame(&cache, frame, FRAME_WIDTH * FRAME_HEIGHT)
          && tile_cache_render_frame(&cache, 0, FRAME_WIDTH, FRAME_HEIGHT,
                                     NULL));
    /* Axial ramp: red falls and blue rises monotonically left to right. */
    int sample_row = 16;
    int columns[3] = {1, 16, 30};
    unsigned red[3], blue[3];
    for (int i = 0; i < 3; i++) {
        uint16_t pixel = frame[(size_t) sample_row * FRAME_WIDTH
                               + columns[i]];
        red[i] = (pixel >> 11) & 0x1fu;
        blue[i] = pixel & 0x1fu;
    }
    if (!(red[0] > red[1] && red[1] > red[2]
          && blue[0] < blue[1] && blue[1] < blue[2]
          && red[0] > 24 && blue[2] > 24)) {
        fprintf(stderr, "gradient raster r=%u/%u/%u b=%u/%u/%u\n",
                red[0], red[1], red[2], blue[0], blue[1], blue[2]);
    }
    CHECK(red[0] > red[1] && red[1] > red[2]
          && blue[0] < blue[1] && blue[1] < blue[2]
          && red[0] > 24 && blue[2] > 24);
    /* A vertical sample must not vary: the axis is horizontal. */
    CHECK(frame[(size_t) 4 * FRAME_WIDTH + 16]
          == frame[(size_t) 28 * FRAME_WIDTH + 16]);
    uint16_t radial_center = frame[(size_t) 16 * FRAME_WIDTH + 48];
    uint16_t radial_corner = frame[(size_t) 1 * FRAME_WIDTH + 33];
    CHECK(radial_center < radial_corner);
    uint64_t first_gradient_hits = cache.gradient_lut_hits;
    tile_cache_invalidate_rect(&cache, 0, 0, FRAME_WIDTH, FRAME_HEIGHT);
    CHECK(cache.gradient_lut_builds == 2
          && tile_cache_render_frame(
                 &cache, 0, FRAME_WIDTH, FRAME_HEIGHT, NULL)
          && cache.gradient_lut_builds == 2
          && cache.gradient_lut_hits >= first_gradient_hits + 2);
    CHECK(cache.gradient_cache != NULL);
    CHECK(tile_cache_reclaim_optional(&cache) > 0
          && cache.gradient_cache == NULL);
    tile_cache_destroy(&cache);
    budget_free(budget, frame);
    layout_destroy(&layout);
    stylesheet_destroy(&sheet);
    document_destroy(&document);
    CHECK(budget->current == 0);
    return 0;
}

static int test_background_layers_paint_front_to_back(Budget *budget)
{
    enum { WIDTH = 16, HEIGHT = 16 };
    static const char page[] =
        "<!doctype html><style>html,body{margin:0;padding:0}"
        "#layers{width:16px;height:16px;background-image:"
        "linear-gradient(#f00,#f00),linear-gradient(#00f,#00f)}"
        "</style><body><div id=layers></div></body>";
    PocDocument document = {0};
    Stylesheet sheet = {0};
    LayoutDocument layout = {0};
    TileCache cache = {0};
    uint16_t *frame = NULL;
    CHECK(document_parse(&document, budget, page, strlen(page), 8)
          && stylesheet_build(&sheet, budget, &document, WIDTH)
          && layout_build(&layout, budget, &document, &sheet,
                          NULL, NULL, WIDTH));
    size_t gradients = 0;
    for (size_t i = 0; i < layout.count; i++) {
        if (draw_command_fill_gradient(&layout.commands[i], NULL)) {
            gradients++;
        }
    }
    frame = budget_malloc(budget, WIDTH * HEIGHT * sizeof(*frame));
    CHECK(gradients == 2 && frame != NULL
          && tile_cache_init(&cache, budget, &layout, 4)
          && tile_cache_set_frame(&cache, frame, WIDTH * HEIGHT)
          && tile_cache_render_frame(&cache, 0, WIDTH, HEIGHT, NULL));
    uint16_t center = frame[(HEIGHT / 2) * WIDTH + WIDTH / 2];
    CHECK(((center >> 11) & 0x1fu) > 24u && (center & 0x1fu) < 4u);
    tile_cache_destroy(&cache);
    budget_free(budget, frame);
    layout_destroy(&layout);
    stylesheet_destroy(&sheet);
    document_destroy(&document);
    CHECK(budget->current == 0);
    return 0;
}

static int test_box_shadow_parse(Budget *budget)
{
    static const char page[] =
        "<!doctype html><style>body{margin:0}"
        "#plain{box-shadow:2px 4px}"
        "#blurred{box-shadow:2px 4px 6px rgba(0,0,0,0.5)}"
        "#full{box-shadow:1px -2px 3px 4px #112233}"
        "#negative{box-shadow:0 0 4px -3px #000}"
        "#leading{box-shadow:#abcdef 5px 6px}"
        "#stacked{box-shadow:1px 1px #000,2px 2px 3px #fff}"
        "#insetting{box-shadow:inset 0 1px 2px #000}"
        "#none{box-shadow:2px 2px #000;box-shadow:none}"
        /* Hostile geometry: parses, but every component is clamped to the
           STYLE_BOX_SHADOW_* caps so the raster job stays bounded. */
        "#hostile{box-shadow:99999px -99999px 99999px 99999px #f0f}"
        /* Each of these is outside the bounded grammar, so the declaration
           is invalid and the preceding shadow must survive. */
        "#overcap{box-shadow:1px 1px #000;box-shadow:"
        "1px 1px red,2px 2px red,3px 3px red,4px 4px red,5px 5px red}"
        "#emunit{box-shadow:1px 1px #000;box-shadow:1em 1em #f00}"
        "#percent{box-shadow:1px 1px #000;box-shadow:10% 10% #f00}"
        "#unitless{box-shadow:1px 1px #000;box-shadow:3 4 #f00}"
        "#onelength{box-shadow:1px 1px #000;box-shadow:4px #f00}"
        "#negblur{box-shadow:1px 1px #000;box-shadow:1px 1px -5px #f00}"
        "#twocolors{box-shadow:1px 1px #000;box-shadow:1px 1px #f00 #0f0}"
        "</style><body>"
        "<i id=plain></i><i id=blurred></i><i id=full></i>"
        "<i id=negative></i><i id=leading></i><i id=stacked></i>"
        "<i id=insetting></i><i id=none></i><i id=hostile></i>"
        "<i id=overcap></i><i id=emunit></i><i id=percent></i>"
        "<i id=unitless></i><i id=onelength></i><i id=negblur></i>"
        "<i id=twocolors></i></body>";
    PocDocument document = {0};
    Stylesheet sheet = {0};
    CHECK(document_parse(&document, budget, page, strlen(page), 31)
          && stylesheet_build(&sheet, budget, &document, 480));
    lxb_dom_node_t *root = lxb_dom_interface_node(document.html);

#define SHADOW_OF(id) style_for_node(&sheet, find_id(root, (id)), NULL)
#define SHADOW_COUNT(style) stylesheet_box_shadow_count(&sheet, &(style))
#define SHADOW_AT(style, index) stylesheet_box_shadow(&sheet, &(style), (index))

    /* Two lengths only: no blur, no spread, and the colour defaults to
       currentColor rather than to a literal. */
    ComputedStyle plain = SHADOW_OF("plain");
    const StyleBoxShadow *plain_shadow = SHADOW_AT(plain, 0);
    CHECK(SHADOW_COUNT(plain) == 1 && plain_shadow != NULL
          && plain_shadow->offset_x == 2
          && plain_shadow->offset_y == 4
          && style_box_shadow_blur(plain_shadow) == 0
          && plain_shadow->spread == 0
          && style_box_shadow_uses_current_color(plain_shadow)
          && !style_box_shadow_is_inset(plain_shadow));

    ComputedStyle blurred = SHADOW_OF("blurred");
    const StyleBoxShadow *blurred_shadow = SHADOW_AT(blurred, 0);
    CHECK(SHADOW_COUNT(blurred) == 1 && blurred_shadow != NULL
          && style_box_shadow_blur(blurred_shadow) == 6
          && !style_box_shadow_uses_current_color(blurred_shadow)
          && (blurred_shadow->argb & 0x00ffffffu) == 0u
          && ((blurred_shadow->argb >> 24) & 0xffu) >= 126u
          && ((blurred_shadow->argb >> 24) & 0xffu) <= 129u);

    ComputedStyle full = SHADOW_OF("full");
    const StyleBoxShadow *full_shadow = SHADOW_AT(full, 0);
    CHECK(SHADOW_COUNT(full) == 1 && full_shadow != NULL
          && full_shadow->offset_x == 1
          && full_shadow->offset_y == -2
          && style_box_shadow_blur(full_shadow) == 3
          && full_shadow->spread == 4
          && full_shadow->argb == 0xff112233u);

    /* A negative spread is legal and shrinks the shadow. */
    ComputedStyle negative = SHADOW_OF("negative");
    const StyleBoxShadow *negative_shadow = SHADOW_AT(negative, 0);
    CHECK(negative_shadow != NULL && negative_shadow->spread == -3);
    /* The colour may lead the lengths. */
    ComputedStyle leading = SHADOW_OF("leading");
    const StyleBoxShadow *leading_shadow = SHADOW_AT(leading, 0);
    CHECK(SHADOW_COUNT(leading) == 1 && leading_shadow != NULL
          && leading_shadow->offset_x == 5
          && leading_shadow->argb == 0xffabcdefu);

    ComputedStyle stacked = SHADOW_OF("stacked");
    const StyleBoxShadow *stacked_first = SHADOW_AT(stacked, 0);
    const StyleBoxShadow *stacked_second = SHADOW_AT(stacked, 1);
    CHECK(SHADOW_COUNT(stacked) == 2
          && stacked_first != NULL && stacked_second != NULL
          && stacked_first->argb == 0xff000000u
          && stacked_second->argb == 0xffffffffu
          && style_box_shadow_blur(stacked_second) == 3);

    /* inset parses and is retained (so the declaration stays valid) even
       though this pass does not paint it. */
    ComputedStyle insetting = SHADOW_OF("insetting");
    CHECK(SHADOW_COUNT(insetting) == 1
          && style_box_shadow_is_inset(SHADOW_AT(insetting, 0)));

    ComputedStyle no_shadow = SHADOW_OF("none");
    CHECK(SHADOW_COUNT(no_shadow) == 0);

    ComputedStyle hostile = SHADOW_OF("hostile");
    const StyleBoxShadow *hostile_shadow = SHADOW_AT(hostile, 0);
    CHECK(SHADOW_COUNT(hostile) == 1 && hostile_shadow != NULL
          && hostile_shadow->offset_x == STYLE_BOX_SHADOW_OFFSET_LIMIT
          && hostile_shadow->offset_y == -STYLE_BOX_SHADOW_OFFSET_LIMIT
          && style_box_shadow_blur(hostile_shadow)
             == STYLE_BOX_SHADOW_BLUR_LIMIT
          && hostile_shadow->spread == STYLE_BOX_SHADOW_SPREAD_LIMIT);

    ComputedStyle emunit = SHADOW_OF("emunit");
    const StyleBoxShadow *emunit_shadow = SHADOW_AT(emunit, 0);
    CHECK(SHADOW_COUNT(emunit) == 1 && emunit_shadow != NULL
          && emunit_shadow->offset_x == 16
          && emunit_shadow->offset_y == 16
          && emunit_shadow->argb == UINT32_C(0xffff0000));

    static const char *const rejected[] = {
        "overcap", "percent", "unitless", "onelength", "negblur",
        "twocolors"
    };
    for (size_t i = 0; i < sizeof(rejected) / sizeof(rejected[0]); i++) {
        ComputedStyle style = SHADOW_OF(rejected[i]);
        const StyleBoxShadow *shadow = SHADOW_AT(style, 0);
        size_t count = SHADOW_COUNT(style);
        if (count != 1 || shadow == NULL
            || shadow->offset_x != 1
            || shadow->argb != 0xff000000u) {
            fprintf(stderr, "box-shadow %s not rejected: count=%u argb=%08x\n",
                    rejected[i], (unsigned) count,
                    shadow == NULL ? 0u : shadow->argb);
        }
        CHECK(count == 1 && shadow != NULL
              && shadow->offset_x == 1
              && shadow->argb == 0xff000000u);
    }
#undef SHADOW_AT
#undef SHADOW_COUNT
#undef SHADOW_OF

    stylesheet_destroy(&sheet);
    document_destroy(&document);
    CHECK(budget->current == 0);
    return 0;
}

static int test_text_shadow_cascade_and_display_list(Budget *budget)
{
    static const char page[] =
        "<!doctype html><style>body{margin:0;color:#123456;"
        "text-shadow:1px 2px 0 currentcolor}"
        "#multi{text-shadow:2px 3px 4px rgba(255,0,0,.5),"
        "-1px 0 0 #00ff00}"
        "#none{text-shadow:none}"
        "#invalid{text-shadow:3px 4px #010203;"
        "text-shadow:1px 2px 3px 4px #ffffff}"
        "#math{font-size:20px;text-shadow:"
        "calc(.5em + 2px) 0 1px #abcdef}"
        "#pseudo::before{content:\"P\";text-shadow:2px 0 #fedcba}"
        "</style><body><span id=child>CHILD</span>"
        "<span id=multi>MULTI</span><span id=none>NONE</span>"
        "<span id=invalid>INVALID</span><span id=math>MATH</span>"
        "<span id=pseudo></span></body>";
    PocDocument document = {0};
    Stylesheet sheet = {0};
    LayoutDocument layout = {0};
    CHECK(document_parse(&document, budget, page, strlen(page), 31)
          && stylesheet_build(&sheet, budget, &document, 480)
          && layout_build(&layout, budget, &document, &sheet,
                          NULL, NULL, 480));
    lxb_dom_node_t *root = lxb_dom_interface_node(document.html);
    lxb_dom_node_t *child = find_id(root, "child");
    lxb_dom_node_t *body = child == NULL ? NULL : child->parent;
    lxb_dom_node_t *multi = find_id(root, "multi");
    lxb_dom_node_t *none = find_id(root, "none");
    lxb_dom_node_t *invalid = find_id(root, "invalid");
    lxb_dom_node_t *math = find_id(root, "math");
    ComputedStyle body_style = style_for_node(&sheet, body, NULL);
    ComputedStyle child_style =
        style_for_node(&sheet, child, &body_style);
    ComputedStyle multi_style =
        style_for_node(&sheet, multi, &body_style);
    ComputedStyle none_style =
        style_for_node(&sheet, none, &body_style);
    ComputedStyle invalid_style =
        style_for_node(&sheet, invalid, &body_style);
    ComputedStyle math_style =
        style_for_node(&sheet, math, &body_style);
    const StylePaintStack *child_paint = stylesheet_paint_stack(
        &sheet, computed_style_paint_stack_id(&child_style));
    const StylePaintStack *multi_paint = stylesheet_paint_stack(
        &sheet, computed_style_paint_stack_id(&multi_style));
    const StylePaintStack *none_paint = stylesheet_paint_stack(
        &sheet, computed_style_paint_stack_id(&none_style));
    const StylePaintStack *invalid_paint = stylesheet_paint_stack(
        &sheet, computed_style_paint_stack_id(&invalid_style));
    const StylePaintStack *math_paint = stylesheet_paint_stack(
        &sheet, computed_style_paint_stack_id(&math_style));
    CHECK(child_paint != NULL
          && child_paint->text_shadow_count == 1
          && child_paint->text_shadows[0].offset_x == 1
          && child_paint->text_shadows[0].offset_y == 2
          && style_box_shadow_uses_current_color(
                 &child_paint->text_shadows[0]));
    CHECK(multi_paint != NULL
          && multi_paint->text_shadow_count == 2
          && style_box_shadow_blur(&multi_paint->text_shadows[0]) == 4
          && multi_paint->text_shadows[1].offset_x == -1);
    CHECK(none_paint != NULL
          && none_paint->text_shadow_count == 0);
    CHECK(invalid_paint != NULL
          && invalid_paint->text_shadow_count == 1
          && invalid_paint->text_shadows[0].offset_x == 3
          && invalid_paint->text_shadows[0].argb == UINT32_C(0xff010203));
    CHECK(math_paint != NULL
          && math_paint->text_shadow_count == 1
          && math_paint->text_shadows[0].offset_x == 12
          && style_box_shadow_blur(&math_paint->text_shadows[0]) == 1);

    size_t child_texts = 0, multi_texts = 0, none_texts = 0;
    bool saw_blurred_red = false;
    bool saw_green = false;
    bool saw_pseudo_shadow = false;
    for (size_t i = 0; i < layout.count; i++) {
        const DrawCommand *command = &layout.commands[i];
        if (command->type != DRAW_TEXT || command->text == NULL) continue;
        if (command->text_length == 5
            && memcmp(command->text, "CHILD", 5) == 0) {
            child_texts++;
        } else if (command->text_length == 5
                   && memcmp(command->text, "MULTI", 5) == 0) {
            multi_texts++;
            if (draw_command_is_text_shadow(command)
                && command->color == UINT32_C(0xff0000)
                && draw_command_text_shadow_blur(command) == 4) {
                saw_blurred_red = true;
            }
            if (draw_command_is_text_shadow(command)
                && command->color == UINT32_C(0x00ff00)) {
                saw_green = true;
            }
        } else if (command->text_length == 4
                   && memcmp(command->text, "NONE", 4) == 0) {
            none_texts++;
        } else if (command->text_length == 1 && command->text[0] == 'P'
                   && draw_command_is_text_shadow(command)
                   && command->color == UINT32_C(0xfedcba)) {
            saw_pseudo_shadow = true;
        }
    }
    CHECK(child_texts == 2);
    CHECK(multi_texts == 3 && saw_blurred_red && saw_green);
    CHECK(none_texts == 1);
    CHECK(saw_pseudo_shadow);

    enum { FRAME_WIDTH = 480, FRAME_HEIGHT = 64 };
    TileCache cache = {0};
    uint16_t *frame = budget_malloc(
        budget, FRAME_WIDTH * FRAME_HEIGHT * sizeof(*frame));
    CHECK(frame != NULL && tile_cache_init(&cache, budget, &layout, 4)
          && tile_cache_set_frame(&cache, frame, FRAME_WIDTH * FRAME_HEIGHT)
          && tile_cache_render_frame(&cache, 0, FRAME_WIDTH, FRAME_HEIGHT,
                                     NULL));
    size_t red_shadow_pixels = 0;
    for (size_t i = 0; i < FRAME_WIDTH * FRAME_HEIGHT; i++) {
        unsigned red = (frame[i] >> 11) & 31u;
        unsigned green = (frame[i] >> 5) & 63u;
        unsigned blue = frame[i] & 31u;
        /* The half-alpha red blur is the only authored surface whose red
           channel can dominate both blue and the doubled green channel. */
        if (red > blue + 6u && red * 2u > green + 8u) {
            red_shadow_pixels++;
        }
    }
    CHECK(red_shadow_pixels != 0);
    tile_cache_destroy(&cache);
    budget_free(budget, frame);

    layout_destroy(&layout);
    stylesheet_destroy(&sheet);
    document_destroy(&document);
    CHECK(budget->current == 0);
    return 0;
}

/* Ordered (Bayer) dithering at the 24-bit -> RGB565 step.  The gradient here
   is deliberately shallow: red runs 0x30..0x34, which is less than one 5-bit
   quantisation step, so truncation collapses the whole ramp to a single code
   and only dithering can produce more than one.  The flat fill beside it
   pins the other half of the contract -- flat surfaces must stay flat, since
   dithering them is pure added noise. */
static int test_rgb565_ordered_dither(Budget *budget)
{
    enum { FRAME_WIDTH = 64, FRAME_HEIGHT = 32 };
    static const char page[] =
        "<!doctype html><style>html,body{margin:0;padding:0;background:#fff}"
        "div{display:block;width:64px;height:16px}"
        "#ramp{background:linear-gradient(to right,#303030,#343434)}"
        "#flat{background:#303030}"
        "</style><body><div id=ramp></div><div id=flat></div></body>";
    PocDocument document = {0};
    Stylesheet sheet = {0};
    LayoutDocument layout = {0};
    TileCache cache = {0};
    uint16_t *frame = NULL;
    uint16_t *again = NULL;
    CHECK(document_parse(&document, budget, page, strlen(page), 14)
          && stylesheet_build(&sheet, budget, &document, FRAME_WIDTH)
          && layout_build(&layout, budget, &document, &sheet,
                          NULL, NULL, FRAME_WIDTH));
    frame = budget_malloc(
        budget, FRAME_WIDTH * FRAME_HEIGHT * sizeof(*frame));
    CHECK(frame != NULL && tile_cache_init(&cache, budget, &layout, 4)
          && tile_cache_set_frame(&cache, frame, FRAME_WIDTH * FRAME_HEIGHT)
          && tile_cache_render_frame(&cache, 0, FRAME_WIDTH, FRAME_HEIGHT,
                                     NULL));

    /* The ramp row must carry more than one code: without dithering the
       whole 0x30..0x34 span truncates to the same 5-bit red. */
    unsigned ramp_low = 32u, ramp_high = 0u;
    for (int x = 0; x < FRAME_WIDTH; x++) {
        unsigned red = (frame[(size_t) 8 * FRAME_WIDTH + x] >> 11) & 0x1fu;
        if (red < ramp_low) ramp_low = red;
        if (red > ramp_high) ramp_high = red;
    }
    if (ramp_high <= ramp_low) {
        fprintf(stderr, "dither ramp not spread: %u..%u\n",
                ramp_low, ramp_high);
    }
    CHECK(ramp_high > ramp_low && ramp_high - ramp_low == 1u);

    /* The flat fill must be exactly uniform -- a flat surface has no band to
       break up, so dithering it would only add stipple. */
    uint16_t flat = frame[(size_t) 24 * FRAME_WIDTH];
    for (int x = 1; x < FRAME_WIDTH; x++) {
        CHECK(frame[(size_t) 24 * FRAME_WIDTH + x] == flat);
    }

    /* A Bayer threshold is a pure function of (x, y, colour), so re-rendering
       the same frame through a fresh cache must be byte-identical.  This is
       the property that error diffusion would not have, and that the M3
       host/device frame-hash discipline depends on. */
    again = budget_malloc(
        budget, FRAME_WIDTH * FRAME_HEIGHT * sizeof(*again));
    TileCache second = {0};
    CHECK(again != NULL && tile_cache_init(&second, budget, &layout, 4)
          && tile_cache_set_frame(&second, again, FRAME_WIDTH * FRAME_HEIGHT)
          && tile_cache_render_frame(&second, 0, FRAME_WIDTH, FRAME_HEIGHT,
                                     NULL)
          && memcmp(frame, again,
                    FRAME_WIDTH * FRAME_HEIGHT * sizeof(*frame)) == 0);
    tile_cache_destroy(&second);
    budget_free(budget, again);

    tile_cache_destroy(&cache);
    budget_free(budget, frame);
    layout_destroy(&layout);
    stylesheet_destroy(&sheet);
    document_destroy(&document);
    CHECK(budget->current == 0);
    return 0;
}

static int test_rgb565_neutral_coordination(Budget *budget)
{
    enum { FRAME_WIDTH = 64, FRAME_HEIGHT = 48 };
    static const char page[] =
        "<!doctype html><style>html,body{margin:0;padding:0;background:#fff}"
        "div{display:block;width:64px;height:16px}"
        "#neutral{background:#f8f9fa}"
        "#neutral-rounded{background:#f8f9fa;border-radius:5px}"
        "#chromatic{background:#f6f6ef}"
        "</style><body><div id=neutral></div>"
        "<div id=neutral-rounded></div><div id=chromatic></div></body>";
    PocDocument document = {0};
    Stylesheet sheet = {0};
    LayoutDocument layout = {0};
    TileCache cache = {0};
    uint16_t *frame = NULL;
    CHECK(document_parse(&document, budget, page, strlen(page), 14)
          && stylesheet_build(&sheet, budget, &document, FRAME_WIDTH)
          && layout_build(&layout, budget, &document, &sheet,
                          NULL, NULL, FRAME_WIDTH));
    frame = budget_malloc(
        budget, FRAME_WIDTH * FRAME_HEIGHT * sizeof(*frame));
    CHECK(frame != NULL && tile_cache_init(&cache, budget, &layout, 4)
          && tile_cache_set_frame(&cache, frame, FRAME_WIDTH * FRAME_HEIGHT)
          && tile_cache_render_frame(&cache, 0, FRAME_WIDTH, FRAME_HEIGHT,
                                     NULL));

    uint16_t coordinated = (uint16_t) ((30u << 11) | (61u << 5) | 30u);
    CHECK(frame[8u * FRAME_WIDTH + 8u] == coordinated
          && frame[24u * FRAME_WIDTH + 8u] == coordinated
          && frame[40u * FRAME_WIDTH + 8u] == test_rgb565(0xf6f6ef));

    tile_cache_destroy(&cache);
    budget_free(budget, frame);
    layout_destroy(&layout);
    stylesheet_destroy(&sheet);
    document_destroy(&document);
    CHECK(budget->current == 0);
    return 0;
}

static int test_per_corner_border_radius(Budget *budget)
{
    enum { FRAME_WIDTH = 32, FRAME_HEIGHT = 24 };
    static const char page[] =
        "<!doctype html><style>html,body{margin:0;padding:0;background:#000}"
        "#paint{display:block;width:20px;height:20px;background:#fff;"
        "border-radius:10px 0 0 0}"
        "#clip{display:block;width:20px;height:20px;overflow:clip;"
        "border:2px solid;padding:2px;border-radius:0 4px 6px 8px}"
        "#merge{border-radius:2px;border-top-left-radius:8px}"
        "#reset{border-top-left-radius:8px;border-radius:3px}"
        ".cross{border-radius:2px}#cross{border-top-left-radius:9px}"
        "#invalid{border-radius:4px;border-radius:1px 2px 3px 4px 5px}"
        "</style><body><div id=paint></div><div id=clip></div><i id=merge></i>"
        "<i id=reset></i><i id=cross class=cross></i>"
        "<i id=invalid></i></body>";
    PocDocument document = {0};
    Stylesheet sheet = {0};
    LayoutDocument layout = {0};
    TileCache cache = {0};
    uint16_t *frame = NULL;
    CHECK(document_parse(&document, budget, page, strlen(page), 14)
          && stylesheet_build(&sheet, budget, &document, FRAME_WIDTH));
    lxb_dom_node_t *paint_node = find_id(
        lxb_dom_interface_node(document.html), "paint");
    lxb_dom_node_t *merge_node = find_id(
        lxb_dom_interface_node(document.html), "merge");
    lxb_dom_node_t *clip_node = find_id(
        lxb_dom_interface_node(document.html), "clip");
    lxb_dom_node_t *reset_node = find_id(
        lxb_dom_interface_node(document.html), "reset");
    lxb_dom_node_t *cross_node = find_id(
        lxb_dom_interface_node(document.html), "cross");
    lxb_dom_node_t *invalid_node = find_id(
        lxb_dom_interface_node(document.html), "invalid");
    ComputedStyle paint_style = style_for_node(&sheet, paint_node, NULL);
    ComputedStyle merge_style = style_for_node(&sheet, merge_node, NULL);
    ComputedStyle reset_style = style_for_node(&sheet, reset_node, NULL);
    ComputedStyle cross_style = style_for_node(&sheet, cross_node, NULL);
    ComputedStyle invalid_style = style_for_node(&sheet, invalid_node, NULL);
    int paint_radius = stylesheet_border_radius_code(&sheet, &paint_style);
    int merge_radius = stylesheet_border_radius_code(&sheet, &merge_style);
    int reset_radius = stylesheet_border_radius_code(&sheet, &reset_style);
    int cross_radius = stylesheet_border_radius_code(&sheet, &cross_style);
    CHECK(style_border_radius_is_packed(paint_radius)
          && style_border_radius_corner(paint_radius, 0) == 10
          && style_border_radius_corner(paint_radius, 1) == 0
          && style_border_radius_corner(paint_radius, 2) == 0
          && style_border_radius_corner(paint_radius, 3) == 0
          && style_border_radius_corner(merge_radius, 0) == 8
          && style_border_radius_corner(merge_radius, 1) == 2
          && style_border_radius_corner(merge_radius, 2) == 2
          && style_border_radius_corner(merge_radius, 3) == 2
          && reset_radius == 3
          && style_border_radius_corner(cross_radius, 0) == 9
          && style_border_radius_corner(cross_radius, 1) == 2
          && style_border_radius_corner(cross_radius, 2) == 2
          && style_border_radius_corner(cross_radius, 3) == 2
          && stylesheet_border_radius_code(&sheet, &invalid_style) == 4
          && layout_build(&layout, budget, &document, &sheet,
                          NULL, NULL, FRAME_WIDTH));
    const LayoutNodeBox *clip_box = layout_box_for_node(&layout, clip_node);
    int clip_radius = layout_node_box_clip_radius_code(&layout, clip_box);
    int effective_clip =
        layout_node_box_effective_clip_radius_code(&layout, clip_box);
    int outset_probe = style_border_radius_outset(
        style_border_radius_adjust(
            style_border_radius_pack(0, 15, 25, 35), -10),
        15, 120, 120);
    CHECK(layout.node_clip_radius_codes != NULL
          && style_border_radius_corner(clip_radius, 0) == 0
          && style_border_radius_corner(clip_radius, 1) == 4
          && style_border_radius_corner(clip_radius, 2) == 6
          && style_border_radius_corner(clip_radius, 3) == 8
          && style_border_radius_corner(effective_clip, 0) == 0
          && style_border_radius_corner(effective_clip, 1) == 2
          && style_border_radius_corner(effective_clip, 2) == 4
          && style_border_radius_corner(effective_clip, 3) == 6
          && style_border_radius_corner(outset_probe, 0) == 0
          && style_border_radius_corner(outset_probe, 1) == 16
          && style_border_radius_corner(outset_probe, 2) == 30
          && style_border_radius_corner(outset_probe, 3) == 40);
    frame = budget_malloc(
        budget, FRAME_WIDTH * FRAME_HEIGHT * sizeof(*frame));
    CHECK(frame != NULL && tile_cache_init(&cache, budget, &layout, 4)
          && tile_cache_set_frame(&cache, frame, FRAME_WIDTH * FRAME_HEIGHT)
          && tile_cache_render_frame(&cache, 0, FRAME_WIDTH, FRAME_HEIGHT,
                                     NULL));
    uint16_t page_background = frame[23u * FRAME_WIDTH + 31u];
    /* Only the authored top-left corner is cut away. The other three square
       corners and the box centre remain filled. */
    CHECK(frame[0] == page_background
          && frame[19] != page_background
          && frame[19u * FRAME_WIDTH] != page_background
          && frame[19u * FRAME_WIDTH + 19u] != page_background
          && frame[10u * FRAME_WIDTH + 10u] != page_background);
    tile_cache_destroy(&cache);
    budget_free(budget, frame);
    layout_destroy(&layout);
    stylesheet_destroy(&sheet);
    document_destroy(&document);
    CHECK(budget->current == 0);
    return 0;
}

static int test_box_shadow_raster(Budget *budget)
{
    enum { FRAME_WIDTH = 64, FRAME_HEIGHT = 64 };
    static const char page[] =
        "<!doctype html><style>html,body{margin:0;padding:0;background:#fff}"
        "#card{display:block;width:32px;height:24px;margin:16px;"
        "border-radius:6px;background:#fff;"
        "box-shadow:0 0 8px 0 #000000}"
        "</style><body><div id=card></div></body>";
    PocDocument document = {0};
    Stylesheet sheet = {0};
    LayoutDocument layout = {0};
    TileCache cache = {0};
    uint16_t *frame = NULL;
    CHECK(document_parse(&document, budget, page, strlen(page), 12)
          && stylesheet_build(&sheet, budget, &document, FRAME_WIDTH)
          && layout_build(&layout, budget, &document, &sheet,
                          NULL, NULL, FRAME_WIDTH));
    const DrawCommand *shadow = NULL;
    for (size_t i = 0; i < layout.count; i++) {
        if (layout.commands[i].type == DRAW_SHADOW_RECT) {
            shadow = &layout.commands[i];
            break;
        }
    }
    /* The command box is the border box inflated by the blur on every side,
       so the tile cull sees the full painted extent. */
    CHECK(shadow != NULL && shadow->scale == 8
          && shadow->x == 16 - 8 && shadow->y == 16 - 8
          && shadow->width == 32 + 16 && shadow->height == 24 + 16
          && shadow->radius == 6);

    frame = budget_malloc(
        budget, FRAME_WIDTH * FRAME_HEIGHT * sizeof(*frame));
    CHECK(frame != NULL && tile_cache_init(&cache, budget, &layout, 4)
          && tile_cache_set_frame(&cache, frame, FRAME_WIDTH * FRAME_HEIGHT)
          && tile_cache_render_frame(&cache, 0, FRAME_WIDTH, FRAME_HEIGHT,
                                     NULL));
#define LUMA(x, y) ((unsigned) ((frame[(size_t) (y) * FRAME_WIDTH + (x)] >> 11) \
                                & 0x1fu))
    /* Along a horizontal cut through the card's middle the shadow must fade
       monotonically outward: dark at the border-box edge, lighter further
       out, and untouched white beyond the blur. */
    unsigned edge = LUMA(15, 28);
    unsigned mid = LUMA(12, 28);
    unsigned far = LUMA(9, 28);
    unsigned clear = LUMA(2, 28);
    if (!(edge < mid && mid < far && far <= clear && clear == 31u
          && edge < 24u)) {
        fprintf(stderr, "shadow ramp %u/%u/%u/%u\n", edge, mid, far, clear);
    }
    CHECK(edge < mid && mid < far && far <= clear && clear == 31u
          && edge < 24u);
    /* The card itself paints over its own shadow and stays white. */
    CHECK(LUMA(32, 28) == 31u);
    /* The falloff is symmetric about the box: a zero-offset shadow must
       read the same on both sides.  The mirrored columns land in different
       Bayer cells, and the shadow ramp is dithered, so ordered dithering may
       separate them by one quantisation code -- but no more than one. */
    unsigned left_side = LUMA(12, 28);
    unsigned right_side = LUMA(51, 28);
    CHECK(left_side - right_side + 1u <= 2u);
#undef LUMA
    tile_cache_destroy(&cache);
    budget_free(budget, frame);
    layout_destroy(&layout);
    stylesheet_destroy(&sheet);
    document_destroy(&document);
    CHECK(budget->current == 0);
    return 0;
}

static bool paint_layout_frame(
    Budget *budget, LayoutDocument *layout, uint16_t *frame,
    int width, int height)
{
    TileCache cache = {0};
    size_t pixels = (size_t) width * (size_t) height;
    memset(frame, 0, pixels * sizeof(*frame));
    bool ok = tile_cache_init(&cache, budget, layout, 4)
        && tile_cache_set_frame(&cache, frame, pixels)
        && tile_cache_render_frame(&cache, 0, width, height, NULL);
    tile_cache_destroy(&cache);
    return ok;
}

/*
 * Rounded-fill coverage squares its sample offsets, and the radius it
 * squares comes from a retained draw command rather than from anything the
 * raster layer bounds. A page cannot reach these numbers through the
 * cascade -- the border-radius encoding caps a non-packed code at
 * 0x0fffffff -- but the command array is the boundary that matters, so this
 * writes the largest legal radius onto a near-INT_MAX box directly, which is
 * what a corrupted or hostile retained layout would present.
 *
 * The arc from a radius that large passes nowhere near the frame, so the
 * correct answer is that every pixel is carved away: the result must equal
 * the frame the same command paints when it covers nothing at all. The
 * squared-radius control proves the command really did reach the frame,
 * so an "identical" result cannot come from the fill being culled. Run
 * under the sanitize preset this also pins that the arithmetic stays
 * defined; run anywhere it pins that the guard did not start eroding or
 * inflating the shape.
 */
static int test_rounded_coverage_hostile_radius(Budget *budget)
{
    enum { FRAME_WIDTH = 32, FRAME_HEIGHT = 32,
           FRAME_PIXELS = FRAME_WIDTH * FRAME_HEIGHT };
    static const char page[] =
        "<!doctype html><style>html,body{margin:0;padding:0;background:#fff}"
        "#box{display:block;width:16px;height:16px;border-radius:4px;"
        "background:#000}"
        "</style><body><div id=box></div></body>";
    PocDocument document = {0};
    Stylesheet sheet = {0};
    LayoutDocument layout = {0};
    uint16_t *frame = NULL;
    uint16_t hostile[FRAME_PIXELS];
    uint16_t absent[FRAME_PIXELS];
    CHECK(document_parse(&document, budget, page, strlen(page), 11)
          && stylesheet_build(&sheet, budget, &document, FRAME_WIDTH)
          && layout_build(&layout, budget, &document, &sheet,
                          NULL, NULL, FRAME_WIDTH));
    DrawCommand *fill = NULL;
    for (size_t i = 0; i < layout.count; i++) {
        if (layout.commands[i].type == DRAW_FILL_RECT
            && layout.commands[i].radius == 4) {
            fill = &layout.commands[i];
            break;
        }
    }
    CHECK(fill != NULL);
    frame = budget_malloc(budget, FRAME_PIXELS * sizeof(*frame));
    CHECK(frame != NULL);

    /* The largest non-packed radius code on a box wide enough to keep it:
       style_border_radius_resolve_corners caps at min(width, height)/2. */
    fill->x = 0;
    fill->y = 0;
    fill->width = 2000000000;
    fill->height = 2000000000;
    fill->radius = 0x0fffffff;
    CHECK(paint_layout_frame(
        budget, &layout, frame, FRAME_WIDTH, FRAME_HEIGHT));
    memcpy(hostile, frame, sizeof(hostile));

    /* Same command, covering nothing: the reference for "carved away". */
    fill->width = 0;
    fill->height = 0;
    CHECK(paint_layout_frame(
        budget, &layout, frame, FRAME_WIDTH, FRAME_HEIGHT));
    memcpy(absent, frame, sizeof(absent));
    CHECK(memcmp(hostile, absent, sizeof(hostile)) == 0);

    /* Square corners on the same hostile box: proof the fill was in range
       and that the previous two results are not just a culled command. */
    fill->width = 2000000000;
    fill->height = 2000000000;
    fill->radius = 0;
    CHECK(paint_layout_frame(
        budget, &layout, frame, FRAME_WIDTH, FRAME_HEIGHT));
    CHECK(memcmp(hostile, frame, sizeof(hostile)) != 0);

    budget_free(budget, frame);
    layout_destroy(&layout);
    stylesheet_destroy(&sheet);
    document_destroy(&document);
    CHECK(budget->current == 0);
    return 0;
}

/* The box starts one pixel inside the frame, so whatever it claims for its
   far edge, the row above it and the column left of it stay the page. */
static bool frame_border_is_untouched(const uint16_t *frame,
                                      const uint16_t *reference,
                                      int width, int height)
{
    for (int x = 0; x < width; x++) {
        if (frame[x] != reference[x]) return false;
    }
    for (int y = 0; y < height; y++) {
        size_t index = (size_t) y * (size_t) width;
        if (frame[index] != reference[index]) return false;
    }
    return true;
}

/*
 * Every span clamp in the rasteriser reduces a command to the tile by
 * comparing command->x + command->width against the tile's right edge, and
 * both operands are retained-layout coordinates with no bound of their own.
 * Computed in plain int that sum is signed overflow -- undefined before any
 * clamp can apply -- which is exactly why the hostile box in b552728 had to
 * stop its extent at 2e9 rather than push the far edge past INT_MAX. That
 * constraint is lifted here: the box sits one pixel inside the frame and
 * claims INT_MAX of extent, so x + width and y + height both step past the
 * signed range.
 *
 * The oracle is that a far edge past INT_MAX has to paint exactly what a
 * far edge merely past the frame paints -- both are equally out of range,
 * and nothing between them is visible. That byte comparison runs for every
 * command kind whose appearance is edge-local: fills, shadows and strokes,
 * square-cornered and rounded, each against a covers-nothing control so
 * the equality cannot be two blank frames. The image path
 * resamples the source across its own box, so its pixels legitimately move
 * with the far edge; it gets the pins that still apply, that it paints, and
 * that it paints only inside the box. Guard words past the frame catch any
 * kind that clamps to a wrapped extent and walks off the buffer, and under
 * the sanitize preset the whole test pins that the arithmetic stays defined.
 */
static int test_command_extent_overflow_clamp(Budget *budget)
{
    enum { FRAME_WIDTH = 32, FRAME_HEIGHT = 32,
           FRAME_PIXELS = FRAME_WIDTH * FRAME_HEIGHT,
           GUARD_PIXELS = 16,
           BOX_ORIGIN = 1,
           /* Past the frame on both axes, so this far edge is out of range
              too and only the overflow can tell the two runs apart. */
           SANE_EXTENT = 4 * FRAME_WIDTH,
           /* Unique in the command array, so the box is findable. */
           BOX_RADIUS = 7 };
    static const uint16_t GUARD_WORD = 0xa5a5u;
    static const char page[] =
        "<!doctype html><style>html,body{margin:0;padding:0;background:#fff}"
        "#box{display:block;width:16px;height:16px;border-radius:7px;"
        "background:#000}"
        "</style><body><div id=box></div></body>";
    static unsigned char icon_pixels[] = {
        255, 0, 0, 255,    0, 255, 0, 255,
        0, 0, 255, 255,    255, 255, 0, 255
    };
    ImageResource icon = {
        .pixels = icon_pixels, .width = 2, .height = 2
    };
    static const struct {
        uint8_t type;
        int radius;
        int scale;
        uint8_t image_fit;
    } kinds[] = {
        {DRAW_FILL_RECT, 0, 1, 0},
        {DRAW_FILL_RECT, BOX_RADIUS, 1, 0},
        /* Blur zero keeps the shadow's falloff edge-local, which is what
           makes the two extents comparable at all. */
        {DRAW_SHADOW_RECT, 0, 0, 0},
        {DRAW_SHADOW_RECT, BOX_RADIUS, 0, 0},
        {DRAW_STROKE_RECT, 0, 3, 0},
        {DRAW_STROKE_RECT, BOX_RADIUS, 3, 0},
        /* The dashed and dotted arms decide which edge a row belongs to
           with command->y + command->height and phase the pattern from the
           command corner, so a hostile far edge reaches sums the solid
           stroke never forms. */
        {DRAW_STROKE_RECT, 0, 3, LAYOUT_STROKE_DASHED},
        {DRAW_STROKE_RECT, BOX_RADIUS, 3, LAYOUT_STROKE_DASHED},
        {DRAW_STROKE_RECT, 0, 3, LAYOUT_STROKE_DOTTED},
        {DRAW_STROKE_RECT, BOX_RADIUS, 3, LAYOUT_STROKE_DOTTED},
        /* A hostile scale overflows on its own -- the inner box and the
           dash period are both built from it -- and the clamped thickness
           swallows the whole box either way: no inner survives and no dash
           gap fits, so both extents paint the same solid edge band. */
        {DRAW_STROKE_RECT, 0, INT_MAX, 0},
        {DRAW_STROKE_RECT, 0, INT_MAX, LAYOUT_STROKE_DASHED},
        {DRAW_STROKE_RECT, BOX_RADIUS, INT_MAX, LAYOUT_STROKE_DOTTED}
    };
    PocDocument document = {0};
    Stylesheet sheet = {0};
    LayoutDocument layout = {0};
    uint16_t *frame = NULL;
    uint16_t hostile[FRAME_PIXELS];
    uint16_t absent[FRAME_PIXELS];
    CHECK(document_parse(&document, budget, page, strlen(page), 11)
          && stylesheet_build(&sheet, budget, &document, FRAME_WIDTH)
          && layout_build(&layout, budget, &document, &sheet,
                          NULL, NULL, FRAME_WIDTH));
    DrawCommand *box = NULL;
    for (size_t i = 0; i < layout.count; i++) {
        if (layout.commands[i].type == DRAW_FILL_RECT
            && layout.commands[i].radius == BOX_RADIUS) {
            box = &layout.commands[i];
            break;
        }
    }
    CHECK(box != NULL);
    frame = budget_malloc(
        budget, (FRAME_PIXELS + GUARD_PIXELS) * sizeof(*frame));
    CHECK(frame != NULL);
    for (size_t i = FRAME_PIXELS; i < FRAME_PIXELS + GUARD_PIXELS; i++) {
        frame[i] = GUARD_WORD;
    }
    box->x = BOX_ORIGIN;
    box->y = BOX_ORIGIN;

    /* The page with the box carrying no extent: the "did it paint" control
       and the reference for the untouched first row and column. */
    box->width = 0;
    box->height = 0;
    CHECK(paint_layout_frame(
        budget, &layout, frame, FRAME_WIDTH, FRAME_HEIGHT));
    memcpy(absent, frame, sizeof(absent));

    for (size_t k = 0; k < sizeof(kinds) / sizeof(kinds[0]); k++) {
        box->type = kinds[k].type;
        box->radius = kinds[k].radius;
        box->scale = kinds[k].scale;
        box->image_fit = kinds[k].image_fit;
        box->width = INT_MAX;
        box->height = INT_MAX;
        CHECK(paint_layout_frame(
            budget, &layout, frame, FRAME_WIDTH, FRAME_HEIGHT));
        memcpy(hostile, frame, sizeof(hostile));
        CHECK(memcmp(hostile, absent, sizeof(hostile)) != 0);
        CHECK(frame_border_is_untouched(
            hostile, absent, FRAME_WIDTH, FRAME_HEIGHT));
        box->width = SANE_EXTENT;
        box->height = SANE_EXTENT;
        CHECK(paint_layout_frame(
            budget, &layout, frame, FRAME_WIDTH, FRAME_HEIGHT));
        CHECK(memcmp(hostile, frame, sizeof(hostile)) == 0);
    }

    /* The image path clamps its own span, in the rotated stretch branch
       that does not share the sizing the other kinds go through. */
    box->type = DRAW_IMAGE;
    box->image = &icon;
    box->image_fit = LAYOUT_IMAGE_FIT_STRETCH;
    box->radius = 0;
    box->scale = 0;
    draw_command_set_rotation_quadrants(box, 1u);
    box->width = INT_MAX;
    box->height = INT_MAX;
    CHECK(paint_layout_frame(
        budget, &layout, frame, FRAME_WIDTH, FRAME_HEIGHT));
    CHECK(memcmp(frame, absent, sizeof(absent)) != 0);
    CHECK(frame_border_is_untouched(
        frame, absent, FRAME_WIDTH, FRAME_HEIGHT));
    draw_command_set_rotation_quadrants(box, 0u);
    box->image = NULL;

    /* A hostile blur is the shadow arm's own scale overflow: command->x +
       blur and the 2 * blur deflation. Blur cannot join the extent loop
       above because its falloff is not edge-local; the defined outcome
       here is that the clamped blur still deflates the sane box away and
       nothing paints. */
    box->type = DRAW_SHADOW_RECT;
    box->image_fit = 0;
    box->radius = 0;
    box->scale = INT_MAX;
    box->width = SANE_EXTENT;
    box->height = SANE_EXTENT;
    CHECK(paint_layout_frame(
        budget, &layout, frame, FRAME_WIDTH, FRAME_HEIGHT));
    CHECK(memcmp(frame, absent, sizeof(absent)) == 0);

    for (size_t i = FRAME_PIXELS; i < FRAME_PIXELS + GUARD_PIXELS; i++) {
        CHECK(frame[i] == GUARD_WORD);
    }

    budget_free(budget, frame);
    layout_destroy(&layout);
    stylesheet_destroy(&sheet);
    document_destroy(&document);
    CHECK(budget->current == 0);
    return 0;
}

/* A page whose every box asks for the maximum shadow the grammar allows
   must still render, and must not emit more than the per-element cap. */
static int test_box_shadow_hostile_clamp(Budget *budget)
{
    enum { FRAME_WIDTH = 64, FRAME_HEIGHT = 64 };
    static const char page[] =
        "<!doctype html><style>html,body{margin:0;padding:0}"
        "div{display:block;width:16px;height:16px;border-radius:32767px;"
        "box-shadow:99999px 99999px 99999px 99999px #f00,"
        "-99999px -99999px 99999px 99999px #0f0,"
        "0 0 99999px 99999px #00f,1px 1px 99999px 99999px #ff0}"
        "</style><body><div></div><div></div><div></div></body>";
    PocDocument document = {0};
    Stylesheet sheet = {0};
    LayoutDocument layout = {0};
    TileCache cache = {0};
    uint16_t *frame = NULL;
    CHECK(document_parse(&document, budget, page, strlen(page), 13)
          && stylesheet_build(&sheet, budget, &document, FRAME_WIDTH)
          && layout_build(&layout, budget, &document, &sheet,
                          NULL, NULL, FRAME_WIDTH));
    size_t shadows = 0;
    for (size_t i = 0; i < layout.count; i++) {
        const DrawCommand *command = &layout.commands[i];
        if (command->type != DRAW_SHADOW_RECT) continue;
        shadows++;
        /* Blur and spread are both capped, so the skirt around a 16px box
           can never exceed 2*(32+32) on either axis. */
        CHECK(command->scale >= 0
              && command->scale <= STYLE_BOX_SHADOW_BLUR_LIMIT
              && command->width <= 16 + 2 * (STYLE_BOX_SHADOW_BLUR_LIMIT
                                             + STYLE_BOX_SHADOW_SPREAD_LIMIT)
              && command->height <= 16 + 2 * (STYLE_BOX_SHADOW_BLUR_LIMIT
                                              + STYLE_BOX_SHADOW_SPREAD_LIMIT));
    }
    CHECK(shadows == 3u * STYLE_BOX_SHADOW_LIMIT);
    frame = budget_malloc(
        budget, FRAME_WIDTH * FRAME_HEIGHT * sizeof(*frame));
    CHECK(frame != NULL && tile_cache_init(&cache, budget, &layout, 4)
          && tile_cache_set_frame(&cache, frame, FRAME_WIDTH * FRAME_HEIGHT)
          && tile_cache_render_frame(&cache, 0, FRAME_WIDTH, FRAME_HEIGHT,
                                     NULL));
    tile_cache_destroy(&cache);
    budget_free(budget, frame);
    layout_destroy(&layout);
    stylesheet_destroy(&sheet);
    document_destroy(&document);
    CHECK(budget->current == 0);
    return 0;
}
