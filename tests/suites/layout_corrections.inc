/* Regressions for layout corrections found in review (relative inlines,
   logical/unsafe content alignment after stretching, RTL grid items,
   track and table bookkeeping). Each page pins geometry that differed
   before its fix. */

#define BOX_ANY INT_MIN
/* A negative expected width or height -N means "at least N". */

typedef struct {
    const char *id;
    int x;
    int y;
    int width;
    int height;
} CorrectionBox;

static int check_correction_page(const char *label, const char *page,
                                 int viewport, const CorrectionBox *expected,
                                 size_t expected_count)
{
    Budget budget;
    budget_init(&budget, 16u * MIB);
    CHECK(budget_install_lexbor(&budget));
    PocDocument document = {0};
    Stylesheet sheet = {0};
    LayoutDocument layout = {0};
    CHECK(document_parse(&document, &budget, page, strlen(page), 17)
          && stylesheet_build(&sheet, &budget, &document, viewport)
          && layout_build(&layout, &budget, &document, &sheet, NULL, NULL,
                          viewport));
    lxb_dom_node_t *root = lxb_dom_interface_node(document.html);
    int failures = 0;
    for (size_t i = 0; i < expected_count; i++) {
        const CorrectionBox *want = &expected[i];
        const LayoutNodeBox *box = layout_box_for_node(
            &layout, find_id(root, want->id));
        if (box == NULL
            || (want->x != BOX_ANY && box->x != want->x)
            || (want->y != BOX_ANY && box->y != want->y)
            || (want->width != BOX_ANY && want->width >= 0
                && box->width != want->width)
            || (want->width != BOX_ANY && want->width < 0
                && box->width < -want->width)
            || (want->height != BOX_ANY && want->height >= 0
                && box->height != want->height)
            || (want->height != BOX_ANY && want->height < 0
                && box->height < -want->height)) {
            fprintf(stderr, "%s: %s got %d,%d %dx%d, want %d,%d %dx%d\n",
                    label, want->id, box ? box->x : -1, box ? box->y : -1,
                    box ? box->width : -1, box ? box->height : -1,
                    want->x, want->y, want->width, want->height);
            failures++;
        }
    }
    layout_destroy(&layout);
    stylesheet_destroy(&sheet);
    document_destroy(&document);
    CHECK(budget.current == 0u && budget_uninstall_lexbor(&budget));
    CHECK(failures == 0);
    return 0;
}

#define CHECK_CORRECTION(label, page, viewport, ...) do { \
        const CorrectionBox expected_[] = {__VA_ARGS__}; \
        CHECK(check_correction_page(label, page, viewport, expected_, \
            sizeof(expected_) / sizeof(expected_[0])) == 0); \
    } while (0)

/* A column flex container stretched by its grid row re-aligns its items
   with the same logical justify-content it was laid out with. */
static int test_stretched_column_justify(void)
{
    static const char *const values[] = {
        "end", "flex-end", "unsafe center", "center", "safe center"
    };
    static const int item_y[] = {180, 180, 90, 90, 90};
    for (size_t v = 0; v < sizeof(values) / sizeof(values[0]); v++) {
        char page[512];
        snprintf(page, sizeof(page),
            "<!doctype html><style>body{margin:0}"
            ".g{display:grid;grid-template-columns:100px 100px}"
            ".c{display:flex;flex-direction:column;justify-content:%s}"
            "#a{display:block;height:20px}</style>"
            "<div class=g><div class=c id=c><span id=a></span></div>"
            "<div style=\"height:200px\"></div></div>", values[v]);
        CHECK_CORRECTION(values[v], page, 480,
            {"c", 0, 0, 100, 200}, {"a", 0, item_y[v], 100, 20});
    }
    /* In column-reverse, `end` is still the block end. */
    CHECK_CORRECTION("column-reverse end",
        "<!doctype html><style>body{margin:0}"
        ".g{display:grid;grid-template-columns:100px 100px}"
        ".c{display:flex;flex-direction:column-reverse;justify-content:end}"
        "#a{display:block;height:20px}</style>"
        "<div class=g><div class=c id=c><span id=a></span></div>"
        "<div style=\"height:200px\"></div></div>", 480,
        {"a", 0, 180, 100, 20});
    CHECK_CORRECTION("column-reverse start",
        "<!doctype html><style>body{margin:0}"
        ".g{display:grid;grid-template-columns:100px 100px}"
        ".c{display:flex;flex-direction:column-reverse;justify-content:start}"
        "#a{display:block;height:20px}</style>"
        "<div class=g><div class=c id=c><span id=a></span></div>"
        "<div style=\"height:200px\"></div></div>", 480,
        {"a", 0, 0, 100, 20});
    return 0;
}

/* Auto rows in a definite-height grid are aligned once their heights are
   known; unsafe center centers the completed rows and may overflow. */
static int test_grid_unsafe_center_auto_rows(void)
{
    CHECK_CORRECTION("unsafe center auto row",
        "<!doctype html><style>body{margin:0}"
        ".g{display:grid;height:200px;align-content:unsafe center}"
        "#a{display:block;height:20px}</style>"
        "<div class=g><span id=a></span></div>", 480,
        {"a", 0, 90, 480, 20});
    CHECK_CORRECTION("unsafe center overflowing auto row",
        "<!doctype html><style>body{margin:0}"
        ".g{display:grid;height:20px;margin-top:100px;"
        "align-content:unsafe center}"
        "#a{display:block;height:60px}</style>"
        "<div class=g><span id=a></span></div>", 480,
        {"a", 0, 80, 480, 60});
    CHECK_CORRECTION("safe center overflowing auto row",
        "<!doctype html><style>body{margin:0}"
        ".g{display:grid;height:20px;margin-top:100px;align-content:center}"
        "#a{display:block;height:60px}</style>"
        "<div class=g><span id=a></span></div>", 480,
        {"a", 0, 100, 480, 60});
    return 0;
}

/* Relative positioning moves an inline's fragments after line layout; it
   never grows the line box (CSS 2.1 9.4.3). */
static int test_relative_inline_line_height(void)
{
    static const char *const offsets[] = {
        "", "top:10px", "bottom:6px", "top:2px;left:5px"
    };
    static const int dy[] = {0, 10, -6, 2};
    int base_y = 0, base_x = 0;
    for (size_t v = 0; v < sizeof(offsets) / sizeof(offsets[0]); v++) {
        char page[640];
        snprintf(page, sizeof(page),
            "<!doctype html><style>body{margin:0}"
            "p{margin:0;font-size:16px;line-height:20px}"
            "#r{position:relative;%s}#after{display:block;height:5px}"
            "</style><p id=p>text <span id=r>x</span> more</p>"
            "<p>second <span style=\"position:relative;%s\">y</span></p>"
            "<span id=after></span>", offsets[v], offsets[v]);
        Budget budget;
        budget_init(&budget, 4u * MIB);
        CHECK(budget_install_lexbor(&budget));
        PocDocument document = {0};
        Stylesheet sheet = {0};
        LayoutDocument layout = {0};
        CHECK(document_parse(&document, &budget, page, strlen(page), 17)
              && stylesheet_build(&sheet, &budget, &document, 480)
              && layout_build(&layout, &budget, &document, &sheet, NULL, NULL,
                              480));
        lxb_dom_node_t *root = lxb_dom_interface_node(document.html);
        const LayoutNodeBox *p = layout_box_for_node(&layout, find_id(root, "p"));
        const LayoutNodeBox *r = layout_box_for_node(&layout, find_id(root, "r"));
        const LayoutNodeBox *after = layout_box_for_node(&layout,
            find_id(root, "after"));
        if (v == 0 && r != NULL) {
            base_y = r->y;
            base_x = r->x;
        }
        if (p == NULL || r == NULL || after == NULL || p->height != 20
            || after->y != 40 || r->y != base_y + dy[v]
            || r->x != base_x + (v == 3 ? 5 : 0)) {
            fprintf(stderr, "relative inline %s: p h%d r %d,%d after y%d\n",
                    offsets[v], p ? p->height : -1, r ? r->x : -1,
                    r ? r->y : -1, after ? after->y : -1);
            CHECK(false);
        }
        layout_destroy(&layout);
        stylesheet_destroy(&sheet);
        document_destroy(&document);
        CHECK(budget.current == 0u && budget_uninstall_lexbor(&budget));
    }
    return 0;
}

/* An RTL grid mirrors its tracks and aligns items to the right of their
   areas: start (and the start fallback of stretch) is the right edge. */
static int test_rtl_grid_item_alignment(void)
{
    CHECK_CORRECTION("rtl grid items",
        "<!doctype html><style>body{margin:0}"
        ".g{display:grid;direction:rtl;width:200px;"
        "grid-template-columns:100px 100px}"
        ".g>span{display:block;width:20px;height:5px}</style>"
        "<div class=g><span id=a></span><span id=b style=\"justify-self:end\">"
        "</span><span id=c style=\"justify-self:center\"></span>"
        "<span id=d style=\"justify-self:start;margin-right:4px\"></span>"
        "<span id=e style=\"width:auto\"></span></div>", 480,
        {"a", 180, 0, 20, 5}, {"b", 0, 0, 20, 5}, {"c", 140, 5, 20, 5},
        {"d", 76, 5, 20, 5}, {"e", 100, 10, 100, 5});
    CHECK_CORRECTION("ltr grid items",
        "<!doctype html><style>body{margin:0}"
        ".g{display:grid;width:200px;grid-template-columns:100px 100px}"
        ".g>span{display:block;width:20px;height:5px}</style>"
        "<div class=g><span id=a></span><span id=b style=\"justify-self:end\">"
        "</span></div>", 480,
        {"a", 0, 0, 20, 5}, {"b", 180, 0, 20, 5});
    return 0;
}

/* Pixel grid floors stay pixel floors: lengths saturate at 32767 px when
   parsed, below the percentage-floor tag bit (0x10000) that shares the
   retained minimum. */
static int test_grid_large_pixel_floor(void)
{
    CHECK_CORRECTION("70000px grid floor",
        "<!doctype html><style>body{margin:0}"
        ".g{display:grid;width:300px;grid-template-columns:minmax(70000px,1fr) 10px}"
        "#a{display:block;height:5px}</style>"
        "<div class=g><span id=a></span></div>", 480,
        {"a", 0, 0, -30000, 5});
    CHECK_CORRECTION("70000px fixed track",
        "<!doctype html><style>body{margin:0}"
        ".g{display:grid;width:300px;grid-template-columns:70000px 10px}"
        "#a{display:block;height:5px}</style>"
        "<div class=g><span id=a></span></div>", 480,
        {"a", 0, 0, -30000, 5});
    return 0;
}

/* An auto-fit grid capped at the track limit counts only the collapsed
   tracks it kept; the visible tracks and their gaps fill the container. */
static int test_auto_fit_collapse_at_track_cap(void)
{
    CHECK_CORRECTION("auto-fit collapse at cap",
        "<!doctype html><style>body{margin:0}"
        ".g{display:grid;width:480px;gap:10px;"
        "grid-template-columns:repeat(auto-fit,minmax(10px,1fr))}"
        ".g>span{display:block;height:5px}</style>"
        "<div class=g><span id=a style=\"grid-column:-26\"></span>"
        "<span id=b style=\"grid-column:1;grid-row:1\"></span></div>", 480,
        {"a", 0, 0, BOX_ANY, 5}, {"b", BOX_ANY, 0, 470, 5});
    return 0;
}

/* A numbered line name counts an area's implicit <area>-start line among
   the explicitly named lines, in line order. */
static int test_grid_numbered_line_names(void)
{
    CHECK_CORRECTION("numbered area lines",
        "<!doctype html><style>body{margin:0}"
        ".g{display:grid;width:200px;grid-template-areas:\"x . . .\";"
        "grid-template-columns:50px [x-start] 50px 50px [x-start] 50px}"
        ".g>span{display:block;height:5px;grid-row:1}</style>"
        "<div class=g><span id=a style=\"grid-column:x-start 2/span 1\"></span>"
        "<span id=b style=\"grid-column:x-start 3/span 1\"></span>"
        "<span id=c style=\"grid-column:x-start/span 1\"></span></div>", 480,
        {"a", 50, 0, 50, 5}, {"b", 150, 0, 50, 5}, {"c", 0, 0, 50, 5});
    CHECK_CORRECTION("numbered lines outside explicit grid",
        "<!doctype html><style>body{margin:0}"
        ".g{display:grid;width:200px;grid-template-columns:[a] 50px [a] 50px;"
        "grid-auto-columns:20px;grid-template-rows:5px}"
        ".g>span{display:block;height:5px;grid-row:1}</style>"
        "<div class=g><span id=a style=\"grid-column:a 3/span 1\"></span>"
        "<span id=b style=\"grid-column:-3 a/span 1\"></span>"
        "<span id=c style=\"grid-column:missing 1/span 1\"></span></div>", 480,
        {"a", 140, 0, 20, 5}, {"b", 0, 0, 20, 5}, {"c", 140, 0, 20, 5});
    return 0;
}

/* A rowspan counts rendered rows (CSS 2.1 17.2.1: a display:none row is
   not part of the table), matching the table's own cell placement. */
static int test_table_rowspan_rendered_rows(void)
{
    CHECK_CORRECTION("rowspan over hidden row",
        "<!doctype html><style>body{margin:0}"
        "table{border-spacing:0}td{padding:0}div{height:10px}</style>"
        "<table><tr id=r1><td rowspan=2 id=a><div></div></td>"
        "<td><div style=\"height:20px\"></div></td></tr>"
        "<tr style=\"display:none\"><td><div style=\"height:50px\"></div></td></tr>"
        "<tr id=r3><td><div style=\"height:30px\"></div></td></tr>"
        "<tr id=r4><td><div style=\"height:40px\"></div></td></tr></table>", 480,
        {"r1", BOX_ANY, 0, BOX_ANY, 20}, {"r3", BOX_ANY, 20, BOX_ANY, 30},
        {"a", BOX_ANY, 0, BOX_ANY, 50});
    return 0;
}

/* A rowspan cell the table did not place (past its placement bound) is
   still stretched by its own row rather than left unaligned. */
static int test_table_unplaced_rowspan_cell(void)
{
    static const char head[] = "<!doctype html><style>body{margin:0}"
        "table{border-spacing:0}td{padding:0}</style><table>";
    static const char filler[] = "<tr><td>.</td><td>.</td><td>.</td></tr>";
    static const char tail[] = "<tr><td rowspan=2 id=a><div style=\"height:10px\">"
        "</div></td><td id=b><div style=\"height:30px\"></div></td></tr>"
        "<tr><td><div style=\"height:20px\"></div></td></tr></table>";
    size_t rows = 171, length = sizeof(head) + rows * (sizeof(filler) - 1)
        + sizeof(tail);
    char *page = malloc(length);
    CHECK(page != NULL);
    char *at = page;
    memcpy(at, head, sizeof(head) - 1);
    at += sizeof(head) - 1;
    for (size_t i = 0; i < rows; i++) {
        memcpy(at, filler, sizeof(filler) - 1);
        at += sizeof(filler) - 1;
    }
    memcpy(at, tail, sizeof(tail));
    int result = check_correction_page("unplaced rowspan cell", page, 480,
        (const CorrectionBox[]) {{"a", BOX_ANY, BOX_ANY, BOX_ANY, -30}}, 1);
    free(page);
    CHECK(result == 0);
    return 0;
}

/* @supports accepts var() values for sparse (retained) properties, such
   as clip-path, like every other implemented property. */
static int test_supports_sparse_var(void)
{
    CHECK_CORRECTION("@supports sparse var()",
        "<!doctype html><style>body{margin:0}span{display:block;height:1px}"
        "@supports (clip-path: var(--x)){#a{height:7px}}"
        "@supports (user-select: var(--y, none)){#b{height:8px}}"
        "@supports (clip-path: inset(1px)){#c{height:9px}}"
        "@supports (clip-path: bogus){#d{height:10px}}</style>"
        "<span id=a></span><span id=b></span><span id=c></span>"
        "<span id=d></span>", 480,
        {"a", 0, BOX_ANY, BOX_ANY, 7}, {"b", 0, BOX_ANY, BOX_ANY, 8},
        {"c", 0, BOX_ANY, BOX_ANY, 9}, {"d", 0, BOX_ANY, BOX_ANY, 1});
    return 0;
}

/* Content-keyword flex bases in a definite-height column measure the
   item's content; they are not zero-length bases. */
static int test_column_content_flex_basis(void)
{
    static const char *const bases[] = {"max-content", "min-content", "fit-content"};
    for (size_t v = 0; v < sizeof(bases) / sizeof(bases[0]); v++) {
        char page[512];
        snprintf(page, sizeof(page),
            "<!doctype html><style>body{margin:0}"
            ".c{display:flex;flex-direction:column;height:200px}"
            "#a{flex:0 0 %s;min-height:0}#b{flex:1;min-height:0}"
            "i{display:block;height:20px}</style>"
            "<div class=c><div id=a><i></i></div><div id=b></div></div>",
            bases[v]);
        CHECK_CORRECTION(bases[v], page, 480,
            {"a", 0, 0, 480, 20}, {"b", 0, 20, 480, 180});
    }
    return 0;
}

/* A bare fr track is minmax(auto, fr): a single-span, non-scrollable item
   floors it at its min-content width, so words wider than the equal share
   overflow the grid instead of being squeezed (the regression suites opt
   out with min-width:0 or minmax(0,1fr) to pin remainder distribution).
   An absolutely positioned item with only a start line spans to the
   containing block's padding edge. */
static int test_fr_automatic_minimums(void)
{
    CHECK_CORRECTION("fr minimum: block grid",
        "<!doctype html><style>body{margin:0}"
        ".g{display:grid;grid-template-columns:repeat(3,1fr);gap:2px;"
        "width:101px}.g>span{display:block;height:5px}"
        ".z>span{min-width:0}</style><body>"
        "<div class=g><span id=a>GRA</span><span id=b>GRB</span>"
        "<span id=c>GRC</span></div>"
        "<div class='g z'><span id=za>GRA</span><span id=zb>GRB</span>"
        "<span id=zc>GRC</span></div>", 480,
        {"a", 0, 0, 36, 5}, {"b", 38, 0, 36, 5}, {"c", 76, 0, 36, 5},
        {"za", 0, 5, 33, 5}, {"zb", 35, 5, 32, 5}, {"zc", 69, 5, 32, 5});
    CHECK_CORRECTION("fr minimum: inline grid",
        "<!doctype html><style>body{margin:0}"
        ".i{display:inline-grid;width:62px;grid-template-columns:repeat(2,1fr);"
        "gap:2px}.i>b{display:block;height:14px}</style><body>"
        "<div style=width:460px><span id=g class=i><b id=a>IGA</b>"
        "<b id=b>IGB</b></span></div>", 480,
        {"g", 0, 0, 62, 14}, {"a", 0, 0, 36, 14}, {"b", 38, 0, 36, 14});
    CHECK_CORRECTION("abspos grid start line only",
        "<!doctype html><style>html,body{margin:0;padding:0}"
        "#fill{display:grid;position:relative;margin-left:20px;"
        "width:300px;height:40px;"
        "grid-template-columns:repeat(auto-fill,minmax(100px,1fr))}"
        "#abs{position:absolute;grid-column:1;inset-inline:0;top:0;"
        "height:10px}#end{position:absolute;grid-column:2 / 3;"
        "inset-inline:0;top:0;height:10px}</style><body><div id=fill>"
        "<span id=abs></span><span id=end></span></div>", 480,
        {"abs", 20, 0, 300, 10}, {"end", 120, 0, 100, 10});
    return 0;
}

/* Shrinking items whose bases sum far past the coordinate limit still
   share the container: the free space is no longer saturated at the limit
   before distribution, and the share arithmetic cannot overflow with the
   largest factors (flex-shrink 100) and clamped bases. */
static int test_flex_huge_shrink(void)
{
    CHECK_CORRECTION("flex huge shrink",
        "<!doctype html><style>body{margin:0}"
        ".c{display:flex;flex-direction:column;height:32767px}"
        ".c>div{flex-grow:0;flex-shrink:100;flex-basis:32767%}"
        "</style><body><div id=c class=c><div id=i0></div><div id=i1></div><div id=i2></div><div id=i3></div><div id=i4></div><div id=i5></div><div id=i6></div><div id=i7></div><div id=i8></div><div id=i9></div><div id=i10></div><div id=i11></div><div id=i12></div><div id=i13></div><div id=i14></div><div id=i15></div><div id=i16></div><div id=i17></div><div id=i18></div><div id=i19></div><div id=i20></div><div id=i21></div><div id=i22></div><div id=i23></div><div id=i24></div><div id=i25></div><div id=i26></div><div id=i27></div><div id=i28></div><div id=i29></div><div id=i30></div><div id=i31></div></div>", 480,
        {"c", 0, 0, 480, 32767},
        {"i0", 0, 0, 480, 1024}, {"i1", 0, 1024, 480, BOX_ANY},
        {"i31", 0, BOX_ANY, 480, BOX_ANY});
    return 0;
}

/* Paint position of the first fill of `color`, or SIZE_MAX. */
static size_t correction_fill_order(const LayoutDocument *layout,
                                    uint32_t color)
{
    for (size_t order = 0; order < layout->paint_order_count; order++) {
        const DrawCommand *command =
            &layout->commands[layout->paint_order[order]];
        if (command->type == DRAW_FILL_RECT && command->color == color)
            return order;
    }
    return SIZE_MAX;
}

/* An absolutely positioned box with both vertical insets and an auto height
   (or auto margins) resolves them against its containing block's final
   padding box, also when that block's height is auto: `inset:0` covers the
   block, not the viewport. Laid out once the block is complete, it still
   paints in tree order, under later positioned siblings. The same holds for
   flex and grid containing blocks, whose positioned children are laid out
   after their items. */
static int test_positioned_cover_auto_height(void)
{
    CHECK_CORRECTION("cover in auto-height blocks",
        "<!doctype html><style>body{margin:0;font:16px/20px sans-serif}"
        ".cb{position:relative;width:200px;margin-bottom:10px}"
        ".ov{position:absolute;inset:0}.c{position:relative;height:30px}"
        "#mid{position:absolute;top:0;bottom:0;left:0;width:20px;"
        "height:10px;margin:auto 0}"
        ".bar{display:flex;width:200px}"
        "#tab{position:relative;flex:1;padding:10px}</style>"
        "<div class=cb><div id=block-cover class=ov></div>"
        "<div class=c></div></div>"
        "<div class=cb><div style=height:40px></div><div id=mid></div></div>"
        "<div class=bar><a id=tab href=#t><span id=tab-cover class=ov></span>"
        "<span>tab</span></a></div>"
        "<div class=cb style=display:flex><div id=flex-cover class=ov></div>"
        "<div class=c style=flex:1></div></div>"
        "<div class=cb style=display:grid><div id=grid-cover class=ov></div>"
        "<div class=c></div></div>", 480,
        {"block-cover", 0, 0, 200, 30}, {"mid", 0, 55, 20, 10},
        {"tab", 0, 90, 200, 40}, {"tab-cover", 0, 90, 200, 40},
        {"flex-cover", 0, 130, 200, 30}, {"grid-cover", 0, 170, 200, 30});

    static const char page[] =
        "<!doctype html><style>body{margin:0}"
        ".cb{position:relative;width:200px}"
        ".ov{position:absolute;inset:0;background:#101010}"
        ".c{position:relative;height:30px;background:#00ff00}"
        ".cap{position:absolute;left:0;right:0;bottom:0;height:10px;"
        "background:#ff0000}</style>"
        "<div class=cb><div class=ov></div><div class=c></div>"
        "<div class=cap></div></div>"
        "<div class=cb style=display:flex><div class=ov "
        "style=background:#202020></div><div class=c style="
        "'flex:1;background:#00fe00'></div></div>"
        "<div class=cb style=display:grid><div class=ov "
        "style=background:#303030></div><div class=c "
        "style=background:#00fd00></div></div>";
    Budget budget;
    budget_init(&budget, 16u * MIB);
    CHECK(budget_install_lexbor(&budget));
    PocDocument document = {0};
    Stylesheet sheet = {0};
    LayoutDocument layout = {0};
    CHECK(document_parse(&document, &budget, page, strlen(page), 17)
          && stylesheet_build(&sheet, &budget, &document, 480)
          && layout_build(&layout, &budget, &document, &sheet, NULL, NULL,
                          480));
    size_t block_cover = correction_fill_order(&layout, 0x101010);
    size_t block_content = correction_fill_order(&layout, 0x00ff00);
    size_t caption = correction_fill_order(&layout, 0xff0000);
    size_t flex_cover = correction_fill_order(&layout, 0x202020);
    size_t flex_content = correction_fill_order(&layout, 0x00fe00);
    size_t grid_cover = correction_fill_order(&layout, 0x303030);
    size_t grid_content = correction_fill_order(&layout, 0x00fd00);
    if (!(block_cover < block_content && block_content < caption
          && caption != SIZE_MAX && flex_cover < flex_content
          && flex_content != SIZE_MAX && grid_cover < grid_content
          && grid_content != SIZE_MAX)) {
        fprintf(stderr, "cover paint order: block %zu %zu %zu flex %zu %zu "
                "grid %zu %zu\n", block_cover, block_content, caption,
                flex_cover, flex_content, grid_cover, grid_content);
        CHECK(false);
    }
    layout_destroy(&layout);
    stylesheet_destroy(&sheet);
    document_destroy(&document);
    CHECK(budget.current == 0u && budget_uninstall_lexbor(&budget));
    return 0;
}

/* An absolutely positioned replaced element with auto width and height
   keeps its natural size (or the 300x150 default of canvas and iframe)
   under `inset:0` (CSS 2.1 10.3.8 / 10.6.5): the opposing insets
   over-constrain the box, so right and bottom are ignored, and only auto
   margins center it. A declared size or one-axis size still follows the
   ratio, `object-fit` boxes with declared 100% sizes still cover, and a
   non-replaced box still stretches. Every <img> here is a decoded 100x50
   image; the expectations are Chromium's. */
static int test_positioned_replaced_insets(void)
{
    static const char page[] =
        "<!doctype html><style>body{margin:0}"
        ".c{position:relative;width:300px;margin-bottom:170px}"
        ".i{position:absolute;inset:0}.h{height:30px}"
        ".e{position:absolute;top:0;right:0;bottom:0;left:0}</style>"
        "<div class=c><img id=natural class=i src=a.png><div class=h></div></div>"
        "<div class=c><img id=wide class=i src=a.png style=width:100%>"
        "<div class=h></div></div>"
        "<div class=c><img id=short class=e src=a.png style=height:20px>"
        "<div class=h></div></div>"
        "<div class=c><canvas id=canvas class=i></canvas><div class=h></div></div>"
        "<div class=c><iframe id=frame class=i style=border:0></iframe>"
        "<div class=h></div></div>"
        "<div class=c><div id=block class=i></div><div class=h></div></div>"
        "<div class=c><img id=offset class=i src=a.png style='left:20px;top:5px'>"
        "<div class=h></div></div>"
        "<div class=c><img id=cover class=i src=a.png style='width:100%;"
        "height:100%;object-fit:cover'><div class=h></div></div>"
        "<div class=c><img id=centered class=i src=a.png style=margin:auto>"
        "<div class=h></div></div>"
        "<div class=c><canvas id=narrow class=i style=width:100px></canvas>"
        "<div class=h></div></div>"
        "<div class=c style=height:100px><img id=middle class=i src=a.png "
        "style=margin:auto><div class=h></div></div>"
        "<div class=c><img id=edged class=i src=a.png style='padding:4px;"
        "border:2px solid #000'><div class=h></div></div>"
        "<div class=c><img id=capped class=i src=a.png style=max-width:50px>"
        "<div class=h></div></div>"
        "<div class=c dir=rtl><img id=rtl class=i src=a.png>"
        "<div class=h></div></div>"
        "<div class=c><video id=video class=i></video><div class=h></div></div>";
    static const CorrectionBox expected[] = {
        {"natural", 0, 0, 100, 50}, {"wide", 0, 200, 300, 150},
        {"short", 0, 400, 40, 20}, {"canvas", 0, 600, 300, 150},
        {"frame", 0, 800, 300, 150}, {"block", 0, 1000, 300, 30},
        {"offset", 20, 1205, 100, 50}, {"cover", 0, 1400, 300, 30},
        {"centered", 100, 1590, 100, 50}, {"narrow", 0, 1800, 100, 50},
        {"middle", 100, 2025, 100, 50}, {"edged", 0, 2270, 112, 62},
        {"capped", 0, 2470, 50, 25}, {"rtl", 200, 2670, 100, 50},
        {"video", 0, 2870, 300, 150}
    };
    static const char *const images[] = {
        "natural", "wide", "short", "offset", "cover", "centered", "middle",
        "edged", "capped", "rtl"
    };
    enum { IMAGE_COUNT = sizeof(images) / sizeof(images[0]) };
    Budget budget;
    budget_init(&budget, 16u * MIB);
    CHECK(budget_install_lexbor(&budget));
    PocDocument document = {0};
    Stylesheet sheet = {0};
    LayoutDocument layout = {0};
    CHECK(document_parse(&document, &budget, page, strlen(page), 17)
          && stylesheet_build(&sheet, &budget, &document, 480));
    lxb_dom_node_t *root = lxb_dom_interface_node(document.html);
    static unsigned char pixels[100 * 50 * 4];
    ImageResource resources[IMAGE_COUNT];
    for (size_t i = 0; i < IMAGE_COUNT; i++) {
        resources[i] = (ImageResource) {
            .node = find_id(root, images[i]), .pixels = pixels,
            .width = 100, .height = 50
        };
        CHECK(resources[i].node != NULL);
    }
    ImageResources image_set = {.items = resources, .count = IMAGE_COUNT};
    CHECK(layout_build(&layout, &budget, &document, &sheet, NULL, &image_set,
                       480));
    int failures = 0;
    for (size_t i = 0; i < sizeof(expected) / sizeof(expected[0]); i++) {
        const CorrectionBox *want = &expected[i];
        const LayoutNodeBox *box = layout_box_for_node(
            &layout, find_id(root, want->id));
        if (box == NULL || box->x != want->x || box->y != want->y
            || box->width != want->width || box->height != want->height) {
            fprintf(stderr, "positioned replaced: %s got %d,%d %dx%d, "
                    "want %d,%d %dx%d\n", want->id, box ? box->x : -1,
                    box ? box->y : -1, box ? box->width : -1,
                    box ? box->height : -1, want->x, want->y, want->width,
                    want->height);
            failures++;
        }
    }
    /* The picture fills the content box, not the stretched insets. */
    static const CorrectionBox drawn[] = {
        {"natural", 0, 0, 100, 50}, {"centered", 100, 1590, 100, 50},
        {"edged", 6, 2276, 100, 50}, {"rtl", 200, 2670, 100, 50}
    };
    for (size_t i = 0; i < sizeof(drawn) / sizeof(drawn[0]); i++) {
        bool found = false;
        for (size_t c = 0; c < layout.count && !found; c++) {
            const DrawCommand *command = &layout.commands[c];
            found = command->type == DRAW_IMAGE
                && command->x == drawn[i].x && command->y == drawn[i].y
                && command->width == drawn[i].width
                && command->height == drawn[i].height;
        }
        if (!found) {
            fprintf(stderr, "positioned replaced: no %s image at %d,%d "
                    "%dx%d\n", drawn[i].id, drawn[i].x, drawn[i].y,
                    drawn[i].width, drawn[i].height);
            failures++;
        }
    }
    layout_destroy(&layout);
    stylesheet_destroy(&sheet);
    document_destroy(&document);
    CHECK(budget.current == 0u && budget_uninstall_lexbor(&budget));
    CHECK(failures == 0);
    return 0;
}

/* A fixed cell width is the column's preferred width, not a floor (CSS
   Tables 3): a 400px cell in a 300px CSS table, laid out as the table's
   anonymous row, fits the table. Auto cells keep their content minimum
   and fixed cells share the room above their minimums in proportion to
   what each asked for beyond it. A row with room for every fixed width
   still gives each its width. The expectations are Chromium's (rounded
   down). */
static int test_table_cell_fixed_width_squeeze(void)
{
    CHECK_CORRECTION("fixed cells in a narrower table",
        "<!doctype html><style>body{margin:0}"
        ".t{display:table;width:300px}.c{display:table-cell}"
        "i{display:inline-block;width:20px;height:10px}</style>"
        "<div class=t><div id=one class=c style=width:400px><i></i></div></div>"
        "<div class=t><div id=fixed class=c style=width:400px><i></i></div>"
        "<div id=auto class=c><i></i></div></div>"
        "<div class=t><div id=small class=c style=width:200px><i></i></div>"
        "<div id=large class=c style=width:300px><i></i></div></div>"
        "<div class=t><div id=padded class=c style='width:400px;"
        "padding:0 10px'><i></i></div></div>"
        "<div class=t><div id=fits class=c style=width:100px><i></i></div>"
        "<div id=rest class=c><i></i></div></div>"
        "<div style=width:300px><div id=wrapped class=c style=width:400px>"
        "<i></i></div></div>"
        "<div class=t><div id=tight class=c style=width:400px>"
        "<i style=width:40px></i></div><div id=tight2 class=c "
        "style=width:400px><i></i></div></div>", 480,
        {"one", 0, BOX_ANY, 300, BOX_ANY},
        {"fixed", 0, BOX_ANY, 280, BOX_ANY},
        {"auto", 280, BOX_ANY, 20, BOX_ANY},
        {"small", 0, BOX_ANY, 121, BOX_ANY},
        {"large", 121, BOX_ANY, 179, BOX_ANY},
        {"padded", 0, BOX_ANY, 300, BOX_ANY},
        {"fits", 0, BOX_ANY, 100, BOX_ANY},
        {"rest", 100, BOX_ANY, 200, BOX_ANY},
        {"wrapped", 0, BOX_ANY, 300, BOX_ANY},
        {"tight", 0, BOX_ANY, 156, BOX_ANY},
        {"tight2", 156, BOX_ANY, 144, BOX_ANY});
    return 0;
}

/* The same for an HTML <table>: a td width is the column's preferred width,
   not its minimum. A table narrower than its specified widths gives each
   column its content minimum and interpolates toward the specified widths
   (percentage columns first), as Chromium does; tables with room keep
   their columns. Widths are Chromium's, rounded down. */
#define TABLE_SQUEEZE_STYLE \
    "<!doctype html><style>body{margin:0}" \
    "table{width:300px;border-spacing:0}td{padding:0}" \
    "i{display:inline-block;width:20px;height:10px}</style>"
static const char table_squeeze_page[] = TABLE_SQUEEZE_STYLE
    "<table><tr><td id=one style=width:400px><i></i></td></tr></table>"
    "<table><tr><td id=fixed style=width:400px><i></i></td>"
    "<td id=auto><i></i></td></tr></table>"
    "<table><tr><td id=small style=width:200px><i></i></td>"
    "<td id=large style=width:300px><i></i></td></tr></table>"
    "<table><tr><td id=padded style='width:400px;padding:0 10px'><i></i>"
    "</td></tr></table>"
    "<table><tr><td id=tight style=width:400px><i style=width:40px></i></td>"
    "<td id=tight2 style=width:400px><i></i></td></tr></table>"
    "<table><tr><td id=pc style=width:50%><i></i></td>"
    "<td id=fx style=width:300px><i></i></td><td id=au><i></i></td></tr>"
    "</table>"
    "<table><tr><td id=span colspan=2 style=width:400px><i></i></td></tr>"
    "<tr><td id=s1><i></i></td><td id=s2><i></i></td></tr></table>"
    "<table><col style=width:400px><col><tr><td id=colw><i></i></td>"
    "<td id=cola><i></i></td></tr></table>"
    "<table style=border-spacing:10px><tr><td id=sp1 style=width:200px>"
    "<i></i></td><td id=sp2 style=width:200px><i></i></td></tr></table>"
    "<table style=border-collapse:collapse><tr>"
    "<td id=bc1 style='width:200px;border:4px solid'><i></i></td>"
    "<td id=bc2 style='width:200px;border:4px solid'><i></i></td></tr>"
    "</table>"
    "<table style=width:auto><tr><td id=aw1 style=width:300px><i></i></td>"
    "<td id=aw2 style=width:300px><i></i></td></tr></table>"
    "<table><tr><td id=room1 style=width:100px><i></i></td>"
    "<td id=room2 style=width:100px><i></i></td></tr></table>";
#define TABLE_SQUEEZE_EXPECTED \
    {"one", 0, BOX_ANY, 300, BOX_ANY}, \
    {"fixed", 0, BOX_ANY, 280, BOX_ANY}, \
    {"auto", 280, BOX_ANY, 20, BOX_ANY}, \
    {"small", 0, BOX_ANY, 121, BOX_ANY}, \
    {"large", 121, BOX_ANY, 179, BOX_ANY}, \
    {"padded", 0, BOX_ANY, 300, BOX_ANY}, \
    {"tight", 0, BOX_ANY, 156, BOX_ANY}, \
    {"tight2", 156, BOX_ANY, 144, BOX_ANY}, \
    {"pc", 0, BOX_ANY, 150, BOX_ANY}, \
    {"fx", 150, BOX_ANY, 130, BOX_ANY}, \
    {"au", 280, BOX_ANY, 20, BOX_ANY}, \
    {"span", 0, BOX_ANY, 300, BOX_ANY}, \
    {"s1", 0, BOX_ANY, 150, BOX_ANY}, \
    {"s2", 150, BOX_ANY, 150, BOX_ANY}, \
    {"colw", 0, BOX_ANY, 280, BOX_ANY}, \
    {"cola", 280, BOX_ANY, 20, BOX_ANY}, \
    {"sp1", 10, BOX_ANY, 135, BOX_ANY}, \
    {"sp2", 155, BOX_ANY, 135, BOX_ANY}, \
    {"bc1", 2, BOX_ANY, 148, BOX_ANY}, \
    {"bc2", 150, BOX_ANY, 148, BOX_ANY}, \
    {"aw1", 0, BOX_ANY, 240, BOX_ANY}, \
    {"aw2", 240, BOX_ANY, 240, BOX_ANY}, \
    {"room1", 0, BOX_ANY, 150, BOX_ANY}, \
    {"room2", 150, BOX_ANY, 150, BOX_ANY}

static size_t count_table_rows(const lxb_dom_node_t *node)
{
    size_t rows = 0;
    for (; node != NULL; node = node->next) {
        if (document_node_is_tag(node, LXB_TAG_TR)) rows++;
        rows += count_table_rows(node->first_child);
    }
    return rows;
}

/* One table's page laid out twice through a reuse cache; the boxes of
   `expected` found in it are checked and counted in `found`. */
static int check_table_row_reuse_page(const char *page,
                                      const CorrectionBox *expected,
                                      size_t expected_count, size_t *found)
{
    Budget budget;
    budget_init(&budget, 16u * MIB);
    CHECK(budget_install_lexbor(&budget));
    PocDocument document = {0};
    Stylesheet sheet = {0};
    ViewportContext viewport;
    CHECK(document_parse(&document, &budget, page, strlen(page), 17)
          && viewport_context_init(&viewport, 480, 272, 480, 272)
          && stylesheet_build_context(&sheet, &budget, &document, &viewport));
    LayoutReuseCache *cache = layout_reuse_cache_create(&budget);
    CHECK(cache != NULL);
    lxb_dom_node_t *root = lxb_dom_interface_node(document.html);
    size_t rows = count_table_rows(root);
    CHECK(rows > 0);
    int failures = 0;
    LayoutReuseStats before, after;
    for (unsigned pass = 0; pass < 2; pass++) {
        layout_reuse_cache_stats(cache, &before);
        LayoutDocument layout = {0};
        CHECK(layout_build_context_reuse(&layout, &budget, &document,
            &sheet, NULL, NULL, &viewport, cache));
        for (size_t i = 0; i < expected_count; i++) {
            lxb_dom_node_t *node = find_id(root, expected[i].id);
            if (node == NULL) continue;
            if (pass == 0) (*found)++;
            const LayoutNodeBox *box = layout_box_for_node(&layout, node);
            if (box == NULL || box->x != expected[i].x
                || (expected[i].width != BOX_ANY
                    && box->width != expected[i].width)) {
                fprintf(stderr, "reuse pass %u: %s got %d %d, want %d %d\n",
                        pass, expected[i].id, box ? box->x : -1,
                        box ? box->width : -1, expected[i].x,
                        expected[i].width);
                failures++;
            }
        }
        layout_destroy(&layout);
    }
    /* The second layout takes every row's measurement from the cache. */
    layout_reuse_cache_stats(cache, &after);
    if (after.table_row_hits - before.table_row_hits < rows) {
        fprintf(stderr, "reuse pass 1: %llu row hits for %zu rows\n",
                (unsigned long long) (after.table_row_hits
                                      - before.table_row_hits), rows);
        failures++;
    }
    layout_reuse_cache_destroy(cache);
    stylesheet_destroy(&sheet);
    document_destroy(&document);
    CHECK(budget.current == 0u && budget_uninstall_lexbor(&budget));
    CHECK(failures == 0);
    return 0;
}

/* Column widths read what the table-row reuse cache keeps for each row
   (content minimums, maximums, fixed and percentage columns): a relayout
   that hits the cache must give the same columns as a fresh layout. Each
   top-level table of `page` is laid out on a page of its own (with the
   page's head): the cache has 32 slots and probes 8 from a slot that
   follows the row's node address, so a page of many rows measured at two
   widths could fill a probe window and evict a row whatever the code
   does, while a table of a few rows cannot. */
static int check_table_row_reuse(const char *page,
                                 const CorrectionBox *expected,
                                 size_t expected_count)
{
    static char piece[4096];
    const char *at = strstr(page, "<table");
    CHECK(at != NULL);
    size_t head = (size_t) (at - page);
    size_t found = 0;
    while (at != NULL) {
        const char *end = NULL;
        int depth = 0;
        for (const char *p = at; *p != '\0' && end == NULL; p++) {
            if (strncmp(p, "<table", 6) == 0) depth++;
            if (strncmp(p, "</table>", 8) == 0 && --depth == 0) {
                end = p + 8;
            }
        }
        CHECK(end != NULL);
        size_t length = (size_t) (end - at);
        CHECK(head + length < sizeof(piece));
        memcpy(piece, page, head);
        memcpy(piece + head, at, length);
        piece[head + length] = '\0';
        CHECK(check_table_row_reuse_page(piece, expected, expected_count,
                                         &found) == 0);
        at = strstr(end, "<table");
    }
    CHECK(found == expected_count);
    return 0;
}

static int test_table_squeeze_row_reuse(void)
{
    static const CorrectionBox expected[] = {TABLE_SQUEEZE_EXPECTED};
    return check_table_row_reuse(table_squeeze_page, expected,
                                 sizeof(expected) / sizeof(expected[0]));
}

static int test_table_html_fixed_width_squeeze(void)
{
    CHECK_CORRECTION("fixed td widths in a narrower table",
        table_squeeze_page, 480, TABLE_SQUEEZE_EXPECTED);
    CHECK(test_table_squeeze_row_reuse() == 0);
    return 0;
}

/* A table with room for its columns grows them the way CSS Tables 3 and
   Chromium do: fixed columns to their widths, then auto columns to their
   max-content, and past every maximum the auto columns (else the fixed
   ones) take the rest in proportion to their maximums. A fixed column's
   width is its maximum, never its minimum: its content minimum can exceed
   it, and auto cells in its column do not raise it. Percentage columns
   take their percentage of the grid (border box, against the grid without
   border-spacing), and an auto-width table widens so each percentage
   column has its percentage. Widths are Chromium's. */
#define TABLE_GUESS_STYLE \
    "<!doctype html><style>body{margin:0}" \
    "table{width:300px;border-spacing:0}td{padding:0}" \
    "i{display:inline-block;width:20px;height:10px;vertical-align:top}" \
    "</style>"
static const char table_guess_page[] = TABLE_GUESS_STYLE
    "<table><tr><td id=fx style=width:100px><i></i></td>"
    "<td id=au><i></i></td></tr></table>"
    "<table><tr><td id=wide style=width:100px><i style=width:250px></i>"
    "</td><td id=wrest><i></i></td></tr></table>"
    "<table><tr><td id=small><i></i></td>"
    "<td id=large><i style=width:60px></i></td></tr></table>"
    "<table><tr><td id=f1 style=width:100px><i></i></td>"
    "<td id=f2 style=width:50px><i></i></td></tr></table>"
    "<table style=width:400px><tr><td id=pc style=width:20%><i></i></td>"
    "<td id=pf style=width:100px><i></i></td><td id=pa><i></i></td></tr>"
    "</table>"
    "<table><tr><td id=cf style=width:100px><i></i></td>"
    "<td id=ca><i></i></td></tr><tr><td><i style=width:200px></i></td>"
    "<td><i></i></td></tr></table>"
    "<table style=width:auto><tr><td id=af style=width:100px><i></i></td>"
    "<td id=aa><i></i></td></tr></table>"
    "<table><tr><td id=pp style='width:50%;padding:0 10px'><i></i></td>"
    "<td id=pr><i></i></td></tr></table>"
    "<table style=width:auto><tr><td id=ep style=width:30%>"
    "<i style=width:60px></i></td><td id=ea><i></i></td></tr></table>"
    "<table style=width:auto><tr><td id=fp style=width:100%><i></i></td>"
    "<td id=fa><i></i></td></tr></table>"
    "<table style=width:auto><tr><td id=op style=width:100%><i></i></td>"
    "</tr></table>"
    "<table style=border-spacing:10px><tr><td id=bp style=width:50%>"
    "<i></i></td><td id=ba><i></i></td></tr></table>";
#define TABLE_GUESS_EXPECTED \
    {"fx", 0, BOX_ANY, 100, BOX_ANY}, {"au", 100, BOX_ANY, 200, BOX_ANY}, \
    {"wide", 0, BOX_ANY, 250, BOX_ANY}, \
    {"wrest", 250, BOX_ANY, 50, BOX_ANY}, \
    {"small", 0, BOX_ANY, 75, BOX_ANY}, \
    {"large", 75, BOX_ANY, 225, BOX_ANY}, \
    {"f1", 0, BOX_ANY, 200, BOX_ANY}, {"f2", 200, BOX_ANY, 100, BOX_ANY}, \
    {"pc", 0, BOX_ANY, 80, BOX_ANY}, {"pf", 80, BOX_ANY, 100, BOX_ANY}, \
    {"pa", 180, BOX_ANY, 220, BOX_ANY}, \
    {"cf", 0, BOX_ANY, 200, BOX_ANY}, {"ca", 200, BOX_ANY, 100, BOX_ANY}, \
    {"af", 0, BOX_ANY, 100, BOX_ANY}, {"aa", 100, BOX_ANY, 20, BOX_ANY}, \
    {"pp", 0, BOX_ANY, 150, BOX_ANY}, {"pr", 150, BOX_ANY, 150, BOX_ANY}, \
    {"ep", 0, BOX_ANY, 60, BOX_ANY}, {"ea", 60, BOX_ANY, 140, BOX_ANY}, \
    {"fp", 0, BOX_ANY, 460, BOX_ANY}, {"fa", 460, BOX_ANY, 20, BOX_ANY}, \
    {"op", 0, BOX_ANY, 20, BOX_ANY}, \
    {"bp", 10, BOX_ANY, 135, BOX_ANY}, {"ba", 155, BOX_ANY, 135, BOX_ANY}

static int test_table_column_guesses(void)
{
    static const CorrectionBox expected[] = {TABLE_GUESS_EXPECTED};
    CHECK(check_correction_page("table column guesses", table_guess_page,
                                480, expected,
                                sizeof(expected) / sizeof(expected[0]))
          == 0);
    CHECK(check_table_row_reuse(table_guess_page, expected,
                                sizeof(expected) / sizeof(expected[0]))
          == 0);
    return 0;
}

/* An HTML table cuts its columns' percentages at 100% in column order, as
   Chromium does: a column whose percentage would take the grid past 100%
   gets only what is left, and none once it is used up, which leaves it
   its content minimum. An auto-width table whose percentages leave the
   other columns nothing fills its container. Widths are Chromium's,
   rounded to whole pixels. */
static const char table_percent_cap_page[] = TABLE_GUESS_STYLE
    "<table><tr><td id=p1 style=width:60%><i></i></td>"
    "<td id=p2 style=width:60%><i></i></td></tr></table>"
    "<table><tr><td id=q1 style=width:70%><i></i></td>"
    "<td id=q2 style=width:50%><i></i></td><td id=q3><i></i></td></tr>"
    "</table>"
    "<table style=width:auto><tr><td id=r1 style=width:120%><i></i></td>"
    "<td id=r2><i></i></td></tr></table>"
    "<table style=width:auto><tr><td id=s1 style=width:50%>"
    "<i style=width:60px></i></td><td id=s2 style=width:80%><i></i></td>"
    "<td id=s3><i></i></td></tr></table>"
    "<table><tr><td id=u1 style=width:100%><i></i></td>"
    "<td id=u2 style=width:100%><i></i></td></tr></table>";
#define TABLE_PERCENT_CAP_EXPECTED \
    {"p1", 0, BOX_ANY, 180, BOX_ANY}, {"p2", 180, BOX_ANY, 120, BOX_ANY}, \
    {"q1", 0, BOX_ANY, 195, BOX_ANY}, {"q2", 195, BOX_ANY, 85, BOX_ANY}, \
    {"q3", 280, BOX_ANY, 20, BOX_ANY}, \
    {"r1", 0, BOX_ANY, 460, BOX_ANY}, {"r2", 460, BOX_ANY, 20, BOX_ANY}, \
    {"s1", 0, BOX_ANY, 231, BOX_ANY}, {"s2", 231, BOX_ANY, 229, BOX_ANY}, \
    {"s3", 460, BOX_ANY, 20, BOX_ANY}, \
    {"u1", 0, BOX_ANY, 280, BOX_ANY}, {"u2", 280, BOX_ANY, 20, BOX_ANY}

static int test_table_percent_cap(void)
{
    static const CorrectionBox expected[] = {TABLE_PERCENT_CAP_EXPECTED};
    CHECK(check_correction_page("table percentages past 100%",
                                table_percent_cap_page, 480, expected,
                                sizeof(expected) / sizeof(expected[0]))
          == 0);
    CHECK(check_table_row_reuse(table_percent_cap_page, expected,
                                sizeof(expected) / sizeof(expected[0]))
          == 0);
    return 0;
}

/* A fixed-width cell that spans columns only raises their maximums, as in
   Chromium: it does not make them fixed columns, so the auto cells below
   it keep their max-content widths, and the spanning cell covers the
   columns it spans. The columns were capped at the cell's width, which
   shrank an auto-width table to it and shared a fixed-width table's room
   as if both columns were fixed. The spanning cell's box is its columns
   and the spacing between them, wider than its width when they are; it
   kept its authored width and left part of its columns uncovered. Boxes
   are Chromium's. */
static const char table_span_fixed_page[] =
    "<!doctype html><style>body{margin:0}table{border-spacing:0}"
    "td{padding:0}i{float:left;width:100px;height:10px}"
    "b{display:block;width:60px;height:10px}</style>"
    "<table id=t1><tr><td id=h1 colspan=2 style=width:100px;height:10px>"
    "</td></tr><tr><td id=a1><b></b></td><td id=c1><i></i><i></i><i></i><i></i>"
    "</td></tr></table>"
    "<table id=t2 style=width:400px><tr><td id=h2 colspan=2 width=100 "
    "style=height:10px></td></tr><tr><td id=a2><b></b></td><td id=c2>"
    "<i></i><i></i><i></i><i></i></td></tr></table>"
    "<table id=t3><tr><td id=h3 colspan=2 style=width:50px;height:10px>"
    "</td></tr><tr><td id=a3><b></b></td><td id=c3><i></i></td></tr>"
    "</table>"
    "<table id=t4 style=border-spacing:5px><tr><td id=h4 colspan=2 "
    "style=width:50px;padding:2px><b style=width:20px></b></td></tr><tr>"
    "<td id=a4><b></b></td><td id=c4><b style=width:100px></b></td></tr>"
    "</table>";
#define TABLE_SPAN_FIXED_EXPECTED \
    {"t1", 0, 0, 460, 20}, {"h1", 0, 0, 460, 10}, \
    {"a1", 0, 10, 60, 10}, {"c1", 60, 10, 400, 10}, \
    {"t2", 0, 20, 400, 30}, {"h2", 0, 20, 400, 10}, \
    {"a2", 0, 30, 60, 20}, {"c2", 60, 30, 340, 20}, \
    {"t3", 0, 50, 160, 20}, {"h3", 0, 50, 160, 10}, \
    {"a3", 0, 60, 60, 10}, {"c3", 60, 60, 100, 10}, \
    {"t4", 0, 70, 175, 39}, {"h4", 5, 75, 165, 14}, \
    {"a4", 5, 94, 60, 10}, {"c4", 70, 94, 100, 10}

static int test_table_span_fixed_width(void)
{
    static const CorrectionBox expected[] = {TABLE_SPAN_FIXED_EXPECTED};
    CHECK(check_correction_page("fixed colspan cell over auto cells",
                                table_span_fixed_page, 480, expected,
                                sizeof(expected) / sizeof(expected[0]))
          == 0);
    CHECK(check_table_row_reuse(table_span_fixed_page, expected,
                                sizeof(expected) / sizeof(expected[0]))
          == 0);
    return 0;
}

/* A spanning cell's minimum and maximum spread over its columns the way
   Chromium does, once the single-column cells are merged: smallest span
   first, by the guesses of the table's own width distribution, so only a
   cell wider than its columns widens them, the auto columns first and in
   proportion to their maximums. A spanning cell's fixed width never fixes
   its columns. They were split evenly: a 100px header over 30 and 300
   columns made the first 50, and 200px over 60 and 100 gave 100/100.
   Boxes are Chromium's. */
static const char table_span_distribution_page[] =
    "<!doctype html><style>body{margin:0}table{border-spacing:0}"
    "td{padding:0}b{display:block;height:10px}</style>"
    "<table><tr><td colspan=2 style=width:100px><b></b></td></tr><tr>"
    "<td id=a1><b style=width:30px></b></td>"
    "<td id=a2><b style=width:300px></b></td></tr></table>"
    "<table><tr><td colspan=2><b style=width:200px></b></td></tr><tr>"
    "<td id=b1><b style=width:60px></b></td>"
    "<td id=b2><b style=width:100px></b></td></tr></table>"
    "<table><tr><td colspan=2 style=width:390px><b></b></td></tr><tr>"
    "<td id=c1><b style=width:30px></b></td>"
    "<td id=c2><b style=width:100px></b></td></tr></table>"
    "<table><tr><td colspan=2 style=width:400px><b></b></td></tr><tr>"
    "<td id=d1 style=width:30px><b></b></td>"
    "<td id=d2><b style=width:100px></b></td></tr></table>"
    "<table><tr><td colspan=2 style=width:300px><b></b></td></tr><tr>"
    "<td id=e1 style=width:40px><b></b></td>"
    "<td id=e2 style=width:60px><b></b></td></tr></table>"
    "<table><tr><td id=g123 colspan=3><b style=width:375px></b></td></tr>"
    "<tr><td id=g12 colspan=2><b style=width:200px></b></td>"
    "<td id=g3><b style=width:50px></b></td></tr><tr>"
    "<td id=g1><b style=width:20px></b></td>"
    "<td id=g2><b style=width:60px></b></td><td></td></tr></table>"
    "<table style=border-spacing:10px><tr><td id=h12 colspan=2>"
    "<b style=width:330px></b></td></tr><tr>"
    "<td id=h1><b style=width:60px></b></td>"
    "<td id=h2><b style=width:100px></b></td></tr></table>";
#define TABLE_SPAN_DISTRIBUTION_EXPECTED \
    {"a1", 0, 10, 30, 10}, {"a2", 30, 10, 300, 10}, \
    {"b1", 0, 30, 75, 10}, {"b2", 75, 30, 125, 10}, \
    {"c1", 0, 50, 90, 10}, {"c2", 90, 50, 300, 10}, \
    {"d1", 0, 70, 30, 10}, {"d2", 30, 70, 370, 10}, \
    {"e1", 0, 90, 120, 10}, {"e2", 120, 90, 180, 10}, \
    {"g123", 0, 100, 375, 10}, {"g12", 0, 110, 300, 10}, \
    {"g3", 300, 110, 75, 10}, {"g1", 0, 120, 75, 10}, \
    {"g2", 75, 120, 225, 10}, \
    {"h12", 10, 140, 330, 10}, {"h1", 10, 160, 120, 10}, \
    {"h2", 140, 160, 200, 10}

static int test_table_span_distribution(void)
{
    static const CorrectionBox expected[] = {
        TABLE_SPAN_DISTRIBUTION_EXPECTED};
    CHECK(check_correction_page("colspan cells spread by column maximums",
                                table_span_distribution_page, 480, expected,
                                sizeof(expected) / sizeof(expected[0]))
          == 0);
    CHECK(check_table_row_reuse(table_span_distribution_page, expected,
                                sizeof(expected) / sizeof(expected[0]))
          == 0);
    return 0;
}

/* Overlapping spans of the same length size in column order regardless
   of which row declares them first. The earlier column keeps its 100px
   share, while the second span widens the shared and last columns. */
static int test_table_span_row_order(void)
{
    static const char *const rows[] = {
        "<tr><td id=a><b></b></td><td id=b><b></b></td>"
        "<td id=c><b></b></td></tr>",
        "<tr><td id=ab colspan=2><b style=width:200px></b></td>"
        "<td><b></b></td></tr>",
        "<tr><td><b></b></td><td id=bc colspan=2>"
        "<b style=width:200px></b></td></tr>"
    };
    static const unsigned orders[][3] = {
        {0, 1, 2}, {0, 2, 1}, {1, 0, 2},
        {1, 2, 0}, {2, 0, 1}, {2, 1, 0}
    };
    static const CorrectionBox expected[] = {
        {"a", 0, BOX_ANY, 100, 5}, {"b", 100, BOX_ANY, 133, 5},
        {"c", 233, BOX_ANY, 67, 5}, {"ab", 0, BOX_ANY, 233, 5},
        {"bc", 100, BOX_ANY, 200, 5}
    };
    for (size_t i = 0; i < sizeof(orders) / sizeof(orders[0]); i++) {
        char page[512];
        int length = snprintf(page, sizeof(page),
            "<!doctype html><style>body{margin:0}table{border-spacing:0}"
            "td{padding:0}b{display:block;width:50px;height:5px}</style>"
            "<table>%s%s%s</table>", rows[orders[i][0]],
            rows[orders[i][1]], rows[orders[i][2]]);
        CHECK(length > 0 && (size_t) length < sizeof(page));
        CHECK(check_correction_page("overlapping colspan row permutations",
                                    page, 480, expected,
                                    sizeof(expected) / sizeof(expected[0]))
              == 0);
    }
    return 0;
}

/* A percentage cell that spans columns gives the part of its percentage
   that its percentage columns do not already hold to its other columns,
   in proportion to their maximums, as Chromium does; they then size as
   percentage columns. The percentage was split evenly: a 100% cell over
   20px and 400px columns in a 400px table gave 200/200. An auto-width
   table widens so the columns get their percentages, and a fixed column
   after the span does not take the room the spanning cell's minimum
   needs (its columns kept only their percentages, 15 each). Boxes are
   Chromium's. */
static const char table_span_percent_page[] =
    "<!doctype html><style>body{margin:0}table{border-spacing:0}"
    "td{padding:0}b{display:block;height:10px}"
    "i{float:left;width:100px;height:10px}</style>"
    "<table style=width:400px><tr><td colspan=2 style=width:100%><b></b>"
    "</td></tr><tr><td id=j1><b style=width:20px></b></td>"
    "<td id=j2><i></i><i></i><i></i><i></i></td></tr></table>"
    "<table style=width:400px><tr><td colspan=2 style=width:50%><b></b>"
    "</td><td id=k3><b style=width:20px></b></td></tr><tr>"
    "<td id=k1><b style=width:40px></b></td>"
    "<td id=k2><b style=width:160px></b></td><td></td></tr></table>"
    "<table style=width:400px><tr><td colspan=2 style=width:60%><b></b>"
    "</td><td id=l3><b style=width:20px></b></td></tr><tr>"
    "<td id=l1 style=width:20%><b style=width:20px></b></td>"
    "<td id=l2><b style=width:100px></b></td><td></td></tr></table>"
    "<table id=m><tr><td colspan=2 style=width:50%><b></b></td></tr><tr>"
    "<td id=m1><b style=width:30px></b></td>"
    "<td id=m2><b style=width:90px></b></td></tr></table>"
    "<table style=width:300px><tr><td id=n1><b style=width:80px></b></td>"
    "<td id=n23 colspan=2 style=width:10%><b style=width:120px></b></td>"
    "<td id=n4 style=width:100px><b style=width:120px></b></td></tr>"
    "</table>";
#define TABLE_SPAN_PERCENT_EXPECTED \
    {"j1", 0, 10, 20, 20}, {"j2", 20, 10, 380, 20}, \
    {"k1", 0, 40, 40, 10}, {"k2", 40, 40, 160, 10}, \
    {"k3", 200, 30, 200, 10}, \
    {"l1", 0, 60, 80, 10}, {"l2", 80, 60, 160, 10}, \
    {"l3", 240, 50, 160, 10}, \
    {"m", 0, 70, 240, 20}, {"m1", 0, 80, 60, 10}, \
    {"m2", 60, 80, 180, 10}, \
    {"n1", 0, 90, 80, 10}, {"n23", 80, 90, 120, 10}, \
    {"n4", 200, 90, 120, 10}

static int test_table_span_percent(void)
{
    static const CorrectionBox expected[] = {TABLE_SPAN_PERCENT_EXPECTED};
    CHECK(check_correction_page("percentage colspan cells",
                                table_span_percent_page, 480, expected,
                                sizeof(expected) / sizeof(expected[0]))
          == 0);
    CHECK(check_table_row_reuse(table_span_percent_page, expected,
                                sizeof(expected) / sizeof(expected[0]))
          == 0);
    return 0;
}

/* When every column a percentage cell spans but its percentage columns
   is empty, the rest of its percentage splits evenly over those other
   columns, as in Chromium, not over the whole span: 90% over a 30%
   column and two empty ones gives each of them 30% (120/120/120/40 in a
   400px table, where the share was 20% and the table 120/80/80/120).
   Boxes are Chromium's. */
static const char table_span_percent_free_page[] =
    "<!doctype html><style>body{margin:0}table{border-spacing:0}"
    "td{padding:0}b{display:block;width:20px;height:10px}</style>"
    "<table style=width:400px><tr><td id=m0 colspan=3 style=width:90%>"
    "</td><td id=m3><b></b></td></tr><tr><td id=n0 style=width:30%><b></b>"
    "</td><td id=n1></td><td id=n2></td><td id=n3></td></tr></table>";

static int test_table_span_percent_free(void)
{
    static const CorrectionBox expected[] = {
        {"m0", 0, 0, 360, 10}, {"m3", 360, 0, 40, 10},
        {"n0", 0, 10, 120, 10}, {"n1", 120, 10, 120, BOX_ANY},
        {"n2", 240, 10, 120, BOX_ANY}, {"n3", 360, 10, 40, BOX_ANY}};
    CHECK(check_correction_page("percentage colspan over empty columns",
                                table_span_percent_free_page, 480, expected,
                                sizeof(expected) / sizeof(expected[0]))
          == 0);
    CHECK(check_table_row_reuse(table_span_percent_free_page, expected,
                                sizeof(expected) / sizeof(expected[0]))
          == 0);
    return 0;
}

/* A colspan past the table's last column ends there, as in Chromium,
   which gives a column no cell starts in no width: colspan=1000 over two
   columns makes a two-column table and the "colspan=99" footer widens
   the three columns above it in proportion. The span went on to the
   16th column, and each of those columns kept 8px, with border spacing
   and collapsed borders alike (a 100px cell made a 212px table). Boxes
   are Chromium's. */
static const char table_span_overlong_page[] =
    "<!doctype html><style>body{margin:0}table{border-spacing:0}"
    "td{padding:0}b{display:block;width:20px;height:10px}</style>"
    "<table><tr><td id=p0 colspan=1000><b style=width:100px></b></td></tr>"
    "<tr><td id=p1><b></b></td><td id=p2><b></b></td></tr></table>"
    "<table><tr><td id=k1><b></b></td><td id=k2><b style=width:60px></b>"
    "</td><td id=k3><b style=width:40px></b></td></tr><tr>"
    "<td id=k0 colspan=99><b style=width:480px></b></td></tr></table>"
    "<table style=border-spacing:5px><tr><td id=t0 colspan=9>"
    "<b style=width:205px></b></td></tr><tr><td id=t1><b></b></td>"
    "<td id=t2><b></b></td></tr></table>"
    "<table style=border-collapse:collapse><tr><td id=z0 colspan=50 "
    "style=\"border:4px solid\"><b style=width:100px></b></td></tr><tr>"
    "<td id=z1 style=\"border:2px solid\"><b></b></td>"
    "<td id=z2 style=\"border:2px solid\"><b></b></td></tr></table>";

static int test_table_span_overlong(void)
{
    static const CorrectionBox expected[] = {
        {"p0", 0, 0, 100, 10}, {"p1", 0, 10, 50, 10},
        {"p2", 50, 10, 50, 10}, {"k1", 0, 20, 80, 10},
        {"k2", 80, 20, 240, 10}, {"k3", 320, 20, 160, 10},
        {"k0", 0, 30, 480, 10}, {"t0", 5, 45, 205, 10},
        {"t1", 5, 60, 100, 10}, {"t2", 110, 60, 100, 10},
        {"z0", 2, 77, 104, BOX_ANY}, {"z1", 2, 91, 52, BOX_ANY},
        {"z2", 54, 91, 52, BOX_ANY}};
    CHECK(check_correction_page("colspan past the last column",
                                table_span_overlong_page, 480, expected,
                                sizeof(expected) / sizeof(expected[0]))
          == 0);
    CHECK(check_table_row_reuse(table_span_overlong_page, expected,
                                sizeof(expected) / sizeof(expected[0]))
          == 0);
    return 0;
}

/* Six spanning cells in one row each widen their own columns by content,
   as in Chromium: 70px over 10px and 40px columns makes them 14 and 56.
   A row kept only its first four spanning cells, and the fifth and sixth
   split evenly (35/35). Boxes are Chromium's. */
static const char table_row_many_spans_page[] =
    "<!doctype html><style>body{margin:0}table{border-spacing:0}"
    "td{padding:0}b{display:block;height:10px}</style><table><tr>"
    "<td id=s0 colspan=2><b style=width:70px></b></td>"
    "<td id=s1 colspan=2><b style=width:70px></b></td>"
    "<td id=s2 colspan=2><b style=width:70px></b></td>"
    "<td id=s3 colspan=2><b style=width:70px></b></td>"
    "<td id=s4 colspan=2><b style=width:70px></b></td>"
    "<td id=s5 colspan=2><b style=width:70px></b></td>"
    "</tr><tr>"
    "<td id=c0><b style=width:10px></b></td>"
    "<td id=c1><b style=width:40px></b></td>"
    "<td id=c2><b style=width:10px></b></td>"
    "<td id=c3><b style=width:40px></b></td>"
    "<td id=c4><b style=width:10px></b></td>"
    "<td id=c5><b style=width:40px></b></td>"
    "<td id=c6><b style=width:10px></b></td>"
    "<td id=c7><b style=width:40px></b></td>"
    "<td id=c8><b style=width:10px></b></td>"
    "<td id=c9><b style=width:40px></b></td>"
    "<td id=c10><b style=width:10px></b></td>"
    "<td id=c11><b style=width:40px></b></td>"
    "</tr></table>";

static int test_table_row_many_spans(void)
{
    static const CorrectionBox expected[] = {
        {"s0", 0, 0, 70, 10}, {"s1", 70, 0, 70, 10}, {"s2", 140, 0, 70, 10},
        {"s3", 210, 0, 70, 10}, {"s4", 280, 0, 70, 10},
        {"s5", 350, 0, 70, 10}, {"c0", 0, 10, 14, 10},
        {"c1", 14, 10, 56, 10}, {"c2", 70, 10, 14, 10},
        {"c3", 84, 10, 56, 10}, {"c4", 140, 10, 14, 10},
        {"c5", 154, 10, 56, 10}, {"c6", 210, 10, 14, 10},
        {"c7", 224, 10, 56, 10}, {"c8", 280, 10, 14, 10},
        {"c9", 294, 10, 56, 10}, {"c10", 350, 10, 14, 10},
        {"c11", 364, 10, 56, 10}};
    CHECK(check_correction_page("six colspan cells in a row",
                                table_row_many_spans_page, 480, expected,
                                sizeof(expected) / sizeof(expected[0]))
          == 0);
    CHECK(check_table_row_reuse(table_row_many_spans_page, expected,
                                sizeof(expected) / sizeof(expected[0]))
          == 0);
    return 0;
}

/* Past 16 distinct spanning cells (column and span) in a table, a cell
   splits evenly over its columns; the first 16 still widen their columns
   by content, as in Chromium (120px over 10px and 30px columns makes
   them 30 and 90). Boxes are Chromium's. */
static const char table_span_limit_page[] =
    "<!doctype html><style>body{margin:0}table{border-spacing:0}"
    "td{padding:0}b{display:block;width:10px;height:10px}</style>"
    "<table><tr><td id=f0><b></b></td><td id=f1><b></b></td><td id=f2>"
    "<b></b></td><td id=f3><b style=width:30px></b></td><td id=f4><b>"
    "</b></td><td id=f5><b></b></td><td id=f6><b></b></td><td id=f7><b>"
    "</b></td><td id=f8><b></b></td><td id=f9><b></b></td><td id=f10>"
    "<b></b></td><td id=f11><b></b></td><td id=f12><b></b></td>"
    "<td id=f13><b></b></td><td id=f14><b></b></td><td id=f15><b></b>"
    "</td></tr><tr><td></td><td></td><td id=g0 colspan=2>"
    "<b style=width:120px></b></td><td colspan=2><b></b></td>"
    "<td colspan=2><b></b></td><td colspan=2><b></b></td></tr><tr><td>"
    "</td><td></td><td></td><td></td><td colspan=3><b></b></td>"
    "<td colspan=3><b></b></td><td colspan=3><b></b></td><td colspan=3>"
    "<b></b></td></tr><tr><td></td><td></td><td></td><td></td><td></td>"
    "<td></td><td></td><td></td><td></td><td></td><td colspan=2><b></b>"
    "</td><td colspan=2><b></b></td><td colspan=2><b></b></td></tr><tr>"
    "<td></td><td></td><td></td><td></td><td></td><td colspan=2><b></b>"
    "</td><td colspan=2><b></b></td><td colspan=2><b></b></td>"
    "<td colspan=2><b></b></td></tr><tr><td></td><td></td><td></td><td>"
    "</td><td></td><td></td><td></td><td></td><td></td><td></td><td>"
    "</td><td></td><td></td><td colspan=2><b></b></td></tr><tr>"
    "<td id=g1 colspan=2><b style=width:120px></b></td></tr></table>";

static int test_table_span_limit(void)
{
    static const CorrectionBox expected[] = {
        {"f0", 0, 0, 60, 10}, {"f1", 60, 0, 60, 10}, {"f2", 120, 0, 30, 10},
        {"f3", 150, 0, 90, 10}, {"f4", 240, 0, 10, 10},
        {"f5", 250, 0, 10, 10}, {"f6", 260, 0, 10, 10},
        {"f7", 270, 0, 10, 10}, {"f8", 280, 0, 10, 10},
        {"f9", 290, 0, 10, 10}, {"f10", 300, 0, 10, 10},
        {"f11", 310, 0, 10, 10}, {"f12", 320, 0, 10, 10},
        {"f13", 330, 0, 10, 10}, {"f14", 340, 0, 10, 10},
        {"f15", 350, 0, 10, 10}, {"g0", 120, 10, 120, 10},
        {"g1", 0, 60, 120, 10}};
    CHECK(check_correction_page("seventeen colspan cells in a table",
                                table_span_limit_page, 480, expected,
                                sizeof(expected) / sizeof(expected[0]))
          == 0);
    CHECK(check_table_row_reuse(table_span_limit_page, expected,
                                sizeof(expected) / sizeof(expected[0]))
          == 0);
    return 0;
}

/* A cell that spans rows and columns widens the columns by content like
   any spanning cell; a table with row spans measures every row afresh
   (no row cache). Boxes are Chromium's. */
static const char table_span_row_span_page[] =
    "<!doctype html><style>body{margin:0}table{border-spacing:0}"
    "td{padding:0}b{display:block;height:10px}</style>"
    "<table><tr><td id=r0 rowspan=2 colspan=2><b style=width:200px></b>"
    "</td><td id=r3><b style=width:20px></b></td></tr><tr><td id=r4>"
    "<b style=width:50px></b></td></tr><tr><td id=r1>"
    "<b style=width:20px></b></td><td id=r2><b style=width:60px></b>"
    "</td><td></td></tr></table>";

static int test_table_span_row_span(void)
{
    static const CorrectionBox expected[] = {
        {"r0", 0, 0, 200, 20}, {"r3", 200, 0, 50, 10},
        {"r4", 200, 10, 50, 10}, {"r1", 0, 20, 50, 10},
        {"r2", 50, 20, 150, 10}};
    CHECK(check_correction_page("colspan and rowspan cell",
                                table_span_row_span_page, 480, expected,
                                sizeof(expected) / sizeof(expected[0]))
          == 0);
    return 0;
}

/* A column no cell starts in, made by a <col> without a width, takes no
   width, as in Chromium: a span over two such columns and two of cells
   leaves the table its content's width, and a third plain <col> beside
   two cells adds nothing. Those columns kept the 8px floor (a 200px
   cell made a 216px table, the two cells a 48px one). Boxes are
   Chromium's; the cells under the border-spaced span are left out, as
   Chromium also drops the empty column's spacing (97.5px each, not 95). */
static const char table_col_only_columns_page[] =
    "<!doctype html><style>body{margin:0}table{border-spacing:0}"
    "td{padding:0}b{display:block;width:20px;height:10px}</style>"
    "<table id=ta><col><col><col span=2><tr><td id=a0 colspan=9>"
    "<b style=width:200px></b></td></tr><tr><td id=a1><b></b></td>"
    "<td id=a2><b></b></td></tr></table>"
    "<table id=tb style=border-spacing:5px><col><col><col><tr>"
    "<td id=b0 colspan=9><b style=width:200px></b></td></tr><tr>"
    "<td><b></b></td><td><b></b></td></tr></table>"
    "<table id=td><col><col><col><tr><td id=d1><b></b></td>"
    "<td id=d2><b></b></td></tr></table>";

static int test_table_col_only_columns(void)
{
    static const CorrectionBox expected[] = {
        {"ta", 0, 0, 200, 20}, {"a0", 0, 0, 200, 10},
        {"a1", 0, 10, 100, 10}, {"a2", 100, 10, 100, 10},
        {"tb", 0, 20, 210, 35}, {"b0", 5, 25, 200, 10},
        {"td", 0, 55, 40, 10}, {"d1", 0, 55, 20, 10},
        {"d2", 20, 55, 20, 10}};
    CHECK(check_correction_page("columns only a col makes",
                                table_col_only_columns_page, 480, expected,
                                sizeof(expected) / sizeof(expected[0]))
          == 0);
    CHECK(check_table_row_reuse(table_col_only_columns_page, expected,
                                sizeof(expected) / sizeof(expected[0]))
          == 0);
    return 0;
}

/* An empty cell's column has no content to take a spanning cell's width
   by, as in Chromium: a spanning cell over it and a column no cell
   covers alone splits evenly. The 8px floor every column keeps counted
   as content, so the empty column took all 120px and the other none
   (128/8 where Chromium has 60/60). Boxes are Chromium's. */
static const char table_span_empty_page[] =
    "<!doctype html><style>body{margin:0}table{border-spacing:0}"
    "td{padding:0}b{display:block;height:10px}</style>"
    "<table><tr><td id=e12 colspan=2><b style=width:120px></b></td>"
    "<td id=e3><b style=width:40px></b></td></tr><tr><td id=e1></td>"
    "<td id=e23 colspan=2><b style=width:20px></b></td></tr></table>";

static int test_table_span_empty_column(void)
{
    static const CorrectionBox expected[] = {
        {"e12", 0, 0, 120, 10}, {"e3", 120, 0, 40, 10},
        {"e1", 0, 10, 60, BOX_ANY}, {"e23", 60, 10, 100, 10}};
    CHECK(check_correction_page("colspan over an empty column",
                                table_span_empty_page, 480, expected,
                                sizeof(expected) / sizeof(expected[0]))
          == 0);
    CHECK(check_table_row_reuse(table_span_empty_page, expected,
                                sizeof(expected) / sizeof(expected[0]))
          == 0);
    return 0;
}

/* A <col> or <colgroup> that spans columns gives each of them its width,
   and a colgroup's width is the width of each of its cols that has none
   of its own, as in HTML's column model and Chromium; auto and fixed
   table layouts alike. The width was divided over the spanned columns
   (col span=2 width:100px made two 50px columns). Boxes are
   Chromium's. */
static const char table_col_span_page[] =
    "<!doctype html><style>body{margin:0}table{border-spacing:0}"
    "td{padding:0}b{display:block;width:20px;height:10px}</style>"
    "<table><col span=2 style=width:100px><tr><td id=a1><b></b></td>"
    "<td id=a2><b></b></td><td id=a3><b></b></td></tr></table>"
    "<table><colgroup span=2 style=width:100px></colgroup><tr>"
    "<td id=b1><b></b></td><td id=b2><b></b></td><td id=b3><b></b></td>"
    "</tr></table>"
    "<table><colgroup style=width:100px><col style=width:30px><col>"
    "</colgroup><tr><td id=c1><b></b></td><td id=c2><b></b></td>"
    "<td id=c3><b></b></td></tr></table>"
    "<table style=width:400px><col span=2 style=width:25%><tr>"
    "<td id=d1><b></b></td><td id=d2><b></b></td><td id=d3><b></b></td>"
    "</tr></table>"
    "<table><col span=2 width=60><tr><td id=e1><b></b></td>"
    "<td id=e2><b></b></td><td id=e3><b></b></td></tr></table>"
    "<table style=table-layout:fixed;width:400px>"
    "<col span=2 style=width:100px><tr><td id=f1><b></b></td>"
    "<td id=f2><b></b></td><td id=f3><b></b></td></tr></table>";
#define TABLE_COL_SPAN_EXPECTED \
    {"a1", 0, 0, 100, 10}, {"a2", 100, 0, 100, 10}, \
    {"a3", 200, 0, 20, 10}, \
    {"b1", 0, 10, 100, 10}, {"b2", 100, 10, 100, 10}, \
    {"b3", 200, 10, 20, 10}, \
    {"c1", 0, 20, 30, 10}, {"c2", 30, 20, 100, 10}, \
    {"c3", 130, 20, 20, 10}, \
    {"d1", 0, 30, 100, 10}, {"d2", 100, 30, 100, 10}, \
    {"d3", 200, 30, 200, 10}, \
    {"e1", 0, 40, 60, 10}, {"e2", 60, 40, 60, 10}, \
    {"e3", 120, 40, 20, 10}, \
    {"f1", 0, 50, 100, 10}, {"f2", 100, 50, 100, 10}, \
    {"f3", 200, 50, 200, 10}

static int test_table_col_span_width(void)
{
    static const CorrectionBox expected[] = {TABLE_COL_SPAN_EXPECTED};
    CHECK(check_correction_page("col span widths", table_col_span_page,
                                480, expected,
                                sizeof(expected) / sizeof(expected[0]))
          == 0);
    CHECK(check_table_row_reuse(table_col_span_page, expected,
                                sizeof(expected) / sizeof(expected[0]))
          == 0);
    return 0;
}

/* A separated-borders HTML table keeps one vertical border-spacing below
   its last row, as above each row; a caption below the table follows it.
   It ended at the last row. Collapsed borders have no spacing. Boxes are
   Chromium's. */
static int test_table_bottom_spacing(void)
{
    CHECK_CORRECTION("table bottom border-spacing",
        "<!doctype html><style>body{margin:0}table{width:300px}"
        "td{padding:0}i{display:block;width:20px;height:10px}</style>"
        "<table id=one style=border-spacing:10px><tr><td id=c1><i></i></td>"
        "</tr></table><div id=after1 style=height:5px></div>"
        "<table id=two style='border-spacing:4px 6px'><tr><td id=c2><i></i>"
        "</td></tr><tr><td id=c3><i></i></td></tr></table>"
        "<div id=after2 style=height:5px></div>"
        "<table id=three style='border-spacing:10px;border:2px solid;"
        "padding:3px'><tbody><tr><td id=c4><i></i></td></tr></tbody></table>"
        "<div id=after3 style=height:5px></div>"
        "<table id=four style='border-spacing:10px;border-collapse:collapse'>"
        "<tr><td id=c5><i></i></td></tr></table>"
        "<div id=after4 style=height:5px></div>"
        "<table id=five style=border-spacing:8px><caption id=cap "
        "style=caption-side:bottom><i></i></caption><tr><td id=c6><i></i>"
        "</td></tr></table><div id=after5 style=height:5px></div>", 480,
        {"one", 0, 0, 300, 30}, {"c1", 10, 10, 280, 10},
        {"after1", 0, 30, BOX_ANY, 5},
        {"two", 0, 35, 300, 38}, {"c2", 4, 41, 292, 10},
        {"c3", 4, 57, 292, 10}, {"after2", 0, 73, BOX_ANY, 5},
        {"three", 0, 78, 300, 40}, {"c4", 15, 93, 270, 10},
        {"after3", 0, 118, BOX_ANY, 5},
        {"four", 0, 123, 300, 10}, {"c5", 0, 123, 300, 10},
        {"after4", 0, 133, BOX_ANY, 5},
        {"five", 0, 138, 300, 36}, {"c6", 8, 146, 284, 10},
        {"cap", 0, 164, 300, 10}, {"after5", 0, 174, BOX_ANY, 5});
    return 0;
}

/* The bottom border-spacing follows the row placed last, in group order
   (head, body, foot), not the last row in the source: a <tfoot> or a
   <thead> written before or after the body moves with its group, and a
   bottom caption still follows the spacing. The spacing went after the
   last source row, which left the foot flush with the table's bottom
   edge. Boxes are Chromium's. */
static int test_table_group_order_spacing(void)
{
    CHECK_CORRECTION("table bottom spacing in group order",
        "<!doctype html><style>body{margin:0}td{padding:0}"
        "i{display:block;width:20px;height:12px}</style>"
        "<table id=t1 style='border-spacing:4px 7px'><thead><tr><td id=h1>"
        "<i></i></td></tr></thead><tfoot><tr><td id=f1><i></i></td></tr>"
        "</tfoot><tbody><tr><td id=b1><i></i></td></tr></tbody></table>"
        "<div id=after1 style=height:5px></div>"
        "<table id=t2 style='border-spacing:4px 7px'><tbody><tr><td id=b2>"
        "<i></i></td></tr></tbody><thead><tr><td id=h2><i></i></td></tr>"
        "</thead></table><div id=after2 style=height:5px></div>"
        "<table id=t3 style='border-spacing:4px 7px'><caption id=cap3 "
        "style=caption-side:bottom><i></i></caption><tfoot><tr><td id=f3>"
        "<i></i></td></tr></tfoot><tbody><tr><td id=b3><i></i></td></tr>"
        "</tbody></table><div id=after3 style=height:5px></div>", 480,
        {"t1", 0, 0, 28, 64}, {"h1", 4, 7, 20, 12}, {"b1", 4, 26, 20, 12},
        {"f1", 4, 45, 20, 12}, {"after1", 0, 64, BOX_ANY, 5},
        {"t2", 0, 69, 28, 45}, {"h2", 4, 76, 20, 12}, {"b2", 4, 95, 20, 12},
        {"after2", 0, 114, BOX_ANY, 5},
        {"t3", 0, 119, 28, 57}, {"b3", 4, 126, 20, 12},
        {"f3", 4, 145, 20, 12}, {"cap3", 0, 164, 28, 12},
        {"after3", 0, 176, BOX_ANY, 5});
    return 0;
}

/* A CSS table row whose cells all have widths gives its leftover width to
   the fixed cells in proportion to their widths, or with none to the
   percentage cells in proportion to their percentages, as Chromium does.
   It was left empty unless the row had an auto cell. A shrink-wrapped
   table, and cells under an ordinary block, keep their widths. */
static int test_table_cell_leftover_width(void)
{
    CHECK_CORRECTION("leftover width without auto cells",
        "<!doctype html><style>body{margin:0}"
        ".t{display:table;width:300px}.c{display:table-cell}"
        "i{display:inline-block;width:20px;height:10px}</style>"
        "<div class=t><div id=half class=c style='width:50%;padding:0 50px'>"
        "<i></i></div></div>"
        "<div class=t><div id=p1 class=c style=width:30%><i></i></div>"
        "<div id=p2 class=c style=width:30%><i></i></div></div>"
        "<div class=t><div id=f1 class=c style=width:100px><i></i></div>"
        "<div id=f2 class=c style=width:50px><i></i></div></div>"
        "<div class=t><div id=mf class=c style=width:100px><i></i></div>"
        "<div id=mp class=c style=width:20%><i></i></div></div>"
        "<div class=t><div id=q1 class=c style=width:10%><i></i></div>"
        "<div id=q2 class=c style=width:30%><i></i></div></div>"
        "<div class=t><div id=b1 class=c style='width:100px;padding:0 10px'>"
        "<i></i></div><div id=b2 class=c style='width:50px;"
        "box-sizing:border-box;padding:0 10px'><i></i></div></div>"
        "<div class=t><div id=fa class=c style=width:100px><i></i></div>"
        "<div id=aa class=c><i></i></div></div>"
        "<div class=t style=width:auto><div id=s1 class=c style=width:100px>"
        "<i></i></div><div id=s2 class=c style=width:50px><i></i></div></div>"
        "<div style=width:300px><div id=o1 class=c style=width:100px><i></i>"
        "</div><div id=o2 class=c style=width:50px><i></i></div></div>", 480,
        {"half", 0, BOX_ANY, 300, BOX_ANY},
        {"p1", 0, BOX_ANY, 150, BOX_ANY}, {"p2", 150, BOX_ANY, 150, BOX_ANY},
        {"f1", 0, BOX_ANY, 200, BOX_ANY}, {"f2", 200, BOX_ANY, 100, BOX_ANY},
        {"mf", 0, BOX_ANY, 240, BOX_ANY}, {"mp", 240, BOX_ANY, 60, BOX_ANY},
        {"q1", 0, BOX_ANY, 75, BOX_ANY}, {"q2", 75, BOX_ANY, 225, BOX_ANY},
        {"b1", 0, BOX_ANY, 211, BOX_ANY}, {"b2", 211, BOX_ANY, 89, BOX_ANY},
        {"fa", 0, BOX_ANY, 100, BOX_ANY}, {"aa", 100, BOX_ANY, 200, BOX_ANY},
        {"s1", 0, BOX_ANY, 100, BOX_ANY}, {"s2", 100, BOX_ANY, 50, BOX_ANY},
        {"o1", 0, BOX_ANY, 100, BOX_ANY}, {"o2", 100, BOX_ANY, 50, BOX_ANY});
    return 0;
}

/* border-spacing of a display:table box whose cells form its anonymous row
   surrounds and separates the cells, as in an explicit row; it was
   ignored. Collapsed borders still have none. */
static int test_css_table_anonymous_row_spacing(void)
{
    CHECK_CORRECTION("anonymous row border-spacing",
        "<!doctype html><style>body{margin:0}"
        ".t{display:table;width:300px;border-spacing:10px}"
        ".c{display:table-cell}b{display:block;width:20px;height:10px}"
        "</style>"
        "<div class=t id=t1><div id=full class=c style=width:100%><b></b>"
        "</div></div>"
        "<div class=t id=t2><div id=a1 class=c><b></b></div>"
        "<div id=a2 class=c><b></b></div></div>"
        "<div class=t id=t3 style='border-spacing:10px 5px'>"
        "<div id=fx class=c style=width:100px><b></b></div>"
        "<div id=tall class=c><b style=height:30px></b></div></div>"
        "<div class=t id=t4 style=border-collapse:collapse>"
        "<div id=k1 class=c><b></b></div><div id=k2 class=c><b></b></div>"
        "</div>"
        "<div class=t id=t5 style='padding:5px;border:2px solid'>"
        "<div id=pad class=c><b></b></div></div>"
        "<div class=t id=t6 style=width:auto><div id=w1 class=c "
        "style=width:50px><b></b></div><div id=w2 class=c style=width:50px>"
        "<b></b></div></div>"
        "<div class=t id=t7><div id=n1 class=c style=width:200px><b></b>"
        "</div><div id=n2 class=c style=width:200px><b></b></div></div>",
        480,
        {"t1", 0, 0, 300, 30}, {"full", 10, 10, 280, 10},
        {"t2", 0, 30, 300, 30}, {"a1", 10, 40, 135, 10},
        {"a2", 155, 40, 135, 10},
        {"t3", 0, 60, 300, 40}, {"fx", 10, 65, 100, 30},
        {"tall", 120, 65, 170, 30},
        {"t4", 0, 100, 300, 10}, {"k1", 0, 100, 150, 10},
        {"k2", 150, 100, 150, 10},
        {"t5", 0, 110, 314, 44}, {"pad", 17, 127, 280, 10},
        {"t6", 0, 154, 130, 30}, {"w1", 10, 164, 50, 10},
        {"w2", 70, 164, 50, 10},
        {"t7", 0, 184, 300, 30}, {"n1", 10, 194, 135, 10},
        {"n2", 155, 194, 135, 10});
    return 0;
}

/* A CSS table row with room to spare grows its auto cells in proportion
   to their max-content widths, and a fixed cell whose content is wider
   than its width takes the content's width, as an HTML table and
   Chromium do. The leftover was split evenly and the fixed width held.
   Boxes are Chromium's. */
static int test_table_cell_auto_growth(void)
{
    CHECK_CORRECTION("CSS table auto cells grow by max-content",
        "<!doctype html><style>body{margin:0}"
        ".t{display:table;width:300px}.c{display:table-cell}"
        "i{display:block;width:20px;height:10px}</style>"
        "<div class=t><div id=a1 class=c><i></i></div><div id=b1 class=c>"
        "<i style=width:60px></i></div></div>"
        "<div class=t><div id=a2 class=c style=width:100px><i></i></div>"
        "<div id=b2 class=c><i></i></div></div>"
        "<div class=t style=border-spacing:10px><div id=a3 class=c><i></i>"
        "</div><div id=b3 class=c><i></i></div></div>"
        "<div class=t><div id=a4 class=c style=width:100px>"
        "<i style=width:250px></i></div><div id=b4 class=c><i></i></div>"
        "</div>", 480,
        {"a1", 0, BOX_ANY, 75, BOX_ANY}, {"b1", 75, BOX_ANY, 225, BOX_ANY},
        {"a2", 0, BOX_ANY, 100, BOX_ANY}, {"b2", 100, BOX_ANY, 200, BOX_ANY},
        {"a3", 10, BOX_ANY, 135, BOX_ANY}, {"b3", 155, BOX_ANY, 135, BOX_ANY},
        {"a4", 0, BOX_ANY, 250, BOX_ANY}, {"b4", 250, BOX_ANY, 50, BOX_ANY});
    return 0;
}

/* A CSS table cell's percentage width sets its border box, padding and
   borders included, as in Chromium; it is still raised to the cell's
   content minimum. The padding was added on top, so 80% and 40% cells with
   10px padding came out 226/74 instead of 240/60. Boxes are Chromium's. */
static int test_table_cell_percent_border_box(void)
{
    CHECK_CORRECTION("percentage cells include their padding",
        "<!doctype html><style>body{margin:0}"
        ".t{display:table;width:300px}.c{display:table-cell}"
        "i{display:block;width:20px;height:10px}</style>"
        "<div class=t><div id=a1 class=c style='width:80%;padding:0 10px'>"
        "<i></i></div><div id=b1 class=c style='width:40%;padding:0 10px'>"
        "<i></i></div></div>"
        "<div class=t><div id=a2 class=c style='width:50%;padding:0 20px'>"
        "<i></i></div><div id=b2 class=c><i></i></div></div>"
        "<div class=t><div id=a3 class=c style='width:50%;padding:0 20px;"
        "box-sizing:border-box'><i></i></div><div id=b3 class=c><i></i>"
        "</div></div>"
        "<div class=t><div id=a4 class=c style='width:30%;border:5px solid'>"
        "<i></i></div><div id=b4 class=c><i></i></div></div>"
        "<div class=t><div id=a5 class=c style='width:10%;padding:0 30px'>"
        "<i></i></div><div id=b5 class=c><i></i></div></div>", 480,
        {"a1", 0, BOX_ANY, 240, BOX_ANY}, {"b1", 240, BOX_ANY, 60, BOX_ANY},
        {"a2", 0, BOX_ANY, 150, BOX_ANY}, {"b2", 150, BOX_ANY, 150, BOX_ANY},
        {"a3", 0, BOX_ANY, 150, BOX_ANY}, {"b3", 150, BOX_ANY, 150, BOX_ANY},
        {"a4", 0, BOX_ANY, 90, BOX_ANY}, {"b4", 90, BOX_ANY, 210, BOX_ANY},
        {"a5", 0, BOX_ANY, 80, BOX_ANY}, {"b5", 80, BOX_ANY, 220, BOX_ANY});
    return 0;
}

/* Percentage cells of a CSS table row past 100% are cut to what is left
   (none, for a second width:100% cell), and a row too narrow for its
   percentages grows the percentage cells from their content minimums
   toward their percentages before any fixed cell grows, as Chromium does.
   Two width:100% cells split the row evenly before. */
static int test_table_cell_percent_overflow(void)
{
    CHECK_CORRECTION("percentages past 100%",
        "<!doctype html><style>body{margin:0}"
        ".t{display:table;width:300px}.c{display:table-cell}"
        "b{display:block;width:20px;height:10px}</style>"
        "<div class=t><div id=a1 class=c style=width:100%><b></b></div>"
        "<div id=a2 class=c style=width:100%><b></b></div></div>"
        "<div class=t><div id=b1 class=c style=width:60%><b></b></div>"
        "<div id=b2 class=c style=width:60%><b></b></div></div>"
        "<div class=t><div id=c1 class=c style=width:70%><b></b></div>"
        "<div id=c2 class=c style=width:50%><b></b></div>"
        "<div id=c3 class=c><b></b></div></div>"
        "<div class=t><div id=d1 class=c style=width:100%><b></b></div>"
        "<div id=d2 class=c style=width:50px><b></b></div></div>"
        "<div class=t><div id=f1 class=c style=width:100%>"
        "<b style=width:50px></b></div><div id=f2 class=c style=width:100%>"
        "<b style=width:60px></b></div></div>"
        "<div class=t style=border-spacing:10px><div id=g1 class=c "
        "style=width:100%><b></b></div><div id=g2 class=c style=width:100%>"
        "<b></b></div></div>", 480,
        {"a1", 0, BOX_ANY, 280, BOX_ANY}, {"a2", 280, BOX_ANY, 20, BOX_ANY},
        {"b1", 0, BOX_ANY, 180, BOX_ANY}, {"b2", 180, BOX_ANY, 120, BOX_ANY},
        {"c1", 0, BOX_ANY, 195, BOX_ANY}, {"c2", 195, BOX_ANY, 85, BOX_ANY},
        {"c3", 280, BOX_ANY, 20, BOX_ANY},
        {"d1", 0, BOX_ANY, 280, BOX_ANY}, {"d2", 280, BOX_ANY, 20, BOX_ANY},
        {"f1", 0, BOX_ANY, 240, BOX_ANY}, {"f2", 240, BOX_ANY, 60, BOX_ANY},
        {"g1", 10, BOX_ANY, 250, BOX_ANY}, {"g2", 270, BOX_ANY, 20, BOX_ANY});
    return 0;
}

/* A row-spanning item is sized after the single-row items: its excess
   height goes to the spanned flexible rows when it has any, else to its
   rows without content of their own, else to all its content-sized rows,
   never to its first row alone. A first row that took it overlapped the
   next row and stretched its other items to the span. */
static int test_grid_row_span_sizing(void)
{
    CHECK_CORRECTION("span into a trailing fr row",
        "<!doctype html><style>body{margin:0;font:16px/20px sans-serif}"
        ".g{display:grid;column-gap:20px;width:440px;"
        "grid-template-columns:repeat(12,1fr);"
        "grid-template-rows:repeat(5,auto) 1fr}"
        ".g>*{grid-column:1/span 8;margin:0}"
        "#side{grid-column:9/span 4;grid-row:1/-1;height:100px}"
        "#top{display:flex;align-items:center;height:auto;margin-bottom:8px}"
        "#mid{height:40px}#low{height:20px}</style>"
        "<div class=g><div id=top><span>a</span></div><div id=mid></div>"
        "<div id=side></div><div id=low></div></div>"
        "<div id=after style=height:10px></div>", 480,
        {"top", 0, 0, 288, 20}, {"mid", 0, 28, 288, 40},
        {"low", 0, 68, 288, 20}, {"side", 308, 0, 132, 100},
        {"after", 0, 100, 480, 10});
    CHECK_CORRECTION("span shares auto rows",
        "<!doctype html><style>body{margin:0}"
        ".g{display:grid;grid-template-columns:100px 100px;row-gap:4px}"
        "#a{grid-row:1/3;grid-column:1;height:200px}"
        "#b{grid-column:2;height:30px}#c{grid-column:2;height:20px}"
        "#d{grid-column:1/3;height:10px}</style>"
        "<div class=g><div id=a></div><div id=b></div><div id=c></div>"
        "<div id=d></div></div>", 480,
        {"a", 0, 0, 100, 200}, {"b", 100, 0, 100, 30},
        {"c", 100, 107, 100, 20}, {"d", 0, 204, 200, 10});
    CHECK_CORRECTION("auto-placed span",
        "<!doctype html><style>body{margin:0}"
        ".g{display:grid;grid-template-columns:100px 100px;row-gap:4px}"
        "#h{grid-row:span 2;height:90px}</style>"
        "<div class=g><div id=p style=height:20px></div><div id=h></div>"
        "<div id=q style=height:10px></div><div id=r style=height:10px></div>"
        "</div>", 480,
        {"p", 0, 0, 100, 20}, {"h", 100, 0, 100, 90},
        {"q", 0, 52, 100, 10}, {"r", 0, 94, 100, 10});
    CHECK_CORRECTION("overlapping spans by area",
        "<!doctype html><style>body{margin:0}"
        ".g{display:grid;width:400px;grid-template-rows:auto auto 1fr;"
        "grid-template-areas:'a s' 'l s' 'l t' 'f f'}"
        "#a{grid-area:a;height:100px}#s{grid-area:s;height:200px}"
        "#l{grid-area:l;height:150px}#t{grid-area:t;height:50px}"
        "#f{grid-area:f;height:57px}</style>"
        "<div class=g><div id=a></div><div id=s></div><div id=l></div>"
        "<div id=t></div><div id=f></div></div>", 480,
        {"a", 0, 0, 200, 100}, {"s", 200, 0, 200, 200},
        {"l", 0, 100, 200, 150}, {"t", 200, 200, 200, 50},
        {"f", 0, 250, 400, 57});
    return 0;
}

/* Native and CSS explicit rows describe the same grid. Chromium gives
   the two tables identical cell boxes, including after the lead width
   changes. A colspan on a div is inert. Check actual paint and hit routing
   as well as geometry, without relying on the reuse path as its own oracle. */
static int test_table_track_equivalence(void)
{
    char page[4096];
    FILE *f = fopen(TILEFINCH_TEST_SOURCE_DIR "/tests/fixtures/table-track-equivalence.html", "rb");
    CHECK(f != NULL);
    size_t length = fread(page, 1, sizeof(page), f);
    bool read_ok = length < sizeof(page) && feof(f) && !ferror(f);
    fclose(f);
    CHECK(read_ok);
    Budget budget;
    budget_init(&budget, 16u * MIB);
    CHECK(budget_install_lexbor(&budget));
    PocDocument doc = {0};
    Stylesheet sheet = {0};
    CHECK(document_parse(&doc, &budget, page, length, 17)
          && stylesheet_build(&sheet, &budget, &doc, 240));
    LayoutReuseCache *reuse = layout_reuse_cache_create(&budget);
    CHECK(reuse != NULL);
    lxb_dom_node_t *root = lxb_dom_interface_node(doc.html);
    static const int widths[] = {240,240,300,240};
    static const int leads[] = {68,92,92,68};
    uint16_t pixels[300*100];
    uint16_t cold[300*49];
    for (size_t step = 0; step < 4; step++) {
        int w = widths[step], lead = leads[step];
        if (step == 1 || step == 3) {
            const char *value = step == 1 ? "width:92px" : "width:68px";
            for (int css = 0; css < 2; css++) {
                lxb_dom_node_t *n = find_id(root, css ? "c0" : "h0");
                CHECK(n && lxb_dom_element_set_attribute(lxb_dom_interface_element(n),
                    (const lxb_char_t *)"style", 5, (const lxb_char_t *)value, strlen(value)));
                layout_reuse_cache_invalidate_inline_style(reuse, n, "width", false);
            }
        }
        ViewportContext vp;
        CHECK(viewport_context_init(&vp, w, 100, w, 100));
        for (unsigned pass = 0; pass < 3; pass++) {
            LayoutDocument l = {0};
            LayoutReuseStats before, after;
            layout_reuse_cache_stats(reuse, &before);
            CHECK(layout_build_context_reuse(&l, &budget, &doc, &sheet, NULL,
                  NULL, &vp, pass ? reuse : NULL));
            layout_reuse_cache_stats(reuse, &after);
            if (pass == 2) CHECK(after.table_row_hits >= before.table_row_hits + 4);
            TileCache tiles = {0};
            CHECK(tile_cache_init(&tiles, &budget, &l, 8)
                  && tile_cache_set_frame(&tiles, pixels, (size_t) w*100)
                  && tile_cache_render_frame(&tiles, 0, w, 100, NULL));
            for (int css = 0; css < 2; css++) {
                int top = css * 49;
                char id[] = {css ? 'c' : 'h','0',0};
                for (int i = 0; i < 4; i++) {
                    id[1] = (char) ('0' + i);
                    const LayoutNodeBox *b = layout_box_for_node(&l, find_id(root,id));
                    int x = i%2 ? 8+lead : 4, y = top + (i<2 ? 3 : 26);
                    int cw = i%2 ? w-12-lead : lead;
                    if (!b || b->x != x || b->y != y || b->width != cw || b->height != 20)
                        fprintf(stderr,"table equivalence step=%zu pass=%u %s got %d,%d %dx%d want %d,%d %dx20\n",
                            step,pass,id,b?b->x:-1,b?b->y:-1,b?b->width:-1,b?b->height:-1,x,y,cw);
                    CHECK(b && b->x == x && b->y == y && b->width == cw && b->height == 20);
                    CHECK(tilefinch_rgb565_red_code(pixels[(y+5)*w+x]) == 31
                          && tilefinch_rgb565_green_code(pixels[(y+5)*w+x]) == 0);
                }
                const LinkRegion *hit = layout_link_at(&l, lead+16, top+34);
                id[1]='a';
                CHECK(hit && hit->node == find_id(root,id)
                      && !layout_link_at(&l, lead+6, top+34));
            }
            CHECK(memcmp(pixels, pixels+w*49, (size_t)w*49*sizeof(*pixels)) == 0);
            if (pass == 0) memcpy(cold, pixels, (size_t)w*49*sizeof(*pixels));
            else CHECK(memcmp(cold, pixels, (size_t)w*49*sizeof(*pixels)) == 0);
            tile_cache_destroy(&tiles);
            layout_destroy(&l);
        }
    }
    layout_reuse_cache_destroy(reuse);
    stylesheet_destroy(&sheet);
    document_destroy(&doc);
    CHECK(budget.current == 0 && budget_uninstall_lexbor(&budget));
    CHECK_CORRECTION("nested table restores outer column widths",
        "<style>body{margin:0}table,.t{border-spacing:0;width:240px}"
        ".t{display:table;width:60px}.r{display:table-row}.c,td{display:table-cell;"
        "padding:0;height:10px;vertical-align:top}</style>"
        "<table><tr><td style='width:100px'><div class=t><div class=r>"
        "<div class=c id=n1 style='width:20px'></div><div class=c id=n2></div>"
        "</div></div></td><td id=o1></td></tr><tr><td id=o2></td>"
        "<td id=o3></td></tr></table>", 240,
        {"n1",0,BOX_ANY,20,BOX_ANY}, {"n2",20,BOX_ANY,40,BOX_ANY},
        {"o1",100,BOX_ANY,140,BOX_ANY}, {"o2",0,BOX_ANY,100,BOX_ANY},
        {"o3",100,BOX_ANY,140,BOX_ANY});
    return 0;
}

static int test_layout_corrections(void)
{
    CHECK(test_table_track_equivalence() == 0);
    puts("test: positioned covers resolve against auto-height blocks");
    CHECK(test_positioned_cover_auto_height() == 0);
    puts("test: positioned replaced elements keep their size under insets");
    CHECK(test_positioned_replaced_insets() == 0);
    puts("test: fixed table cell widths yield to a narrower table");
    CHECK(test_table_cell_fixed_width_squeeze() == 0);
    puts("test: fixed td widths yield to a narrower HTML table");
    CHECK(test_table_html_fixed_width_squeeze() == 0);
    puts("test: roomy tables grow columns by the CSS Tables 3 guesses");
    CHECK(test_table_column_guesses() == 0);
    puts("test: HTML table percentages past 100% are cut");
    CHECK(test_table_percent_cap() == 0);
    puts("test: HTML table fixed colspan cells leave their columns auto");
    CHECK(test_table_span_fixed_width() == 0);
    puts("test: HTML table colspan cells spread by column maximums");
    CHECK(test_table_span_distribution() == 0);
    CHECK(test_table_span_row_order() == 0);
    puts("test: HTML table percentage colspan cells follow column maximums");
    CHECK(test_table_span_percent() == 0);
    CHECK(test_table_span_percent_free() == 0);
    CHECK(test_table_span_overlong() == 0);
    CHECK(test_table_row_many_spans() == 0);
    CHECK(test_table_span_limit() == 0);
    CHECK(test_table_span_row_span() == 0);
    CHECK(test_table_col_only_columns() == 0);
    puts("test: HTML table empty columns take no share of a colspan cell");
    CHECK(test_table_span_empty_column() == 0);
    puts("test: HTML table col and colgroup spans give each column the width");
    CHECK(test_table_col_span_width() == 0);
    puts("test: HTML tables keep border-spacing below the last row");
    CHECK(test_table_bottom_spacing() == 0);
    puts("test: HTML table bottom spacing follows the group order");
    CHECK(test_table_group_order_spacing() == 0);
    puts("test: CSS table rows without auto cells hand out leftover width");
    CHECK(test_table_cell_leftover_width() == 0);
    puts("test: CSS table anonymous rows keep border-spacing");
    CHECK(test_css_table_anonymous_row_spacing() == 0);
    puts("test: CSS table auto cells grow by their max-content widths");
    CHECK(test_table_cell_auto_growth() == 0);
    puts("test: CSS table percentage cells include their padding");
    CHECK(test_table_cell_percent_border_box() == 0);
    puts("test: CSS table percentages past 100% are cut");
    CHECK(test_table_cell_percent_overflow() == 0);
    puts("test: grid row spans size after their single-row items");
    CHECK(test_grid_row_span_sizing() == 0);
    puts("test: shrinking flex items with huge bases share the container");
    CHECK(test_flex_huge_shrink() == 0);
    puts("test: bare fr tracks keep automatic minimums");
    CHECK(test_fr_automatic_minimums() == 0);
    puts("test: content-keyword flex bases in a planned column");
    CHECK(test_column_content_flex_basis() == 0);
    puts("test: @supports accepts var() for retained sparse properties");
    CHECK(test_supports_sparse_var() == 0);
    puts("test: unplaced rowspan cells still stretch to their row");
    CHECK(test_table_unplaced_rowspan_cell() == 0);
    puts("test: table rowspans count rendered rows");
    CHECK(test_table_rowspan_rendered_rows() == 0);
    puts("test: numbered grid line names include implicit area lines");
    CHECK(test_grid_numbered_line_names() == 0);
    puts("test: auto-fit collapse stays inside the capped track count");
    CHECK(test_auto_fit_collapse_at_track_cap() == 0);
    puts("test: large pixel grid floors stay pixel floors");
    CHECK(test_grid_large_pixel_floor() == 0);
    puts("test: rtl grid items align to the inline start of their areas");
    CHECK(test_rtl_grid_item_alignment() == 0);
    puts("test: relative inline offsets do not grow the line box");
    CHECK(test_relative_inline_line_height() == 0);
    puts("test: stretched column flex keeps logical justify-content");
    CHECK(test_stretched_column_justify() == 0);
    puts("test: grid unsafe center aligns completed auto rows");
    CHECK(test_grid_unsafe_center_auto_rows() == 0);
    return 0;
}
