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

static int test_layout_corrections(void)
{
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
