    puts("test: root cascade and definite percentage heights");
    static const char root_height_page[] =
        "<!doctype html><style>"
        "html{height:100%;color:#123456}body{height:100%;margin:0;padding:0}"
        "#shell{height:100%;display:flex;flex-direction:column;justify-content:center}"
        "#half{height:100%;max-height:50%;display:flex;flex-direction:column;justify-content:center}"
        "#quarter{min-height:50%}#bounded-width{width:100%;min-width:25%;max-width:50%}"
        "</style><body><div id=shell><div id=half><div id=quarter>"
        "<div id=bounded-width>ROOT</div></div></div></div></body>";
    PocDocument root_height_document;
    Stylesheet root_height_sheet;
    LayoutDocument root_height_layout;
    CHECK(document_parse(&root_height_document, &budget, root_height_page,
                         strlen(root_height_page), 41)
          && stylesheet_build(&root_height_sheet, &budget,
                              &root_height_document, 480)
          && layout_build(&root_height_layout, &budget,
                          &root_height_document, &root_height_sheet,
                          NULL, NULL, 480));
    lxb_dom_node_t *root_body = document_body_node(&root_height_document);
    lxb_dom_node_t *root_html = root_body == NULL ? NULL : root_body->parent;
    lxb_dom_node_t *root_shell = find_id(
        lxb_dom_interface_node(root_height_document.html), "shell");
    lxb_dom_node_t *root_half = find_id(
        lxb_dom_interface_node(root_height_document.html), "half");
    lxb_dom_node_t *root_quarter = find_id(
        lxb_dom_interface_node(root_height_document.html), "quarter");
    lxb_dom_node_t *bounded_width = find_id(
        lxb_dom_interface_node(root_height_document.html), "bounded-width");
    CHECK(root_html != NULL && root_body != NULL && root_shell != NULL
          && root_half != NULL && root_quarter != NULL
          && bounded_width != NULL);
    ComputedStyle root_html_style = style_for_node(
        &root_height_sheet, root_html, NULL);
    ComputedStyle root_body_style = style_for_node(
        &root_height_sheet, root_body, &root_html_style);
    ComputedStyle root_shell_style = style_for_node(
        &root_height_sheet, root_shell, &root_body_style);
    ComputedStyle root_half_style = style_for_node(
        &root_height_sheet, root_half, &root_shell_style);
    ComputedStyle root_quarter_style = style_for_node(
        &root_height_sheet, root_quarter, &root_half_style);
    ComputedStyle bounded_width_style = style_for_node(
        &root_height_sheet, bounded_width, &root_quarter_style);
    CHECK(root_html_style.color == 0x123456
          && root_body_style.color == 0x123456
          && root_shell_style.color == 0x123456
          && root_half_style.max_height_percent
          && root_half_style.max_height == 50
          && root_quarter_style.min_height_percent
          && root_quarter_style.min_height == 50
          && bounded_width_style.min_width_percent
          && bounded_width_style.min_width == 25
          && bounded_width_style.max_width_percent
          && bounded_width_style.max_width == 50);
    const LayoutNodeBox *root_shell_box = layout_box_for_node(
        &root_height_layout, root_shell);
    const LayoutNodeBox *root_half_box = layout_box_for_node(
        &root_height_layout, root_half);
    const LayoutNodeBox *root_quarter_box = layout_box_for_node(
        &root_height_layout, root_quarter);
    const LayoutNodeBox *bounded_width_box = layout_box_for_node(
        &root_height_layout, bounded_width);
    CHECK(root_shell_box != NULL && root_half_box != NULL
          && root_quarter_box != NULL && bounded_width_box != NULL
          && root_shell_box->height == 272
          && root_half_box->height == 136
          && root_half_box->y == root_shell_box->y + 68
          && root_quarter_box->height == 68
          && root_quarter_box->y == root_half_box->y + 34
          && bounded_width_box->width == 240
          && root_height_layout.height == 272);
    layout_destroy(&root_height_layout);
    stylesheet_destroy(&root_height_sheet);
    document_destroy(&root_height_document);
    CHECK(budget.current == 0);

    puts("test: visible root overflow extends the document scroller");
    static const char root_overflow_page[] =
        "<!doctype html><style>html,body{height:100%;margin:0;padding:0}"
        "#tail{height:640px;background:#123456}</style>"
        "<body><main id=tail>ROOT OVERFLOW</main></body>";
    PocDocument root_overflow_document;
    Stylesheet root_overflow_sheet;
    LayoutDocument root_overflow_layout;
    CHECK(document_parse(&root_overflow_document, &budget,
                         root_overflow_page, strlen(root_overflow_page), 42)
          && stylesheet_build(&root_overflow_sheet, &budget,
                              &root_overflow_document, 480)
          && layout_build(&root_overflow_layout, &budget,
                          &root_overflow_document, &root_overflow_sheet,
                          NULL, NULL, 480));
    lxb_dom_node_t *root_overflow_body = document_body_node(
        &root_overflow_document);
    const LayoutNodeBox *root_overflow_body_box = layout_box_for_node(
        &root_overflow_layout, root_overflow_body);
    CHECK(root_overflow_body_box != NULL
          && root_overflow_body_box->height == 272
          && root_overflow_body_box->content_height >= 640
          && root_overflow_layout.height >= 640);
    layout_destroy(&root_overflow_layout);
    stylesheet_destroy(&root_overflow_sheet);
    document_destroy(&root_overflow_document);
    CHECK(budget.current == 0);

    puts("test: body overflow propagation follows the root scroller");
    static const char propagated_body_overflow_page[] =
        "<!doctype html><style>html,body{height:100%;margin:0;padding:0}"
        "body{overflow-y:auto}#tail{height:640px;background:#123456}"
        "</style><body><main id=tail>PROPAGATED</main></body>";
    PocDocument propagated_body_overflow_document;
    Stylesheet propagated_body_overflow_sheet;
    LayoutDocument propagated_body_overflow_layout;
    CHECK(document_parse(&propagated_body_overflow_document, &budget,
                         propagated_body_overflow_page,
                         strlen(propagated_body_overflow_page), 43)
          && stylesheet_build(&propagated_body_overflow_sheet, &budget,
                              &propagated_body_overflow_document, 480)
          && layout_build(&propagated_body_overflow_layout, &budget,
                          &propagated_body_overflow_document,
                          &propagated_body_overflow_sheet,
                          NULL, NULL, 480));
    lxb_dom_node_t *propagated_body = document_body_node(
        &propagated_body_overflow_document);
    const LayoutNodeBox *propagated_body_box = layout_box_for_node(
        &propagated_body_overflow_layout, propagated_body);
    CHECK(propagated_body_box != NULL
          && propagated_body_box->height == 272
          && propagated_body_box->content_height >= 640
          && !propagated_body_box->clips_y
          && propagated_body_overflow_layout.height >= 640);
    layout_destroy(&propagated_body_overflow_layout);
    stylesheet_destroy(&propagated_body_overflow_sheet);
    document_destroy(&propagated_body_overflow_document);
    CHECK(budget.current == 0);

    puts("test: explicit root overflow preserves a local body scroller");
    static const char local_body_overflow_page[] =
        "<!doctype html><style>html{height:100%;overflow:hidden}"
        "body{height:100%;margin:0;padding:0;overflow:auto}"
        "#tail{height:640px;background:#123456}"
        "</style><body><main id=tail>LOCAL</main></body>";
    PocDocument local_body_overflow_document;
    Stylesheet local_body_overflow_sheet;
    LayoutDocument local_body_overflow_layout;
    CHECK(document_parse(&local_body_overflow_document, &budget,
                         local_body_overflow_page,
                         strlen(local_body_overflow_page), 44)
          && stylesheet_build(&local_body_overflow_sheet, &budget,
                              &local_body_overflow_document, 480)
          && layout_build(&local_body_overflow_layout, &budget,
                          &local_body_overflow_document,
                          &local_body_overflow_sheet, NULL, NULL, 480));
    lxb_dom_node_t *local_body = document_body_node(
        &local_body_overflow_document);
    const LayoutNodeBox *local_body_box = layout_box_for_node(
        &local_body_overflow_layout, local_body);
    CHECK(local_body_box != NULL && local_body_box->height == 272
          && local_body_box->content_height >= 640
          && local_body_box->clips_y
          && local_body_overflow_layout.height == 272);
    layout_destroy(&local_body_overflow_layout);
    stylesheet_destroy(&local_body_overflow_sheet);
    document_destroy(&local_body_overflow_document);
    CHECK(budget.current == 0);

    puts("test: bounded table-wide intrinsic tracks and colspan");
    static const char table_track_page[] =
        "<!doctype html><style>body{margin:0;padding:0}table{width:300px}"
        "th,td{padding:2px;border:1px solid #123456}"
        "</style><body><table id=data><tbody id=table-body>"
        "<tr id=row-a><th id=key-a>Key</th><td id=value-a>"
        "A substantially longer value column with useful mobile content"
        "</td></tr><tr id=row-b><th id=key-b>ID</th><td id=value-b>"
        "Another long value that must share the same column boundary"
        "</td></tr><tr id=row-span><td id=span colspan=2>"
        "A full-width table note</td></tr></tbody></table>"
        "<div id=bounded-span colspan=1000 hidden></div></body>";
    PocDocument table_track_document;
    Stylesheet table_track_sheet;
    LayoutDocument table_track_layout;
    CHECK(document_parse(&table_track_document, &budget, table_track_page,
                         strlen(table_track_page), 43)
          && stylesheet_build(&table_track_sheet, &budget,
                              &table_track_document, 480)
          && layout_build(&table_track_layout, &budget,
                          &table_track_document, &table_track_sheet,
                          NULL, NULL, 480));
    lxb_dom_node_t *table_body = find_id(
        lxb_dom_interface_node(table_track_document.html), "table-body");
    lxb_dom_node_t *table_data = find_id(
        lxb_dom_interface_node(table_track_document.html), "data");
    lxb_dom_node_t *table_row_a = find_id(
        lxb_dom_interface_node(table_track_document.html), "row-a");
    lxb_dom_node_t *table_row_b = find_id(
        lxb_dom_interface_node(table_track_document.html), "row-b");
    lxb_dom_node_t *table_key_a = find_id(
        lxb_dom_interface_node(table_track_document.html), "key-a");
    lxb_dom_node_t *table_key_b = find_id(
        lxb_dom_interface_node(table_track_document.html), "key-b");
    lxb_dom_node_t *table_value_a = find_id(
        lxb_dom_interface_node(table_track_document.html), "value-a");
    lxb_dom_node_t *table_value_b = find_id(
        lxb_dom_interface_node(table_track_document.html), "value-b");
    lxb_dom_node_t *table_span = find_id(
        lxb_dom_interface_node(table_track_document.html), "span");
    lxb_dom_node_t *bounded_table_span = find_id(
        lxb_dom_interface_node(table_track_document.html), "bounded-span");
    extern size_t table_cell_span(lxb_dom_node_t *cell);
    CHECK(table_body != NULL && table_data != NULL && table_row_a != NULL
          && table_row_b != NULL && table_key_a != NULL
          && table_key_b != NULL && table_value_a != NULL
          && table_value_b != NULL && table_span != NULL
          && bounded_table_span != NULL
          && table_cell_span(bounded_table_span) == 16);
    ComputedStyle table_body_style = style_for_node(
        &table_track_sheet, table_body, NULL);
    const LayoutNodeBox *table_data_box = layout_box_for_node(
        &table_track_layout, table_data);
    const LayoutNodeBox *table_row_a_box = layout_box_for_node(
        &table_track_layout, table_row_a);
    const LayoutNodeBox *table_row_b_box = layout_box_for_node(
        &table_track_layout, table_row_b);
    const LayoutNodeBox *table_key_a_box = layout_box_for_node(
        &table_track_layout, table_key_a);
    const LayoutNodeBox *table_key_b_box = layout_box_for_node(
        &table_track_layout, table_key_b);
    const LayoutNodeBox *table_value_a_box = layout_box_for_node(
        &table_track_layout, table_value_a);
    const LayoutNodeBox *table_value_b_box = layout_box_for_node(
        &table_track_layout, table_value_b);
    const LayoutNodeBox *table_span_box = layout_box_for_node(
        &table_track_layout, table_span);
    CHECK(table_body_style.display == DISPLAY_TABLE_ROW_GROUP
          && table_data_box != NULL && table_data_box->width == 300
          && table_row_a_box != NULL && table_row_a_box->width == 300
          && table_row_b_box != NULL && table_row_b_box->width == 300
          && table_key_a_box != NULL && table_key_b_box != NULL
          && table_value_a_box != NULL && table_value_b_box != NULL
          && table_span_box != NULL
          && table_key_a_box->width == table_key_b_box->width
          && table_value_a_box->x == table_value_b_box->x
          && table_value_a_box->width == table_value_b_box->width
          && table_key_a_box->width < table_value_a_box->width
          && table_key_a_box->width + table_value_a_box->width == 300
          && table_span_box->width == 300
          && table_row_b_box->y > table_row_a_box->y);
    layout_destroy(&table_track_layout);
    stylesheet_destroy(&table_track_sheet);
    document_destroy(&table_track_document);
    CHECK(budget.current == 0);

    puts("test: bounded mixed grid tracks");
    static const char grid_track_page[] =
        "<!doctype html><style>body{margin:0;padding:0}.grid{display:grid;"
        "width:300px;column-gap:4px}.grid>*{display:block;height:20px}"
        "#weighted{grid-template-columns:40px 1fr 2fr}"
        "#percent{grid-template-columns:25% 1fr 50px}"
        "#minimum{grid-template-columns:minmax(60px,1fr) 2fr}"
        "#intrinsic{grid-template-columns:auto 1fr auto}"
        "#centered{justify-content:center}"
        "#centered-pill{--basis:60px;--pad:16px;"
        "padding:0 calc(var(--pad) - 1px);"
        "min-width:calc(var(--basis) - var(--pad) * 2)}"
        "#spanning{grid-template-columns:repeat(3,1fr);row-gap:5px}"
        "#span-a{grid-column:span 2}#span-full{grid-column:1/-1}"
        "#span-explicit{grid-column:2/4}"
        "</style><body><div id=weighted class=grid><span id=weighted-a>A</span>"
        "<span id=weighted-b>B</span><span id=weighted-c>C</span></div>"
        "<div id=percent class=grid><span id=percent-a>A</span>"
        "<span id=percent-b>B</span><span id=percent-c>C</span></div>"
        "<div id=minimum class=grid><span id=minimum-a>A</span>"
        "<span id=minimum-b>B</span></div>"
        "<div id=intrinsic class=grid><span id=intrinsic-a>Menu</span>"
        "<span id=intrinsic-b>Flexible middle region</span>"
        "<span id=intrinsic-c>Go</span></div>"
        "<div id=centered class=grid><span id=centered-pill>Shop</span></div>"
        "<div id=spanning class=grid><span id=span-a>Two tracks</span>"
        "<span id=span-b>Third</span><span id=span-full>Full row</span>"
        "<span id=span-explicit>Columns two through three</span></div>"
        "</body>";
    PocDocument grid_track_document;
    Stylesheet grid_track_sheet;
    LayoutDocument grid_track_layout;
    CHECK(document_parse(&grid_track_document, &budget, grid_track_page,
                         strlen(grid_track_page), 44)
          && stylesheet_build(&grid_track_sheet, &budget,
                              &grid_track_document, 480)
          && layout_build(&grid_track_layout, &budget,
                          &grid_track_document, &grid_track_sheet,
                          NULL, NULL, 480)
          && grid_track_sheet.grid_tracks != NULL
          && grid_track_sheet.grid_tracks->count != 0
          && grid_track_sheet.grid_tracks->capacity
             < STYLE_GRID_TRACK_BLOCK_COUNT
          && grid_track_sheet.grid_areas == NULL);
    lxb_dom_node_t *weighted = find_id(
        lxb_dom_interface_node(grid_track_document.html), "weighted");
    lxb_dom_node_t *weighted_a = find_id(
        lxb_dom_interface_node(grid_track_document.html), "weighted-a");
    lxb_dom_node_t *weighted_b = find_id(
        lxb_dom_interface_node(grid_track_document.html), "weighted-b");
    lxb_dom_node_t *weighted_c = find_id(
        lxb_dom_interface_node(grid_track_document.html), "weighted-c");
    lxb_dom_node_t *percent_a = find_id(
        lxb_dom_interface_node(grid_track_document.html), "percent-a");
    lxb_dom_node_t *percent_b = find_id(
        lxb_dom_interface_node(grid_track_document.html), "percent-b");
    lxb_dom_node_t *percent_c = find_id(
        lxb_dom_interface_node(grid_track_document.html), "percent-c");
    lxb_dom_node_t *minimum_a = find_id(
        lxb_dom_interface_node(grid_track_document.html), "minimum-a");
    lxb_dom_node_t *minimum_b = find_id(
        lxb_dom_interface_node(grid_track_document.html), "minimum-b");
    lxb_dom_node_t *intrinsic_a = find_id(
        lxb_dom_interface_node(grid_track_document.html), "intrinsic-a");
    lxb_dom_node_t *intrinsic_b = find_id(
        lxb_dom_interface_node(grid_track_document.html), "intrinsic-b");
    lxb_dom_node_t *intrinsic_c = find_id(
        lxb_dom_interface_node(grid_track_document.html), "intrinsic-c");
    lxb_dom_node_t *grid_centered = find_id(
        lxb_dom_interface_node(grid_track_document.html), "centered");
    lxb_dom_node_t *centered_pill = find_id(
        lxb_dom_interface_node(grid_track_document.html), "centered-pill");
    lxb_dom_node_t *span_a = find_id(
        lxb_dom_interface_node(grid_track_document.html), "span-a");
    lxb_dom_node_t *span_b = find_id(
        lxb_dom_interface_node(grid_track_document.html), "span-b");
    lxb_dom_node_t *span_full = find_id(
        lxb_dom_interface_node(grid_track_document.html), "span-full");
    lxb_dom_node_t *span_explicit = find_id(
        lxb_dom_interface_node(grid_track_document.html), "span-explicit");
    CHECK(weighted != NULL && weighted_a != NULL && weighted_b != NULL
          && weighted_c != NULL && percent_a != NULL && percent_b != NULL
          && percent_c != NULL && minimum_a != NULL && minimum_b != NULL
          && intrinsic_a != NULL && intrinsic_b != NULL
          && intrinsic_c != NULL && grid_centered != NULL
          && centered_pill != NULL && span_a != NULL && span_b != NULL
          && span_full != NULL && span_explicit != NULL);
    ComputedStyle weighted_style = style_for_node(
        &grid_track_sheet, weighted, NULL);
    ComputedStyle centered_pill_style = style_for_node(
        &grid_track_sheet, centered_pill, NULL);
    CHECK(computed_style_grid_column_count(&weighted_style) == 3
          && stylesheet_grid_track_type(
                 &grid_track_sheet, &weighted_style, false, 0)
             == GRID_TRACK_FIXED
          && stylesheet_grid_track_type(
                 &grid_track_sheet, &weighted_style, false, 1)
             == GRID_TRACK_FLEX
          && stylesheet_grid_track_type(
                 &grid_track_sheet, &weighted_style, false, 2)
             == GRID_TRACK_FLEX
          && stylesheet_grid_track_value(
                 &grid_track_sheet, &weighted_style, false, 0) == 40
          && stylesheet_grid_track_value(
                 &grid_track_sheet, &weighted_style, false, 1) == 1000
          && stylesheet_grid_track_value(
                 &grid_track_sheet, &weighted_style, false, 2) == 2000);
    CHECK(centered_pill_style.min_width == 28
          && centered_pill_style.padding.left == 15
          && centered_pill_style.padding.right == 15);
    const LayoutNodeBox *weighted_a_box = layout_box_for_node(
        &grid_track_layout, weighted_a);
    const LayoutNodeBox *weighted_b_box = layout_box_for_node(
        &grid_track_layout, weighted_b);
    const LayoutNodeBox *weighted_c_box = layout_box_for_node(
        &grid_track_layout, weighted_c);
    const LayoutNodeBox *percent_a_box = layout_box_for_node(
        &grid_track_layout, percent_a);
    const LayoutNodeBox *percent_b_box = layout_box_for_node(
        &grid_track_layout, percent_b);
    const LayoutNodeBox *percent_c_box = layout_box_for_node(
        &grid_track_layout, percent_c);
    const LayoutNodeBox *minimum_a_box = layout_box_for_node(
        &grid_track_layout, minimum_a);
    const LayoutNodeBox *minimum_b_box = layout_box_for_node(
        &grid_track_layout, minimum_b);
    const LayoutNodeBox *intrinsic_a_box = layout_box_for_node(
        &grid_track_layout, intrinsic_a);
    const LayoutNodeBox *intrinsic_b_box = layout_box_for_node(
        &grid_track_layout, intrinsic_b);
    const LayoutNodeBox *intrinsic_c_box = layout_box_for_node(
        &grid_track_layout, intrinsic_c);
    const LayoutNodeBox *grid_centered_box = layout_box_for_node(
        &grid_track_layout, grid_centered);
    const LayoutNodeBox *centered_pill_box = layout_box_for_node(
        &grid_track_layout, centered_pill);
    const LayoutNodeBox *span_a_box = layout_box_for_node(
        &grid_track_layout, span_a);
    const LayoutNodeBox *span_b_box = layout_box_for_node(
        &grid_track_layout, span_b);
    const LayoutNodeBox *span_full_box = layout_box_for_node(
        &grid_track_layout, span_full);
    const LayoutNodeBox *span_explicit_box = layout_box_for_node(
        &grid_track_layout, span_explicit);
    CHECK(weighted_a_box != NULL && weighted_b_box != NULL
          && weighted_c_box != NULL && weighted_a_box->width == 40
          && weighted_b_box->width == 84 && weighted_c_box->width == 168
          && weighted_b_box->x == weighted_a_box->x + 44
          && weighted_c_box->x == weighted_b_box->x + 88
          && weighted_c_box->x + weighted_c_box->width
             == weighted_a_box->x + 300);
    CHECK(percent_a_box != NULL && percent_b_box != NULL
          && percent_c_box != NULL && percent_a_box->width == 73
          && percent_b_box->width == 169 && percent_c_box->width == 50
          && percent_b_box->x == percent_a_box->x + 77
          && percent_c_box->x == percent_b_box->x + 173
          && percent_c_box->x + percent_c_box->width
             == percent_a_box->x + 300);
    CHECK(minimum_a_box != NULL && minimum_b_box != NULL
          && minimum_a_box->width == 99 && minimum_b_box->width == 197
          && minimum_b_box->x == minimum_a_box->x + 103);
    CHECK(intrinsic_a_box != NULL && intrinsic_b_box != NULL
          && intrinsic_c_box != NULL
          && intrinsic_a_box->width > intrinsic_c_box->width
          && intrinsic_b_box->width > intrinsic_a_box->width
          && intrinsic_b_box->x
             == intrinsic_a_box->x + intrinsic_a_box->width + 4
          && intrinsic_c_box->x
             == intrinsic_b_box->x + intrinsic_b_box->width + 4
          && intrinsic_c_box->x + intrinsic_c_box->width
             == intrinsic_a_box->x + 300);
    CHECK(grid_centered_box != NULL && centered_pill_box != NULL
          && grid_centered_box->width == 300
          && centered_pill_box->width >= 58
          && centered_pill_box->width < grid_centered_box->width
          && centered_pill_box->x - grid_centered_box->x
             == (grid_centered_box->width - centered_pill_box->width) / 2);
    ComputedStyle span_a_style = style_for_node(
        &grid_track_sheet, span_a, NULL);
    ComputedStyle span_full_style = style_for_node(
        &grid_track_sheet, span_full, NULL);
    CHECK(computed_style_grid_column_span(&span_a_style) == 2
          && computed_style_grid_column_start(&span_full_style) == 1
          && computed_style_grid_column_end(&span_full_style)
                 == COMPUTED_GRID_LINE_LAST
          && span_a_box != NULL && span_b_box != NULL
          && span_full_box != NULL && span_explicit_box != NULL
          && span_a_box->width == 199 && span_b_box->width == 97
          && span_b_box->x == span_a_box->x + 203
          && span_b_box->y == span_a_box->y
          && span_full_box->width == 300
          && span_full_box->x == span_a_box->x
          && span_full_box->y == span_a_box->y + span_a_box->height + 5
          && span_explicit_box->width == 198
          && span_explicit_box->x == span_a_box->x + 102
          && span_explicit_box->y
             == span_full_box->y + span_full_box->height + 5);
    layout_destroy(&grid_track_layout);
    stylesheet_destroy(&grid_track_sheet);
    document_destroy(&grid_track_document);
    CHECK(budget.current == 0);

    puts("test: intrinsic and implicit grid track sizing");
    static const char intrinsic_grid_page[] =
        "<!doctype html><style>body{margin:0;padding:0}"
        ".grid{display:grid;width:240px;justify-content:start}"
        ".grid>*{display:block;height:12px}"
        "#content{grid-template-columns:min-content max-content auto}"
        "#repeat{grid-template-columns:repeat(2,min-content)}"
        "#fit{grid-template-columns:fit-content(40px)}"
        "#implicit{width:120px;grid-template-columns:40px;"
        "grid-auto-columns:minmax(0, 1fr)}"
        "#implicit-item{grid-column:2}"
        "#twelve{width:120px;grid-template-columns:repeat(12,1fr)}"
        "</style><body>"
        "<div id=content class=grid><i id=min-track>A A</i>"
        "<i id=max-track>A A</i><i id=auto-track>A A</i></div>"
        "<div id=repeat class=grid><i id=repeat-a>A A</i>"
        "<i id=repeat-b>B B</i></div>"
        "<div id=fit class=grid><i id=fit-item>A A A A A A</i></div>"
        "<div id=implicit class=grid><i>fixed</i>"
        "<i id=implicit-item>i</i></div>"
        "<div id=twelve class=grid><i id=twelve-first></i><i></i><i></i>"
        "<i></i><i></i><i></i><i></i><i></i><i></i><i></i><i></i>"
        "<i id=twelve-last></i></div></body>";
    PocDocument intrinsic_grid_document;
    Stylesheet intrinsic_grid_sheet;
    LayoutDocument intrinsic_grid_layout;
    CHECK(document_parse(
              &intrinsic_grid_document, &budget, intrinsic_grid_page,
              strlen(intrinsic_grid_page), 44)
          && stylesheet_build(
              &intrinsic_grid_sheet, &budget, &intrinsic_grid_document, 480)
          && layout_build(
              &intrinsic_grid_layout, &budget, &intrinsic_grid_document,
              &intrinsic_grid_sheet, NULL, NULL, 480));
    lxb_dom_node_t *intrinsic_grid_root = lxb_dom_interface_node(
        intrinsic_grid_document.html);
    lxb_dom_node_t *min_track = find_id(intrinsic_grid_root, "min-track");
    lxb_dom_node_t *max_track = find_id(intrinsic_grid_root, "max-track");
    lxb_dom_node_t *auto_track = find_id(intrinsic_grid_root, "auto-track");
    lxb_dom_node_t *repeat_a = find_id(intrinsic_grid_root, "repeat-a");
    lxb_dom_node_t *repeat_b = find_id(intrinsic_grid_root, "repeat-b");
    lxb_dom_node_t *fit_item = find_id(intrinsic_grid_root, "fit-item");
    lxb_dom_node_t *implicit_grid = find_id(
        intrinsic_grid_root, "implicit");
    lxb_dom_node_t *implicit_item = find_id(
        intrinsic_grid_root, "implicit-item");
    lxb_dom_node_t *twelve = find_id(intrinsic_grid_root, "twelve");
    lxb_dom_node_t *twelve_first = find_id(
        intrinsic_grid_root, "twelve-first");
    lxb_dom_node_t *twelve_last = find_id(
        intrinsic_grid_root, "twelve-last");
    const LayoutNodeBox *min_track_box = layout_box_for_node(
        &intrinsic_grid_layout, min_track);
    const LayoutNodeBox *max_track_box = layout_box_for_node(
        &intrinsic_grid_layout, max_track);
    const LayoutNodeBox *auto_track_box = layout_box_for_node(
        &intrinsic_grid_layout, auto_track);
    const LayoutNodeBox *repeat_a_box = layout_box_for_node(
        &intrinsic_grid_layout, repeat_a);
    const LayoutNodeBox *repeat_b_box = layout_box_for_node(
        &intrinsic_grid_layout, repeat_b);
    const LayoutNodeBox *fit_item_box = layout_box_for_node(
        &intrinsic_grid_layout, fit_item);
    const LayoutNodeBox *implicit_grid_box = layout_box_for_node(
        &intrinsic_grid_layout, implicit_grid);
    const LayoutNodeBox *implicit_item_box = layout_box_for_node(
        &intrinsic_grid_layout, implicit_item);
    const LayoutNodeBox *twelve_first_box = layout_box_for_node(
        &intrinsic_grid_layout, twelve_first);
    const LayoutNodeBox *twelve_last_box = layout_box_for_node(
        &intrinsic_grid_layout, twelve_last);
    ComputedStyle intrinsic_grid_style = style_for_node(
        &intrinsic_grid_sheet,
        find_id(intrinsic_grid_root, "content"), NULL);
    ComputedStyle implicit_grid_style = style_for_node(
        &intrinsic_grid_sheet, implicit_grid, NULL);
    ComputedStyle twelve_style = style_for_node(
        &intrinsic_grid_sheet, twelve, NULL);
    CHECK(min_track_box != NULL && max_track_box != NULL
          && auto_track_box != NULL && repeat_a_box != NULL
          && repeat_b_box != NULL && fit_item_box != NULL
          && implicit_grid_box != NULL
          && implicit_item_box != NULL
          && min_track_box->width < max_track_box->width
          && auto_track_box->width == max_track_box->width
          && repeat_a_box->width == min_track_box->width
          && repeat_b_box->width == min_track_box->width
          && fit_item_box->width > min_track_box->width
          && fit_item_box->width <= 40
          && implicit_grid_style.grid_auto_column_type == GRID_TRACK_FLEX
          && implicit_grid_style.grid_auto_column_value == 1000
          && implicit_item_box->x == implicit_grid_box->x + 40
          && implicit_item_box->width == 80
          && stylesheet_grid_track_value(
                 &intrinsic_grid_sheet, &intrinsic_grid_style, false, 0)
             != stylesheet_grid_track_value(
                    &intrinsic_grid_sheet, &intrinsic_grid_style, false, 1)
          && stylesheet_grid_track_count(
                 &intrinsic_grid_sheet, &twelve_style, false)
             == GRID_TRACK_REPEAT_LIMIT
          && twelve_first_box != NULL && twelve_last_box != NULL
          && twelve_first_box->width == 10
          && twelve_last_box->width == 10
          && twelve_last_box->x == twelve_first_box->x + 110);
    layout_destroy(&intrinsic_grid_layout);
    stylesheet_destroy(&intrinsic_grid_sheet);
    document_destroy(&intrinsic_grid_document);
    CHECK(budget.current == 0);

    puts("test: standard grid row placement, sizing, order, and empty paint");
    static const char grid_rows_page[] =
        "<!doctype html><style>body{margin:0;padding:0}"
        "#rows{display:grid;width:120px;height:100px;"
        "grid-template-columns:60px 60px;"
        "grid-template-rows:30px 1fr;row-gap:4px}"
        "#row-two{grid-row:2;grid-column:1}"
        "#intrinsic-row{height:max-content}"
        "#ordered{display:grid;width:80px;"
        "grid-template-columns:40px 40px;grid-template-rows:20px 20px}"
        "#order-a{order:2}#order-b{order:-1}#order-c{order:1}"
        "#empty{height:0;background:#123456}"
        "</style><body><div id=rows>"
        "<span id=row-two>ROW2</span><span id=intrinsic-row>SHORT</span>"
        "<span id=row-one-stretch>STRETCH</span></div>"
        "<div id=ordered><i id=order-a>A</i><i id=order-b>B</i>"
        "<i id=order-c>C</i></div><div id=empty></div></body>";
    PocDocument grid_rows_document;
    Stylesheet grid_rows_sheet;
    LayoutDocument grid_rows_layout;
    CHECK(document_parse(&grid_rows_document, &budget, grid_rows_page,
                         strlen(grid_rows_page), 41)
          && stylesheet_build(&grid_rows_sheet, &budget,
                              &grid_rows_document, 480)
          && layout_build(&grid_rows_layout, &budget,
                          &grid_rows_document, &grid_rows_sheet,
                          NULL, NULL, 480));
    lxb_dom_node_t *grid_rows_root = lxb_dom_interface_node(
        grid_rows_document.html);
    lxb_dom_node_t *grid_rows = find_id(grid_rows_root, "rows");
    lxb_dom_node_t *row_two = find_id(grid_rows_root, "row-two");
    lxb_dom_node_t *intrinsic_row = find_id(
        grid_rows_root, "intrinsic-row");
    lxb_dom_node_t *row_one_stretch = find_id(
        grid_rows_root, "row-one-stretch");
    lxb_dom_node_t *order_a = find_id(grid_rows_root, "order-a");
    lxb_dom_node_t *order_b = find_id(grid_rows_root, "order-b");
    lxb_dom_node_t *order_c = find_id(grid_rows_root, "order-c");
    ComputedStyle grid_rows_style = style_for_node(
        &grid_rows_sheet, grid_rows, NULL);
    ComputedStyle row_two_style = style_for_node(
        &grid_rows_sheet, row_two, NULL);
    ComputedStyle intrinsic_row_style = style_for_node(
        &grid_rows_sheet, intrinsic_row, NULL);
    const LayoutNodeBox *row_two_box = layout_box_for_node(
        &grid_rows_layout, row_two);
    const LayoutNodeBox *intrinsic_row_box = layout_box_for_node(
        &grid_rows_layout, intrinsic_row);
    const LayoutNodeBox *row_one_stretch_box = layout_box_for_node(
        &grid_rows_layout, row_one_stretch);
    const LayoutNodeBox *order_a_box = layout_box_for_node(
        &grid_rows_layout, order_a);
    const LayoutNodeBox *order_b_box = layout_box_for_node(
        &grid_rows_layout, order_b);
    const LayoutNodeBox *order_c_box = layout_box_for_node(
        &grid_rows_layout, order_c);
    bool painted_empty = false;
    for (size_t i = 0; i < grid_rows_layout.count; i++) {
        const DrawCommand *command = &grid_rows_layout.commands[i];
        if (command->type == DRAW_FILL_RECT
            && command->color == 0x123456
            && command->width > 0 && command->height > 0
            && command->opacity_scale != 0) painted_empty = true;
    }
    CHECK(stylesheet_grid_track_count(
              &grid_rows_sheet, &grid_rows_style, true) == 2
          && stylesheet_grid_track_type(
                 &grid_rows_sheet, &grid_rows_style, true, 0)
             == GRID_TRACK_FIXED
          && stylesheet_grid_track_value(
                 &grid_rows_sheet, &grid_rows_style, true, 0) == 30
          && stylesheet_grid_track_type(
                 &grid_rows_sheet, &grid_rows_style, true, 1)
             == GRID_TRACK_FLEX
          && computed_style_grid_row_start(&row_two_style) == 2
          && computed_style_grid_column_start(&row_two_style) == 1
          && intrinsic_row_style.has_height
          && intrinsic_row_style.height == STYLE_LENGTH_NONE
          && row_two_box != NULL && row_two_box->x == 0
          && row_two_box->y == 34 && row_two_box->width == 60
          && row_two_box->height == 66
          && intrinsic_row_box != NULL && intrinsic_row_box->x == 0
          && intrinsic_row_box->y == 0 && intrinsic_row_box->width == 60
          && intrinsic_row_box->height > 0
          && intrinsic_row_box->height < 30
          && row_one_stretch_box != NULL && row_one_stretch_box->x == 60
          && row_one_stretch_box->y == 0
          && row_one_stretch_box->height == 30
          && order_b_box != NULL && order_b_box->x == 0
          && order_b_box->width == 40
          && order_c_box != NULL && order_c_box->x == 40
          && order_a_box != NULL && order_a_box->x == 0
          && order_a_box->y == order_b_box->y + 20
          && !painted_empty);
    layout_destroy(&grid_rows_layout);
    stylesheet_destroy(&grid_rows_sheet);
    document_destroy(&grid_rows_document);
    CHECK(budget.current == 0);

    puts("test: grid flex rows consume a definite minimum block size");
    static const char grid_minimum_page[] =
        "<!doctype html><style>html,body{margin:0;padding:0}"
        "#shell{display:grid;min-height:100dvh;"
        "grid-template-rows:auto 1fr auto}"
        "header{height:20px}footer{height:10px}"
        "</style><body><div id=shell><header id=min-head></header>"
        "<main id=min-main></main><footer id=min-foot></footer>"
        "</div></body>";
    PocDocument grid_minimum_document;
    Stylesheet grid_minimum_sheet;
    LayoutDocument grid_minimum_layout;
    CHECK(document_parse(&grid_minimum_document, &budget,
                         grid_minimum_page, strlen(grid_minimum_page), 41)
          && stylesheet_build(&grid_minimum_sheet, &budget,
                              &grid_minimum_document, 480)
          && layout_build(&grid_minimum_layout, &budget,
                          &grid_minimum_document, &grid_minimum_sheet,
                          NULL, NULL, 480));
    lxb_dom_node_t *grid_minimum_root = lxb_dom_interface_node(
        grid_minimum_document.html);
    const LayoutNodeBox *minimum_main_box = layout_box_for_node(
        &grid_minimum_layout, find_id(grid_minimum_root, "min-main"));
    const LayoutNodeBox *minimum_footer_box = layout_box_for_node(
        &grid_minimum_layout, find_id(grid_minimum_root, "min-foot"));
    const LayoutNodeBox *minimum_shell_box = layout_box_for_node(
        &grid_minimum_layout, find_id(grid_minimum_root, "shell"));
    if (minimum_main_box == NULL || minimum_footer_box == NULL
        || minimum_shell_box == NULL
        || minimum_main_box->height < 241
        || minimum_main_box->height > 243
        || minimum_footer_box->y < 261
        || minimum_footer_box->y > 263
        || minimum_shell_box->height != 272) {
        fprintf(stderr,
                "grid minimum rows shell=%d main=%d footer-y=%d\n",
                minimum_shell_box == NULL ? -1 : minimum_shell_box->height,
                minimum_main_box == NULL ? -1 : minimum_main_box->height,
                minimum_footer_box == NULL ? -1 : minimum_footer_box->y);
    }
    CHECK(minimum_main_box != NULL && minimum_footer_box != NULL
          && minimum_shell_box != NULL
          && minimum_main_box->height >= 241
          && minimum_main_box->height <= 243
          && minimum_footer_box->y >= 261
          && minimum_footer_box->y <= 263
          && minimum_shell_box->height == 272);
    layout_destroy(&grid_minimum_layout);
    stylesheet_destroy(&grid_minimum_sheet);
    document_destroy(&grid_minimum_document);
    CHECK(budget.current == 0);

    puts("test: hidden zero-inset image backdrop has a usable fallback");
    static const char staged_backdrop_page[] =
        "<!doctype html><body><div id=staged style=\"position:fixed;"
        "inset:0;display:none;opacity:0;"
        "background-image:url(/hero.webp)\"></div></body>";
    PocDocument staged_backdrop_document;
    Stylesheet staged_backdrop_sheet;
    CHECK(document_parse(&staged_backdrop_document, &budget,
                         staged_backdrop_page,
                         strlen(staged_backdrop_page), 41)
          && stylesheet_build(&staged_backdrop_sheet, &budget,
                              &staged_backdrop_document, 480));
    lxb_dom_node_t *staged_backdrop = find_id(
        lxb_dom_interface_node(staged_backdrop_document.html), "staged");
    ComputedStyle staged_backdrop_style = style_for_node(
        &staged_backdrop_sheet, staged_backdrop, NULL);
    CHECK(staged_backdrop_style.display == DISPLAY_BLOCK
          && staged_backdrop_style.opacity == 255
          && staged_backdrop_style.fixed_position
          && staged_backdrop_style.has_z_index
          && staged_backdrop_style.z_index == -1
          && staged_backdrop_style.background_image_kind
                 == STYLE_BACKGROUND_IMAGE_URL);
    stylesheet_destroy(&staged_backdrop_sheet);
    document_destroy(&staged_backdrop_document);
    CHECK(budget.current == 0);

    puts("test: grid dense/column auto-flow and item alignment");
    static const char grid_flow_page[] =
        "<!doctype html><style>body{margin:0;padding:0}"
        ".item{display:block;height:10px}"
        "#dense{display:grid;width:60px;grid-template-columns:20px 20px 20px;"
        "grid-template-rows:20px 20px;grid-auto-flow:row dense}"
        "#dense-a,#dense-b{grid-column:span 2}"
        "#column{display:grid;width:60px;grid-template-columns:30px 30px;"
        "grid-template-rows:20px 20px;grid-auto-flow:column}"
        "#aligned{display:grid;width:120px;grid-template-columns:40px 40px 40px;"
        "grid-template-rows:40px;align-items:start}"
        "#align-a{width:10px;align-self:start;justify-self:start}"
        "#align-b{width:10px;align-self:center;justify-self:center}"
        "#align-c{width:10px;align-self:end;justify-self:end}"
        "#implicit{display:grid;width:50px;grid-template-columns:20px;"
        "grid-auto-columns:30px;grid-auto-rows:25px}"
        "#implicit-a{grid-column:1;grid-row:1}"
        "#implicit-b{grid-column:2;grid-row:1}"
        "#implicit-c{grid-column:1;grid-row:2}"
        "#area{display:grid;width:60px;grid-template-columns:20px 20px 20px;"
        "grid-template-rows:25px 25px}"
        "#area-a{grid-area:2 / 2 / 3 / 4}"
        "#placed{display:grid;width:120px;height:50px;"
        "grid-template-columns:40px 40px;grid-template-rows:20px;"
        "place-items:center end;place-content:end center}"
        "#placed .item{width:10px}"
        "#placed-b{place-self:start end}"
        "#named{display:grid;width:90px;grid-template-columns:30px 60px;"
        "grid-template-rows:20px 30px;"
        "grid-template-areas:'lead main' 'foot main';"
        "grid-template-areas:'cross other' 'other cross'}"
        "#named-lead{grid-area:lead}#named-main{grid-area:main}"
        "#named-foot{grid-area:foot;grid-area:1st}"
        "#named-auto{display:grid;width:90px;grid-template-columns:30px;"
        "grid-template-rows:20px;grid-auto-columns:15px;"
        "grid-auto-rows:12px;grid-template-areas:'a b' 'c d'}"
        "#named-auto-a{grid-area:a}#named-auto-b{grid-area:b}"
        "#named-auto-c{grid-area:c}#named-auto-d{grid-area:d}"
        "</style><body>"
        "<div id=dense><i id=dense-a class=item>A</i>"
        "<i id=dense-b class=item>B</i><i id=dense-c class=item>C</i></div>"
        "<div id=column><i id=column-a class=item>A</i>"
        "<i id=column-b class=item>B</i><i id=column-c class=item>C</i>"
        "<i id=column-d class=item>D</i></div>"
        "<div id=aligned><i id=align-a class=item>A</i>"
        "<i id=align-b class=item>B</i><i id=align-c class=item>C</i></div>"
        "<div id=implicit><i id=implicit-a class=item>A</i>"
        "<i id=implicit-b class=item>B</i>"
        "<i id=implicit-c class=item>C</i></div>"
        "<div id=area><i id=area-a class=item>A</i></div>"
        "<div id=placed><i id=placed-a class=item>A</i>"
        "<i id=placed-b class=item>B</i></div>"
        "<div id=named><i id=named-lead class=item>L</i>"
        "<i id=named-main class=item>M</i>"
        "<i id=named-foot class=item>F</i></div>"
        "<div id=named-auto><i id=named-auto-a class=item>A</i>"
        "<i id=named-auto-b class=item>B</i>"
        "<i id=named-auto-c class=item>C</i>"
        "<i id=named-auto-d class=item>D</i></div>"
        "</body>";
    PocDocument grid_flow_document;
    Stylesheet grid_flow_sheet;
    LayoutDocument grid_flow_layout;
    CHECK(document_parse(&grid_flow_document, &budget, grid_flow_page,
                         strlen(grid_flow_page), 47)
          && stylesheet_build(&grid_flow_sheet, &budget,
                              &grid_flow_document, 480)
          && layout_build(&grid_flow_layout, &budget,
                          &grid_flow_document, &grid_flow_sheet,
                          NULL, NULL, 480));
    lxb_dom_node_t *grid_flow_root = lxb_dom_interface_node(
        grid_flow_document.html);
    lxb_dom_node_t *dense = find_id(grid_flow_root, "dense");
    lxb_dom_node_t *dense_a = find_id(grid_flow_root, "dense-a");
    lxb_dom_node_t *dense_b = find_id(grid_flow_root, "dense-b");
    lxb_dom_node_t *dense_c = find_id(grid_flow_root, "dense-c");
    lxb_dom_node_t *column = find_id(grid_flow_root, "column");
    lxb_dom_node_t *grid_column_a = find_id(grid_flow_root, "column-a");
    lxb_dom_node_t *grid_column_b = find_id(grid_flow_root, "column-b");
    lxb_dom_node_t *grid_column_c = find_id(grid_flow_root, "column-c");
    lxb_dom_node_t *grid_column_d = find_id(grid_flow_root, "column-d");
    lxb_dom_node_t *align_a = find_id(grid_flow_root, "align-a");
    lxb_dom_node_t *align_b = find_id(grid_flow_root, "align-b");
    lxb_dom_node_t *align_c = find_id(grid_flow_root, "align-c");
    lxb_dom_node_t *implicit = find_id(grid_flow_root, "implicit");
    lxb_dom_node_t *implicit_a = find_id(grid_flow_root, "implicit-a");
    lxb_dom_node_t *implicit_b = find_id(grid_flow_root, "implicit-b");
    lxb_dom_node_t *implicit_c = find_id(grid_flow_root, "implicit-c");
    lxb_dom_node_t *area_a = find_id(grid_flow_root, "area-a");
    lxb_dom_node_t *placed = find_id(grid_flow_root, "placed");
    lxb_dom_node_t *placed_a = find_id(grid_flow_root, "placed-a");
    lxb_dom_node_t *placed_b = find_id(grid_flow_root, "placed-b");
    lxb_dom_node_t *named = find_id(grid_flow_root, "named");
    lxb_dom_node_t *named_lead = find_id(grid_flow_root, "named-lead");
    lxb_dom_node_t *named_main = find_id(grid_flow_root, "named-main");
    lxb_dom_node_t *named_foot = find_id(grid_flow_root, "named-foot");
    lxb_dom_node_t *named_auto = find_id(grid_flow_root, "named-auto");
    lxb_dom_node_t *named_auto_a = find_id(
        grid_flow_root, "named-auto-a");
    lxb_dom_node_t *named_auto_b = find_id(
        grid_flow_root, "named-auto-b");
    lxb_dom_node_t *named_auto_c = find_id(
        grid_flow_root, "named-auto-c");
    lxb_dom_node_t *named_auto_d = find_id(
        grid_flow_root, "named-auto-d");
    CHECK(dense != NULL && dense_a != NULL && dense_b != NULL
          && dense_c != NULL && column != NULL && grid_column_a != NULL
          && grid_column_b != NULL && grid_column_c != NULL
          && grid_column_d != NULL
          && align_a != NULL && align_b != NULL && align_c != NULL
          && implicit != NULL && implicit_a != NULL && implicit_b != NULL
          && implicit_c != NULL && area_a != NULL && placed != NULL
          && placed_a != NULL && placed_b != NULL && named != NULL
          && named_lead != NULL && named_main != NULL
          && named_foot != NULL && named_auto != NULL
          && named_auto_a != NULL && named_auto_b != NULL
          && named_auto_c != NULL && named_auto_d != NULL);
    ComputedStyle dense_style = style_for_node(
        &grid_flow_sheet, dense, NULL);
    ComputedStyle column_style = style_for_node(
        &grid_flow_sheet, column, NULL);
    ComputedStyle align_b_style = style_for_node(
        &grid_flow_sheet, align_b, NULL);
    ComputedStyle implicit_style = style_for_node(
        &grid_flow_sheet, implicit, NULL);
    ComputedStyle area_a_style = style_for_node(
        &grid_flow_sheet, area_a, NULL);
    ComputedStyle placed_style = style_for_node(
        &grid_flow_sheet, placed, NULL);
    ComputedStyle placed_b_style = style_for_node(
        &grid_flow_sheet, placed_b, NULL);
    ComputedStyle named_style = style_for_node(
        &grid_flow_sheet, named, NULL);
    ComputedStyle named_main_style = style_for_node(
        &grid_flow_sheet, named_main, &named_style);
    ComputedStyle resolved_named_main_style = named_main_style;
    char named_serialized[96];
    CHECK(stylesheet_resolve_named_grid_area(
        &grid_flow_sheet, &named_style, &resolved_named_main_style));
    const LayoutNodeBox *dense_a_box = layout_box_for_node(
        &grid_flow_layout, dense_a);
    const LayoutNodeBox *dense_b_box = layout_box_for_node(
        &grid_flow_layout, dense_b);
    const LayoutNodeBox *dense_c_box = layout_box_for_node(
        &grid_flow_layout, dense_c);
    const LayoutNodeBox *grid_column_a_box = layout_box_for_node(
        &grid_flow_layout, grid_column_a);
    const LayoutNodeBox *grid_column_b_box = layout_box_for_node(
        &grid_flow_layout, grid_column_b);
    const LayoutNodeBox *grid_column_c_box = layout_box_for_node(
        &grid_flow_layout, grid_column_c);
    const LayoutNodeBox *grid_column_d_box = layout_box_for_node(
        &grid_flow_layout, grid_column_d);
    const LayoutNodeBox *align_a_box = layout_box_for_node(
        &grid_flow_layout, align_a);
    const LayoutNodeBox *align_b_box = layout_box_for_node(
        &grid_flow_layout, align_b);
    const LayoutNodeBox *align_c_box = layout_box_for_node(
        &grid_flow_layout, align_c);
    const LayoutNodeBox *implicit_a_box = layout_box_for_node(
        &grid_flow_layout, implicit_a);
    const LayoutNodeBox *implicit_b_box = layout_box_for_node(
        &grid_flow_layout, implicit_b);
    const LayoutNodeBox *implicit_c_box = layout_box_for_node(
        &grid_flow_layout, implicit_c);
    const LayoutNodeBox *area_a_box = layout_box_for_node(
        &grid_flow_layout, area_a);
    const LayoutNodeBox *placed_box = layout_box_for_node(
        &grid_flow_layout, placed);
    const LayoutNodeBox *placed_a_box = layout_box_for_node(
        &grid_flow_layout, placed_a);
    const LayoutNodeBox *placed_b_box = layout_box_for_node(
        &grid_flow_layout, placed_b);
    const LayoutNodeBox *named_box = layout_box_for_node(
        &grid_flow_layout, named);
    const LayoutNodeBox *named_lead_box = layout_box_for_node(
        &grid_flow_layout, named_lead);
    const LayoutNodeBox *named_main_box = layout_box_for_node(
        &grid_flow_layout, named_main);
    const LayoutNodeBox *named_foot_box = layout_box_for_node(
        &grid_flow_layout, named_foot);
    const LayoutNodeBox *named_auto_box = layout_box_for_node(
        &grid_flow_layout, named_auto);
    const LayoutNodeBox *named_auto_a_box = layout_box_for_node(
        &grid_flow_layout, named_auto_a);
    const LayoutNodeBox *named_auto_b_box = layout_box_for_node(
        &grid_flow_layout, named_auto_b);
    const LayoutNodeBox *named_auto_c_box = layout_box_for_node(
        &grid_flow_layout, named_auto_c);
    const LayoutNodeBox *named_auto_d_box = layout_box_for_node(
        &grid_flow_layout, named_auto_d);
    CHECK(computed_style_grid_auto_flow_dense(&dense_style)
          && !computed_style_grid_auto_flow_column(&dense_style)
          && computed_style_grid_auto_flow_column(&column_style)
          && !computed_style_grid_auto_flow_dense(&column_style)
          && computed_style_justify_self(&align_b_style)
             == ALIGN_SELF_CENTER
          && implicit_style.grid_auto_column_type == GRID_TRACK_FIXED
          && implicit_style.grid_auto_column_value == 30
          && implicit_style.grid_auto_row_type == GRID_TRACK_FIXED
          && implicit_style.grid_auto_row_value == 25
          && computed_style_grid_row_start(&area_a_style) == 2
          && computed_style_grid_column_start(&area_a_style) == 2
          && computed_style_grid_row_end(&area_a_style) == 3
          && computed_style_grid_column_end(&area_a_style) == 4
          && placed_style.align_items == ALIGN_CENTER
          && placed_style.justify_items == ALIGN_END
          && placed_style.align_content == JUSTIFY_END
          && placed_style.justify_content == JUSTIFY_CENTER
          && placed_b_style.align_self == ALIGN_SELF_START
          && computed_style_justify_self(&placed_b_style)
             == ALIGN_SELF_END
          && computed_style_grid_template_area_id(&named_style) != 0
          && computed_style_grid_area_is_named(&named_main_style)
          && computed_style_grid_row_start(&resolved_named_main_style) == 1
          && computed_style_grid_row_end(&resolved_named_main_style) == 3
          && computed_style_grid_column_start(&resolved_named_main_style) == 2
          && computed_style_grid_column_end(&resolved_named_main_style) == 3
          && stylesheet_serialize_grid_template_areas(
                 &grid_flow_sheet, &named_style,
                 named_serialized, sizeof(named_serialized))
          && strcmp(named_serialized,
                    "\"lead main\" \"foot main\"") == 0);
    CHECK(dense_a_box != NULL && dense_b_box != NULL && dense_c_box != NULL
          && dense_a_box->x == 0 && dense_a_box->y == 0
          && dense_b_box->x == 0 && dense_b_box->y == 20
          && dense_c_box->x == 40 && dense_c_box->y == 0);
    CHECK(grid_column_a_box != NULL && grid_column_b_box != NULL
          && grid_column_c_box != NULL && grid_column_d_box != NULL
          && grid_column_a_box->x == 0 && grid_column_a_box->y == 40
          && grid_column_b_box->x == 0 && grid_column_b_box->y == 60
          && grid_column_c_box->x == 30 && grid_column_c_box->y == 40
          && grid_column_d_box->x == 30 && grid_column_d_box->y == 60);
    CHECK(align_a_box != NULL && align_b_box != NULL && align_c_box != NULL
          && align_a_box->x == 0 && align_a_box->y == 80
          && align_b_box->x == 55 && align_b_box->y == 95
          && align_c_box->x == 110 && align_c_box->y == 110);
    CHECK(implicit_a_box != NULL && implicit_b_box != NULL
          && implicit_c_box != NULL
          && implicit_a_box->x == 0 && implicit_a_box->y == 120
          && implicit_a_box->width == 20
          && implicit_b_box->x == 20 && implicit_b_box->y == 120
          && implicit_b_box->width == 30
          && implicit_c_box->x == 0 && implicit_c_box->y == 145
          && implicit_c_box->width == 20);
    CHECK(area_a_box != NULL && area_a_box->x == 20
          && area_a_box->y == 195 && area_a_box->width == 40
          && area_a_box->height == 10);
    CHECK(placed_box != NULL && placed_a_box != NULL && placed_b_box != NULL
          && placed_box->width == 120 && placed_box->height == 50
          && placed_a_box->x == placed_box->x + 50
          && placed_a_box->y == placed_box->y + 35
          && placed_b_box->x == placed_box->x + 90
          && placed_b_box->y == placed_box->y + 30);
    CHECK(named_box != NULL && named_lead_box != NULL
          && named_main_box != NULL && named_foot_box != NULL
          && named_box->width == 90 && named_box->height == 50
          && named_lead_box->x == named_box->x
          && named_lead_box->y == named_box->y
          && named_lead_box->width == 30
          && named_main_box->x == named_box->x + 30
          && named_main_box->y == named_box->y
          && named_main_box->width == 60
          && named_foot_box->x == named_box->x
          && named_foot_box->y == named_box->y + 20
          && named_foot_box->width == 30);
    CHECK(named_auto_box != NULL && named_auto_a_box != NULL
          && named_auto_b_box != NULL && named_auto_c_box != NULL
          && named_auto_d_box != NULL
          && named_auto_box->width == 90
          && named_auto_a_box->x == named_auto_box->x
          && named_auto_a_box->y == named_auto_box->y
          && named_auto_a_box->width == 30
          && named_auto_b_box->x == named_auto_box->x + 30
          && named_auto_b_box->y == named_auto_box->y
          && named_auto_b_box->width == 15
          && named_auto_c_box->x == named_auto_box->x
          && named_auto_c_box->y == named_auto_box->y + 20
          && named_auto_d_box->x == named_auto_box->x + 30
          && named_auto_d_box->y == named_auto_box->y + 20);
    layout_destroy(&grid_flow_layout);
    stylesheet_destroy(&grid_flow_sheet);
    document_destroy(&grid_flow_document);
    CHECK(budget.current == 0);

    puts("test: bounded named grid lines and parent-track subgrid");
    static const char subgrid_page[] =
        "<!doctype html><style>body{margin:0;padding:0}"
        "#parent,#override-parent{display:grid;width:132px;height:54px;"
        "grid-template-columns:[left]30px[content]50px[right]40px[end];"
        "grid-template-rows:[top]20px[middle]30px[bottom];"
        "column-gap:6px;row-gap:4px}"
        "#child{display:grid;grid-column:2/4;grid-row:1/3;"
        "grid-template-columns:subgrid [local-start] [local-middle] "
        "[local-end];grid-template-rows:subgrid}"
        "#inherited-column{grid-column:content/right;grid-row:1}"
        "#local-column{grid-column:local-middle/local-end;grid-row:1}"
        "#inherited-row{grid-column:1;grid-row:middle/bottom}"
        "#override-child{display:grid;grid-column:2/4;grid-row:1/3;"
        "grid-template:subgrid/subgrid;column-gap:2px;row-gap:8px}"
        "#override-column{grid-column:2;grid-row:1}"
        "#override-row{grid-column:1;grid-row:2}"
        "#wide{display:grid;width:186px;"
        "grid-template-columns:10px 11px 12px 13px 14px 15px "
        "16px 17px 18px 19px 20px 21px}"
        "#row-min{display:grid;grid-template-rows:minmax(25px,1fr);"
        "grid-template-columns:25% 1.5fr}"
        "</style><body><div id=parent><div id=child>"
        "<i id=inherited-column></i><i id=local-column></i>"
        "<i id=inherited-row></i></div></div>"
        "<div id=override-parent><div id=override-child>"
        "<i id=override-column></i><i id=override-row></i></div></div>"
        "<div id=wide><i></i><i></i><i></i><i></i><i></i><i></i>"
        "<i></i><i></i><i></i><i></i><i></i>"
        "<i id=wide-last></i></div><div id=row-min></div></body>";
    PocDocument subgrid_document;
    Stylesheet subgrid_sheet;
    LayoutDocument subgrid_layout;
    CHECK(document_parse(&subgrid_document, &budget, subgrid_page,
                         strlen(subgrid_page), 48)
          && stylesheet_build(&subgrid_sheet, &budget,
                              &subgrid_document, 480)
          && layout_build(&subgrid_layout, &budget, &subgrid_document,
                          &subgrid_sheet, NULL, NULL, 480));
    lxb_dom_node_t *subgrid_root =
        lxb_dom_interface_node(subgrid_document.html);
    lxb_dom_node_t *subgrid_parent = find_id(subgrid_root, "parent");
    lxb_dom_node_t *subgrid_child = find_id(subgrid_root, "child");
    lxb_dom_node_t *inherited_column =
        find_id(subgrid_root, "inherited-column");
    lxb_dom_node_t *local_column =
        find_id(subgrid_root, "local-column");
    lxb_dom_node_t *inherited_row =
        find_id(subgrid_root, "inherited-row");
    lxb_dom_node_t *wide = find_id(subgrid_root, "wide");
    lxb_dom_node_t *wide_last = find_id(subgrid_root, "wide-last");
    lxb_dom_node_t *row_min = find_id(subgrid_root, "row-min");
    lxb_dom_node_t *override_parent =
        find_id(subgrid_root, "override-parent");
    lxb_dom_node_t *override_child =
        find_id(subgrid_root, "override-child");
    lxb_dom_node_t *override_column =
        find_id(subgrid_root, "override-column");
    lxb_dom_node_t *override_row =
        find_id(subgrid_root, "override-row");
    CHECK(subgrid_parent != NULL && subgrid_child != NULL
          && inherited_column != NULL && local_column != NULL
          && inherited_row != NULL && wide != NULL && wide_last != NULL
          && row_min != NULL
          && override_parent != NULL && override_child != NULL
          && override_column != NULL && override_row != NULL);
    ComputedStyle subgrid_child_style = style_for_node(
        &subgrid_sheet, subgrid_child, NULL);
    ComputedStyle wide_style = style_for_node(
        &subgrid_sheet, wide, NULL);
    ComputedStyle row_min_style = style_for_node(
        &subgrid_sheet, row_min, NULL);
    char subgrid_columns[160];
    char wide_columns[256];
    char fractional_columns[64];
    char minimum_rows[64];
    const LayoutNodeBox *subgrid_parent_box = layout_box_for_node(
        &subgrid_layout, subgrid_parent);
    const LayoutNodeBox *subgrid_child_box = layout_box_for_node(
        &subgrid_layout, subgrid_child);
    const LayoutNodeBox *inherited_column_box = layout_box_for_node(
        &subgrid_layout, inherited_column);
    const LayoutNodeBox *local_column_box = layout_box_for_node(
        &subgrid_layout, local_column);
    const LayoutNodeBox *inherited_row_box = layout_box_for_node(
        &subgrid_layout, inherited_row);
    const LayoutNodeBox *wide_box = layout_box_for_node(
        &subgrid_layout, wide);
    const LayoutNodeBox *wide_last_box = layout_box_for_node(
        &subgrid_layout, wide_last);
    const LayoutNodeBox *override_parent_box = layout_box_for_node(
        &subgrid_layout, override_parent);
    const LayoutNodeBox *override_child_box = layout_box_for_node(
        &subgrid_layout, override_child);
    const LayoutNodeBox *override_column_box = layout_box_for_node(
        &subgrid_layout, override_column);
    const LayoutNodeBox *override_row_box = layout_box_for_node(
        &subgrid_layout, override_row);
    CHECK(computed_style_grid_subgrid_columns(&subgrid_child_style)
          && computed_style_grid_subgrid_rows(&subgrid_child_style)
          && stylesheet_grid_track_count(
                 &subgrid_sheet, &wide_style, false) == 12
          && stylesheet_serialize_grid_template_tracks(
                 &subgrid_sheet, &subgrid_child_style, false,
                 subgrid_columns, sizeof(subgrid_columns))
          && strcmp(subgrid_columns,
                    "subgrid [local-start] [local-middle] [local-end]") == 0
          && stylesheet_serialize_grid_template_tracks(
                 &subgrid_sheet, &wide_style, false,
                 wide_columns, sizeof(wide_columns))
          && strstr(wide_columns, "10px 11px 12px") == wide_columns
          && strstr(wide_columns, "20px 21px") != NULL
          && stylesheet_grid_track_count(
                 &subgrid_sheet, &row_min_style, true) == 1
          && stylesheet_grid_track_minimum(
                 &subgrid_sheet, &row_min_style, true, 0) == 25
          && stylesheet_serialize_grid_template_tracks(
                 &subgrid_sheet, &row_min_style, false,
                 fractional_columns, sizeof(fractional_columns))
          && strcmp(fractional_columns, "25% 1.5fr") == 0
          && stylesheet_serialize_grid_template_tracks(
                 &subgrid_sheet, &row_min_style, true,
                 minimum_rows, sizeof(minimum_rows))
          && strcmp(minimum_rows, "minmax(25px,1fr)") == 0);
    CHECK(subgrid_parent_box != NULL && subgrid_child_box != NULL
          && inherited_column_box != NULL && local_column_box != NULL
          && inherited_row_box != NULL
          && subgrid_child_box->x == subgrid_parent_box->x + 36
          && subgrid_child_box->width == 96
          && inherited_column_box->x == subgrid_child_box->x
          && inherited_column_box->width == 50
          && local_column_box->x == subgrid_child_box->x + 56
          && local_column_box->width == 40
          && inherited_row_box->x == subgrid_child_box->x
          && inherited_row_box->y == subgrid_child_box->y + 24
          && inherited_row_box->width == 50
          && inherited_row_box->height == 30);
    CHECK(override_parent_box != NULL && override_child_box != NULL
          && override_column_box != NULL && override_row_box != NULL
          && override_child_box->x == override_parent_box->x + 36
          && override_child_box->width == 96
          && override_column_box->x == override_child_box->x + 54
          && override_column_box->width == 42
          && override_row_box->x == override_child_box->x
          && override_row_box->width == 52
          && override_row_box->y == override_child_box->y + 26
          && override_row_box->height == 28);
    CHECK(wide_box != NULL && wide_last_box != NULL
          && wide_box->width == 186
          && wide_last_box->x == wide_box->x + 165
          && wide_last_box->width == 21);
    layout_destroy(&subgrid_layout);
    stylesheet_destroy(&subgrid_sheet);
    document_destroy(&subgrid_document);
    CHECK(budget.current == 0);

    puts("test: bounded float exclusions and clearing");
    static const char float_page[] =
        "<!doctype html><style>body{margin:0;padding:0}#flow{width:240px}"
        "#left{float:left;width:60px;height:40px;margin-right:10px}"
        "#right{float:right;width:50px;height:30px;margin-left:10px}"
        "#between{display:block;height:20px}"
        "#clear-right{display:block;clear:right;height:10px}"
        "#clear-both{display:block;clear:both;height:8px}"
        "#drop{width:240px}#wide-a{float:left;width:150px;height:25px}"
        "#wide-b{float:left;width:120px;height:12px}"
        "#drop-clear{clear:both;height:8px}"
        "#zero-wrap{clear:both;width:500px;font-size:12px}"
        "#zero-a{float:left;width:10px;height:30px}"
        "#zero-b{float:left;clear:left;width:100px;height:1px}"
        "#zero-blue,#zero-purple,#zero-fuchsia{display:inline-block;"
        "width:300px;height:20px}"
        "#overflow-x{overflow-x:scroll;scrollbar-gutter:stable}"
        "#overflow-y{overflow-y:hidden}"
        "#overflow-clip{overflow-x:hidden;overflow-y:clip}"
        "</style><body><div id=flow><div id=left></div><div id=right></div>"
        "<div id=between>BETWEEN</div><div id=clear-right>CLEAR-RIGHT</div>"
        "<div id=clear-both>CLEAR-BOTH</div></div><div id=drop>"
        "<div id=wide-a></div><div id=wide-b></div>"
        "<div id=drop-clear></div></div><div id=zero-wrap>"
        "<div id=zero-a></div> <div id=zero-b></div> "
        "<span id=zero-blue></span> <span id=zero-purple></span> "
        "<span id=zero-fuchsia></span></div>"
        "<div id=overflow-x></div><div id=overflow-y></div>"
        "<div id=overflow-clip></div></body>";
    PocDocument float_document;
    Stylesheet float_sheet;
    LayoutDocument float_layout;
    CHECK(document_parse(&float_document, &budget, float_page,
                         strlen(float_page), 45)
          && stylesheet_build(&float_sheet, &budget, &float_document, 480)
          && layout_build(&float_layout, &budget, &float_document,
                          &float_sheet, NULL, NULL, 480));
    lxb_dom_node_t *float_root = lxb_dom_interface_node(float_document.html);
    lxb_dom_node_t *float_left = find_id(float_root, "left");
    lxb_dom_node_t *float_right = find_id(float_root, "right");
    lxb_dom_node_t *float_between = find_id(float_root, "between");
    lxb_dom_node_t *float_clear_right = find_id(float_root, "clear-right");
    lxb_dom_node_t *float_clear_both = find_id(float_root, "clear-both");
    lxb_dom_node_t *float_wide_a = find_id(float_root, "wide-a");
    lxb_dom_node_t *float_wide_b = find_id(float_root, "wide-b");
    lxb_dom_node_t *float_drop_clear = find_id(float_root, "drop-clear");
    lxb_dom_node_t *zero_wrap = find_id(float_root, "zero-wrap");
    lxb_dom_node_t *zero_blue = find_id(float_root, "zero-blue");
    lxb_dom_node_t *zero_purple = find_id(float_root, "zero-purple");
    lxb_dom_node_t *zero_fuchsia = find_id(float_root, "zero-fuchsia");
    lxb_dom_node_t *overflow_x = find_id(float_root, "overflow-x");
    lxb_dom_node_t *overflow_y = find_id(float_root, "overflow-y");
    lxb_dom_node_t *overflow_clip = find_id(float_root, "overflow-clip");
    const LayoutNodeBox *float_left_box = layout_box_for_node(
        &float_layout, float_left);
    const LayoutNodeBox *float_right_box = layout_box_for_node(
        &float_layout, float_right);
    const LayoutNodeBox *float_between_box = layout_box_for_node(
        &float_layout, float_between);
    const LayoutNodeBox *float_clear_right_box = layout_box_for_node(
        &float_layout, float_clear_right);
    const LayoutNodeBox *float_clear_both_box = layout_box_for_node(
        &float_layout, float_clear_both);
    const LayoutNodeBox *float_wide_a_box = layout_box_for_node(
        &float_layout, float_wide_a);
    const LayoutNodeBox *float_wide_b_box = layout_box_for_node(
        &float_layout, float_wide_b);
    const LayoutNodeBox *float_drop_clear_box = layout_box_for_node(
        &float_layout, float_drop_clear);
    const LayoutNodeBox *zero_wrap_box = layout_box_for_node(
        &float_layout, zero_wrap);
    const LayoutNodeBox *zero_blue_box = layout_box_for_node(
        &float_layout, zero_blue);
    const LayoutNodeBox *zero_purple_box = layout_box_for_node(
        &float_layout, zero_purple);
    const LayoutNodeBox *zero_fuchsia_box = layout_box_for_node(
        &float_layout, zero_fuchsia);
    ComputedStyle float_left_style = style_for_node(
        &float_sheet, float_left, NULL);
    ComputedStyle float_clear_both_style = style_for_node(
        &float_sheet, float_clear_both, NULL);
    ComputedStyle overflow_x_style = style_for_node(
        &float_sheet, overflow_x, NULL);
    ComputedStyle overflow_y_style = style_for_node(
        &float_sheet, overflow_y, NULL);
    ComputedStyle overflow_clip_style = style_for_node(
        &float_sheet, overflow_clip, NULL);
    CHECK(float_left_style.float_mode == FLOAT_LEFT
          && float_clear_both_style.clear_mode == CLEAR_BOTH
          && float_left_box != NULL && float_left_box->x == 0
          && float_left_box->y == 0 && float_left_box->width == 60
          && float_right_box != NULL && float_right_box->x == 190
          && float_right_box->y == 0 && float_right_box->width == 50
          && float_between_box != NULL && float_between_box->x == 0
          && float_between_box->y == 0 && float_between_box->width == 240
          && command_x(&float_layout, "BETWEEN") == 70
          && float_clear_right_box != NULL
          && float_clear_right_box->x == 0
          && float_clear_right_box->y == 30
          && float_clear_right_box->width == 240
          && command_x(&float_layout, "CLEAR-RIGHT") == 70
          && float_clear_both_box != NULL
          && float_clear_both_box->x == 0
          && float_clear_both_box->y == 40
          && float_clear_both_box->width == 240
          && command_x(&float_layout, "CLEAR-BOTH") == 0
          && float_wide_a_box != NULL && float_wide_b_box != NULL
          && float_wide_b_box->y
             == float_wide_a_box->y + float_wide_a_box->height
          && float_wide_b_box->x == float_wide_a_box->x
          && float_drop_clear_box != NULL
          && float_drop_clear_box->y
             == float_wide_b_box->y + float_wide_b_box->height
          && zero_wrap_box != NULL && zero_blue_box != NULL
          && zero_purple_box != NULL && zero_fuchsia_box != NULL
          && zero_blue_box->x == zero_wrap_box->x + 10
          && zero_purple_box->x == zero_wrap_box->x + 100
          && zero_fuchsia_box->x == zero_wrap_box->x
          && overflow_x_style.overflow_x_scroll
          && overflow_x_style.overflow_y_scroll
          && overflow_x_style.scrollbar_gutter_stable
          && overflow_y_style.overflow_x_scroll
          && overflow_y_style.overflow_y_scroll
          && overflow_clip_style.overflow_x_scroll
          && overflow_clip_style.overflow_y_scroll
          && overflow_clip_style.overflow_y_clip_only);
    layout_destroy(&float_layout);
    stylesheet_destroy(&float_sheet);
    document_destroy(&float_document);
    CHECK(budget.current == 0);

    puts("test: text-decoration cascade, propagation, clipping, and transform");
    static const char decoration_page[] =
        "<!doctype html><style>body{margin:0;padding:0}"
        "#removed{text-decoration:none}"
        "#plain{text-decoration-line:underline}"
        "#ancestor{text-decoration:underline;text-underline-offset:4px}"
        "#descendant{text-decoration:none;text-underline-offset:-2px}"
        "#own-decoration{text-decoration:underline;"
        "text-underline-offset:-2px}"
        "#cascaded{text-decoration:underline;text-underline-offset:2px}"
        "#cascaded{text-underline-offset:5px}"
        "#invalid-offset{text-decoration:underline;text-underline-offset:3px;"
        "text-underline-offset:25%;text-underline-offset:2em;"
        "text-underline-offset:16px}"
        "#auto-offset{text-decoration:underline;text-underline-offset:5px;"
        "text-underline-offset:auto}"
        "#zero-offset{text-decoration:underline;text-underline-offset:0}"
        "#atomic{display:inline-block}"
        "#clipped{display:block;width:24px;overflow:hidden;white-space:nowrap;"
        "text-decoration:underline}"
        "#scaled-underline{display:block;width:100px;"
        "text-decoration:underline;text-underline-offset:4px;"
        "transform:scale(.5)}"
        "</style><body><a id=removed href=/removed>REMOVED</a>"
        "<span id=plain>PLAIN</span>"
        "<span id=ancestor><span id=inherited-offset>INHERITED</span>"
        "<span id=descendant>PROPAGATED</span>"
        "<span id=own-decoration>OWNOFFSET</span>"
        "<span id=atomic>ATOMIC</span></span>"
        "<span id=cascaded>CASCADED</span>"
        "<span id=invalid-offset>INVALIDOFFSET</span>"
        "<span id=auto-offset>AUTOOFFSET</span>"
        "<span id=zero-offset>ZEROOFFSET</span>"
        "<span id=clipped>CLIPPEDLONG</span>"
        "<span id=scaled-underline>SCALEUNDER</span></body>";
    PocDocument decoration_document;
    Stylesheet decoration_sheet;
    LayoutDocument decoration_layout;
    CHECK(document_parse(&decoration_document, &budget, decoration_page,
                         strlen(decoration_page), 17)
          && stylesheet_build(&decoration_sheet, &budget,
                              &decoration_document, 120));
    lxb_dom_node_t *decoration_root = lxb_dom_interface_node(
        decoration_document.html);
    lxb_dom_node_t *removed = find_id(decoration_root, "removed");
    lxb_dom_node_t *plain = find_id(decoration_root, "plain");
    lxb_dom_node_t *ancestor = find_id(decoration_root, "ancestor");
    lxb_dom_node_t *inherited_offset = find_id(
        decoration_root, "inherited-offset");
    lxb_dom_node_t *descendant = find_id(decoration_root, "descendant");
    lxb_dom_node_t *own_decoration = find_id(
        decoration_root, "own-decoration");
    lxb_dom_node_t *atomic = find_id(decoration_root, "atomic");
    lxb_dom_node_t *cascaded = find_id(decoration_root, "cascaded");
    lxb_dom_node_t *invalid_offset = find_id(
        decoration_root, "invalid-offset");
    lxb_dom_node_t *auto_offset = find_id(
        decoration_root, "auto-offset");
    lxb_dom_node_t *zero_offset = find_id(
        decoration_root, "zero-offset");
    lxb_dom_node_t *clipped = find_id(decoration_root, "clipped");
    lxb_dom_node_t *scaled_underline = find_id(
        decoration_root, "scaled-underline");
    CHECK(removed != NULL && plain != NULL && ancestor != NULL
          && inherited_offset != NULL && descendant != NULL
          && own_decoration != NULL && atomic != NULL && cascaded != NULL
          && invalid_offset != NULL && auto_offset != NULL
          && zero_offset != NULL && clipped != NULL
          && scaled_underline != NULL);
    ComputedStyle removed_style = style_for_node(
        &decoration_sheet, removed, NULL);
    ComputedStyle plain_style = style_for_node(
        &decoration_sheet, plain, NULL);
    ComputedStyle ancestor_style = style_for_node(
        &decoration_sheet, ancestor, NULL);
    ComputedStyle inherited_offset_style = style_for_node(
        &decoration_sheet, inherited_offset, &ancestor_style);
    ComputedStyle descendant_style = style_for_node(
        &decoration_sheet, descendant, &ancestor_style);
    ComputedStyle own_decoration_style = style_for_node(
        &decoration_sheet, own_decoration, &ancestor_style);
    ComputedStyle atomic_style = style_for_node(
        &decoration_sheet, atomic, &ancestor_style);
    ComputedStyle cascaded_style = style_for_node(
        &decoration_sheet, cascaded, NULL);
    ComputedStyle invalid_offset_style = style_for_node(
        &decoration_sheet, invalid_offset, NULL);
    ComputedStyle auto_offset_style = style_for_node(
        &decoration_sheet, auto_offset, NULL);
    ComputedStyle zero_offset_style = style_for_node(
        &decoration_sheet, zero_offset, NULL);
    int inherited_pixels = 0, descendant_pixels = 0, effective_pixels = 0;
    int own_pixels = 0, cascaded_pixels = 0, invalid_pixels = 0;
    int zero_pixels = 1;
    CHECK(!computed_style_has_text_underline(&removed_style)
          && !computed_style_has_ancestor_text_underline(&removed_style)
          && computed_style_has_text_underline(&plain_style)
          && computed_style_has_text_underline(&ancestor_style)
          && computed_style_text_underline_offset(
                 &inherited_offset_style, &inherited_pixels)
          && inherited_pixels == 4
          && computed_style_effective_text_underline_offset(
                 &inherited_offset_style, &effective_pixels)
          && effective_pixels == 4
          && !computed_style_has_text_underline(&descendant_style)
          && computed_style_has_ancestor_text_underline(&descendant_style)
          && computed_style_text_underline_offset(
                 &descendant_style, &descendant_pixels)
          && descendant_pixels == -2
          && computed_style_effective_text_underline_offset(
                 &descendant_style, &effective_pixels)
          && effective_pixels == 4
          && computed_style_has_text_underline(&own_decoration_style)
          && computed_style_effective_text_underline_offset(
                 &own_decoration_style, &own_pixels)
          && own_pixels == -2
          && !computed_style_has_text_underline(&atomic_style)
          && !computed_style_has_ancestor_text_underline(&atomic_style)
          && computed_style_text_underline_offset(
                 &cascaded_style, &cascaded_pixels)
          && cascaded_pixels == 5
          && computed_style_text_underline_offset(
                 &invalid_offset_style, &invalid_pixels)
          && invalid_pixels == 3
          && !computed_style_text_underline_offset(
                  &auto_offset_style, NULL)
          && computed_style_text_underline_offset(
                 &zero_offset_style, &zero_pixels)
          && zero_pixels == 0
          && sizeof(ComputedStyle) == 376
          && layout_build(&decoration_layout, &budget,
                          &decoration_document, &decoration_sheet,
                          NULL, NULL, 120));
    const DrawCommand *removed_command = text_command(
        &decoration_layout, "REMOVED");
    const DrawCommand *plain_command = text_command(
        &decoration_layout, "PLAIN");
    const DrawCommand *propagated_command = text_command(
        &decoration_layout, "PROPAGATED");
    const DrawCommand *own_decoration_command = text_command(
        &decoration_layout, "OWNOFFSET");
    const DrawCommand *atomic_command = text_command(
        &decoration_layout, "ATOMIC");
    const DrawCommand *clipped_command = text_command(
        &decoration_layout, "CLIPPEDLONG");
    const DrawCommand *scaled_underline_command = text_command(
        &decoration_layout, "SCALEUNDER");
    const LayoutNodeBox *clipped_box = layout_box_for_node(
        &decoration_layout, clipped);
    int command_offset = 0;
    CHECK(removed_command != NULL
          && (removed_command->image_fit
              & LAYOUT_TEXT_DECORATION_UNDERLINE) == 0
          && plain_command != NULL
          && (plain_command->image_fit
              & LAYOUT_TEXT_DECORATION_UNDERLINE) != 0
          && propagated_command != NULL
          && (propagated_command->image_fit
              & LAYOUT_TEXT_DECORATION_UNDERLINE) != 0
          && draw_command_text_underline_offset(
                 propagated_command, &command_offset)
          && command_offset == 4
          && own_decoration_command != NULL
          && draw_command_text_underline_offset(
                 own_decoration_command, &command_offset)
          && command_offset == -2
          && atomic_command != NULL
          && (atomic_command->image_fit
              & LAYOUT_TEXT_DECORATION_UNDERLINE) == 0
          && clipped_command != NULL && clipped_box != NULL
          && clipped_box->clips_x
          && clipped_command->width > clipped_box->client_width
          && (clipped_command->image_fit
              & LAYOUT_TEXT_DECORATION_UNDERLINE) != 0
          && scaled_underline_command != NULL
          && scaled_underline_command->font_size == 8
          && (scaled_underline_command->image_fit
              & LAYOUT_TEXT_DECORATION_UNDERLINE) != 0
          && draw_command_text_underline_offset(
                 scaled_underline_command, &command_offset)
          && command_offset == 2);
    layout_destroy(&decoration_layout);
    stylesheet_destroy(&decoration_sheet);
    document_destroy(&decoration_document);
    CHECK(budget.current == 0);

    puts("test: stacking, opacity, and translated geometry");
    static const char compositing_page[] =
        "<!doctype html><style>body{margin:0;padding:0;background:#ffffff}"
        "#behind,#low,#high{position:absolute;left:10px;top:10px;width:40px;"
        "height:40px}#low{background:#0000ff;z-index:1}"
        "#behind{background:#00ff00;z-index:-2}"
        "#high{background:#ff0000;z-index:2;opacity:.5;"
        "transform:translate(5px,6px)}"
        "#scaled{position:absolute;left:60px;top:10px;width:50px;height:50px;"
        "background:#00ff00;transform:scale(.4)}"
        "#origin{position:absolute;left:60px;top:70px;width:20px;height:20px;"
        "background:#0000ff;transform:scale(.5);transform-origin:left top}"
        "body #origin{background-origin:content-box}"
        "#origin-offset{display:none;transform-origin:left 10px}"
        "#phase-positioned{position:absolute;left:0;top:60px;width:10px;"
        "height:10px;background:LiMeGrEeN}"
        "#phase-flow{margin-top:60px;width:10px;height:10px;"
        "background:#0000ff}</style><body>"
        "<div id=behind></div><div id=low></div><div id=high></div>"
        "<div id=scaled></div><div id=origin></div>"
        "<div id=origin-offset></div>"
        "<div id=phase-positioned></div>"
        "<div id=phase-flow></div></body>";
    PocDocument compositing_document;
    Stylesheet compositing_sheet;
    LayoutDocument compositing_layout;
    CHECK(document_parse(&compositing_document, &budget, compositing_page,
                         strlen(compositing_page), 17)
          && stylesheet_build(&compositing_sheet, &budget,
                              &compositing_document, 100));
    lxb_dom_node_t *high = find_id(
        lxb_dom_interface_node(compositing_document.html), "high");
    lxb_dom_node_t *scaled = find_id(
        lxb_dom_interface_node(compositing_document.html), "scaled");
    lxb_dom_node_t *origin = find_id(
        lxb_dom_interface_node(compositing_document.html), "origin");
    lxb_dom_node_t *origin_offset = find_id(
        lxb_dom_interface_node(compositing_document.html), "origin-offset");
    CHECK(high != NULL && scaled != NULL && origin != NULL
          && origin_offset != NULL);
    ComputedStyle high_style = style_for_node(&compositing_sheet, high, NULL);
    ComputedStyle scaled_style = style_for_node(
        &compositing_sheet, scaled, NULL);
    ComputedStyle origin_style = style_for_node(
        &compositing_sheet, origin, NULL);
    ComputedStyle origin_offset_style = style_for_node(
        &compositing_sheet, origin_offset, NULL);
    const StylePaintStack *origin_paint = stylesheet_paint_stack(
        &compositing_sheet,
        computed_style_paint_stack_id(&origin_style));
    const StylePaintStack *origin_offset_paint = stylesheet_paint_stack(
        &compositing_sheet,
        computed_style_paint_stack_id(&origin_offset_style));
    CHECK(high_style.has_z_index && high_style.z_index == 2
          && high_style.opacity >= 127 && high_style.opacity <= 128
          && high_style.has_transform && high_style.transform_x == 5
          && high_style.transform_y == 6
          && scaled_style.has_transform
          && scaled_style.transform_scale_q6 == 26
          && origin_paint != NULL
          && (origin_paint->components
              & STYLE_PAINT_COMPONENT_TRANSFORM_ORIGIN) != 0
          && (origin_paint->components
              & STYLE_PAINT_COMPONENT_BACKGROUND_BOX) != 0
          && style_object_position_percent(
                 origin_paint->transform_origin_x) == 0
          && style_object_position_percent(
                 origin_paint->transform_origin_y) == 0
          && origin_offset_paint != NULL
          && style_object_position_percent(
                 origin_offset_paint->transform_origin_x) == 0
          && style_object_position_offset(
                 origin_offset_paint->transform_origin_x) == 0
          && style_object_position_percent(
                 origin_offset_paint->transform_origin_y) == 0
          && style_object_position_offset(
                 origin_offset_paint->transform_origin_y) == 10);
    CHECK(layout_build(&compositing_layout, &budget, &compositing_document,
                       &compositing_sheet, NULL, NULL, 100));
    const LayoutNodeBox *high_box = layout_box_for_node(&compositing_layout,
                                                        high);
    const LayoutNodeBox *scaled_box = layout_box_for_node(
        &compositing_layout, scaled);
    const LayoutNodeBox *origin_box = layout_box_for_node(
        &compositing_layout, origin);
    CHECK(high_box != NULL && high_box->x == 15 && high_box->y == 16
          && scaled_box != NULL && scaled_box->x == 75
          && scaled_box->y == 25 && scaled_box->width == 20
          && scaled_box->height == 20
          && origin_box != NULL && origin_box->x == 60
          && origin_box->y == 70 && origin_box->width == 10
          && origin_box->height == 10
          && compositing_layout.paint_order_count
             == compositing_layout.count);
    /* Nested stacking contexts are atomic, so their internal z order is not
       globally monotonic.  The overlap pixels below assert the observable
       ordering rather than the obsolete flattened implementation detail. */
    TileCache compositing_cache;
    uint16_t *compositing_frame = budget_malloc(
        &budget, 100u * 100u * sizeof(*compositing_frame));
    CHECK(compositing_frame != NULL
          && tile_cache_init(&compositing_cache, &budget,
                             &compositing_layout, 2)
          && tile_cache_set_frame(&compositing_cache, compositing_frame,
                                  100u * 100u)
          && tile_cache_render_frame(&compositing_cache, 0, 100, 100, NULL));
    CHECK(compositing_frame[11u * 100u + 11u] == test_rgb565(0x0000ff));
    CHECK(compositing_frame[61u * 100u + 1u]
          == test_rgb565(0x32cd32));
    uint16_t blended = compositing_frame[17u * 100u + 16u];
    unsigned blended_red = (blended >> 11) & 31u;
    unsigned blended_green = (blended >> 5) & 63u;
    unsigned blended_blue = blended & 31u;
    CHECK(blended_red >= 14 && blended_red <= 17
          && blended_green == 0
          && blended_blue >= 14 && blended_blue <= 17);
    tile_cache_destroy(&compositing_cache);
    budget_free(&budget, compositing_frame);
    layout_destroy(&compositing_layout);
    stylesheet_destroy(&compositing_sheet);
    document_destroy(&compositing_document);
    CHECK(budget.current == 0);

    puts("test: ordered flex siblings retain absolute children");
    static const char ordered_flex_absolute_page[] =
        "<!doctype html><style>body{margin:0}"
        "#flex-host{display:flex;position:relative;width:100px;height:40px}"
        "#flow-order{order:4;width:30px;height:10px}"
        "#absolute-order{position:absolute;order:-3;left:7px;top:5px;"
        "width:20px;height:11px}</style><body><div id=flex-host>"
        "<i id=flow-order>FLOW</i><b id=absolute-order>ABS</b>"
        "</div></body>";
    PocDocument ordered_flex_absolute_document;
    Stylesheet ordered_flex_absolute_sheet;
    LayoutDocument ordered_flex_absolute_layout;
    CHECK(document_parse(
              &ordered_flex_absolute_document, &budget,
              ordered_flex_absolute_page,
              strlen(ordered_flex_absolute_page), 42)
          && stylesheet_build(
              &ordered_flex_absolute_sheet, &budget,
              &ordered_flex_absolute_document, 100)
          && layout_build(
              &ordered_flex_absolute_layout, &budget,
              &ordered_flex_absolute_document,
              &ordered_flex_absolute_sheet, NULL, NULL, 100));
    lxb_dom_node_t *ordered_flex_absolute_root =
        lxb_dom_interface_node(ordered_flex_absolute_document.html);
    const LayoutNodeBox *ordered_flex_absolute_box = layout_box_for_node(
        &ordered_flex_absolute_layout,
        find_id(ordered_flex_absolute_root, "absolute-order"));
    CHECK(ordered_flex_absolute_box != NULL
          && ordered_flex_absolute_box->x == 7
          && ordered_flex_absolute_box->y == 5
          && ordered_flex_absolute_box->width == 20
          && ordered_flex_absolute_box->height == 11);
    layout_destroy(&ordered_flex_absolute_layout);
    stylesheet_destroy(&ordered_flex_absolute_sheet);
    document_destroy(&ordered_flex_absolute_document);
    CHECK(budget.current == 0);

    puts("test: mixed calc insets and relative generated inline flow");
    static const char inset_generated_page[] =
        "<!doctype html><style>body{margin:0}"
        "#host{position:relative;width:480px;height:20px}"
        "#logo{position:absolute;left:calc(50% - 60px);top:0;"
        "width:120px;height:10px}"
        "#label::after{content:'V';position:relative;top:2px;"
        "padding-left:7px}</style><body><div id=host>"
        "<span id=label>Discover</span><i id=logo></i></div></body>";
    PocDocument inset_generated_document;
    Stylesheet inset_generated_sheet;
    LayoutDocument inset_generated_layout;
    CHECK(document_parse(
              &inset_generated_document, &budget, inset_generated_page,
              strlen(inset_generated_page), 44)
          && stylesheet_build(
              &inset_generated_sheet, &budget, &inset_generated_document,
              480)
          && layout_build(
              &inset_generated_layout, &budget, &inset_generated_document,
              &inset_generated_sheet, NULL, NULL, 480));
    lxb_dom_node_t *inset_generated_root =
        lxb_dom_interface_node(inset_generated_document.html);
    const LayoutNodeBox *inset_logo_box = layout_box_for_node(
        &inset_generated_layout, find_id(inset_generated_root, "logo"));
    const DrawCommand *inset_label_command = text_command(
        &inset_generated_layout, "Discover");
    const DrawCommand *inset_chevron_command = text_command(
        &inset_generated_layout, "V");
    CHECK(inset_logo_box != NULL && inset_logo_box->x == 180
          && inset_label_command != NULL && inset_chevron_command != NULL
          && inset_chevron_command->x
             >= inset_label_command->x + inset_label_command->width + 7
          && inset_chevron_command->y == inset_label_command->y + 2);
    layout_destroy(&inset_generated_layout);
    stylesheet_destroy(&inset_generated_sheet);
    document_destroy(&inset_generated_document);
    CHECK(budget.current == 0);

    puts("test: transforms apply to generated pseudo paint spans");
    static const char transformed_pseudo_page[] =
        "<!doctype html><style>body{margin:0}#host{position:relative;"
        "display:block;width:20px;height:60px}#host::after{content:'';"
        "position:absolute;left:0;top:0;width:10px;height:10px;"
        "background:#010203;transform:translateY(40px)}"
        "</style><body><div id=host></div></body>";
    PocDocument transformed_pseudo_document;
    Stylesheet transformed_pseudo_sheet;
    LayoutDocument transformed_pseudo_layout;
    CHECK(document_parse(
              &transformed_pseudo_document, &budget,
              transformed_pseudo_page, strlen(transformed_pseudo_page), 45)
          && stylesheet_build(
              &transformed_pseudo_sheet, &budget,
              &transformed_pseudo_document, 100)
          && layout_build(
              &transformed_pseudo_layout, &budget,
              &transformed_pseudo_document,
              &transformed_pseudo_sheet, NULL, NULL, 100));
    const DrawCommand *transformed_pseudo_fill = NULL;
    for (size_t i = 0; i < transformed_pseudo_layout.count; i++) {
        const DrawCommand *command = &transformed_pseudo_layout.commands[i];
        if (command->type == DRAW_FILL_RECT && command->color == 0x010203
            && command->width == 10 && command->height == 10) {
            transformed_pseudo_fill = command;
            break;
        }
    }
    CHECK(transformed_pseudo_fill != NULL
          && transformed_pseudo_fill->x == 0
          && transformed_pseudo_fill->y == 40);
    layout_destroy(&transformed_pseudo_layout);
    stylesheet_destroy(&transformed_pseudo_sheet);
    document_destroy(&transformed_pseudo_document);
    CHECK(budget.current == 0);

    puts("test: bounded scroll interaction metadata and snap settling");
    static const char scroll_interaction_page[] =
        "<!doctype html><style>body{margin:0}"
        "#scroller{display:block;width:120px;height:80px;overflow:auto;"
        "scroll-snap-type:y mandatory;scroll-padding-top:1px;"
        "scroll-padding:10px;scroll-padding-right:14px;"
        "overscroll-behavior-y:auto;overscroll-behavior:contain;"
        "overscroll-behavior-x:auto;scrollbar-width:thin}"
        "#scroller>div{display:block;height:60px}"
        "#target{scroll-snap-align:start;scroll-margin-top:1px;"
        "scroll-margin:5px;scroll-margin-right:7px;"
        "scroll-snap-stop:always;cursor:pointer}"
        "</style><body><div id=scroller><div></div>"
        "<div id=target>TARGET</div><div></div></div></body>";
    PocDocument scroll_interaction_document;
    Stylesheet scroll_interaction_sheet;
    LayoutDocument scroll_interaction_layout;
    CHECK(document_parse(
              &scroll_interaction_document, &budget,
              scroll_interaction_page, strlen(scroll_interaction_page), 43)
          && stylesheet_build(
              &scroll_interaction_sheet, &budget,
              &scroll_interaction_document, 160)
          && layout_build(
              &scroll_interaction_layout, &budget,
              &scroll_interaction_document,
              &scroll_interaction_sheet, NULL, NULL, 160));
    lxb_dom_node_t *scroll_interaction_root =
        lxb_dom_interface_node(scroll_interaction_document.html);
    lxb_dom_node_t *scroll_scroller =
        find_id(scroll_interaction_root, "scroller");
    lxb_dom_node_t *scroll_target =
        find_id(scroll_interaction_root, "target");
    const LayoutNodeBox *scroll_scroller_box = layout_box_for_node(
        &scroll_interaction_layout, scroll_scroller);
    int remaining_x = 0, remaining_y = 0;
    CHECK(scroll_scroller_box != NULL
          && scroll_interaction_sheet.has_scroll_interaction_rules
          && scroll_interaction_sheet.has_cursor_rules
          && scroll_interaction_layout.scroll_container_count == 1
          && scroll_interaction_layout.scroll_snap_candidate_count == 1
          && !scroll_interaction_layout.scroll_metadata_truncated);
    CHECK(scroll_interaction_layout.scroll_containers[0].padding_top == 10
          && scroll_interaction_layout.scroll_containers[0].padding_right
             == 14);
    CHECK(scroll_interaction_layout.scroll_snap_candidates[0].margin_top == 5
          && scroll_interaction_layout.scroll_snap_candidates[0].margin_right
             == 7);
    CHECK(scroll_scroller_box != NULL
          && layout_cursor_for_node(
                 &scroll_interaction_sheet, scroll_target)
                 == LAYOUT_CURSOR_POINTER
          && layout_scroll_node_reveal(
                 &scroll_interaction_layout, scroll_target));
    CHECK(scroll_scroller_box->scroll_y == 55
          && layout_scroll_node(
                 &scroll_interaction_layout, scroll_scroller, 0, 0)
          && layout_scroll_node_chain(
                 &scroll_interaction_layout, scroll_target,
                 0, 70, NULL, &remaining_y)
          && remaining_y == 0
          && scroll_scroller_box->scroll_y == 70
          && layout_scroll_node_settle(
                 &scroll_interaction_layout, scroll_target)
          && scroll_scroller_box->scroll_y == 45);
    CHECK(layout_scroll_node_chain(
              &scroll_interaction_layout, scroll_target,
              40, 100, &remaining_x, &remaining_y)
          && remaining_x == 40 && remaining_y == 0);
    layout_destroy(&scroll_interaction_layout);
    stylesheet_destroy(&scroll_interaction_sheet);
    document_destroy(&scroll_interaction_document);
    CHECK(budget.current == 0);
