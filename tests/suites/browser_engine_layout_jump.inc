/* Jumps into a provisional layout (navigation_complete_layout_through): a
   contents link and Find lay a long scriptless page out only as far as
   their target and the screens below it, and land exactly where the
   complete layout puts them. Each case compares a page relaid out
   provisionally from the top against the same page laid out completely,
   in a second engine: the same scroll position, the same frame, and after
   idle work completes the rest, the same layout with the reader unmoved. */

static BrowserEngine *layout_jump_engine(const char *html, size_t length)
{
    BrowserDeviceProfile profile;
    browser_device_profile_psp3000(&profile);
    BrowserConfig config;
    browser_config_init(&config, &profile);
    config.memory_limit = 24u * MIB;
    config.tile_capacity = 8;
    config.resources.enabled = false;
    config.javascript.enabled = false;
    char error[256] = {0};
    BrowserEngine *engine = browser_engine_create(&config, error,
                                                  sizeof(error));
    if (engine == NULL) return NULL;
    if (!browser_engine_commit_html(engine, "https://jump.test/page", html,
                                    length, true)) {
        fprintf(stderr, "layout jump: commit refused (%s)\n",
                browser_engine_last_error(engine));
        browser_engine_destroy(engine);
        return NULL;
    }
    return engine;
}

/* The page as an in-place relayout publishes it on a slow page: the
   screens at the top, the rest left to idle work. */
static bool layout_jump_make_provisional(BrowserEngine *engine)
{
    NavigationSession *navigation = browser_engine_navigation(engine);
    uint64_t threshold = navigation->relayout_preview_threshold_us;
    navigation->relayout_preview_threshold_us = 0;
    bool relaid = navigation_set_scroll(navigation, 0)
        && navigation_relayout(navigation);
    navigation->relayout_preview_threshold_us = threshold;
    return relaid && navigation->page.layout_provisional
        && navigation->page.layout.content_limit_y > 0;
}

static bool layout_jump_activate(BrowserEngine *engine, const char *url)
{
    ControllerAction action;
    memset(&action, 0, sizeof(action));
    action.type = CONTROLLER_ACTION_NAVIGATE;
    snprintf(action.url, sizeof(action.url), "%s", url);
    snprintf(action.method, sizeof(action.method), "GET");
    return browser_engine_execute_action(engine, &action, 1u * MIB, 1000);
}

static int layout_jump_scroll(BrowserEngine *engine)
{
    const NavigationEntry *entry =
        navigation_current(browser_engine_navigation(engine));
    return entry == NULL ? -1 : entry->scroll_y;
}

static uint64_t layout_jump_frame(BrowserEngine *engine)
{
    size_t pixels = 0;
    if (!browser_engine_render_frame(engine, NULL)) return 0;
    return frame_checksum(browser_engine_framebuffer(engine, &pixels),
                          pixels);
}

static bool layout_jump_complete_in_idle(BrowserEngine *engine)
{
    NavigationSession *navigation = browser_engine_navigation(engine);
    for (size_t turns = 0;
         turns < 64 && navigation->page.layout_provisional; turns++) {
        bool visual_changed = false;
        (void) browser_engine_run_idle_work(engine, &visual_changed);
    }
    return !navigation->page.layout_provisional;
}

typedef struct {
    int scroll;
    uint64_t frame;
    size_t count;
    int height;
} LayoutJumpResult;

/* Jumps to `url` on a completely laid-out copy of the page. */
static bool layout_jump_reference(const char *html, size_t length,
                                  const char *url, LayoutJumpResult *result)
{
    BrowserEngine *engine = layout_jump_engine(html, length);
    NavigationSession *navigation =
        engine == NULL ? NULL : browser_engine_navigation(engine);
    bool ok = navigation != NULL && !navigation->page.layout_provisional
        && layout_jump_activate(engine, url);
    if (ok) {
        result->scroll = layout_jump_scroll(engine);
        result->frame = layout_jump_frame(engine);
        result->count = navigation->page.layout.count;
        result->height = navigation->page.layout.height;
    }
    browser_engine_destroy(engine);
    return ok && result->scroll > 0 && result->frame != 0;
}

/* The jump on the provisional copy: same position and frame, and laid
   out through the target unless `fallback` (the case cannot be proven and
   is completed instead). Idle work then completes the same layout without
   moving the reader. */
static int layout_jump_case(const char *label, const char *html,
                            size_t length, const char *url, bool fallback)
{
    printf("test: a jump into a provisional layout (%s) lands where the "
           "complete layout puts it\n", label);
    LayoutJumpResult reference = {0};
    CHECK(layout_jump_reference(html, length, url, &reference));
    BrowserEngine *engine = layout_jump_engine(html, length);
    CHECK(engine != NULL && layout_jump_make_provisional(engine));
    NavigationSession *navigation = browser_engine_navigation(engine);
    int first_limit = navigation->page.layout.content_limit_y;
    CHECK(layout_jump_activate(engine, url));
    int scroll = layout_jump_scroll(engine);
    if (scroll != reference.scroll)
        fprintf(stderr, "layout jump %s: scroll %d, complete layout %d\n",
                label, scroll, reference.scroll);
    CHECK(scroll == reference.scroll && scroll > first_limit);
    CHECK(navigation->performance.layout_through_builds == 1u
          && navigation->performance.layout_through_fallbacks
                 == (fallback ? 1u : 0u)
          && navigation->page.layout_provisional == !fallback);
    uint64_t frame = layout_jump_frame(engine);
    if (frame != reference.frame)
        fprintf(stderr, "layout jump %s: frame differs from the complete "
                "layout's\n", label);
    CHECK(frame == reference.frame);
    CHECK(layout_jump_complete_in_idle(engine)
          && layout_jump_scroll(engine) == reference.scroll
          && navigation->performance.layout_jump_corrections == 0u
          && navigation->page.layout.count == reference.count
          && navigation->page.layout.height == reference.height
          && layout_jump_frame(engine) == reference.frame);
    browser_engine_destroy(engine);
    return 0;
}

/* A long article: contents links at the top, sections far below. */
static size_t layout_jump_article(char *page, size_t capacity,
                                  const char *style, const char *before,
                                  unsigned sections)
{
    size_t used = (size_t) snprintf(page, capacity,
        "<!doctype html><meta name=viewport content='width=device-width'>"
        "<style>body{margin:8px;font-size:15px}h2{margin:12px 0}"
        "p{margin:0 0 8px}%s</style><ul>", style);
    for (unsigned s = 0; s < sections && used < capacity; s++)
        used += (size_t) snprintf(page + used, capacity - used,
            "<li><a href='#s%u'>Section %u</a></li>", s, s);
    used += (size_t) snprintf(page + used, capacity - used, "</ul>%s",
                              before);
    for (unsigned s = 0; s < sections && used < capacity; s++) {
        used += (size_t) snprintf(page + used, capacity - used,
            "<h2 id=s%u>Section %u</h2>", s, s);
        for (unsigned p = 0; p < 12 && used < capacity; p++)
            used += (size_t) snprintf(page + used, capacity - used,
                "<p>Paragraph %u.%u of the article, long enough to wrap "
                "over two lines on the narrow screen.</p>", s, p);
    }
    return used;
}

static int test_layout_jump_contents(void)
{
    static char page[65536];
    size_t used = layout_jump_article(page, sizeof(page), "", "", 16);
    CHECK(used < sizeof(page));
    return layout_jump_case("contents link", page, used,
                            "https://jump.test/page#s11", false);
}

/* Floats beside the flow, relative and absolutely positioned boxes, a
   sticky bar, list and CSS counters, a table and a column flex box, all
   above the target: complete before it, so it lies where they put it. */
static int test_layout_jump_after_floats_and_positioned(void)
{
    static char page[65536];
    static const char style[] =
        ".f{float:right;width:120px;height:400px;background:#cde;"
        "margin:0 0 8px 8px}.l{float:left;width:60px;height:900px;"
        "background:#edc}.r{position:relative;top:30px;left:10px}"
        ".box{position:relative;height:60px}.abs{position:absolute;"
        "right:0;bottom:-40px;width:50px;height:50px;background:#c00}"
        ".bar{position:sticky;top:0;background:#ffe;height:20px}"
        "body{counter-reset:h}h2::before{counter-increment:h;"
        "content:counter(h) '. '}.col{display:flex;"
        "flex-direction:column}";
    static char before[8192];
    size_t at = (size_t) snprintf(before, sizeof(before),
        "<div class=bar>Sticky bar</div><div class=f></div><div class=l>"
        "</div><div class=box><div class=abs></div><p class=r>Shifted"
        "</p></div><ol>");
    for (unsigned i = 0; i < 20; i++)
        at += (size_t) snprintf(before + at, sizeof(before) - at,
                                "<li>Item %u beside the floats</li>", i);
    at += (size_t) snprintf(before + at, sizeof(before) - at,
        "</ol><table border=1><tr><td>Wide cell with text</td><td>B</td>"
        "</tr><tr><td>C</td><td>A longer second column cell</td></tr>"
        "</table><div class=col><p>Column item one</p><p>Column item "
        "two</p></div>");
    CHECK(at < sizeof(before));
    size_t used = layout_jump_article(page, sizeof(page), style, before, 14);
    CHECK(used < sizeof(page));
    return layout_jump_case("floats and positioned content", page, used,
                            "https://jump.test/page#s9", false);
}

/* The target sits in a flex row centred against a sibling taller than
   the screens laid out below it: the cut runs through that sibling, so
   the row's alignment is unknown and the page is completed instead. */
static int test_layout_jump_unprovable_falls_back(void)
{
    static char page[65536];
    static char before[16384];
    size_t at = (size_t) snprintf(before, sizeof(before),
        "<div style='display:flex;align-items:center'><div>"
        "<h3 id=centred>Centred</h3></div><div>");
    for (unsigned i = 0; i < 80; i++)
        at += (size_t) snprintf(before + at, sizeof(before) - at,
            "<p>Tall sibling paragraph %u, which wraps onto more lines "
            "than a few screens hold.</p>", i);
    at += (size_t) snprintf(before + at, sizeof(before) - at,
                            "</div></div>");
    CHECK(at < sizeof(before));
    size_t used = layout_jump_article(page, sizeof(page), "", before, 6);
    CHECK(used < sizeof(page));
    return layout_jump_case("unprovable alignment", page, used,
                            "https://jump.test/page#centred", true);
}

/* A tall box pinned to the bottom of the body, which is cut: placed from
   the body's final height, which only the complete layout knows, it would
   paint over the target's screen at the cut's bottom instead. */
static int test_layout_jump_bottom_pinned_falls_back(void)
{
    static char page[65536];
    static const char style[] =
        "body{position:relative}.pin{position:absolute;right:0;bottom:0;"
        "width:40px;height:1400px;background:#08c}";
    size_t used = layout_jump_article(page, sizeof(page), style,
                                      "<div class=pin></div>", 14);
    CHECK(used < sizeof(page));
    return layout_jump_case("box pinned to a cut bottom", page, used,
                            "https://jump.test/page#s9", true);
}

/* Find from the top of a provisional page for text that only appears far
   below its cut, then next across the cut and previous with wrapping: each
   lands where Find on the complete layout does, the first two without
   completing the layout. The count reads as a lower bound until the
   completion, then exactly. */
static int test_layout_jump_find_beyond(void)
{
    puts("test: Find on a provisional layout lays out to matches past "
         "the cut and selects as on the complete layout");
    static char page[65536];
    size_t used = layout_jump_article(page, sizeof(page), "", "", 16);
    CHECK(used < sizeof(page));
    static const char deep[] = "Paragraph 10.3 ";
    int reference_scroll[3] = {0};
    BrowserFindSnapshot reference = {0}, snapshot = {0};
    BrowserEngine *engine = layout_jump_engine(page, used);
    CHECK(engine != NULL
          && browser_engine_find_begin(engine, "paragraph 1", &reference));
    BrowserFindSnapshot full_count = reference;
    CHECK(!full_count.truncated && full_count.match_count > 2);
    browser_engine_find_clear(engine);
    CHECK(browser_engine_find_begin(engine, deep, &reference)
          && reference.match_count == 1);
    reference_scroll[0] = layout_jump_scroll(engine);
    browser_engine_find_clear(engine);
    CHECK(navigation_set_scroll(browser_engine_navigation(engine), 0)
          && browser_engine_find_begin(engine, "paragraph 1", &reference)
          && browser_engine_find_move(engine, 1, &reference)
          && browser_engine_find_move(engine, 1, &reference));
    reference_scroll[1] = layout_jump_scroll(engine);
    size_t reference_selected = reference.selected;
    CHECK(browser_engine_find_move(engine, -1, &reference)
          && browser_engine_find_move(engine, -1, &reference)
          && browser_engine_find_move(engine, -1, &reference)
          && reference.wrapped);
    reference_scroll[2] = layout_jump_scroll(engine);
    browser_engine_destroy(engine);

    engine = layout_jump_engine(page, used);
    CHECK(engine != NULL && layout_jump_make_provisional(engine));
    NavigationSession *navigation = browser_engine_navigation(engine);
    int first_limit = navigation->page.layout.content_limit_y;
    CHECK(browser_engine_find_begin(engine, deep, &snapshot)
          && snapshot.match_count == 1 && snapshot.truncated
          && layout_jump_scroll(engine) == reference_scroll[0]
          && reference_scroll[0] > first_limit
          && navigation->page.layout_provisional
          && navigation->performance.layout_through_builds != 0);
    browser_engine_find_clear(engine);
    /* From the top: each move goes to a match the layout so far proves,
       laying out further when the next one is not. */
    CHECK(navigation_set_scroll(navigation, 0)
          && layout_jump_make_provisional(engine)
          && browser_engine_find_begin(engine, "paragraph 1", &snapshot)
          && snapshot.selected == 0
          && browser_engine_find_move(engine, 1, &snapshot)
          && browser_engine_find_move(engine, 1, &snapshot)
          && snapshot.selected == reference_selected
          && layout_jump_scroll(engine) == reference_scroll[1]
          && navigation->page.layout_provisional && snapshot.truncated);
    /* Back past the first wraps to the last: that needs the whole page. */
    CHECK(browser_engine_find_move(engine, -1, &snapshot)
          && browser_engine_find_move(engine, -1, &snapshot)
          && browser_engine_find_move(engine, -1, &snapshot)
          && snapshot.wrapped && !snapshot.truncated
          && snapshot.match_count == full_count.match_count
          && layout_jump_scroll(engine) == reference_scroll[2]
          && !navigation->page.layout_provisional);
    browser_engine_destroy(engine);
    return 0;
}

static int test_layout_jump(void)
{
    CHECK(test_layout_jump_contents() == 0);
    CHECK(test_layout_jump_after_floats_and_positioned() == 0);
    CHECK(test_layout_jump_unprovable_falls_back() == 0);
    CHECK(test_layout_jump_bottom_pinned_falls_back() == 0);
    CHECK(test_layout_jump_find_beyond() == 0);
    return 0;
}
