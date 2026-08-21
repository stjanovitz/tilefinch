    Budget budget;
    budget_init(&budget, 16 * MIB);
    budget_install_lexbor(&budget);

    puts("test: bounded deep block, inline, and intrinsic layout stacks");
    CHECK(test_reuse_rejects_same_address_stylesheet_rebuild(&budget) == 0);
    CHECK(test_reuse_preserves_generation_across_stylesheet_move(&budget)
          == 0);
    CHECK(test_has_attribute_dependency_classification(&budget) == 0);
    CHECK(test_level_four_selector_matching(&budget) == 0);
    CHECK(test_selector_walk_bounds(&budget) == 0);
    CHECK(test_has_unrelated_scoped_reuse_converges(&budget) == 0);
    CHECK(test_deep_layout_case(&budget, DEEP_LAYOUT_BLOCK, 16, true));
    /* Ordinary framework wrapper depth must retain real layout boxes rather
       than falling into the content-only hostile-depth approximation. */
    CHECK(test_deep_layout_case(&budget, DEEP_LAYOUT_BLOCK, 40, true));
    CHECK(test_deep_layout_case(&budget, DEEP_LAYOUT_BLOCK, 80, false));
    CHECK(test_deep_layout_case(&budget, DEEP_LAYOUT_INLINE, 80, false));
    CHECK(test_deep_layout_case(
        &budget, DEEP_LAYOUT_FLEX_INTRINSIC, 80, false));

    puts("test: empty blocks and block-boundary whitespace");
    CHECK(test_block_boundary_whitespace(&budget) == 0);

    puts("test: overflow-wrap parsing, flow, and intrinsic sizing");
    CHECK(test_overflow_wrap_correctness(&budget) == 0);

    puts("test: mobile overflow containment and automatic minimums");
    CHECK(test_mobile_overflow_intrinsic_minimums(&budget) == 0);

    puts("test: modern mobile interaction and presentation CSS");
    CHECK(test_modern_mobile_css_interaction_and_presentation(&budget) == 0);

    puts("test: calculated unitless line-height multipliers");
    CHECK(test_calculated_unitless_line_height(&budget) == 0);

    puts("test: native form-control geometry and retained painting");
    CHECK(test_native_form_control_fidelity(&budget) == 0);

    puts("test: direction-aware logical dimensions, edges, and insets");
    CHECK(test_directional_logical_properties(&budget) == 0);

    puts("test: Unicode line opportunities and bounded RTL flow");
    CHECK(test_international_line_flow(&budget) == 0);

    puts("test: HTML UA body margin and author spacing resets");
    CHECK(test_ua_body_spacing(&budget) == 0);

    puts("test: inline replaced-element box model and decoration");
    CHECK(test_inline_replaced_box_model(&budget) == 0);

    puts("test: author auto sizing overrides HTML image dimensions");
    CHECK(test_css_auto_overrides_image_dimension_hint(&budget) == 0);

    puts("test: responsive replaced sizing and object positioning");
    CHECK(test_responsive_replaced_sizing(&budget) == 0);
    puts("test: bottom-positioned assets use final containing height");
    CHECK(test_bottom_positioned_asset_uses_final_containing_height(&budget)
          == 0);
    puts("test: failed image declared geometry converges across preview");
    CHECK(test_declared_failed_image_geometry_converges(&budget) == 0);
    puts("test: width-only failed flex images retain alt text height");
    CHECK(test_width_only_failed_image_in_flex_shows_alt(&budget) == 0);

    puts("test: resumable authoritative layout is exact and transactional");
    CHECK(test_resumable_authoritative_layout(&budget) == 0);

    puts("test: table column constraints, rowspan placement, and captions");
    CHECK(test_table_column_and_rowspan_construction(&budget) == 0);

    puts("test: percentage images do not inflate flex auto minimums");
    CHECK(test_flex_percentage_image_minimum(&budget) == 0);

    puts("test: percentage widths remain indefinite in flex intrinsic sizing");
    CHECK(test_flex_percentage_width_intrinsic(&budget) == 0);

    puts("test: hidden fixed descendants do not inflate flex intrinsics");
    static const char fixed_intrinsic_page[] =
        "<!doctype html><style>"
        "body{margin:0}.host{display:flex;width:200px}"
        ".toggle{display:block;width:20px;height:10px}"
        ".drawer{position:fixed;visibility:hidden;min-width:150px;"
        "height:500px}"
        ".brand{display:block;width:40px;height:10px}"
        "</style><div class=host><nav id=nav><span class=toggle></span>"
        "<div class=drawer>OFFSCREEN MENU</div></nav>"
        "<span id=brand class=brand></span></div>";
    PocDocument fixed_intrinsic_document;
    Stylesheet fixed_intrinsic_sheet;
    LayoutDocument fixed_intrinsic_layout;
    CHECK(document_parse(
              &fixed_intrinsic_document, &budget, fixed_intrinsic_page,
              strlen(fixed_intrinsic_page), 47)
          && stylesheet_build(
              &fixed_intrinsic_sheet, &budget, &fixed_intrinsic_document, 200)
          && layout_build(
              &fixed_intrinsic_layout, &budget, &fixed_intrinsic_document,
              &fixed_intrinsic_sheet, NULL, NULL, 200));
    lxb_dom_node_t *fixed_intrinsic_root =
        lxb_dom_interface_node(fixed_intrinsic_document.html);
    const LayoutNodeBox *fixed_intrinsic_nav_box = layout_box_for_node(
        &fixed_intrinsic_layout, find_id(fixed_intrinsic_root, "nav"));
    const LayoutNodeBox *fixed_intrinsic_brand_box = layout_box_for_node(
        &fixed_intrinsic_layout, find_id(fixed_intrinsic_root, "brand"));
    CHECK(fixed_intrinsic_nav_box != NULL
          && fixed_intrinsic_brand_box != NULL
          && fixed_intrinsic_nav_box->width == 20
          && fixed_intrinsic_nav_box->content_height == 10
          && fixed_intrinsic_brand_box->x == 20
          && fixed_intrinsic_layout.height == 272);
    layout_destroy(&fixed_intrinsic_layout);
    stylesheet_destroy(&fixed_intrinsic_sheet);
    document_destroy(&fixed_intrinsic_document);
    CHECK(budget.current == 0);

    puts("test: hidden inputs do not become flex items or painted values");
    static const char hidden_input_page[] =
        "<!doctype html><style>body{margin:0}form{display:flex;gap:6px}"
        "input[type=search]{width:100px}button{width:50px}</style>"
        "<form><input id=search type=search value=fixture>"
        "<input id=layout type=hidden value=compact>"
        "<button id=submit>Search</button></form>";
    PocDocument hidden_input_document;
    Stylesheet hidden_input_sheet;
    LayoutDocument hidden_input_layout;
    CHECK(document_parse(
              &hidden_input_document, &budget, hidden_input_page,
              strlen(hidden_input_page), 47)
          && stylesheet_build(
              &hidden_input_sheet, &budget, &hidden_input_document, 480)
          && layout_build(
              &hidden_input_layout, &budget, &hidden_input_document,
              &hidden_input_sheet, NULL, NULL, 480));
    lxb_dom_node_t *hidden_input_root =
        lxb_dom_interface_node(hidden_input_document.html);
    lxb_dom_node_t *hidden_input = find_id(hidden_input_root, "layout");
    const LayoutNodeBox *search_input_box = layout_box_for_node(
        &hidden_input_layout, find_id(hidden_input_root, "search"));
    const LayoutNodeBox *submit_button_box = layout_box_for_node(
        &hidden_input_layout, find_id(hidden_input_root, "submit"));
    bool hidden_value_painted = false;
    for (size_t i = 0; i < hidden_input_layout.count; i++) {
        const DrawCommand *command = &hidden_input_layout.commands[i];
        if (command->type == DRAW_TEXT && command->text_length == 7
            && memcmp(command->text, "compact", 7) == 0) {
            hidden_value_painted = true;
        }
    }
    CHECK(hidden_input != NULL
          && layout_box_for_node(&hidden_input_layout, hidden_input) == NULL
          && search_input_box != NULL && submit_button_box != NULL
          && submit_button_box->x == search_input_box->x
                                      + search_input_box->width + 6
          && !hidden_value_painted);
    layout_destroy(&hidden_input_layout);
    stylesheet_destroy(&hidden_input_sheet);
    document_destroy(&hidden_input_document);
    CHECK(budget.current == 0);

    puts("test: generated block pseudo-elements participate in inline flow");
    CHECK(test_generated_block_pseudo_flow(&budget) == 0);

    puts("test: generated content counters and list markers");
    CHECK(test_generated_content_counters_and_markers(&budget) == 0);

    puts("test: font math uses inherited em and computed root rem bases");
    CHECK(test_font_relative_math_bases(&budget) == 0);

    puts("test: flex-flow shorthand sets direction and wrap");
    CHECK(test_flex_flow_shorthand(&budget) == 0);

    puts("test: width fit-content retains the shrink-to-fit mode");
    CHECK(test_width_fit_content(&budget) == 0);

    puts("test: intrinsic sizing, line clamp and content visibility");
    CHECK(test_intrinsic_keywords_line_clamp_and_content_visibility(
              &budget) == 0);

    puts("test: root font-size cascade with CSS-wide keywords and rem");
    CHECK(test_root_font_size_cascade_keywords(&budget) == 0);

    puts("test: underlines and link hits include collapsed internal spaces");
    CHECK(test_underlined_internal_whitespace(&budget) == 0);

    puts("test: explicit underline offsets move ink without moving glyphs");
    CHECK(test_text_underline_offset_raster(&budget) == 0);

    puts("test: order-preserving stylesheet suffix continuation");
    CHECK(test_stylesheet_ordered_append(&budget) == 0);

    puts("test: bounded linear-gradient parsing and degradation");
    CHECK(test_linear_gradient_parse(&budget) == 0);
    CHECK(test_retained_paint_stack_addresses_are_stable(&budget) == 0);
    CHECK(test_bounded_shared_paint_stack(&budget) == 0);
    CHECK(test_compact_visual_css_extensions(&budget) == 0);
    CHECK(test_bounded_typography_and_compositing(&budget) == 0);
    CHECK(test_backdrop_filter_aggregate_budget(&budget) == 0);

    puts("test: unresolved CSS masks stay transparent without layout shifts");
    CHECK(test_unresolved_mask_keeps_transparent_geometry(&budget) == 0);

    puts("test: linear-gradient rasterizes as an axial ramp");
    CHECK(test_linear_gradient_raster(&budget) == 0);
    CHECK(test_background_layers_paint_front_to_back(&budget) == 0);

    puts("test: flex overlays use final size and flex-item paint order");
    CHECK(test_flex_overlay_used_size_and_paint_order(&budget) == 0);

    puts("test: bounded box-shadow parsing, clamping and rejection");
    CHECK(test_box_shadow_parse(&budget) == 0);

    puts("test: inherited text-shadow emits bounded shadow text");
    CHECK(test_text_shadow_cascade_and_display_list(&budget) == 0);

    puts("test: ordered dithering smooths ramps and leaves flats alone");
    CHECK(test_rgb565_ordered_dither(&budget) == 0);

    puts("test: near-neutral RGB565 surfaces keep coordinated channels");
    CHECK(test_rgb565_neutral_coordination(&budget) == 0);

    puts("test: per-corner radii cascade and rasterize independently");
    CHECK(test_per_corner_border_radius(&budget) == 0);

    puts("test: box-shadow rasterizes a rounded analytic falloff");
    CHECK(test_box_shadow_raster(&budget) == 0);

    puts("test: hostile box-shadow geometry stays raster-bounded");
    CHECK(test_box_shadow_hostile_clamp(&budget) == 0);

    puts("test: hostile rounded-fill radius stays raster-bounded");
    CHECK(test_rounded_coverage_hostile_radius(&budget) == 0);

    puts("test: overflowing command extents stay raster-bounded");
    CHECK(test_command_extent_overflow_clamp(&budget) == 0);

    puts("test: hostile transformed text and radii stay raster-bounded");
    CHECK(test_bounded_transformed_text_and_radius(&budget) == 0);

    puts("test: CSS table cells form an implicit row");
    CHECK(test_css_table_implicit_row(&budget) == 0);

    puts("test: end padding contributes once to scroll extent");
    CHECK(test_padding_scroll_extent(&budget) == 0);

    puts("test: vertical margin collapse and formatting-context boundaries");
    CHECK(test_vertical_margin_collapse(&budget) == 0);

    puts("test: reference-resolved CSS math and transactional parsing");
    CHECK(test_reference_resolved_css_math(&budget) == 0);

    puts("test: conditional cascade layers and bounded CSS nesting");
    CHECK(test_modern_conditional_cascade(&budget) == 0);
    CHECK(test_bounded_container_queries(&budget) == 0);
    CHECK(test_bounded_multicolumn_blocks(&budget) == 0);

    puts("test: bounded large layout values and aspect products");
    CHECK(test_bounded_large_layout_values(&budget) == 0);

    puts("test: UA table and form-control box models");
    CHECK(test_table_and_control_ua_box_models(&budget) == 0);

    puts("test: table-cell vertical alignment and flattened row groups");
    CHECK(test_table_cell_vertical_alignment(&budget) == 0);

    puts("test: responsive CSS and flexbox");
    PocDocument responsive;
    CHECK(document_parse(&responsive, &budget, responsive_page,
                         strlen(responsive_page), 31));
    lxb_dom_node_t *cards = find_id(lxb_dom_interface_node(responsive.html),
                                    "cards");
    lxb_dom_node_t *left = find_id(lxb_dom_interface_node(responsive.html),
                                   "left");
    lxb_dom_node_t *right = find_id(lxb_dom_interface_node(responsive.html),
                                    "right");
    lxb_dom_node_t *pixel = find_id(lxb_dom_interface_node(responsive.html),
                                    "pixel");
    lxb_dom_node_t *icon = find_id(lxb_dom_interface_node(responsive.html),
                                   "icon");
    lxb_dom_node_t *nobullet = find_id(lxb_dom_interface_node(responsive.html),
                                       "nobullet");
    lxb_dom_node_t *slant = find_id(lxb_dom_interface_node(responsive.html),
                                    "slant");
    lxb_dom_node_t *sticky = find_id(lxb_dom_interface_node(responsive.html),
                                     "sticky");
    lxb_dom_node_t *fixed = find_id(lxb_dom_interface_node(responsive.html),
                                    "fixed");
    lxb_dom_node_t *rounded = find_id(lxb_dom_interface_node(responsive.html),
                                      "rounded");
    lxb_dom_node_t *empty_slot = find_id(
        lxb_dom_interface_node(responsive.html), "empty-slot");
    lxb_dom_node_t *centered = find_id(
        lxb_dom_interface_node(responsive.html), "centered");
    lxb_dom_node_t *nowrap = find_id(
        lxb_dom_interface_node(responsive.html), "nowrap");
    lxb_dom_node_t *grid = find_id(
        lxb_dom_interface_node(responsive.html), "grid");
    lxb_dom_node_t *grid_a = find_id(
        lxb_dom_interface_node(responsive.html), "grid-a");
    lxb_dom_node_t *grid_b = find_id(
        lxb_dom_interface_node(responsive.html), "grid-b");
    lxb_dom_node_t *grid_c = find_id(
        lxb_dom_interface_node(responsive.html), "grid-c");
    lxb_dom_node_t *fluid = find_id(
        lxb_dom_interface_node(responsive.html), "fluid");
    lxb_dom_node_t *spaced = find_id(
        lxb_dom_interface_node(responsive.html), "spaced");
    lxb_dom_node_t *space_a = find_id(
        lxb_dom_interface_node(responsive.html), "space-a");
    lxb_dom_node_t *space_b = find_id(
        lxb_dom_interface_node(responsive.html), "space-b");
    lxb_dom_node_t *grow_a = find_id(
        lxb_dom_interface_node(responsive.html), "grow-a");
    lxb_dom_node_t *grow_b = find_id(
        lxb_dom_interface_node(responsive.html), "grow-b");
    lxb_dom_node_t *grow_c = find_id(
        lxb_dom_interface_node(responsive.html), "grow-c");
    lxb_dom_node_t *justify_a = find_id(
        lxb_dom_interface_node(responsive.html), "justify-a");
    lxb_dom_node_t *justify_b = find_id(
        lxb_dom_interface_node(responsive.html), "justify-b");
    lxb_dom_node_t *justify_c = find_id(
        lxb_dom_interface_node(responsive.html), "justify-c");
    lxb_dom_node_t *explicit_grow_a = find_id(
        lxb_dom_interface_node(responsive.html), "explicit-grow-a");
    lxb_dom_node_t *explicit_grow_b = find_id(
        lxb_dom_interface_node(responsive.html), "explicit-grow-b");
    lxb_dom_node_t *flex_table_cell_a = find_id(
        lxb_dom_interface_node(responsive.html), "flex-table-cell-a");
    lxb_dom_node_t *flex_table_cell_b = find_id(
        lxb_dom_interface_node(responsive.html), "flex-table-cell-b");
    lxb_dom_node_t *shrink_a = find_id(
        lxb_dom_interface_node(responsive.html), "shrink-a");
    lxb_dom_node_t *shrink_b = find_id(
        lxb_dom_interface_node(responsive.html), "shrink-b");
    lxb_dom_node_t *shrink_c = find_id(
        lxb_dom_interface_node(responsive.html), "shrink-c");
    lxb_dom_node_t *anywhere_wrap = find_id(
        lxb_dom_interface_node(responsive.html), "anywhere-wrap");
    lxb_dom_node_t *break_all_wrap = find_id(
        lxb_dom_interface_node(responsive.html), "break-all-wrap");
    lxb_dom_node_t *content_box_a = find_id(
        lxb_dom_interface_node(responsive.html), "content-box-a");
    lxb_dom_node_t *content_box_b = find_id(
        lxb_dom_interface_node(responsive.html), "content-box-b");
    lxb_dom_node_t *border_box_a = find_id(
        lxb_dom_interface_node(responsive.html), "border-box-a");
    lxb_dom_node_t *border_box_b = find_id(
        lxb_dom_interface_node(responsive.html), "border-box-b");
    lxb_dom_node_t *column_align = find_id(
        lxb_dom_interface_node(responsive.html), "column-align");
    lxb_dom_node_t *column_a = find_id(
        lxb_dom_interface_node(responsive.html), "column-a");
    lxb_dom_node_t *column_b = find_id(
        lxb_dom_interface_node(responsive.html), "column-b");
    lxb_dom_node_t *column_end = find_id(
        lxb_dom_interface_node(responsive.html), "column-end");
    lxb_dom_node_t *column_end_item = find_id(
        lxb_dom_interface_node(responsive.html), "column-end-item");
    lxb_dom_node_t *column_between = find_id(
        lxb_dom_interface_node(responsive.html), "column-between");
    lxb_dom_node_t *column_between_a = find_id(
        lxb_dom_interface_node(responsive.html), "column-between-a");
    lxb_dom_node_t *column_between_b = find_id(
        lxb_dom_interface_node(responsive.html), "column-between-b");
    lxb_dom_node_t *column_center = find_id(
        lxb_dom_interface_node(responsive.html), "column-center");
    lxb_dom_node_t *column_center_item = find_id(
        lxb_dom_interface_node(responsive.html), "column-center-item");
    lxb_dom_node_t *column_center_link = find_id(
        lxb_dom_interface_node(responsive.html), "column-center-link");
    lxb_dom_node_t *tracking_child = find_id(
        lxb_dom_interface_node(responsive.html), "tracking-child");
    lxb_dom_node_t *grid_remainder = find_id(
        lxb_dom_interface_node(responsive.html), "grid-remainder");
    lxb_dom_node_t *grid_rem_a = find_id(
        lxb_dom_interface_node(responsive.html), "grid-rem-a");
    lxb_dom_node_t *grid_rem_b = find_id(
        lxb_dom_interface_node(responsive.html), "grid-rem-b");
    lxb_dom_node_t *grid_rem_c = find_id(
        lxb_dom_interface_node(responsive.html), "grid-rem-c");
    lxb_dom_node_t *ratio = find_id(
        lxb_dom_interface_node(responsive.html), "ratio");
    lxb_dom_node_t *explicit_background = find_id(
        lxb_dom_interface_node(responsive.html), "explicit-background");
    lxb_dom_node_t *pseudo_background = find_id(
        lxb_dom_interface_node(responsive.html), "pseudo-background");
    lxb_dom_node_t *sibling_b = find_id(
        lxb_dom_interface_node(responsive.html), "sibling-b");
    lxb_dom_node_t *sibling_c = find_id(
        lxb_dom_interface_node(responsive.html), "sibling-c");
    lxb_dom_node_t *nth_one = find_id(
        lxb_dom_interface_node(responsive.html), "nth-one");
    lxb_dom_node_t *nth_two = find_id(
        lxb_dom_interface_node(responsive.html), "nth-two");
    lxb_dom_node_t *nth_three = find_id(
        lxb_dom_interface_node(responsive.html), "nth-three");
    lxb_dom_node_t *attribute_operators = find_id(
        lxb_dom_interface_node(responsive.html), "attribute-operators");
    lxb_dom_node_t *only_child = find_id(
        lxb_dom_interface_node(responsive.html), "only-child");
    lxb_dom_node_t *only_type = find_id(
        lxb_dom_interface_node(responsive.html), "only-type");
    lxb_dom_node_t *disabled_control = find_id(
        lxb_dom_interface_node(responsive.html), "disabled-control");
    lxb_dom_node_t *enabled_control = find_id(
        lxb_dom_interface_node(responsive.html), "enabled-control");
    lxb_dom_node_t *checked_control = find_id(
        lxb_dom_interface_node(responsive.html), "checked-control");
    lxb_dom_node_t *optional_control = find_id(
        lxb_dom_interface_node(responsive.html), "optional-control");
    lxb_dom_node_t *any_link = find_id(
        lxb_dom_interface_node(responsive.html), "any-link");
    lxb_dom_node_t *focus_host = find_id(
        lxb_dom_interface_node(responsive.html), "focus-host");
    lxb_dom_node_t *focus_probe = find_id(
        lxb_dom_interface_node(responsive.html), "focus-probe");
    lxb_dom_node_t *variable_child = find_id(
        lxb_dom_interface_node(responsive.html), "variable-child");
    lxb_dom_node_t *fallback_variable = find_id(
        lxb_dom_interface_node(responsive.html), "fallback-variable");
    lxb_dom_node_t *logical_box = find_id(
        lxb_dom_interface_node(responsive.html), "logical-box");
    lxb_dom_node_t *important = find_id(
        lxb_dom_interface_node(responsive.html), "important");
    lxb_dom_node_t *specificity = find_id(
        lxb_dom_interface_node(responsive.html), "specificity");
    lxb_dom_node_t *specificity_heavy = find_id(
        lxb_dom_interface_node(responsive.html), "specificity-heavy");
    lxb_dom_node_t *layered = find_id(
        lxb_dom_interface_node(responsive.html), "layered");
    lxb_dom_node_t *var_layered = find_id(
        lxb_dom_interface_node(responsive.html), "var-layered");
    lxb_dom_node_t *var_inline = find_id(
        lxb_dom_interface_node(responsive.html), "var-inline");
    lxb_dom_node_t *var_user = find_id(
        lxb_dom_interface_node(responsive.html), "var-user");
    lxb_dom_node_t *var_user_important = find_id(
        lxb_dom_interface_node(responsive.html), "var-user-important");
    lxb_dom_node_t *modern_rgb = find_id(
        lxb_dom_interface_node(responsive.html), "modern-rgb");
    lxb_dom_node_t *modern_hsl = find_id(
        lxb_dom_interface_node(responsive.html), "modern-hsl");
    lxb_dom_node_t *modern_oklch = find_id(
        lxb_dom_interface_node(responsive.html), "modern-oklch");
    lxb_dom_node_t *alpha_zero = find_id(
        lxb_dom_interface_node(responsive.html), "alpha-zero");
    lxb_dom_node_t *hex_alpha = find_id(
        lxb_dom_interface_node(responsive.html), "hex-alpha");
    lxb_dom_node_t *partial_alpha = find_id(
        lxb_dom_interface_node(responsive.html), "partial-alpha");
    lxb_dom_node_t *generated = find_id(
        lxb_dom_interface_node(responsive.html), "generated");
    lxb_dom_node_t *attr_generated = find_id(
        lxb_dom_interface_node(responsive.html), "attr-generated");
    CHECK(cards != NULL && left != NULL && right != NULL && pixel != NULL
          && icon != NULL && nobullet != NULL && slant != NULL
          && sticky != NULL && fixed != NULL && rounded != NULL
          && empty_slot != NULL && centered != NULL && nowrap != NULL
          && grid != NULL && grid_a != NULL && grid_b != NULL
          && grid_c != NULL && fluid != NULL && spaced != NULL
          && space_a != NULL && space_b != NULL && ratio != NULL
          && explicit_background != NULL && pseudo_background != NULL
          && grow_a != NULL && grow_b != NULL && grow_c != NULL
          && explicit_grow_a != NULL && explicit_grow_b != NULL
          && flex_table_cell_a != NULL && flex_table_cell_b != NULL
          && shrink_a != NULL && shrink_b != NULL && shrink_c != NULL
          && anywhere_wrap != NULL && break_all_wrap != NULL
          && content_box_a != NULL && content_box_b != NULL
          && border_box_a != NULL && border_box_b != NULL
          && column_align != NULL && column_a != NULL && column_b != NULL
          && column_end != NULL && column_end_item != NULL
          && column_between != NULL && column_between_a != NULL
          && column_between_b != NULL && column_center != NULL
          && column_center_item != NULL && column_center_link != NULL
          && tracking_child != NULL
          && grid_remainder != NULL && grid_rem_a != NULL
          && grid_rem_b != NULL && grid_rem_c != NULL
          && justify_a != NULL && justify_b != NULL && justify_c != NULL
          && sibling_b != NULL && sibling_c != NULL && nth_one != NULL
          && nth_two != NULL && nth_three != NULL
          && attribute_operators != NULL && only_child != NULL
          && only_type != NULL && disabled_control != NULL
          && enabled_control != NULL && checked_control != NULL
          && optional_control != NULL && any_link != NULL
          && variable_child != NULL && fallback_variable != NULL
          && logical_box != NULL && important != NULL
          && specificity != NULL && layered != NULL
          && var_layered != NULL && var_inline != NULL
          && var_user != NULL && var_user_important != NULL
          && modern_rgb != NULL && modern_hsl != NULL
          && modern_oklch != NULL && alpha_zero != NULL
          && hex_alpha != NULL && partial_alpha != NULL
          && generated != NULL && attr_generated != NULL);
    Stylesheet mobile_sheet;
    CHECK(stylesheet_build(&mobile_sheet, &budget, &responsive, 480));
    static const char responsive_user_css[] =
        "#important{color:#444444!important;background:#555555}"
        "#var-user{--user-color:#222222}"
        "#var-user-important{--user-color:#333333!important}";
    CHECK(stylesheet_add_user_css(&mobile_sheet, responsive_user_css,
                                  strlen(responsive_user_css)));
    CHECK(mobile_sheet.variable_count == 3 && mobile_sheet.layer_count == 2);
    ComputedStyle cards_style = style_for_node(&mobile_sheet, cards, NULL);
    CHECK(cards_style.display == DISPLAY_FLEX);
    CHECK(cards_style.gap == 12 && cards_style.color == 0x123456
          && cards_style.word_spacing == 1);
    CHECK(cards_style.align_items == ALIGN_CENTER
          && cards_style.justify_content == JUSTIFY_CENTER
          && cards_style.min_height == 50);
    CHECK(cards_style.border.top == 3
          && stylesheet_border_color(
                 &mobile_sheet, &cards_style,
                 STYLE_BORDER_TOP, NULL) == 0x0a0b0c);
    CHECK(cards_style.padding.top == 16 && cards_style.padding.right == 16
          && cards_style.padding.bottom == 16 && cards_style.padding.left == 16);
    ComputedStyle left_style = style_for_node(&mobile_sheet, left, &cards_style);
    CHECK(left_style.display == DISPLAY_BLOCK && left_style.has_background);
    CHECK(left_style.padding.left == 9 && left_style.color != 0xabcdef);
    CHECK(left_style.margin.top == 11 && left_style.display == DISPLAY_BLOCK
          && left_style.border.bottom == 2
          && stylesheet_border_color(
                 &mobile_sheet, &left_style,
                 STYLE_BORDER_BOTTOM, NULL) == 0x010203);
    ComputedStyle right_style = style_for_node(&mobile_sheet, right, &cards_style);
    CHECK(right_style.display == DISPLAY_BLOCK && right_style.color == 0xabcdef
          && right_style.padding.left == 9 && right_style.margin.bottom == 13
          && right_style.relative_position && right_style.left == 3
          && right_style.top == -2);
    ComputedStyle pixel_style = style_for_node(&mobile_sheet, pixel,
                                               &right_style);
    CHECK(pixel_style.object_fit == LAYOUT_IMAGE_FIT_COVER
          && pixel_style.has_width && pixel_style.width == 4
          && pixel_style.has_height && pixel_style.height == 2);
    ComputedStyle icon_style = style_for_node(&mobile_sheet, icon, &left_style);
    CHECK(icon_style.has_width && icon_style.width == 20
          && icon_style.has_height && icon_style.height == 20
          && icon_style.min_width == 18
          && icon_style.margin_left_auto && icon_style.margin_right_auto
          && icon_style.flex_grow == 0 && icon_style.flex_shrink == 0
          && !icon_style.has_flex_basis
          && strcmp(icon_style.mask_image,
                    "https://example.com/icon.svg") == 0);
    ComputedStyle sticky_style = style_for_node(&mobile_sheet, sticky, NULL);
    CHECK(sticky_style.sticky_position && sticky_style.has_top
          && sticky_style.top == 12 && !sticky_style.out_of_flow);
    ComputedStyle fixed_style = style_for_node(&mobile_sheet, fixed, NULL);
    CHECK(fixed_style.fixed_position && fixed_style.has_top
          && fixed_style.top == 20 && !fixed_style.out_of_flow);
    ComputedStyle rounded_style = style_for_node(&mobile_sheet, rounded, NULL);
    CHECK(rounded_style.border_radius == 8);
    ComputedStyle empty_style = style_for_node(&mobile_sheet, empty_slot, NULL);
    CHECK(empty_style.display == DISPLAY_NONE);
    ComputedStyle centered_style = style_for_node(&mobile_sheet, centered,
                                                   NULL);
    ComputedStyle nowrap_style = style_for_node(&mobile_sheet, nowrap, NULL);
    ComputedStyle grid_style = style_for_node(&mobile_sheet, grid, NULL);
    ComputedStyle fluid_style = style_for_node(&mobile_sheet, fluid, NULL);
    ComputedStyle spaced_style = style_for_node(&mobile_sheet, spaced, NULL);
    ComputedStyle ratio_style = style_for_node(&mobile_sheet, ratio, NULL);
    ComputedStyle explicit_background_style = style_for_node(
        &mobile_sheet, explicit_background, NULL);
    ComputedStyle tracking_parent_style = style_for_node(
        &mobile_sheet, tracking_child->parent, NULL);
    ComputedStyle tracking_child_style = style_for_node(
        &mobile_sheet, tracking_child, &tracking_parent_style);
    ComputedStyle sibling_b_style = style_for_node(&mobile_sheet, sibling_b,
                                                    NULL);
    ComputedStyle sibling_c_style = style_for_node(&mobile_sheet, sibling_c,
                                                    NULL);
    ComputedStyle nth_one_style = style_for_node(&mobile_sheet, nth_one, NULL);
    ComputedStyle nth_two_style = style_for_node(&mobile_sheet, nth_two, NULL);
    ComputedStyle nth_three_style = style_for_node(&mobile_sheet, nth_three,
                                                   NULL);
    ComputedStyle attribute_operators_style = style_for_node(
        &mobile_sheet, attribute_operators, NULL);
    ComputedStyle only_child_style = style_for_node(&mobile_sheet, only_child,
                                                    NULL);
    ComputedStyle only_type_style = style_for_node(&mobile_sheet, only_type,
                                                   NULL);
    ComputedStyle disabled_control_style = style_for_node(
        &mobile_sheet, disabled_control, NULL);
    ComputedStyle enabled_control_style = style_for_node(
        &mobile_sheet, enabled_control, NULL);
    ComputedStyle checked_control_style = style_for_node(
        &mobile_sheet, checked_control, NULL);
    ComputedStyle optional_control_style = style_for_node(
        &mobile_sheet, optional_control, NULL);
    ComputedStyle any_link_style = style_for_node(&mobile_sheet, any_link,
                                                  NULL);
    ComputedStyle focus_host_style = style_for_node(&mobile_sheet, focus_host,
                                                    NULL);
    ComputedStyle focus_probe_style = style_for_node(
        &mobile_sheet, focus_probe, NULL);
    ComputedStyle variable_child_style = style_for_node(
        &mobile_sheet, variable_child, NULL);
    ComputedStyle fallback_variable_style = style_for_node(
        &mobile_sheet, fallback_variable, NULL);
    ComputedStyle logical_box_style = style_for_node(&mobile_sheet,
                                                     logical_box, NULL);
    ComputedStyle important_style = style_for_node(&mobile_sheet, important,
                                                   NULL);
    ComputedStyle specificity_style = style_for_node(&mobile_sheet,
                                                     specificity, NULL);
    ComputedStyle specificity_heavy_style = style_for_node(
        &mobile_sheet, specificity_heavy, NULL);
    ComputedStyle layered_style = style_for_node(&mobile_sheet, layered,
                                                 NULL);
    ComputedStyle var_layered_style = style_for_node(&mobile_sheet,
                                                     var_layered, NULL);
    ComputedStyle var_inline_style = style_for_node(&mobile_sheet, var_inline,
                                                    NULL);
    ComputedStyle var_user_style = style_for_node(&mobile_sheet, var_user,
                                                  NULL);
    ComputedStyle var_user_important_style = style_for_node(
        &mobile_sheet, var_user_important, NULL);
    ComputedStyle modern_rgb_style = style_for_node(&mobile_sheet, modern_rgb,
                                                    NULL);
    ComputedStyle modern_hsl_style = style_for_node(&mobile_sheet, modern_hsl,
                                                    NULL);
    ComputedStyle modern_oklch_style = style_for_node(
        &mobile_sheet, modern_oklch, NULL);
    ComputedStyle alpha_zero_style = style_for_node(&mobile_sheet, alpha_zero,
                                                    NULL);
    ComputedStyle hex_alpha_style = style_for_node(&mobile_sheet, hex_alpha,
                                                   NULL);
    ComputedStyle partial_alpha_style = style_for_node(
        &mobile_sheet, partial_alpha, NULL);
    ComputedStyle generated_style = style_for_node(&mobile_sheet, generated,
                                                   NULL);
    ComputedStyle generated_before = style_for_pseudo(
        &mobile_sheet, generated, PSEUDO_BEFORE, &generated_style);
    ComputedStyle attr_generated_style = style_for_node(
        &mobile_sheet, attr_generated, NULL);
    ComputedStyle attr_generated_after = style_for_pseudo(
        &mobile_sheet, attr_generated, PSEUDO_AFTER, &attr_generated_style);
    CHECK(centered_style.text_align == TEXT_ALIGN_CENTER
          && nowrap_style.white_space_mode == WHITE_SPACE_NOWRAP
          && grid_style.display == DISPLAY_GRID
          && computed_style_grid_column_count(&grid_style) == 2
          && grid_style.gap == 10
          && grid_style.row_gap == 6 && grid_style.has_background
          && grid_style.background == 0x112244
          && grid_style.border.left == 2
          && stylesheet_border_color(
                 &mobile_sheet, &grid_style,
                 STYLE_BORDER_LEFT, NULL) == 0x334455
          && grid_style.opacity >= 127 && grid_style.opacity <= 128);
    CHECK(spaced_style.display == DISPLAY_FLEX
          && spaced_style.justify_content == JUSTIFY_SPACE_BETWEEN);
    CHECK(ratio_style.aspect_width == 16 && ratio_style.aspect_height == 9
          && ratio_style.has_background
          && ratio_style.background == 0x202020
          && strcmp(ratio_style.background_image,
                    "https://example.com/background.png") == 0
          && ratio_style.background_fit == LAYOUT_IMAGE_FIT_COVER);
    CHECK((explicit_background_style.background_size_flags
           & STYLE_BACKGROUND_SIZE_EXPLICIT) != 0
          && explicit_background_style.background_width == 82
          && explicit_background_style.background_height == 20
          && explicit_background_style.background_position_x == 50
          && explicit_background_style.background_position_y == 50
          && explicit_background_style.text_indent == -9999);
    CHECK(tracking_parent_style.letter_spacing == 3
          && tracking_child_style.letter_spacing == 3);
    CHECK(sibling_b_style.color == 0x314159
          && sibling_c_style.has_background
          && sibling_c_style.background == 0x271828);
    CHECK(nth_one_style.color == 0x102938
          && nth_one_style.border.left == 1
          && nth_two_style.has_background
          && nth_two_style.background == 0x475869
          && nth_two_style.border.top == 3
          && nth_three_style.color == 0x102938
          && nth_three_style.border.right == 2
          && stylesheet_border_color(
                 &mobile_sheet, &nth_three_style,
                 STYLE_BORDER_RIGHT, NULL) == 0x556677);
    CHECK(attribute_operators_style.border.right == 4
          && stylesheet_border_color(
                 &mobile_sheet, &attribute_operators_style,
                 STYLE_BORDER_RIGHT, NULL) == 0x246813);
    CHECK(only_child_style.border.bottom == 4
          && stylesheet_border_color(
                 &mobile_sheet, &only_child_style,
                 STYLE_BORDER_BOTTOM, NULL) == 0x765432
          && only_type_style.border.bottom == 4
          && stylesheet_border_color(
                 &mobile_sheet, &only_type_style,
                 STYLE_BORDER_BOTTOM, NULL) == 0x765432);
    CHECK(disabled_control_style.opacity >= 63
          && disabled_control_style.opacity <= 64
          && enabled_control_style.has_background
          && enabled_control_style.background == 0x135724
          && checked_control_style.border.left == 2
          && checked_control_style.border.bottom == 3
          && stylesheet_border_color(
                 &mobile_sheet, &checked_control_style,
                 STYLE_BORDER_LEFT, NULL) == 0xabcdef
          && optional_control_style.border.top == 1
          && stylesheet_border_color(
                 &mobile_sheet, &optional_control_style,
                 STYLE_BORDER_TOP, NULL) == 0xabcdef
          && any_link_style.border.right == 2
          && stylesheet_border_color(
                 &mobile_sheet, &any_link_style,
                 STYLE_BORDER_RIGHT, NULL) == 0xabcdef
          && focus_host_style.has_background
          && focus_host_style.background == 0xabcdef
          && focus_probe_style.color == 0x2468ac
          && focus_probe_style.border.left == 3
          && stylesheet_border_color(
                 &mobile_sheet, &focus_probe_style,
                 STYLE_BORDER_LEFT, NULL) == 0x13579b);
    CHECK(variable_child_style.color == 0x335577
          && variable_child_style.border.left == 5
          && stylesheet_border_color(
                 &mobile_sheet, &variable_child_style,
                 STYLE_BORDER_LEFT, NULL) == 0x335577
          && variable_child_style.padding.left == 7
          && fallback_variable_style.color == 0x112244
          && mobile_sheet.custom_rule_count >= 3);
    CHECK(logical_box_style.margin_left_auto
          && logical_box_style.margin.right == 6
          && logical_box_style.padding.top == 3
          && logical_box_style.padding.bottom == 4
          && logical_box_style.has_left && logical_box_style.left == 7
          && logical_box_style.has_right && logical_box_style.right == 9
          && logical_box_style.has_top && logical_box_style.top == 11
          && logical_box_style.has_bottom && logical_box_style.bottom == 13
          && logical_box_style.border.left == 2
          && logical_box_style.border.right == 2
          && logical_box_style.border.top == 3
          && logical_box_style.border.bottom == 4
          && stylesheet_border_color(
                 &mobile_sheet, &logical_box_style,
                 STYLE_BORDER_LEFT, NULL) == 0x456789);
    CHECK(important_style.color == 0x444444
          && important_style.has_background
          && important_style.background == 0x333333
          && important_style.padding.left == 6);
    CHECK(specificity_style.color == 0x111111
          && specificity_style.has_background
          && specificity_style.background == 0x333333
          && specificity_style.border.left == 2
          && stylesheet_border_color(
                 &mobile_sheet, &specificity_style,
                 STYLE_BORDER_LEFT, NULL) == 0x123456);
    CHECK(specificity_heavy_style.color == 0x123456);
    CHECK(layered_style.color == 0x111111
          && layered_style.has_background
          && layered_style.background == 0x333333
          && layered_style.border.left == 3
          && stylesheet_border_color(
                 &mobile_sheet, &layered_style,
                 STYLE_BORDER_LEFT, NULL) == 0x334455);
    CHECK(var_layered_style.color == 0x110000
          && var_inline_style.color == 0x222200
          && var_user_style.color == 0x111111
          && var_user_important_style.color == 0x333333);
    CHECK(modern_rgb_style.color == 0x0a141e);
    CHECK(modern_hsl_style.color == 0x008000);
    CHECK(modern_oklch_style.color == 0xfb2c36);
    CHECK(!alpha_zero_style.has_background);
    CHECK(hex_alpha_style.color == 0xaabbcc);
    uint8_t partial_border_alpha = 0;
    uint32_t partial_border_color = stylesheet_border_color(
        &mobile_sheet, &partial_alpha_style,
        STYLE_BORDER_LEFT, &partial_border_alpha);
    CHECK(partial_alpha_style.color == 0xff0000
          && partial_alpha_style.color_alpha == 128
          && partial_alpha_style.has_background
          && partial_alpha_style.background == 0x0000ff
          && partial_alpha_style.background_alpha == 64
          && partial_alpha_style.border.left == 4
          && partial_border_color == 0x04fa80
          && partial_border_alpha == 191);
    CHECK(generated_before.generated_content
          && generated_before.generated_text_length == 16
          && strcmp(generated_before.generated_text,
                    "Ask Anything \xe2\x9c\x93") == 0
          && mobile_sheet.generated_text_count >= 1);
    CHECK(attr_generated_after.generated_content
          && attr_generated_after.generated_attr_length == 10
          && strcmp(attr_generated_after.generated_attr, "data-label") == 0
          && attr_generated_after.generated_text_length == 10
          && memcmp(attr_generated_after.generated_text, "Live label", 10)
             == 0);
    CHECK(fluid_style.has_width && fluid_style.width == 240
          && !fluid_style.width_percent && fluid_style.has_height
          && fluid_style.height >= 27 && fluid_style.height <= 28
          && fluid_style.margin.left == 15
          && fluid_style.font_size == 14);
    LayoutDocument responsive_layout;
    static unsigned char rgba[] = {
        255, 0, 0, 255, 0, 255, 0, 255,
        0, 0, 255, 255, 255, 255, 255, 128
    };
    ImageResource pixel_resource = {.node = pixel, .pixels = rgba,
                                    .width = 2, .height = 2};
    ImageResource image_items[6] = {
        pixel_resource,
        {.node = icon, .pixels = rgba, .width = 2, .height = 2,
         .is_mask = true},
        {.node = icon, .pixels = rgba, .width = 2, .height = 2,
         .is_mask = true, .pseudo = PSEUDO_AFTER},
        {.node = ratio, .pixels = rgba, .width = 2, .height = 2,
         .is_background = true},
        /* Keep the raw source extent honest; CSS background-size tests the
         * scaled 82x20 draw independently. */
        {.node = explicit_background, .pixels = rgba, .width = 2,
         .height = 2, .is_background = true},
        {.node = pseudo_background, .pixels = rgba, .width = 2,
         .height = 2, .is_background = true, .pseudo = PSEUDO_BEFORE}
    };
    ImageResources responsive_images = {.items = image_items, .count = 6};
    CHECK(layout_build(&responsive_layout, &budget, &responsive,
                       &mobile_sheet, NULL, &responsive_images, 480));
    CHECK(command_color(&responsive_layout, "RGB") == 0x0a141e
          && command_color(&responsive_layout, "HSL") == 0x008000
          && command_color(&responsive_layout, "OKLCH") == 0xfb2c36
          && command_color(&responsive_layout, "HEXALPHA") == 0xaabbcc
          && command_color(&responsive_layout,
                           "Ask Anything \xe2\x9c\x93") == 0x214365
          /* Text-only generated content participates in the inline flow,
             so attr()-generated text is word-split like any other run. */
          && command_color(&responsive_layout, "Live") == 0x365214
          && command_color(&responsive_layout, "label") == 0x365214);
    CHECK(command_opacity(&responsive_layout, "PARTIAL") == 129
          && fill_opacity(&responsive_layout, 0x0000ff) == 64
          && fill_opacity(&responsive_layout, 0x04fa80) == 192);
    const DrawCommand *tracked_text = NULL;
    const DrawCommand *plain_text = NULL;
    for (size_t i = 0; i < responsive_layout.count; i++) {
        const DrawCommand *command = &responsive_layout.commands[i];
        if (command->type != DRAW_TEXT || command->text_length != 5
            || memcmp(command->text, "ZZZZZ", 5) != 0) continue;
        if (command->letter_spacing == 3) tracked_text = command;
        else if (command->letter_spacing == 0) plain_text = command;
    }
    CHECK(tracked_text != NULL && plain_text != NULL
          && tracked_text->width == plain_text->width + 12);
    int left_x = command_x(&responsive_layout, "LEFT");
    int right_x = command_x(&responsive_layout, "RIGHT");
    CHECK(left_x >= 0 && right_x > left_x + 100);
    CHECK(command_x(&responsive_layout, "OFFFLOW") >= 0);
    /* visibility:hidden retains layout geometry while suppressing paint. */
    CHECK(command_x(&responsive_layout, "HIDDEN") >= 0
          && command_opacity(&responsive_layout, "HIDDEN") == 0);
    CHECK(command_x(&responsive_layout, "-") == -1);
    const DrawCommand *ordered_one = text_command(
        &responsive_layout, "1.");
    const DrawCommand *ordered_two = text_command(
        &responsive_layout, "2.");
    CHECK(ordered_one != NULL
          && ordered_one->x < command_x(&responsive_layout, "ORDEREDONE"));
    CHECK(ordered_two != NULL
          && ordered_two->x < command_x(&responsive_layout, "ORDEREDTWO"));
    CHECK(command_x(&responsive_layout, "TYPE") >= 0
          && command_x(&responsive_layout, "HERE") >
             command_x(&responsive_layout, "TYPE"));
    const LayoutNodeBox *centered_box = layout_box_for_node(
        &responsive_layout, centered);
    const LayoutNodeBox *grid_a_box = layout_box_for_node(
        &responsive_layout, grid_a);
    const LayoutNodeBox *grid_b_box = layout_box_for_node(
        &responsive_layout, grid_b);
    const LayoutNodeBox *grid_c_box = layout_box_for_node(
        &responsive_layout, grid_c);
    const LayoutNodeBox *grid_remainder_box = layout_box_for_node(
        &responsive_layout, grid_remainder);
    const LayoutNodeBox *grid_rem_a_box = layout_box_for_node(
        &responsive_layout, grid_rem_a);
    const LayoutNodeBox *grid_rem_b_box = layout_box_for_node(
        &responsive_layout, grid_rem_b);
    const LayoutNodeBox *grid_rem_c_box = layout_box_for_node(
        &responsive_layout, grid_rem_c);
    const LayoutNodeBox *space_a_box = layout_box_for_node(
        &responsive_layout, space_a);
    const LayoutNodeBox *space_b_box = layout_box_for_node(
        &responsive_layout, space_b);
    const LayoutNodeBox *grow_a_box = layout_box_for_node(
        &responsive_layout, grow_a);
    const LayoutNodeBox *grow_b_box = layout_box_for_node(
        &responsive_layout, grow_b);
    const LayoutNodeBox *grow_c_box = layout_box_for_node(
        &responsive_layout, grow_c);
    const LayoutNodeBox *justify_a_box = layout_box_for_node(
        &responsive_layout, justify_a);
    const LayoutNodeBox *justify_b_box = layout_box_for_node(
        &responsive_layout, justify_b);
    const LayoutNodeBox *justify_c_box = layout_box_for_node(
        &responsive_layout, justify_c);
    const LayoutNodeBox *explicit_grow_a_box = layout_box_for_node(
        &responsive_layout, explicit_grow_a);
    const LayoutNodeBox *explicit_grow_b_box = layout_box_for_node(
        &responsive_layout, explicit_grow_b);
    const LayoutNodeBox *flex_table_cell_a_box = layout_box_for_node(
        &responsive_layout, flex_table_cell_a);
    const LayoutNodeBox *flex_table_cell_b_box = layout_box_for_node(
        &responsive_layout, flex_table_cell_b);
    const LayoutNodeBox *shrink_a_box = layout_box_for_node(
        &responsive_layout, shrink_a);
    const LayoutNodeBox *shrink_b_box = layout_box_for_node(
        &responsive_layout, shrink_b);
    const LayoutNodeBox *shrink_c_box = layout_box_for_node(
        &responsive_layout, shrink_c);
    const LayoutNodeBox *anywhere_wrap_box = layout_box_for_node(
        &responsive_layout, anywhere_wrap);
    const LayoutNodeBox *break_all_wrap_box = layout_box_for_node(
        &responsive_layout, break_all_wrap);
    const LayoutNodeBox *content_box_a_box = layout_box_for_node(
        &responsive_layout, content_box_a);
    const LayoutNodeBox *content_box_b_box = layout_box_for_node(
        &responsive_layout, content_box_b);
    const LayoutNodeBox *border_box_a_box = layout_box_for_node(
        &responsive_layout, border_box_a);
    const LayoutNodeBox *border_box_b_box = layout_box_for_node(
        &responsive_layout, border_box_b);
    const LayoutNodeBox *column_align_box = layout_box_for_node(
        &responsive_layout, column_align);
    const LayoutNodeBox *column_a_box = layout_box_for_node(
        &responsive_layout, column_a);
    const LayoutNodeBox *column_b_box = layout_box_for_node(
        &responsive_layout, column_b);
    const LayoutNodeBox *column_end_box = layout_box_for_node(
        &responsive_layout, column_end);
    const LayoutNodeBox *column_end_item_box = layout_box_for_node(
        &responsive_layout, column_end_item);
    const LayoutNodeBox *column_between_box = layout_box_for_node(
        &responsive_layout, column_between);
    const LayoutNodeBox *column_between_a_box = layout_box_for_node(
        &responsive_layout, column_between_a);
    const LayoutNodeBox *column_between_b_box = layout_box_for_node(
        &responsive_layout, column_between_b);
    const LayoutNodeBox *column_center_box = layout_box_for_node(
        &responsive_layout, column_center);
    const LayoutNodeBox *column_center_item_box = layout_box_for_node(
        &responsive_layout, column_center_item);
    const LayoutNodeBox *ratio_box = layout_box_for_node(
        &responsive_layout, ratio);
    const LayoutNodeBox *explicit_background_box = layout_box_for_node(
        &responsive_layout, explicit_background);
    const LayoutNodeBox *pseudo_background_box = layout_box_for_node(
        &responsive_layout, pseudo_background);
    CHECK(centered_box != NULL
          && command_x(&responsive_layout, "CENTERED") > centered_box->x
          && command_x(&responsive_layout, "SECOND")
             > command_x(&responsive_layout, "FIRST")
          && command_y(&responsive_layout, "FIRST")
             == command_y(&responsive_layout, "SECOND"));
    CHECK(grid_a_box != NULL && grid_b_box != NULL && grid_c_box != NULL
          && grid_b_box->x == grid_a_box->x + grid_a_box->width + 10
          && grid_b_box->y == grid_a_box->y
          && grid_c_box->x == grid_a_box->x
          && grid_c_box->y >= grid_a_box->y + grid_a_box->height + 6);
    CHECK(grid_remainder_box != NULL && grid_rem_a_box != NULL
          && grid_rem_b_box != NULL && grid_rem_c_box != NULL
          && grid_rem_a_box->width == 33 && grid_rem_b_box->width == 32
          && grid_rem_c_box->width == 32
          && grid_rem_b_box->x == grid_rem_a_box->x + 35
          && grid_rem_c_box->x == grid_rem_b_box->x + 34
          && grid_rem_c_box->x + grid_rem_c_box->width
             == grid_remainder_box->x + grid_remainder_box->width);
    CHECK(space_a_box != NULL && space_b_box != NULL
          && space_b_box->x - space_a_box->x >= 170);
    CHECK(grow_a_box != NULL && grow_b_box != NULL && grow_c_box != NULL
          && grow_a_box->width == 48 && grow_b_box->width == 48
          && grow_b_box->x == grow_a_box->x + 52
          && grow_b_box->y == grow_a_box->y
          && grow_c_box->width == 100 && grow_c_box->x == grow_a_box->x
          && grow_c_box->y >= grow_a_box->y + grow_a_box->height + 7);
    CHECK(justify_a_box != NULL && justify_b_box != NULL
          && justify_c_box != NULL
          && justify_a_box->width == 40 && justify_b_box->width == 40
          && justify_b_box->x == justify_a_box->x + 60
          && justify_b_box->y == justify_a_box->y
          && justify_c_box->x == justify_a_box->x
          && justify_c_box->y
             >= justify_a_box->y + justify_a_box->height + 9);
    CHECK(explicit_grow_a_box != NULL && explicit_grow_b_box != NULL
          && explicit_grow_a_box->width == 35
          && explicit_grow_b_box->width == 65
          && explicit_grow_b_box->x
             == explicit_grow_a_box->x + explicit_grow_a_box->width);
    CHECK(flex_table_cell_a_box != NULL && flex_table_cell_b_box != NULL
          && flex_table_cell_a_box->width == 100
          && flex_table_cell_b_box->width == 100
          && flex_table_cell_b_box->x
             == flex_table_cell_a_box->x + flex_table_cell_a_box->width);
    CHECK(shrink_a_box != NULL && shrink_b_box != NULL
          && shrink_c_box != NULL && shrink_a_box->width == 60
          && shrink_b_box->width == 40 && shrink_c_box->width == 20
          && shrink_b_box->x == shrink_a_box->x + shrink_a_box->width
          && shrink_c_box->x == shrink_b_box->x + shrink_b_box->width);
    size_t anywhere_pieces = 0;
    size_t break_all_pieces = 0;
    bool anywhere_contained = true;
    bool break_all_contained = true;
    for (size_t i = 0; i < responsive_layout.count; i++) {
        const DrawCommand *command = &responsive_layout.commands[i];
        if (command->type != DRAW_TEXT || command->text_length == 0) continue;
        bool all_w = true;
        bool all_q = true;
        for (size_t j = 0; j < command->text_length; j++) {
            all_w = all_w && command->text[j] == 'W';
            all_q = all_q && command->text[j] == 'Q';
        }
        if (all_w) {
            anywhere_pieces++;
            anywhere_contained = anywhere_contained
                && anywhere_wrap_box != NULL
                && command->x + command->width
                   <= anywhere_wrap_box->x + anywhere_wrap_box->width;
        }
        if (all_q) {
            break_all_pieces++;
            break_all_contained = break_all_contained
                && break_all_wrap_box != NULL
                && command->x + command->width
                   <= break_all_wrap_box->x + break_all_wrap_box->width;
        }
    }
    CHECK(anywhere_wrap_box != NULL && break_all_wrap_box != NULL
          && anywhere_pieces > 1 && break_all_pieces > 1
          && anywhere_contained && break_all_contained);
    CHECK(content_box_a_box != NULL && content_box_b_box != NULL
          && content_box_a_box->width == 34
          && content_box_b_box->width == 34
          && content_box_b_box->x == content_box_a_box->x + 40);
    CHECK(border_box_a_box != NULL && border_box_b_box != NULL
          && border_box_a_box->width == 30
          && border_box_b_box->width == 30
          && border_box_b_box->x == border_box_a_box->x + 40);
    CHECK(column_align_box != NULL && column_a_box != NULL
          && column_b_box != NULL && column_a_box->width == 20
          && column_b_box->width == 20
          && column_a_box->x == column_align_box->x + 40
          && column_b_box->x == column_a_box->x
          && column_b_box->y == column_a_box->y + column_a_box->height + 7
          && column_align_box->height
             == column_a_box->height + column_b_box->height + 7);
    CHECK(column_end_box != NULL && column_end_item_box != NULL
          && column_end_item_box->x
             == column_end_box->x + column_end_box->width
                - column_end_item_box->width - 3);
    CHECK(column_between_box != NULL && column_between_a_box != NULL
          && column_between_b_box != NULL
          && column_between_a_box->y == column_between_box->y
          && column_between_b_box->y + column_between_b_box->height
             == column_between_box->y + column_between_box->height);
    CHECK(column_center_box != NULL && column_center_item_box != NULL
          && column_center_item_box->y
             == column_center_box->y
                + (column_center_box->height
                   - column_center_item_box->height) / 2);
    const LinkRegion *centered_link = NULL;
    for (size_t i = 0; i < responsive_layout.link_count; i++) {
        if (responsive_layout.links[i].node == column_center_link) {
            centered_link = &responsive_layout.links[i];
            break;
        }
    }
    CHECK(centered_link != NULL && column_center_item_box != NULL
          && centered_link->y >= column_center_item_box->y
          && centered_link->y
             < column_center_item_box->y + column_center_item_box->height);
    CHECK(ratio_box != NULL && ratio_box->width == 160
          && ratio_box->height >= 90);
    const DrawCommand *background_image_command = NULL;
    const DrawCommand *explicit_background_command = NULL;
    const DrawCommand *pseudo_background_command = NULL;
    const DrawCommand *pseudo_background_fill = NULL;
    size_t pseudo_background_image_index = SIZE_MAX;
    size_t pseudo_background_fill_index = SIZE_MAX;
    for (size_t i = 0; i < responsive_layout.count; i++) {
        const DrawCommand *command = &responsive_layout.commands[i];
        if (command->type == DRAW_IMAGE
            && command->image == &image_items[3]
            && command->width == 160 && command->height >= 90
            && command->image_fit == LAYOUT_IMAGE_FIT_COVER) {
            background_image_command = command;
        }
        if (command->type == DRAW_IMAGE
            && command->image == &image_items[4]) {
            explicit_background_command = command;
        }
        if (command->type == DRAW_IMAGE
            && command->image == &image_items[5]) {
            pseudo_background_command = command;
            pseudo_background_image_index = i;
        }
        if (command->type == DRAW_FILL_RECT
            && command->color == 0x123abc) {
            pseudo_background_fill = command;
            pseudo_background_fill_index = i;
        }
    }
    CHECK(background_image_command != NULL);
    CHECK(explicit_background_box != NULL
          && explicit_background_command != NULL);
    CHECK(explicit_background_command->width
             == explicit_background_box->width
          && explicit_background_command->height
             == explicit_background_box->height
          && explicit_background_command->x == explicit_background_box->x
          && explicit_background_command->y == explicit_background_box->y
          && explicit_background_command->image_fit
             == LAYOUT_IMAGE_FIT_SPRITE_TILE_XY
          && draw_command_image_sprite_width(explicit_background_command)
             == 82
          && draw_command_image_sprite_height(explicit_background_command)
             == 20
          && draw_command_image_offset_x(explicit_background_command) == 19
          && draw_command_image_offset_y(explicit_background_command) == 20);
    CHECK(command_x(&responsive_layout, "BRAND")
          < explicit_background_box->x - 9000);
    CHECK(pseudo_background_box != NULL
          && pseudo_background_command != NULL
          && pseudo_background_fill != NULL
          && pseudo_background_fill_index < pseudo_background_image_index
          && pseudo_background_fill->width == 120
          && pseudo_background_fill->height == 40
          && pseudo_background_command->width
             == pseudo_background_box->width
          && pseudo_background_command->height
             == pseudo_background_box->height
          && pseudo_background_command->x == pseudo_background_box->x
          && pseudo_background_command->y == pseudo_background_box->y
          && draw_command_image_sprite_width(pseudo_background_command) == 30
          && draw_command_image_sprite_height(pseudo_background_command) == 20
          && draw_command_image_offset_x(pseudo_background_command) == 45
          && draw_command_image_offset_y(pseudo_background_command) == 0);
    bool found_italic = false;
    const DrawCommand *adjacent_left = NULL, *adjacent_right = NULL;
    const DrawCommand *table_key = NULL, *table_value = NULL;
    const DrawCommand *baseline = NULL, *superscript = NULL;
    for (size_t i = 0; i < responsive_layout.count; i++) {
        if (responsive_layout.commands[i].type == DRAW_TEXT
            && responsive_layout.commands[i].text_length == 6
