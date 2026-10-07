/* News-site compatibility repros. Each test loads a page from
   tests/fixtures/news-compat/, which reduces one engine gap found on CNN,
   BBC News or The New York Times to a few elements, and checks the layout
   a standards browser produces for it. */

typedef struct {
    Budget budget;
    PocDocument document;
    Stylesheet sheet;
    ImageResources images;
    LayoutDocument layout;
    lxb_dom_node_t *root;
    bool document_ready;
    bool sheet_ready;
    bool layout_ready;
} NewsFixture;

static char *news_fixture_read(const char *name, size_t *length)
{
    char path[512];
    snprintf(path, sizeof(path), "%s/tests/fixtures/news-compat/%s",
             TILEFINCH_TEST_SOURCE_DIR, name);
    FILE *file = fopen(path, "rb");
    if (file == NULL) return NULL;
    char *data = NULL;
    if (fseek(file, 0, SEEK_END) == 0) {
        long size = ftell(file);
        if (size > 0 && size < 65536 && fseek(file, 0, SEEK_SET) == 0) {
            data = malloc((size_t) size + 1u);
            if (data != NULL
                && fread(data, 1, (size_t) size, file) == (size_t) size) {
                data[size] = '\0';
                *length = (size_t) size;
            } else {
                free(data);
                data = NULL;
            }
        }
    }
    fclose(file);
    return data;
}

static void news_fixture_destroy(NewsFixture *fixture)
{
    if (fixture->layout_ready) layout_destroy(&fixture->layout);
    images_destroy(&fixture->images);
    if (fixture->sheet_ready) stylesheet_destroy(&fixture->sheet);
    if (fixture->document_ready) document_destroy(&fixture->document);
    fixture->layout_ready = false;
    fixture->sheet_ready = false;
    fixture->document_ready = false;
}

static bool news_fixture_load_with_images(NewsFixture *fixture,
                                          const char *name,
                                          bool load_images)
{
    *fixture = (NewsFixture) {0};
    budget_init(&fixture->budget, 8u * MIB);
    budget_install_lexbor(&fixture->budget);
    size_t length = 0;
    char *html = news_fixture_read(name, &length);
    if (html == NULL) {
        fprintf(stderr, "news fixture %s is missing\n", name);
        return false;
    }
    fixture->document_ready = document_parse(
        &fixture->document, &fixture->budget, html, length, 4096);
    free(html);
    if (!fixture->document_ready) return false;
    fixture->sheet_ready = stylesheet_build(
        &fixture->sheet, &fixture->budget, &fixture->document, 480);
    if (!fixture->sheet_ready) return false;
    /* Inline SVG is rasterized by the image pass; the fixture has no
       external resources, so the base URL is never fetched. */
    if (load_images
        && !images_load_external(
               &fixture->document, &fixture->sheet, &fixture->images,
               &fixture->budget, "https://news-compat.test/",
               "https://news-compat.test/", NULL, 8, 64 * 1024,
               32 * 1024, 256 * 1024, 1000, NULL, NULL)) return false;
    fixture->layout_ready = layout_build(
        &fixture->layout, &fixture->budget, &fixture->document,
        &fixture->sheet, NULL, load_images ? &fixture->images : NULL, 480);
    fixture->root = lxb_dom_interface_node(fixture->document.html);
    return fixture->layout_ready;
}

static bool news_fixture_load(NewsFixture *fixture, const char *name)
{
    return news_fixture_load_with_images(fixture, name, false);
}

/* The index'th element (document order, zero-based) whose class attribute
   is exactly `wanted`. */
static lxb_dom_node_t *news_nth_class(lxb_dom_node_t *node,
                                      const char *wanted, int *index)
{
    for (; node != NULL; node = node->next) {
        size_t length = 0;
        const char *value = document_attribute(node, "class", &length);
        if (value != NULL && length == strlen(wanted)
            && memcmp(value, wanted, length) == 0) {
            if (*index == 0) return node;
            (*index)--;
        }
        lxb_dom_node_t *found = news_nth_class(
            node->first_child, wanted, index);
        if (found != NULL) return found;
    }
    return NULL;
}

static const LayoutNodeBox *news_class_box(const NewsFixture *fixture,
                                           const char *wanted, int index)
{
    lxb_dom_node_t *node = news_nth_class(fixture->root, wanted, &index);
    return node == NULL ? NULL
                        : layout_box_for_node(&fixture->layout, node);
}

static int test_news_grid_wide_spans(void)
{
    puts("test: news-compat grid spans beyond eight tracks");
    NewsFixture fixture;
    CHECK(news_fixture_load(&fixture, "grid-span2.html"));
    /* Twelve 40px tracks; each .i item spans the count in its label. */
    static const int spans[] = {6, 7, 8, 9, 10, 11, 12, 9, 9, 12, 12};
    for (int i = 0; i < (int) (sizeof(spans) / sizeof(spans[0])); i++) {
        const LayoutNodeBox *grid = news_class_box(&fixture, "g", i);
        const LayoutNodeBox *item = news_class_box(&fixture, "i", i);
        if (grid == NULL || item == NULL || item->x != grid->x
            || item->width != 40 * spans[i]) {
            fprintf(stderr, "grid-span2 item %d: x=%d width=%d (span %d)\n",
                    i, item != NULL ? item->x : -1,
                    item != NULL ? item->width : -1, spans[i]);
        }
        CHECK(grid != NULL && item != NULL && grid->width == 480
              && item->x == grid->x && item->width == 40 * spans[i]);
    }
    news_fixture_destroy(&fixture);
    CHECK(fixture.budget.current == 0);

    /* Editorial grids with more than twelve tracks, negative lines from
       the far end, and authored values beyond every supported bound. */
    Budget budget;
    budget_init(&budget, 4u * MIB);
    budget_install_lexbor(&budget);
    static const char page[] =
        "<!doctype html><style>body{margin:0}"
        ".g{display:grid;width:448px}.g>i{display:block;height:4px}"
        "#nyt{grid-template-columns:repeat(14,1fr)}"
        "#body-col{grid-column:2 / 14}#full-col{grid-column:1 / -1}"
        "#neg-col{grid-column:-4 / -2}#late-col{grid-column:14}"
        "#wide{width:480px;grid-template-columns:repeat(24,20px)}"
        "#wide-a{grid-column:3 / -3}#wide-b{grid-column:span 24}"
        "#wide-c{grid-column:24}"
        "#huge{width:480px;grid-template-columns:repeat(12,1fr)}"
        "#huge-a{grid-column:1 / span 400}#huge-b{grid-column:-300 / 999}"
        "</style><body>"
        "<div id=nyt class=g><i id=body-col></i><i id=full-col></i>"
        "<i id=neg-col></i><i id=late-col></i></div>"
        "<div id=wide class=g><i id=wide-a></i><i id=wide-b></i>"
        "<i id=wide-c></i></div>"
        "<div id=huge class=g><i id=huge-a></i><i id=huge-b></i>"
        "<i id=huge-tail>tail</i></div></body>";
    PocDocument document;
    Stylesheet sheet;
    LayoutDocument layout;
    CHECK(document_parse(&document, &budget, page, strlen(page), 4096)
          && stylesheet_build(&sheet, &budget, &document, 480)
          && layout_build(&layout, &budget, &document, &sheet,
                          NULL, NULL, 480));
    lxb_dom_node_t *root = lxb_dom_interface_node(document.html);
#define NEWS_BOX(id) layout_box_for_node(&layout, find_id(root, id))
    const LayoutNodeBox *nyt = NEWS_BOX("nyt");
    const LayoutNodeBox *body_col = NEWS_BOX("body-col");
    const LayoutNodeBox *full_col = NEWS_BOX("full-col");
    const LayoutNodeBox *neg_col = NEWS_BOX("neg-col");
    const LayoutNodeBox *late_col = NEWS_BOX("late-col");
    const LayoutNodeBox *wide = NEWS_BOX("wide");
    const LayoutNodeBox *wide_a = NEWS_BOX("wide-a");
    const LayoutNodeBox *wide_b = NEWS_BOX("wide-b");
    const LayoutNodeBox *wide_c = NEWS_BOX("wide-c");
    const LayoutNodeBox *huge = NEWS_BOX("huge");
    const LayoutNodeBox *huge_a = NEWS_BOX("huge-a");
    const LayoutNodeBox *huge_b = NEWS_BOX("huge-b");
#undef NEWS_BOX
    CHECK(nyt != NULL && body_col != NULL && full_col != NULL
          && neg_col != NULL && late_col != NULL);
    /* 448 / 14 = 32px tracks. */
    CHECK(body_col->x == nyt->x + 32 && body_col->width == 12 * 32);
    CHECK(full_col->x == nyt->x && full_col->width == 448);
    CHECK(neg_col->x == nyt->x + 11 * 32 && neg_col->width == 2 * 32);
    CHECK(late_col->x == nyt->x + 13 * 32 && late_col->width == 32);
    CHECK(wide != NULL && wide_a != NULL && wide_b != NULL
          && wide_c != NULL);
    CHECK(wide_a->x == wide->x + 40 && wide_a->width == 20 * 20);
    CHECK(wide_b->x == wide->x && wide_b->width == 480);
    CHECK(wide_c->x == wide->x + 23 * 20 && wide_c->width == 20);
    /* Out-of-range authored values degrade to the bounded track envelope:
       the area covers the explicit grid plus at most GRID_TRACK_LIMIT
       implicit (one-pixel) tracks instead of being dropped. */
    CHECK(huge != NULL && huge_a != NULL && huge_b != NULL);
    CHECK(huge_a->x == huge->x && huge_a->width >= huge->width
          && huge_a->width <= huge->width + GRID_TRACK_LIMIT);
    CHECK(huge_b->x == huge->x && huge_b->width >= huge->width
          && huge_b->width <= huge->width + GRID_TRACK_LIMIT);
    ComputedStyle huge_a_style = style_for_node(
        &sheet, find_id(root, "huge-a"), NULL);
    ComputedStyle huge_b_style = style_for_node(
        &sheet, find_id(root, "huge-b"), NULL);
    CHECK(computed_style_grid_column_span(&huge_a_style)
              == COMPUTED_GRID_SPAN_LIMIT
          && computed_style_decode_grid_line(
                 computed_style_grid_column_start(&huge_b_style))
              == -COMPUTED_GRID_LINE_LIMIT
          && computed_style_grid_column_end(&huge_b_style)
              == COMPUTED_GRID_LINE_LIMIT);
    layout_destroy(&layout);
    stylesheet_destroy(&sheet);
    document_destroy(&document);
    CHECK(budget.current == 0);
    return 0;
}

static size_t news_link_regions(const LayoutDocument *layout,
                                const char *url, lxb_dom_node_t **node,
                                int *left, int *top, int *right,
                                int *bottom)
{
    size_t count = 0;
    size_t url_length = strlen(url);
    for (size_t i = 0; i < layout->link_count; i++) {
        const LinkRegion *link = &layout->links[i];
        if ((link->url_length & LAYOUT_LINK_URL_LENGTH_MASK) != url_length
            || memcmp(link->url, url, url_length) != 0) continue;
        if (count == 0) {
            *node = link->node;
            *left = link->x;
            *top = link->y;
            *right = link->x + link->width;
            *bottom = link->y + link->height;
        } else {
            /* Every fragment must name the same anchor: one focus stop. */
            if (link->node != *node) return SIZE_MAX;
            if (link->x < *left) *left = link->x;
            if (link->y < *top) *top = link->y;
            if (link->x + link->width > *right) {
                *right = link->x + link->width;
            }
            if (link->y + link->height > *bottom) {
                *bottom = link->y + link->height;
            }
        }
        count++;
    }
    return count;
}

static int test_news_block_content_links(void)
{
    puts("test: news-compat links wrapping block content");
    NewsFixture fixture;
    CHECK(news_fixture_load(&fixture, "link-blocks.html"));
    static const char *const urls[] = {
        "/a1", "/a2", "/a3", "/a4", "/a5", "/a6", "/a7", "/a8"
    };
    lxb_dom_node_t *anchors[8] = {0};
    for (size_t i = 0; i < 8; i++) {
        lxb_dom_node_t *link_node = NULL;
        int left = 0, top = 0, right = 0, bottom = 0;
        size_t regions = news_link_regions(
            &fixture.layout, urls[i], &link_node,
            &left, &top, &right, &bottom);
        if (regions == 0 || regions == SIZE_MAX) {
            fprintf(stderr, "link-blocks %s: regions=%zu\n", urls[i],
                    regions);
        }
        CHECK(regions != 0 && regions != SIZE_MAX && link_node != NULL
              && layout_node_name_is(link_node, "a"));
        anchors[i] = link_node;
        /* Each heading is covered by its link's focus rectangle. */
        if (i == 0) continue;
        lxb_dom_node_t *heading = link_node;
        while (heading != NULL && !layout_node_name_is(heading, "h2")) {
            heading = heading->first_child != NULL
                ? heading->first_child : heading->next;
        }
        const LayoutNodeBox *heading_box = heading == NULL ? NULL
            : layout_box_for_node(&fixture.layout, heading);
        CHECK(heading_box != NULL && heading_box->width > 0
              && heading_box->x >= left && heading_box->y >= top
              && heading_box->x + heading_box->width <= right
              && heading_box->y + heading_box->height <= bottom);
    }
    /* Inline and display:contents anchors carry one region for their
       outermost block, not one per nested box. */
    lxb_dom_node_t *ignored = NULL;
    int l = 0, t = 0, r = 0, b = 0;
    CHECK(news_link_regions(&fixture.layout, "/a4", &ignored,
                            &l, &t, &r, &b) == 1
          && r - l == 464);
    CHECK(news_link_regions(&fixture.layout, "/a8", &ignored,
                            &l, &t, &r, &b) == 1
          && r - l == 464);
    news_fixture_destroy(&fixture);
    CHECK(fixture.budget.current == 0);

    Budget budget;
    budget_init(&budget, 4u * MIB);
    budget_install_lexbor(&budget);
    static const char page[] =
        "<!doctype html><style>body{margin:0;font:16px sans-serif}"
        ".contents{display:contents}.flex{display:flex}"
        ".grid{display:grid;grid-template-columns:1fr 1fr}"
        ".chip{display:inline-block;width:60px;height:20px}"
        ".badge{position:absolute;left:300px;top:0;width:20px;height:20px}"
        "</style><body>"
        "<p id=p1><a href=/b1 class=contents><span>Inline span</span></a></p>"
        "<p id=p2><a href=/b2><span class=chip></span></a></p>"
        "<div class=flex><a href=/b3 class=contents><div id=b3>Flex card"
        "</div></a></div>"
        "<div class=grid><a href=/b4 class=contents><div id=b4a>Left</div>"
        "<div id=b4b>Right</div></a></div>"
        "<p><a href=/b5>text<span class=badge></span></a></p>"
        "<div id=plain><div>Not a link</div></div></body>";
    PocDocument document;
    Stylesheet sheet;
    LayoutDocument layout;
    CHECK(document_parse(&document, &budget, page, strlen(page), 4096)
          && stylesheet_build(&sheet, &budget, &document, 480)
          && layout_build(&layout, &budget, &document, &sheet,
                          NULL, NULL, 480));
    lxb_dom_node_t *root = lxb_dom_interface_node(document.html);
    static const char *const inline_urls[] = {
        "/b1", "/b2", "/b3", "/b4", "/b5"
    };
    for (size_t i = 0; i < 5; i++) {
        lxb_dom_node_t *link_node = NULL;
        size_t regions = news_link_regions(&layout, inline_urls[i],
                                           &link_node, &l, &t, &r, &b);
        if (regions == 0 || regions == SIZE_MAX) {
            fprintf(stderr, "inline links %s: regions=%zu\n",
                    inline_urls[i], regions);
        }
        CHECK(regions != 0 && regions != SIZE_MAX
              && layout_node_name_is(link_node, "a"));
    }
    const LayoutNodeBox *b3 = layout_box_for_node(
        &layout, find_id(root, "b3"));
    const LayoutNodeBox *b4b = layout_box_for_node(
        &layout, find_id(root, "b4b"));
    CHECK(news_link_regions(&layout, "/b3", &ignored, &l, &t, &r, &b) == 1
          && b3 != NULL && l == b3->x && r == b3->x + b3->width);
    /* Two grid items from one contents anchor: two fragments, one node. */
    CHECK(news_link_regions(&layout, "/b4", &ignored, &l, &t, &r, &b) == 2
          && b4b != NULL && r == b4b->x + b4b->width);
    /* Positioned descendants of an inline link are part of the link. */
    CHECK(news_link_regions(&layout, "/b5", &ignored, &l, &t, &r, &b) >= 2
          && r >= 320);
    CHECK(layout.link_count == 8u);
    layout_destroy(&layout);
    stylesheet_destroy(&sheet);
    document_destroy(&document);
    CHECK(budget.current == 0);
    return 0;
}

static int test_news_grid_locked_rows_first(void)
{
    puts("test: news-compat grid places row-locked items before auto items");
    NewsFixture fixture;
    CHECK(news_fixture_load(&fixture, "grid-autoplace.html"));
    const LayoutNodeBox *grid = news_class_box(&fixture, "g", 0);
    const LayoutNodeBox *auto_1 = news_class_box(&fixture, "a", 0);
    const LayoutNodeBox *auto_2 = news_class_box(&fixture, "a", 1);
    const LayoutNodeBox *row_2 = news_class_box(&fixture, "e", 0);
    const LayoutNodeBox *row_3 = news_class_box(&fixture, "e", 1);
    CHECK(grid != NULL && auto_1 != NULL && auto_2 != NULL
          && row_2 != NULL && row_3 != NULL);
    if (auto_2->y != row_3->y + 26) {
        fprintf(stderr, "grid-autoplace rows: %d %d %d %d grid=%d+%d\n",
                auto_1->y, row_2->y, row_3->y, auto_2->y, grid->y,
                grid->height);
    }
    /* 22px items, 4px row gap: rows 1-4 in that order, no overlap. */
    CHECK(auto_1->y == grid->y && row_2->y == auto_1->y + 26
          && row_3->y == row_2->y + 26 && auto_2->y == row_3->y + 26
          && grid->height == 4 * 22 + 3 * 4);
    news_fixture_destroy(&fixture);
    CHECK(fixture.budget.current == 0);

    Budget budget;
    budget_init(&budget, 4u * MIB);
    budget_install_lexbor(&budget);
    static const char page[] =
        "<!doctype html><style>body{margin:0}"
        ".g{display:grid;width:200px}.g>i{display:block;height:20px}"
        "#rows{grid-template-columns:100px 100px}#row-c{grid-row:1}"
        "#cols{grid-auto-flow:column;grid-template-rows:20px 20px;"
        "grid-auto-columns:100px}#col-b{grid-column:1}"
        "#late{grid-template-columns:100px 100px}#late-e{grid-row:2;"
        "grid-column:1}#late-r{grid-row:2}"
        "</style><body>"
        "<div id=rows class=g><i id=row-a></i><i id=row-b></i>"
        "<i id=row-c></i></div>"
        "<div id=cols class=g><i id=col-a></i><i id=col-b></i></div>"
        "<div id=late class=g><i id=late-a></i><i id=late-b></i>"
        "<i id=late-c></i><i id=late-r></i><i id=late-e></i></div>"
        "</body>";
    PocDocument document;
    Stylesheet sheet;
    LayoutDocument layout;
    CHECK(document_parse(&document, &budget, page, strlen(page), 4096)
          && stylesheet_build(&sheet, &budget, &document, 480)
          && layout_build(&layout, &budget, &document, &sheet,
                          NULL, NULL, 480));
    lxb_dom_node_t *root = lxb_dom_interface_node(document.html);
#define NEWS_BOX(id) layout_box_for_node(&layout, find_id(root, id))
    const LayoutNodeBox *rows = NEWS_BOX("rows");
    const LayoutNodeBox *row_a = NEWS_BOX("row-a");
    const LayoutNodeBox *row_b = NEWS_BOX("row-b");
    const LayoutNodeBox *row_c = NEWS_BOX("row-c");
    const LayoutNodeBox *cols = NEWS_BOX("cols");
    const LayoutNodeBox *col_a = NEWS_BOX("col-a");
    const LayoutNodeBox *col_b = NEWS_BOX("col-b");
    const LayoutNodeBox *late = NEWS_BOX("late");
    const LayoutNodeBox *late_a = NEWS_BOX("late-a");
    const LayoutNodeBox *late_b = NEWS_BOX("late-b");
    const LayoutNodeBox *late_c = NEWS_BOX("late-c");
    const LayoutNodeBox *late_r = NEWS_BOX("late-r");
    const LayoutNodeBox *late_e = NEWS_BOX("late-e");
#undef NEWS_BOX
    CHECK(rows != NULL && row_a != NULL && row_b != NULL && row_c != NULL);
    /* The row-1 item takes the first cell; auto items flow around it. */
    CHECK(row_c->x == rows->x && row_c->y == rows->y
          && row_a->x == rows->x + 100 && row_a->y == rows->y
          && row_b->x == rows->x && row_b->y == rows->y + 20
          && rows->height == 40);
    /* Column flow locks items to columns first. */
    CHECK(cols != NULL && col_a != NULL && col_b != NULL
          && col_b->x == cols->x && col_b->y == cols->y
          && col_a->x == cols->x && col_a->y == cols->y + 20);
    /* Fully explicit and row-locked items later in the document still
       claim row 2 before auto items 1-3 are placed. */
    CHECK(late != NULL && late_a != NULL && late_b != NULL
          && late_c != NULL && late_r != NULL && late_e != NULL
          && late_a->y == late->y && late_b->y == late->y
          && late_e->x == late->x && late_e->y == late->y + 20
          && late_r->x == late->x + 100 && late_r->y == late->y + 20
          && late_c->x == late->x && late_c->y == late->y + 40
          && late->height == 60);
    layout_destroy(&layout);
    stylesheet_destroy(&sheet);
    document_destroy(&document);
    CHECK(budget.current == 0);
    return 0;
}

static int test_news_clip_path_clips_fixed(void)
{
    puts("test: news-compat clip-path clips fixed descendants");
    NewsFixture fixture;
    CHECK(news_fixture_load(&fixture, "clip-path-fixed.html"));
    const LayoutNodeBox *wrap = news_class_box(&fixture, "wrap", 0);
    CHECK(wrap != NULL && wrap->clip_path_clip && wrap->height == 100
          && fixture.layout.fixed_count == 1
          && fixture.layout.fixed_ranges[0].clip_node == wrap->node
          && fixture.layout.fixed_ranges[0].scroll_end == wrap->y + 100);
    TileCache cache;
    static uint16_t frame[480u * 272u];
    CHECK(tile_cache_init(&cache, &fixture.budget, &fixture.layout, 8)
          && tile_cache_set_frame(&cache, frame,
                                  sizeof(frame) / sizeof(frame[0])));
    const uint16_t red = test_rgb565(0xcc0000);
    /* Unscrolled, the pinned ad covers the wrapper. */
    CHECK(tile_cache_render_frame(&cache, 0, 480, 272, NULL)
          && frame[10u * 480u + 400u] == red
          && frame[90u * 480u + 400u] == red
          && frame[150u * 480u + 400u] != red);
    /* Half scrolled: only the part of the viewport the wrapper still
       occupies shows the ad. */
    CHECK(tile_cache_render_frame(&cache, 50, 480, 272, NULL)
          && frame[10u * 480u + 400u] == red
          && frame[45u * 480u + 400u] == red
          && frame[55u * 480u + 400u] != red
          && frame[90u * 480u + 400u] != red);
    /* Once the wrapper has scrolled away the ad is gone. */
    CHECK(tile_cache_render_frame(&cache, 150, 480, 272, NULL)
          && frame[10u * 480u + 400u] != red
          && frame[90u * 480u + 400u] != red);
    tile_cache_destroy(&cache);
    news_fixture_destroy(&fixture);
    CHECK(fixture.budget.current == 0);

    /* An absolutely positioned descendant whose containing block lies
       outside the clip-path element is still clipped by it, unlike an
       overflow clip. */
    Budget budget;
    budget_init(&budget, 4u * MIB);
    budget_install_lexbor(&budget);
    static const char page[] =
        "<!doctype html><style>body{margin:0}"
        "#clip{clip-path:inset(0);width:100px;height:40px;background:#eee}"
        "#overflow{overflow:hidden;width:100px;height:40px;margin-top:20px}"
        ".abs{position:absolute;left:0;width:300px;height:20px;"
        "background:#00f}"
        "</style><body><div id=clip><div class=abs></div></div>"
        "<div id=overflow><div class=abs></div></div></body>";
    PocDocument document;
    Stylesheet sheet;
    LayoutDocument layout;
    CHECK(document_parse(&document, &budget, page, strlen(page), 4096)
          && stylesheet_build(&sheet, &budget, &document, 480)
          && layout_build(&layout, &budget, &document, &sheet,
                          NULL, NULL, 480));
    const LayoutNodeBox *overflow_box = layout_box_for_node(
        &layout, find_id(lxb_dom_interface_node(document.html),
                         "overflow"));
    CHECK(overflow_box != NULL);
    CHECK(tile_cache_init(&cache, &budget, &layout, 8)
          && tile_cache_set_frame(&cache, frame,
                                  sizeof(frame) / sizeof(frame[0]))
          && tile_cache_render_frame(&cache, 0, 480, 272, NULL));
    const uint16_t blue = test_rgb565(0x0000ff);
    size_t overflow_row = (size_t) overflow_box->y + 5u;
    CHECK(frame[5u * 480u + 50u] == blue
          && frame[5u * 480u + 200u] != blue
          && frame[overflow_row * 480u + 50u] == blue
          && frame[overflow_row * 480u + 200u] == blue);
    tile_cache_destroy(&cache);
    layout_destroy(&layout);
    stylesheet_destroy(&sheet);
    document_destroy(&document);
    CHECK(budget.current == 0);
    return 0;
}

static int test_news_svg_viewbox_ratio(void)
{
    puts("test: news-compat viewBox-only SVG keeps its aspect ratio");
    NewsFixture fixture;
    CHECK(news_fixture_load_with_images(
        &fixture, "svg-logo-ratio.html", true));
    const LayoutNodeBox *boxes[3] = {
        news_class_box(&fixture, "icon", 0),
        news_class_box(&fixture, "plain", 0),
        news_class_box(&fixture, "wide", 0)
    };
    /* viewBox 112x32: 30px tall -> 105px, 26px tall -> 91px, 56px wide ->
       16px tall. */
    static const int widths[3] = {105, 91, 56};
    static const int heights[3] = {30, 26, 16};
    for (size_t i = 0; i < 3; i++) {
        const LayoutNodeBox *box = boxes[i];
        CHECK(box != NULL);
        const ImageResource *image = images_find_node(
            &fixture.images, box->node);
        const DrawCommand *drawn = NULL;
        for (size_t at = 0; image != NULL && at < fixture.layout.count;
             at++) {
            if (fixture.layout.commands[at].type == DRAW_IMAGE
                && fixture.layout.commands[at].image == image) {
                drawn = &fixture.layout.commands[at];
                break;
            }
        }
        if (drawn == NULL || drawn->width != widths[i]
            || drawn->height != heights[i] || box->width != widths[i]) {
            fprintf(stderr,
                    "svg %zu: box=%dx%d image=%dx%d drawn=%dx%d\n", i,
                    box->width, box->height,
                    image != NULL ? image->width : -1,
                    image != NULL ? image->height : -1,
                    drawn != NULL ? drawn->width : -1,
                    drawn != NULL ? drawn->height : -1);
        }
        CHECK(image != NULL && drawn != NULL
              && drawn->width == widths[i] && drawn->height == heights[i]
              && box->width == widths[i] && box->height == heights[i]);
    }
    /* The centred header logo sits in the middle of the 480px bar. */
    CHECK(boxes[0]->x == (480 - 105) / 2);
    /* With no dimension at all the viewBox ratio still holds: the masthead
       logo fills its 189px link and is 184:25 tall, not 300x150 squeezed. */
    const LayoutNodeBox *masthead = news_class_box(&fixture, "masthead", 0);
    const ImageResource *nyt = images_find_node(
        &fixture.images, news_nth_class(fixture.root, "nyt", &(int) {0}));
    const DrawCommand *nyt_drawn = NULL;
    for (size_t at = 0; nyt != NULL && at < fixture.layout.count; at++) {
        if (fixture.layout.commands[at].type == DRAW_IMAGE
            && fixture.layout.commands[at].image == nyt) {
            nyt_drawn = &fixture.layout.commands[at];
        }
    }
    if (nyt_drawn != NULL && masthead != NULL
        && (nyt_drawn->width != 189 || nyt_drawn->height > 26)) {
        fprintf(stderr, "masthead=%d+%d drawn=%d,%d %dx%d image=%dx%d\n",
                masthead->x, masthead->width, nyt_drawn->x, nyt_drawn->y,
                nyt_drawn->width, nyt_drawn->height, nyt->width,
                nyt->height);
    }
    CHECK(masthead != NULL && nyt_drawn != NULL
          && nyt_drawn->x == masthead->x && nyt_drawn->width == 189
          && nyt_drawn->height >= 25 && nyt_drawn->height <= 26);
    news_fixture_destroy(&fixture);
    CHECK(fixture.budget.current == 0);
    return 0;
}

static int test_news_compat(void)
{
    CHECK(test_news_grid_wide_spans() == 0);
    CHECK(test_news_block_content_links() == 0);
    CHECK(test_news_grid_locked_rows_first() == 0);
    CHECK(test_news_clip_path_clips_fixed() == 0);
    CHECK(test_news_svg_viewbox_ratio() == 0);
    return 0;
}
