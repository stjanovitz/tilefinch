/* Focus geometry inside scroll containers: the focus rect, reveal,
   directional scoring and pointer hit testing must all see a link or
   control where the scroll containers around it present it, as paint does.
   Included by test_tilefinch.c and run as the scroller-focus suite. */

typedef struct {
    NavigationSession navigation;
    BrowserController controller;
} ScrollerFocusPage;

static bool scroller_focus_open(ScrollerFocusPage *page, Budget *budget,
                                const char *html)
{
    memset(page, 0, sizeof(*page));
    return navigation_init(&page->navigation, budget, 2)
        && navigation_commit_html(
               &page->navigation, navigation_begin(&page->navigation),
               "https://scroller.test/page", html, strlen(html), 480,
               NULL, NULL, true)
        && controller_init(&page->controller, &page->navigation);
}

static lxb_dom_node_t *scroller_focus_node(ScrollerFocusPage *page,
                                           const char *id)
{
    return find_id(
        lxb_dom_interface_node(page->navigation.page.document.html), id);
}

static const LayoutNodeBox *scroller_focus_box(ScrollerFocusPage *page,
                                               const char *id)
{
    return layout_box_for_node(&page->navigation.page.layout,
                               scroller_focus_node(page, id));
}

static int scroller_focus_scroll(ScrollerFocusPage *page)
{
    return navigation_current(&page->navigation)->scroll_y;
}

/* The focus rect lies inside the viewport and inside every clip around the
   focused element. */
static bool scroller_focus_rect_visible(ScrollerFocusPage *page,
                                        int *x, int *y,
                                        int *width, int *height)
{
    int left = 0, top = 0, right = 0, bottom = 0;
    bool clips_x = false, clips_y = false;
    if (!controller_focused_rect(&page->controller, x, y, width, height)
        || !controller_focused_clip(&page->controller, &left, &top,
                                    &right, &bottom, &clips_x, &clips_y))
        return false;
    int scroll = scroller_focus_scroll(page);
    if (*y < scroll || *y + *height > scroll + 272) return false;
    if (clips_x && (*x < left || *x + *width > right)) return false;
    if (clips_y && (*y < top || *y + *height > bottom)) return false;
    return true;
}

static int test_scroller_focus_fixed_overlay(void)
{
    puts("test: focus inside a fixed overlay scroller stays in the "
         "viewport without scrolling the page");
    Budget budget;
    budget_init(&budget, 32 * MIB);
    budget_install_lexbor(&budget);
    static const char html[] =
        "<!doctype html><meta name=viewport content='width=device-width'>"
        "<style>body{margin:0}p{margin:0;height:100px}"
        "#overlay{position:fixed;left:0;top:0;width:480px;height:272px;"
        "overflow:auto}#intro{height:500px}"
        "#go{display:block;width:100px;height:24px;margin:0}</style>"
        "<p>1</p><p>2</p><p>3</p><p>4</p><p>5</p><p>6</p><p>7</p><p>8</p>"
        "<p>9</p><p>10</p><p>11</p><p>12</p>"
        "<div id=overlay><div id=intro>Intro</div>"
        "<button id=go>Go</button></div>";
    static ScrollerFocusPage page;
    CHECK(scroller_focus_open(&page, &budget, html));
    const LayoutNodeBox *overlay = scroller_focus_box(&page, "overlay");
    const LayoutNodeBox *go = scroller_focus_box(&page, "go");
    CHECK(overlay != NULL && go != NULL && overlay->scroll_y == 0
          && scroller_focus_scroll(&page) == 0);
    CHECK(controller_focus_node(&page.controller,
                                scroller_focus_node(&page, "go")));
    int x = 0, y = 0, width = 0, height = 0;
    bool visible = scroller_focus_rect_visible(&page, &x, &y, &width, &height);
    if (!visible || scroller_focus_scroll(&page) != 0
        || y != go->y - overlay->y - overlay->scroll_y)
        fprintf(stderr, "overlay y=%d scroll=%d page=%d go=%d rect=%d,%d "
                "%dx%d\n", overlay->y, overlay->scroll_y,
                scroller_focus_scroll(&page), go->y, x, y, width, height);
    /* The overlay scrolled the button into view; the page had no reason
       to move. The overlay is laid out in flow and pinned to the top of
       the viewport. */
    CHECK(overlay->scroll_y > 0 && scroller_focus_scroll(&page) == 0
          && visible && y == go->y - overlay->y - overlay->scroll_y);
    /* Scrolling the page leaves the fixed overlay, and its focus, pinned. */
    CHECK(navigation_set_scroll(&page.navigation, 300));
    int pinned_y = 0;
    CHECK(controller_focused_rect(&page.controller, NULL, &pinned_y,
                                  NULL, NULL)
          && pinned_y == y + 300);
    navigation_destroy(&page.navigation);
    CHECK(budget.current == 0);
    return 0;
}

static int test_scroller_focus_panel_in_long_page(void)
{
    puts("test: focus inside an overflow panel of a long page reveals "
         "the panel first and the page only as needed");
    Budget budget;
    budget_init(&budget, 32 * MIB);
    budget_install_lexbor(&budget);
    static const char html[] =
        "<!doctype html><meta name=viewport content='width=device-width'>"
        "<style>body{margin:0}p{margin:0;height:100px}"
        "#panel{height:100px;overflow:auto}"
        ".row{height:30px}</style>"
        "<p>1</p><p>2</p><p>3</p><p>4</p><p>5</p>"
        "<div id=panel>"
        "<div class=row><a href=/0 id=i0>Item 0</a></div>"
        "<div class=row><a href=/1 id=i1>Item 1</a></div>"
        "<div class=row><a href=/2 id=i2>Item 2</a></div>"
        "<div class=row><a href=/3 id=i3>Item 3</a></div>"
        "<div class=row><a href=/4 id=i4>Item 4</a></div>"
        "<div class=row><a href=/5 id=i5>Item 5</a></div>"
        "<div class=row><a href=/6 id=i6>Item 6</a></div>"
        "<div class=row><a href=/7 id=i7>Item 7</a></div>"
        "<div class=row><a href=/8 id=i8>Item 8</a></div>"
        "<div class=row><a href=/9 id=i9>Item 9</a></div>"
        "</div><p>6</p><p>7</p><p>8</p><p>9</p><p>10</p><p>11</p>";
    static ScrollerFocusPage page;
    CHECK(scroller_focus_open(&page, &budget, html));
    const LayoutNodeBox *panel = scroller_focus_box(&page, "panel");
    CHECK(panel != NULL && panel->y == 500
          && panel->content_height > panel->client_height);
    /* An inline link has no box of its own: the panel must still bring it
       into view. */
    CHECK(controller_focus_node(&page.controller,
                                scroller_focus_node(&page, "i8")));
    int x = 0, y = 0, width = 0, height = 0;
    bool visible = scroller_focus_rect_visible(&page, &x, &y, &width, &height);
    int page_scroll = scroller_focus_scroll(&page);
    if (!visible)
        fprintf(stderr, "panel scroll=%d page=%d rect=%d,%d %dx%d\n",
                panel->scroll_y, page_scroll, x, y, width, height);
    CHECK(visible && panel->scroll_y > 0
          && y >= panel->y && y + height <= panel->y + panel->client_height);
    /* The page scrolled only far enough to show the panel's row. */
    CHECK(page_scroll == y + height - 272 + page.controller.focus_margin);
    /* Back to the first row: the panel scrolls back, the page stays. */
    CHECK(controller_focus_node(&page.controller,
                                scroller_focus_node(&page, "i0"))
          && scroller_focus_rect_visible(&page, &x, &y, &width, &height)
          && panel->scroll_y == 0
          && scroller_focus_scroll(&page) == page_scroll);
    navigation_destroy(&page.navigation);
    CHECK(budget.current == 0);
    return 0;
}

static int test_scroller_focus_nested(void)
{
    puts("test: focus two scrollers deep reveals inner then outer");
    Budget budget;
    budget_init(&budget, 32 * MIB);
    budget_install_lexbor(&budget);
    static const char html[] =
        "<!doctype html><meta name=viewport content='width=device-width'>"
        "<style>body{margin:0}p{margin:0;height:100px}"
        "#outer{height:150px;overflow:auto}#outer-top{height:200px}"
        "#inner{height:80px;overflow:auto}#inner-top{height:300px}"
        "#deep{display:block;width:100px;height:24px;margin:0}"
        "#outer-end{height:200px}</style>"
        "<p>1</p><div id=outer><div id=outer-top>Top</div>"
        "<div id=inner><div id=inner-top>Inner</div>"
        "<button id=deep>Deep</button></div>"
        "<div id=outer-end>End</div></div>"
        "<p>2</p><p>3</p><p>4</p><p>5</p>";
    static ScrollerFocusPage page;
    CHECK(scroller_focus_open(&page, &budget, html));
    const LayoutNodeBox *outer = scroller_focus_box(&page, "outer");
    const LayoutNodeBox *inner = scroller_focus_box(&page, "inner");
    const LayoutNodeBox *deep = scroller_focus_box(&page, "deep");
    CHECK(outer != NULL && inner != NULL && deep != NULL);
    CHECK(controller_focus_node(&page.controller,
                                scroller_focus_node(&page, "deep")));
    int x = 0, y = 0, width = 0, height = 0;
    bool visible = scroller_focus_rect_visible(&page, &x, &y, &width, &height);
    /* Inner scrolls the button to its bottom edge; outer then scrolls only
       as far as the button where inner now presents it. */
    int inner_expected = deep->y + deep->height
        - (inner->y + inner->client_height);
    int outer_expected = (deep->y - inner_expected) + deep->height
        - (outer->y + outer->client_height);
    if (!visible || inner->scroll_y != inner_expected
        || outer->scroll_y != outer_expected)
        fprintf(stderr, "nested inner=%d/%d outer=%d/%d page=%d "
                "rect=%d,%d %dx%d\n", inner->scroll_y, inner_expected,
                outer->scroll_y, outer_expected,
                scroller_focus_scroll(&page), x, y, width, height);
    CHECK(visible && inner->scroll_y == inner_expected
          && outer->scroll_y == outer_expected
          && y == deep->y - inner->scroll_y - outer->scroll_y
          && scroller_focus_scroll(&page) == 0);
    navigation_destroy(&page.navigation);
    CHECK(budget.current == 0);
    return 0;
}

static int test_scroller_focus_direction(void)
{
    puts("test: directional focus scores a scrolled panel's items where "
         "they are shown");
    Budget budget;
    budget_init(&budget, 32 * MIB);
    budget_install_lexbor(&budget);
    static const char html[] =
        "<!doctype html><meta name=viewport content='width=device-width'>"
        "<style>body{margin:0}#row{display:flex}"
        "#panel{width:200px;height:100px;overflow:auto}"
        "#side{width:200px}a{display:block;height:30px}"
        "#gap1{height:60px}#gap2{height:110px}</style>"
        "<div id=row><div id=panel>"
        "<a href=/0 id=i0>Item 0</a><a href=/1 id=i1>Item 1</a>"
        "<a href=/2 id=i2>Item 2</a><a href=/3 id=i3>Item 3</a>"
        "<a href=/4 id=i4>Item 4</a><a href=/5 id=i5>Item 5</a>"
        "<a href=/6 id=i6>Item 6</a><a href=/7 id=i7>Item 7</a>"
        "<a href=/8 id=i8>Item 8</a><a href=/9 id=i9>Item 9</a>"
        "</div><div id=side><div id=gap1></div>"
        "<a href=/beside id=beside>Beside</a><div id=gap2></div>"
        "<a href=/below id=below>Below</a></div></div>";
    static ScrollerFocusPage page;
    CHECK(scroller_focus_open(&page, &budget, html));
    const LayoutNodeBox *panel = scroller_focus_box(&page, "panel");
    const LayoutNodeBox *beside = scroller_focus_box(&page, "beside");
    const LayoutNodeBox *below = scroller_focus_box(&page, "below");
    CHECK(panel != NULL && beside != NULL && below != NULL
          && beside->y == 60 && below->y == 200);
    /* Item 7 (laid out at 210..240) is shown at 70..100 once the panel
       scrolls it into view: beside "beside", not "below". */
    CHECK(controller_focus_node(&page.controller,
                                scroller_focus_node(&page, "i7"))
          && panel->scroll_y == 140);
    CHECK(controller_focus_direction(&page.controller,
                                     CONTROLLER_FOCUS_RIGHT));
    lxb_dom_node_t *moved = controller_focused_node(&page.controller);
    if (moved != scroller_focus_node(&page, "beside"))
        fprintf(stderr, "right moved to %s\n",
                moved == scroller_focus_node(&page, "below")
                    ? "below" : "another target");
    CHECK(moved == scroller_focus_node(&page, "beside"));
    /* And back: Left from "beside" lands on the panel row shown beside
       it, which the panel already shows. */
    CHECK(controller_focus_direction(&page.controller,
                                     CONTROLLER_FOCUS_LEFT));
    moved = controller_focused_node(&page.controller);
    CHECK(moved == scroller_focus_node(&page, "i7")
          || moved == scroller_focus_node(&page, "i6"));
    CHECK(panel->scroll_y == 140);
    navigation_destroy(&page.navigation);
    CHECK(budget.current == 0);
    return 0;
}

static int test_scroller_focus_pointer(void)
{
    puts("test: pointer hits a scrolled panel's items where they are "
         "shown, and not those scrolled out of it");
    Budget budget;
    budget_init(&budget, 32 * MIB);
    budget_install_lexbor(&budget);
    static const char html[] =
        "<!doctype html><meta name=viewport content='width=device-width'>"
        "<style>body{margin:0}#panel{width:200px;height:100px;"
        "overflow:auto}button{display:block;width:200px;height:30px;"
        "margin:0;padding:0;border:0}#after{height:400px}</style>"
        "<div id=panel>"
        "<button id=b0>0</button><button id=b1>1</button>"
        "<button id=b2>2</button><button id=b3>3</button>"
        "<button id=b4>4</button><button id=b5>5</button>"
        "<button id=b6>6</button><button id=b7>7</button>"
        "<button id=b8>8</button><button id=b9>9</button>"
        "</div><div id=after></div>";
    static ScrollerFocusPage page;
    CHECK(scroller_focus_open(&page, &budget, html));
    lxb_dom_node_t *panel_node = scroller_focus_node(&page, "panel");
    const LayoutNodeBox *panel = scroller_focus_box(&page, "panel");
    CHECK(panel != NULL
          && layout_scroll_node(&page.navigation.page.layout, panel_node,
                                0, 140)
          && panel->scroll_y == 140);
    /* y=85 shows laid-out y=225: button 7 (210..240). */
    CHECK(controller_focus_at(&page.controller, 10, 85));
    lxb_dom_node_t *hit = controller_focused_node(&page.controller);
    if (hit != scroller_focus_node(&page, "b7"))
        fprintf(stderr, "click at 85 focused %s\n",
                hit == scroller_focus_node(&page, "b2") ? "b2 (unscrolled)"
                    : "another target");
    CHECK(hit == scroller_focus_node(&page, "b7"));
    int y = 0;
    CHECK(controller_focused_rect(&page.controller, NULL, &y, NULL, NULL)
          && y == 70 && panel->scroll_y == 140);
    /* Below the panel, where button 5 would be unscrolled, nothing of the
       panel is shown. */
    bool below_hit = controller_focus_at(&page.controller, 10, 160);
    hit = controller_focused_node(&page.controller);
    CHECK(!below_hit || (hit != scroller_focus_node(&page, "b5")
                         && hit != scroller_focus_node(&page, "b9")));
    navigation_destroy(&page.navigation);
    CHECK(budget.current == 0);
    return 0;
}

/* The hit test as it was before its cheap rejection: every region is
   placed through its scroll containers before the rect test. */
static lxb_dom_node_t *scroller_focus_reference_hit(
    const LayoutDocument *layout, int x, int y)
{
    for (int pass = 0; pass < 3; pass++) {
        size_t count = pass == 0 ? layout->control_count
            : (pass == 1 ? layout->link_count : layout->node_box_count);
        for (size_t n = 0; n < count; n++) {
            size_t i = pass == 2 ? n : count - 1u - n;
            lxb_dom_node_t *node;
            int left, top, width, height;
            if (pass == 0) {
                const ControlRegion *r = &layout->controls[i];
                node = r->node; left = r->x; top = r->y;
                width = r->width; height = r->height;
            } else if (pass == 1) {
                const LinkRegion *r = &layout->links[i];
                node = r->node; left = r->x; top = r->y;
                width = r->width; height = r->height;
            } else {
                const LayoutNodeBox *r = &layout->node_boxes[i];
                if (r->node == NULL || r->width <= 0 || r->height <= 0)
                    continue;
                node = r->node; left = r->x; top = r->y;
                width = r->width; height = r->height;
            }
            LayoutScrollPlacement placement;
            if (layout_node_scroll_placement(layout, NULL, node,
                                             &placement)) {
                if ((placement.clipped_x
                     && (x < placement.clip_left
                         || x >= placement.clip_right))
                    || (placement.clipped_y
                        && (y < placement.clip_top
                            || y >= placement.clip_bottom))) continue;
                left += placement.dx;
                top += placement.dy;
            }
            if (x >= left && x < left + width && y >= top
                && y < top + height) return node;
        }
    }
    return NULL;
}

static int scroller_focus_hits_match(int extra_scrollers)
{
    Budget budget;
    budget_init(&budget, 32 * MIB);
    budget_install_lexbor(&budget);
    static char html[8192];
    size_t n = (size_t) snprintf(html, sizeof(html),
        "<!doctype html><meta name=viewport content='width=device-width'>"
        "<style>body{margin:0}button,a{display:block;margin:0;padding:0;"
        "border:0;height:20px}#outer{width:300px;height:150px;"
        "overflow:auto}#gap{height:40px}#inner{width:200px;height:80px;"
        "overflow:auto}#wide{width:400px}#wide button{width:380px}"
        "#tail{height:200px}.x{width:60px;height:12px;overflow:auto;"
        "float:left}.x a{height:30px}</style>"
        "<div id=outer><div id=gap>Gap</div><div id=inner><div id=wide>");
    for (int i = 0; i < 8; i++) {
        n += (size_t) snprintf(html + n, sizeof(html) - n,
            "<button id=b%d>B%d</button><a href=/i%d>I%d</a>",
            i, i, i, i);
    }
    n += (size_t) snprintf(html + n, sizeof(html) - n,
        "</div></div><a href=/o>Outer</a><div id=tail>Tail</div></div>");
    for (int i = 0; i < extra_scrollers; i++) {
        n += (size_t) snprintf(html + n, sizeof(html) - n,
            "<div class=x id=x%d><a href=/x%d>X%d</a></div>", i, i, i);
    }
    CHECK(n < sizeof(html));
    static ScrollerFocusPage page;
    CHECK(scroller_focus_open(&page, &budget, html));
    LayoutDocument *layout = &page.navigation.page.layout;
    CHECK(layout_scroll_node(layout, scroller_focus_node(&page, "outer"),
                             0, 60)
          && layout_scroll_node(layout, scroller_focus_node(&page, "inner"),
                                40, 50));
    if (extra_scrollers > 0) {
        CHECK(layout_scroll_node(layout, scroller_focus_node(&page, "x0"),
                                 0, 9));
    }
    const LayoutNodeBox *outer = scroller_focus_box(&page, "outer");
    const LayoutNodeBox *inner = scroller_focus_box(&page, "inner");
    CHECK(outer->scroll_y == 60 && inner->scroll_x == 40
          && inner->scroll_y == 50);
    CHECK(viewport_device_to_css(&layout->viewport, 100) == 100);
    size_t compared = 0, scrolled_hits = 0;
    for (int y = 0; y < 272; y += 3) {
        for (int x = 0; x < 480; x += 5) {
            lxb_dom_node_t *expected =
                scroller_focus_reference_hit(layout, x, y);
            bool activate = false;
            CHECK(controller_pointer_event(&page.controller,
                                           CONTROLLER_POINTER_MOVE, x, y,
                                           &activate));
            ScriptRuntime *runtime = page.navigation.page.runtime;
            lxb_dom_node_t *actual = runtime == NULL
                ? page.controller.pointer_hover_node
                : (page.controller.pointer_hover_handle == 0 ? NULL
                   : script_runtime_node_handle_resolve(
                         runtime, page.controller.pointer_hover_handle));
            if (actual != expected) {
                fprintf(stderr, "hit mismatch at %d,%d (%d extra)\n",
                        x, y, extra_scrollers);
                CHECK(false);
            }
            compared++;
            if (expected != NULL
                && layout_node_scroll_placement(layout, NULL, expected,
                                                &(LayoutScrollPlacement){0}))
                scrolled_hits++;
        }
    }
    CHECK(compared > 1000 && scrolled_hits > 100);
    navigation_destroy(&page.navigation);
    CHECK(budget.current == 0);
    return 0;
}

static int test_scroller_focus_hits_match_exact_path(void)
{
    puts("test: rejecting regions before placement keeps every hit in "
         "nested scrolled containers");
    CHECK(scroller_focus_hits_match(0) == 0);
    /* Past the placement index, where every box is walked. */
    CHECK(scroller_focus_hits_match(20) == 0);
    return 0;
}

static int test_scroller_focus(void)
{
    CHECK(test_scroller_focus_fixed_overlay() == 0);
    CHECK(test_scroller_focus_panel_in_long_page() == 0);
    CHECK(test_scroller_focus_nested() == 0);
    CHECK(test_scroller_focus_direction() == 0);
    CHECK(test_scroller_focus_pointer() == 0);
    CHECK(test_scroller_focus_hits_match_exact_path() == 0);
    return 0;
}
