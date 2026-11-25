/* Child browsing contexts: their images and media features against the
   top-level page's. Included by test_tilefinch.c and run from the
   web-runtime suite. */

#define FRAME_MEDIA_SWATCH(color)                                           \
    "data:image/svg+xml,<svg xmlns=%22http://www.w3.org/2000/svg%22 "       \
    "width=%2240%22 height=%2220%22><rect width=%2240%22 height=%2220%22 "  \
    "fill=%22%23" color "%22/></svg>"

/* The test supplies the child document itself: the frame's own fetch is
   cancelled and the child's realm is created over the parsed document. */
static bool frame_media_supply_child(NavigationSession *navigation,
                                     Budget *budget, NavigationFrame *frame,
                                     const char *origin, const char *child,
                                     size_t child_length)
{
    char url[128];
    snprintf(url, sizeof(url), "%schild.html", origin);
    if (frame->fetch_pending) {
        if (!fetch_scheduler_cancel(navigation->page.resource_scheduler,
                                    frame->fetch_request_id,
                                    "test supplies child")
            || !fetch_scheduler_discard(navigation->page.resource_scheduler,
                                        frame->fetch_request_id))
            return false;
        frame->fetch_pending = false;
    }
    if (frame->retired) {
        frame->retired = false;
        if (!script_runtime_set_frame_window_state(
                navigation->page.runtime, frame->parent_handle, true, true,
                false, url, true, &navigation->page.script_result))
            return false;
    }
    if (!document_parse(&frame->document, budget, child, child_length, 16))
        return false;
    frame->runtime = script_runtime_create_deferred(
        &frame->document, budget, 3 * MIB, 1000, url, NULL,
        &frame->script_result);
    if (frame->runtime == NULL) return false;
    script_runtime_set_images(frame->runtime, &frame->images);
    frame->loaded = true;
    navigation->images = &navigation->page.images;
    return true;
}

static const ImageResource *frame_media_snapshot(
    const NavigationSession *navigation, const NavigationFrame *frame)
{
    const ImageResource *snapshot = NULL;
    for (size_t i = 0; i < navigation->page.images.count; i++) {
        const ImageResource *candidate = &navigation->page.images.items[i];
        if (candidate->node == frame->element && !candidate->is_mask
            && !candidate->is_background) snapshot = candidate;
    }
    return snapshot;
}

/* 'r', 'g', 'b' or 'w' for the snapshot pixel at (x, y); '?' otherwise. */
static char frame_media_pixel(const ImageResource *snapshot, int x, int y)
{
    if (snapshot == NULL || snapshot->pixels == NULL || x >= snapshot->width
        || y >= snapshot->height) return '?';
    const unsigned char *pixel = snapshot->pixels
        + ((size_t) y * (size_t) snapshot->width + (size_t) x) * 4u;
    if (pixel[0] > 200 && pixel[1] < 60 && pixel[2] < 60) return 'r';
    if (pixel[0] < 60 && pixel[1] < 60 && pixel[2] > 200) return 'b';
    if (pixel[0] < 60 && pixel[1] > 200 && pixel[2] < 60) return 'g';
    if (pixel[0] > 200 && pixel[1] > 200 && pixel[2] > 200) return 'w';
    return '?';
}

static size_t frame_media_node_entries(const ImageResources *images,
                                       const lxb_dom_node_t *node)
{
    size_t entries = 0;
    for (size_t i = 0; i < images->count; i++)
        if (images->items[i].node == node && !images->items[i].is_mask
            && !images->items[i].is_background) entries++;
    return entries;
}

/* An <img> inside a frame whose source changes shows the new image, as the
   top-level page's does, and the frame keeps one entry for it however often
   it changes. A replacement that fails leaves the element as a failed image
   in both, never the old pixels. */
static int test_frame_image_source_swaps(void)
{
    Budget budget;
    budget_init(&budget, 48 * MIB);
    budget_install_lexbor(&budget);
    static const char child[] =
        "<!doctype html><body style='margin:0;background:#fff'>"
        "<img id=pic alt=x width=40 height=20 style='display:block' "
        "src='" FRAME_MEDIA_SWATCH("ff0000") "' "
        "data-r='" FRAME_MEDIA_SWATCH("ff0000") "' "
        "data-b='" FRAME_MEDIA_SWATCH("0000ff") "' "
        "data-g='" FRAME_MEDIA_SWATCH("00ff00") "' "
        "data-bad='data:image/png;base64,AAAA'></body>";
    static const char parent[] =
        "<!doctype html><body style='margin:0'><iframe id=host "
        "src='/child.html' style='border:0;width:60px;height:30px'>"
        "</iframe></body>";
    static BrowserSession browser;
    static NavigationSession navigation;
    CHECK(browser_session_init(&browser, &budget, 64 * 1024)
          && navigation_init(&navigation, &budget, 4));
    navigation_attach_browser_session(&navigation, &browser);
    navigation_enable_scripts(&navigation, 6 * MIB, 1000);
    navigation_enable_document_scripts(&navigation, 8, 128 * 1024,
                                       128 * 1024, 1000);
    navigation_enable_external_resources(&navigation, 2, 32768, 32768,
                                         8, 65536, 65536, 4 * MIB, 1000);
    CHECK(navigation_commit_html(
              &navigation, navigation_begin(&navigation),
              "https://frame-swap.test/", parent, strlen(parent), 480,
              NULL, NULL, true)
          && navigation_advance_runtime(&navigation, 1, 8)
          && navigation.page.frame_count == 1);
    NavigationFrame *frame = &navigation.page.frames[0];
    CHECK(frame_media_supply_child(&navigation, &budget, frame,
                                   "https://frame-swap.test/", child,
                                   sizeof(child) - 1));
    CHECK(navigation_test_refresh_frame_presentation(&navigation, frame));
    lxb_dom_node_t *pic = find_id(
        lxb_dom_interface_node(frame->document.html), "pic");
    CHECK(pic != NULL);
    CHECK(frame_media_pixel(frame_media_snapshot(&navigation, frame), 10, 10)
          == 'r');
    CHECK(frame_media_node_entries(&frame->images, pic) == 1);

    static const char *const swaps[] = {"b", "r", "b", "g", "r", "g", "b"};
    for (size_t i = 0; i < sizeof(swaps) / sizeof(swaps[0]); i++) {
        char script[96];
        snprintf(script, sizeof(script),
                 "{const p=document.getElementById('pic');p.src=p.dataset.%s}",
                 swaps[i]);
        CHECK(script_runtime_evaluate_diagnostic(
            frame->runtime, script, "https://frame-swap.test/swap.js",
            &frame->script_result));
        CHECK(navigation_test_refresh_frame_presentation(&navigation, frame));
        char shown = frame_media_pixel(
            frame_media_snapshot(&navigation, frame), 10, 10);
        size_t entries = frame_media_node_entries(&frame->images, pic);
        if (shown != swaps[i][0] || entries != 1)
            fprintf(stderr, "frame swap %zu to %s: shown=%c entries=%zu "
                    "table=%zu attempted=%zu\n", i, swaps[i], shown, entries,
                    frame->images.count, frame->images.stats.attempted);
        CHECK(shown == swaps[i][0] && entries == 1);
    }

    /* A replacement that cannot decode: the frame shows what the top-level
       page shows for the same change. */
    CHECK(script_runtime_evaluate_diagnostic(
        frame->runtime,
        "{const p=document.getElementById('pic');p.src=p.dataset.bad}",
        "https://frame-swap.test/swap.js", &frame->script_result));
    CHECK(navigation_test_refresh_frame_presentation(&navigation, frame));
    char failed_shown = frame_media_pixel(
        frame_media_snapshot(&navigation, frame), 10, 10);
    bool frame_entry = images_find_node(&frame->images, pic) != NULL;
    bool frame_failed = images_node_failed(&frame->images, pic);
    navigation_destroy(&navigation);
    browser_session_destroy(&browser);

    static char top_page[sizeof(child) + 64];
    snprintf(top_page, sizeof(top_page), "%s", child);
    CHECK(browser_session_init(&browser, &budget, 64 * 1024)
          && navigation_init(&navigation, &budget, 4));
    navigation_attach_browser_session(&navigation, &browser);
    navigation_enable_scripts(&navigation, 6 * MIB, 1000);
    navigation_enable_document_scripts(&navigation, 8, 128 * 1024,
                                       128 * 1024, 1000);
    navigation_enable_external_resources(&navigation, 2, 32768, 32768,
                                         8, 65536, 65536, 4 * MIB, 1000);
    CHECK(navigation_commit_html(
              &navigation, navigation_begin(&navigation),
              "https://frame-swap.test/", top_page, strlen(top_page), 480,
              NULL, NULL, true)
          && navigation_advance_runtime(&navigation, 1, 8));
    lxb_dom_node_t *top_pic = find_id(
        lxb_dom_interface_node(navigation.page.document.html), "pic");
    CHECK(top_pic != NULL
          && images_find_node(&navigation.page.images, top_pic) != NULL);
    CHECK(script_runtime_evaluate_diagnostic(
        navigation.page.runtime,
        "{const p=document.getElementById('pic');p.src=p.dataset.bad}",
        "https://frame-swap.test/swap.js", &navigation.page.script_result));
    for (int tick = 0; tick < 4; tick++)
        CHECK(navigation_advance_runtime(&navigation, 1, 8));
    bool top_entry = images_find_node(&navigation.page.images, top_pic)
        != NULL;
    bool top_failed = images_node_failed(&navigation.page.images, top_pic);
    navigation_destroy(&navigation);
    browser_session_destroy(&browser);
    if (frame_entry != top_entry || frame_failed != top_failed
        || failed_shown == 'r' || failed_shown == 'b'
        || failed_shown == 'g')
        fprintf(stderr, "failed frame swap: shown=%c entry=%d failed=%d; "
                "top-level entry=%d failed=%d\n", failed_shown, frame_entry,
                frame_failed, top_entry, top_failed);
    CHECK(frame_entry == top_entry && frame_failed == top_failed
          && failed_shown != 'r' && failed_shown != 'b'
          && failed_shown != 'g');
    CHECK(budget.current == 0);
    return 0;
}

/* A frame narrower than the screen lays out at its own width, while its
   device-* media features describe the screen, in its stylesheet as in its
   matchMedia and screen. */
static int test_frame_media_device_features(void)
{
    Budget budget;
    budget_init(&budget, 48 * MIB);
    budget_install_lexbor(&budget);
    static const char child[] =
        "<!doctype html><style>#probe{width:10px;height:10px}"
        "@media (device-width:480px){#probe{width:77px}}"
        "@media (width:200px){#probe{height:33px}}"
        "@media (device-height:272px){#probe{margin-left:5px}}"
        "@media (device-aspect-ratio:30/17){#probe{margin-top:3px}}"
        "</style><body style='margin:0'><div id=probe></div></body>";
    static const char parent[] =
        "<!doctype html><body style='margin:0'><iframe id=host "
        "src='/child.html' style='border:0;width:200px;height:100px'>"
        "</iframe></body>";
    static BrowserSession browser;
    static NavigationSession navigation;
    CHECK(browser_session_init(&browser, &budget, 64 * 1024)
          && navigation_init(&navigation, &budget, 4));
    navigation_attach_browser_session(&navigation, &browser);
    navigation_enable_scripts(&navigation, 6 * MIB, 1000);
    navigation_enable_document_scripts(&navigation, 8, 128 * 1024,
                                       128 * 1024, 1000);
    CHECK(navigation_commit_html(
              &navigation, navigation_begin(&navigation),
              "https://frame-media.test/", parent, strlen(parent), 480,
              NULL, NULL, true)
          && navigation_advance_runtime(&navigation, 1, 8)
          && navigation.page.frame_count == 1);
    NavigationFrame *frame = &navigation.page.frames[0];
    CHECK(frame_media_supply_child(&navigation, &budget, frame,
                                   "https://frame-media.test/", child,
                                   sizeof(child) - 1));
    CHECK(navigation_test_refresh_frame_presentation(&navigation, frame));
    CHECK(script_runtime_evaluate_diagnostic(
        frame->runtime,
        "globalThis.pocSummary=(()=>{const s=getComputedStyle(document.getElementById('probe')),"
        "m=q=>matchMedia(q).matches;return [s.width,s.height,s.marginLeft,"
        "s.marginTop,m('(device-width:480px)'),m('(width:200px)'),"
        "m('(device-height:272px)'),m('(device-aspect-ratio:30/17)'),"
        "screen.width,innerWidth].join()})()",
        "https://frame-media.test/probe.js", &frame->script_result));
    static const char expected[] =
        "77px,33px,5px,3px,true,true,true,true,480,200";
    if (strcmp(frame->script_result.summary, expected) != 0)
        fprintf(stderr, "frame media features: %s\n",
                frame->script_result.summary);
    CHECK(strcmp(frame->script_result.summary, expected) == 0);
    navigation_destroy(&navigation);
    browser_session_destroy(&browser);
    CHECK(budget.current == 0);
    return 0;
}

/* Solid 8x8 PNG photographs (red, blue, green), served from the session's
   HTTP cache so they spend the image count as network images do. */
#define SWAP_PHOTO_R "iVBORw0KGgoAAAANSUhEUgAAAAgAAAAICAIAAABLbSncAAAAEUlEQVR42mP4z8CAFTEMLQkAKP8/wc53yE8AAAAASUVORK5CYII="
#define SWAP_PHOTO_B "iVBORw0KGgoAAAANSUhEUgAAAAgAAAAICAIAAABLbSncAAAAEElEQVR42mNgYPiPAw0pCQCpcD/B/MtF/AAAAABJRU5ErkJggg=="
#define SWAP_PHOTO_G "iVBORw0KGgoAAAANSUhEUgAAAAgAAAAICAIAAABLbSncAAAAEElEQVR42mNg+M+AHQ0tCQDpMD/BHYHcAQAAAABJRU5ErkJggg=="
#define SWAP_ORIGIN "https://swap.test/"
/* External SVG artwork whose raster is larger than an icon, so it spends
   the image count, and whose markup is kept for retargeting. */
#define SWAP_ART "<svg xmlns='http://www.w3.org/2000/svg' width='100' " \
    "height='100'><rect width='100' height='100' fill='#ff0000'/></svg>"

static bool swap_cache_put(BrowserSession *browser, Budget *budget,
                           const char *path, const char *base64,
                           const char *raw)
{
    unsigned char *data = NULL;
    size_t length = 0;
    if (base64 != NULL) {
        char url[256];
        char media[32];
        snprintf(url, sizeof(url), "data:image/png;base64,%s", base64);
        if (data_url_decode(budget, url, strlen(url), 4096u, &data, &length,
                            media, sizeof(media)) != DATA_URL_DECODED)
            return false;
    } else {
        length = strlen(raw);
        data = budget_malloc(budget, length);
        if (data == NULL) return false;
        memcpy(data, raw, length);
    }
    BrowserSharedBody *body = browser_shared_body_take(budget, data, length);
    if (body == NULL) {
        budget_free(budget, data);
        return false;
    }
    char url[128];
    snprintf(url, sizeof(url), SWAP_ORIGIN "%s", path);
    TilefinchRequestContext context = {
        .target_url = url, .initiator_url = SWAP_ORIGIN,
        .top_level_url = SWAP_ORIGIN, .method = "GET",
        .mode = TILEFINCH_REQUEST_MODE_NO_CORS,
        .credentials = TILEFINCH_CREDENTIALS_INCLUDE,
        .destination = TILEFINCH_DESTINATION_IMAGE};
    TilefinchResourceGrant grant = {.destination = TILEFINCH_DESTINATION_IMAGE,
        .mode = TILEFINCH_REQUEST_MODE_NO_CORS,
        .credentials = TILEFINCH_CREDENTIALS_INCLUDE,
        .final_same_origin = true, .final_same_site = true};
    bool stored = browser_session_cache_put_http_shared_resource(
            browser, url, body, "", "",
            base64 == NULL && raw[0] == '<' ? "image/svg+xml" : "image/png",
            "immutable", "",
            tilefinch_platform_monotonic_time_ns(), &context, &grant)
        && browser_session_cache_set_resource_response_provenance(
            browser, url, &context, url, "");
    browser_shared_body_release(body);
    return stored;
}

typedef struct {
    NavigationSession *navigation;
    NavigationFrame *frame;
} SwapHost;

static ImageResources *swap_images(const SwapHost *host)
{
    return host->frame != NULL ? &host->frame->images
                               : &host->navigation->page.images;
}

static lxb_dom_node_t *swap_find(const SwapHost *host, const char *id)
{
    const PocDocument *document = host->frame != NULL
        ? &host->frame->document : &host->navigation->page.document;
    return find_id(lxb_dom_interface_node(document->html), id);
}

/* Runs `script` in the document and lets the change reach its images: the
   top-level page's relayout, or the frame's presentation refresh. */
static bool swap_run(const SwapHost *host, const char *script)
{
    ScriptRuntime *runtime = host->frame != NULL
        ? host->frame->runtime : host->navigation->page.runtime;
    ScriptResult *result = host->frame != NULL
        ? &host->frame->script_result : &host->navigation->page.script_result;
    if (!script_runtime_evaluate_diagnostic(
            runtime, script, SWAP_ORIGIN "swap.js", result)) return false;
    if (host->frame != NULL)
        return navigation_test_refresh_frame_presentation(
            host->navigation, host->frame);
    for (int tick = 0; tick < 4; tick++)
        if (!navigation_advance_runtime(host->navigation, 1, 8)) return false;
    return true;
}

/* The color `id` shows, '-' without an image: its entry's first pixel on
   the page, the frame's snapshot (where `id` is the `row`th 8 px tall
   block) in a frame, whose entries keep only their encoded bytes until
   painted. '2' when the element holds more than one entry. */
static char swap_shown(const SwapHost *host, const char *id, int row)
{
    const ImageResources *images = swap_images(host);
    lxb_dom_node_t *node = swap_find(host, id);
    if (node == NULL) return '?';
    if (frame_media_node_entries(images, node) > 1u) return '2';
    const ImageResource *entry = images_find_node(images, node);
    if (entry == NULL) return '-';
    if (host->frame == NULL) return frame_media_pixel(entry, 0, 0);
    return frame_media_pixel(frame_media_snapshot(host->navigation,
                                                  host->frame),
                             3, 3 + 8 * row);
}

static bool swap_set(const SwapHost *host, const char *id, const char *src)
{
    char script[256];
    snprintf(script, sizeof(script),
             "document.getElementById('%s').src='%s'", id, src);
    return swap_run(host, script);
}

static bool swap_open(NavigationSession *navigation, BrowserSession *browser,
                      Budget *budget, size_t maximum_images, bool in_frame,
                      bool art, const char *body, SwapHost *host)
{
    static const char bad[] = "this body is not an image";
    char path[32];
    if (!browser_session_init(browser, budget, 256 * 1024)
        || !navigation_init(navigation, budget, 4)
        || !swap_cache_put(browser, budget, "r.png", SWAP_PHOTO_R, NULL)
        || !swap_cache_put(browser, budget, "b.png", SWAP_PHOTO_B, NULL)
        || !swap_cache_put(browser, budget, "g.png", SWAP_PHOTO_G, NULL))
        return false;
    for (int i = 0; i < 48; i++) {
        /* The cache holds 64 entries: either set, never both. */
        snprintf(path, sizeof(path), art ? "art-%02d.svg" : "bad-%02d.png",
                 i);
        if (!swap_cache_put(browser, budget, path, NULL,
                            art ? SWAP_ART : bad)) return false;
    }
    navigation_attach_browser_session(navigation, browser);
    navigation_enable_scripts(navigation, 6 * MIB, 1000);
    navigation_enable_document_scripts(navigation, 8, 128 * 1024,
                                       128 * 1024, 1000);
    navigation_enable_external_resources(navigation, 2, 32768, 32768,
                                         maximum_images, 65536, 65536,
                                         4 * MIB, 1000);
    static char child[1024];
    snprintf(child, sizeof(child),
             "<!doctype html><body style='margin:0;background:#fff'>%s"
             "</body>", body);
    static const char parent[] =
        "<!doctype html><body style='margin:0'><iframe id=host "
        "src='/child.html' style='border:0;width:60px;height:60px'>"
        "</iframe></body>";
    const char *page = in_frame ? parent : child;
    if (!navigation_commit_html(
            navigation, navigation_begin(navigation), SWAP_ORIGIN, page,
            strlen(page), 480, NULL, NULL, true)
        || !navigation_advance_runtime(navigation, 1, 8)) return false;
    host->navigation = navigation;
    host->frame = NULL;
    if (!in_frame) {
        for (int tick = 0; tick < 4; tick++)
            if (!navigation_advance_runtime(navigation, 1, 8)) return false;
        return true;
    }
    if (navigation->page.frame_count != 1) return false;
    host->frame = &navigation->page.frames[0];
    return frame_media_supply_child(navigation, budget, host->frame,
                                    SWAP_ORIGIN, child, strlen(child))
        && navigation_test_refresh_frame_presentation(navigation,
                                                      host->frame);
}

#define SWAP_IMG(id, src) \
    "<img id=" id " alt=x width=8 height=8 style='display:block' " src ">"

/* Return a failed source's slot without reloading the element's live CSS
   background or shifting it out from under existing layout bindings. */
static int test_frame_failed_image_keeps_background(void)
{
    Budget budget;
    budget_init(&budget, 48 * MIB);
    budget_install_lexbor(&budget);
    static BrowserSession browser;
    static NavigationSession navigation;
    SwapHost host;
    CHECK(swap_open(&navigation, &browser, &budget, 2, true, false,
                    "<img id=pic alt=x width=8 height=8 src=/bad-00.png "
                    "style='display:block;background-image:url(/g.png)'>",
                    &host));
    ImageResources *images = swap_images(&host);
    lxb_dom_node_t *pic = swap_find(&host, "pic");
    CHECK(pic != NULL && images_node_failed(images, pic));
    CHECK(images_find_background_node(images, pic) != NULL);
    CHECK(images->stats.attempted == 2u);
    size_t discovered = images->stats.discovered;
    CHECK(swap_set(&host, "pic", "/b.png"));
    CHECK(swap_shown(&host, "pic", 0) == 'b');
    CHECK(!images_node_failed(images, pic));
    CHECK(images_find_background_node(images, pic) != NULL);
    CHECK(images->stats.discovered == discovered + 1u);
    navigation_destroy(&navigation);
    browser_session_destroy(&browser);
    CHECK(budget.current == 0);
    return 0;
}

/* An image whose source changes gives its slot of the image count back: a
   page that shows one photograph at a time, swapping between two it has
   already shown many more times than the count allows, still shows each
   new source, and an unrelated image loads afterwards. A frame's images
   account for it as the page's do. */
static int test_image_swap_returns_slot(bool in_frame)
{
    Budget budget;
    budget_init(&budget, 48 * MIB);
    budget_install_lexbor(&budget);
    static BrowserSession browser;
    static NavigationSession navigation;
    SwapHost host;
    CHECK(swap_open(&navigation, &browser, &budget, 2, in_frame, false,
                    SWAP_IMG("pic", "src=/r.png") SWAP_IMG("late", ""),
                    &host));
    CHECK(swap_shown(&host, "pic", 0) == 'r');
    for (int i = 0; i < 10; i++) {
        char want = i % 2 == 0 ? 'b' : 'r';
        CHECK(swap_set(&host, "pic", want == 'b' ? "/b.png" : "/r.png"));
        char shown = swap_shown(&host, "pic", 0);
        if (shown != want)
            fprintf(stderr, "%s swap %d: shown=%c want=%c attempted=%zu "
                    "exempt=%zu table=%zu\n", in_frame ? "frame" : "page", i,
                    shown, want, swap_images(&host)->stats.attempted,
                    swap_images(&host)->stats.count_exempt,
                    swap_images(&host)->count);
        CHECK(shown == want);
    }
    CHECK(swap_set(&host, "late", "/g.png"));
    char late = swap_shown(&host, "late", 1);
    if (late != 'g')
        fprintf(stderr, "%s unrelated image: shown=%c attempted=%zu\n",
                in_frame ? "frame" : "page", late,
                swap_images(&host)->stats.attempted);
    CHECK(late == 'g' && swap_shown(&host, "pic", 0) == 'r');
    navigation_destroy(&navigation);
    browser_session_destroy(&browser);
    CHECK(budget.current == 0);
    return 0;
}

/* Swapping to sources that fail returns their slots too, but only until the
   page's image work reaches its limit (IMAGE_SWAP_WORK_FACTOR times the
   image count): past it no swap returns a slot, so the failing sources stop
   being fetched once the count is spent, while the table, its failure
   records and memory stay bounded. A frame's images do the same. */
static int test_image_swap_work_limit(bool in_frame)
{
    Budget budget;
    budget_init(&budget, 48 * MIB);
    budget_install_lexbor(&budget);
    static BrowserSession browser;
    static NavigationSession navigation;
    SwapHost host;
    CHECK(swap_open(&navigation, &browser, &budget, 2, in_frame, false,
                    SWAP_IMG("pic", "src=/r.png"), &host));
    ImageResources *images = swap_images(&host);
    CHECK(swap_shown(&host, "pic", 0) == 'r');
    char path[32];
    for (int i = 0; i < 4; i++) {
        snprintf(path, sizeof(path), "/bad-%02d.png", i);
        CHECK(swap_set(&host, "pic", path));
        CHECK(swap_shown(&host, "pic", 0) == '-');
    }
    /* The page does not fetch a failed source again while it stays
       current, so other changes spend nothing. */
    size_t failed_hits = images->stats.cache_hits;
    if (!in_frame) {
        CHECK(swap_run(&host, "document.body.style.color='#010101'"));
        CHECK(images->stats.cache_hits == failed_hits);
    }
    CHECK(swap_set(&host, "pic", "/b.png"));
    char recovered = swap_shown(&host, "pic", 0);
    if (recovered != 'b')
        fprintf(stderr, "%s after failing swaps: shown=%c attempted=%zu\n",
                in_frame ? "frame" : "page", recovered,
                images->stats.attempted);
    CHECK(recovered == 'b');
    size_t hits = 0, memory = 0;
    for (int i = 4; i < 48; i++) {
        if (i == 40) {
            hits = images->stats.cache_hits;
            memory = budget.current;
        }
        snprintf(path, sizeof(path), "/bad-%02d.png", i);
        CHECK(swap_set(&host, "pic", path));
        CHECK(swap_shown(&host, "pic", 0) == '-');
        CHECK(images->count <= 1u && images->failure_count <= 1u);
    }
    /* Every fetch is work, so swaps return slots until the sixteenth; the
       one slot the failure then shown leaves takes one fetch more. */
    size_t bound = IMAGE_SWAP_WORK_FACTOR * 2u + 1u;
    if (images->stats.cache_hits != hits || hits != bound
        || images->stats.work != bound || budget.current > memory)
        fprintf(stderr, "%s failing swaps: hits=%zu then %zu work=%zu "
                "memory=%zu then %zu attempted=%zu\n",
                in_frame ? "frame" : "page", hits, images->stats.cache_hits,
                images->stats.work, memory, budget.current,
                images->stats.attempted);
    CHECK(images->stats.cache_hits == hits && hits == bound
          && images->stats.work == bound && budget.current <= memory);
    navigation_destroy(&navigation);
    browser_session_destroy(&browser);
    CHECK(budget.current == 0);
    return 0;
}

/* A short data: URL spends no image count and no image work, as before:
   swapping one many more times than the work limit leaves a photograph's
   swaps their slots. */
static int test_image_swap_short_data_urls(void)
{
    Budget budget;
    budget_init(&budget, 48 * MIB);
    budget_install_lexbor(&budget);
    static BrowserSession browser;
    static NavigationSession navigation;
    SwapHost host;
    CHECK(swap_open(&navigation, &browser, &budget, 1, false, false,
                    SWAP_IMG("pic", "src=/r.png")
                    SWAP_IMG("icon", "src='data:image/png;base64,"
                             SWAP_PHOTO_G "'"), &host));
    ImageResources *images = swap_images(&host);
    CHECK(swap_shown(&host, "pic", 0) == 'r' && swap_shown(&host, "icon", 1) == 'g');
    size_t spent = images->stats.attempted - images->stats.count_exempt;
    static const char *const icons[] = {
        "data:image/png;base64," SWAP_PHOTO_B,
        "data:image/png;base64," SWAP_PHOTO_R,
        "data:image/png;base64," SWAP_PHOTO_G
    };
    for (int i = 0; i < 12; i++) {
        CHECK(swap_set(&host, "icon", icons[i % 3]));
        CHECK(swap_shown(&host, "icon", 1) == "brg"[i % 3]);
        CHECK(images->stats.attempted - images->stats.count_exempt == spent);
    }
    CHECK(swap_set(&host, "pic", "/b.png"));
    CHECK(swap_shown(&host, "pic", 0) == 'b');
    CHECK(swap_set(&host, "pic", "/r.png"));
    char shown = swap_shown(&host, "pic", 0);
    if (shown != 'r')
        fprintf(stderr, "photo after icon swaps: shown=%c attempted=%zu "
                "exempt=%zu\n", shown, images->stats.attempted,
                images->stats.count_exempt);
    CHECK(shown == 'r');
    navigation_destroy(&navigation);
    browser_session_destroy(&browser);
    CHECK(budget.current == 0);
    return 0;
}

/* An <img> cycling through external SVG sources is held to the same image
   work limit as one cycling rasters: fetches stop once the work reaches
   IMAGE_SWAP_WORK_FACTOR times the count and the table and memory stay
   bounded, while an SVG image whose element only restyles keeps its raster
   and its slot and spends no work, before the limit and after it. A frame's
   images do the same. */
static int test_image_svg_swap_work_limit(bool in_frame)
{
    Budget budget;
    budget_init(&budget, 48 * MIB);
    budget_install_lexbor(&budget);
    static BrowserSession browser;
    static NavigationSession navigation;
    SwapHost host;
    CHECK(swap_open(&navigation, &browser, &budget, 2, in_frame, true,
                    SWAP_IMG("pic", "src=/art-00.svg")
                    SWAP_IMG("keep", "src=/art-47.svg"), &host));
    ImageResources *images = swap_images(&host);
    CHECK(swap_shown(&host, "pic", 0) == 'r'
          && swap_shown(&host, "keep", 1) == 'r');
    size_t spent = images->stats.attempted - images->stats.count_exempt;
    size_t work = images->stats.work;
    size_t hits = images->stats.cache_hits;
    for (int i = 0; i < 6; i++) {
        char script[96];
        snprintf(script, sizeof(script),
                 "document.getElementById('keep').style.opacity='0.9%d'", i);
        CHECK(swap_run(&host, script));
        size_t now = images->stats.attempted - images->stats.count_exempt;
        if (swap_shown(&host, "keep", 1) != 'r' || now > spent
            || images->stats.work != work || images->stats.cache_hits != hits)
            fprintf(stderr, "%s svg restyle %d: shown=%c spent=%zu->%zu "
                    "work=%zu->%zu\n", in_frame ? "frame" : "page", i,
                    swap_shown(&host, "keep", 1), spent, now, work,
                    images->stats.work);
        CHECK(swap_shown(&host, "keep", 1) == 'r' && now <= spent
              && images->stats.work == work
              && images->stats.cache_hits == hits);
    }
    char path[32];
    size_t stopped = 0, memory = 0;
    for (int i = 1; i < 47; i++) {
        if (i == 40) {
            stopped = images->stats.cache_hits;
            memory = budget.current;
        }
        snprintf(path, sizeof(path), "/art-%02d.svg", i);
        CHECK(swap_set(&host, "pic", path));
        CHECK(images->count <= 2u && images->failure_count == 0u);
    }
    /* Every fetch is work, so swaps return slots until the sixteenth; then
       the count is spent, the other slot being the restyled image's. */
    size_t bound = IMAGE_SWAP_WORK_FACTOR * 2u;
    if (images->stats.cache_hits != stopped || stopped != bound
        || budget.current > memory)
        fprintf(stderr, "%s svg swaps: hits=%zu then %zu work=%zu memory=%zu "
                "then %zu attempted=%zu exempt=%zu\n",
                in_frame ? "frame" : "page", stopped,
                images->stats.cache_hits, images->stats.work, memory,
                budget.current, images->stats.attempted,
                images->stats.count_exempt);
    CHECK(images->stats.cache_hits == stopped && stopped == bound
          && images->stats.work == bound && budget.current <= memory);
    /* Past the limit a restyled SVG image still keeps its raster and slot. */
    spent = images->stats.attempted - images->stats.count_exempt;
    work = images->stats.work;
    for (int i = 0; i < 4; i++) {
        char script[96];
        snprintf(script, sizeof(script),
                 "document.getElementById('keep').style.opacity='0.8%d'", i);
        CHECK(swap_run(&host, script));
        CHECK(swap_shown(&host, "keep", 1) == 'r'
              && images->stats.attempted - images->stats.count_exempt
                     <= spent
              && images->stats.work == work);
    }
    navigation_destroy(&navigation);
    browser_session_destroy(&browser);
    CHECK(budget.current == 0);
    return 0;
}

static int test_image_swap_accounting(void)
{
    CHECK(test_image_swap_returns_slot(false) == 0);
    CHECK(test_image_swap_returns_slot(true) == 0);
    CHECK(test_image_swap_work_limit(false) == 0);
    CHECK(test_image_swap_work_limit(true) == 0);
    CHECK(test_image_swap_short_data_urls() == 0);
    CHECK(test_image_svg_swap_work_limit(false) == 0);
    CHECK(test_image_svg_swap_work_limit(true) == 0);
    return 0;
}
