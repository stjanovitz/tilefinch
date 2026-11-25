static BrowserNavigationJobStatus youtube_journey_finish(BrowserEngine *engine)
{
    BrowserNavigationJobStatus status = browser_engine_navigation_status(engine);
    for (unsigned turn = 0; turn < 512u && status == BROWSER_NAVIGATION_JOB_PENDING; turn++)
        status = browser_engine_pump_navigation(engine, NULL);
    return status;
}

static lxb_dom_node_t *youtube_journey_id(BrowserEngine *engine, const char *id)
{
    return test_reader_find_id(lxb_dom_interface_node(
        browser_engine_navigation(engine)->page.document.html), id);
}

static bool youtube_journey_search(BrowserEngine *engine, const char *query)
{
    lxb_dom_node_t *search = youtube_journey_id(engine, "yt-search");
    lxb_dom_node_t *form = search ? search->parent : NULL;
    lxb_dom_node_t *button = form ? form->last_child : NULL;
    ControllerAction action = {0};
    return search && button && button->local_name == LXB_TAG_BUTTON
        && browser_engine_focus_node(engine, search)
        && browser_engine_replace_text(engine, query, strlen(query))
        && browser_engine_focus_node(engine, button)
        && browser_engine_activate(engine, &action)
        && strstr(action.url, query)
        && browser_engine_begin_navigation_action(engine, &action, MIB, 1000);
}

/* Choose by href and role, not by position in the rebuilt focus list. */
static lxb_dom_node_t *youtube_journey_link(BrowserEngine *engine, const char *id,
                                             const char *role)
{
    const LayoutDocument *layout = &browser_engine_navigation(engine)->page.layout;
    for (size_t i = 0; i < layout->node_box_count; i++) {
        lxb_dom_node_t *node = layout->node_boxes[i].node;
        if (node->local_name != LXB_TAG_A) continue;
        size_t length = 0, href_length = 0;
        const char *class = document_attribute(node, "class", &length);
        const char *href = document_attribute(node, "href", &href_length);
        if (class && length >= strlen(role) && !memcmp(class, role, strlen(role))
            && href && href_length && strstr(href, id)) return node;
    }
    return NULL;
}

static bool youtube_journey_title_is(lxb_dom_node_t *card, const char *title)
{
    lxb_dom_node_t *wrapper = card == NULL ? NULL : card->first_child;
    lxb_dom_node_t *info = wrapper == NULL ? NULL : wrapper->next;
    if (!info || !info->first_child) return false;
    size_t length = 0;
    lxb_char_t *text = lxb_dom_node_text_content(info->first_child, &length);
    bool equal = text && length == strlen(title) && !memcmp(text, title, length);
    if (text) lxb_dom_document_destroy_text(card->owner_document, text);
    return equal;
}

static uint32_t youtube_journey_thumb_ink(BrowserEngine *engine, lxb_dom_node_t *image)
{
    const NavigationSession *nav = browser_engine_navigation(engine);
    const LayoutNodeBox *box = layout_box_for_node(&nav->page.layout, image);
    if (!box) return 0;
    const int scroll = navigation_current(nav)->scroll_y;
    const int stride = nav->viewport.device_width;
    const int height = nav->viewport.device_height;
    int x = viewport_css_to_device(&nav->viewport, box->x);
    int y = viewport_css_to_device(&nav->viewport, box->y - scroll);
    int right = viewport_css_to_device(&nav->viewport, box->x + box->width);
    int bottom = viewport_css_to_device(&nav->viewport, box->y + box->height - scroll);
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    if (right > stride) right = stride;
    if (bottom > height) bottom = height;
    size_t count = 0;
    const uint16_t *pixels = browser_engine_framebuffer(engine, &count);
    if (!pixels || count < (size_t)stride * (size_t)height || x >= right || y >= bottom) return 0;
    uint32_t hash = 2166136261u;
    for (int py = y; py < bottom; py++)
        for (int px = x; px < right; px++)
            hash = (hash ^ pixels[(size_t)py * (size_t)stride + (size_t)px]) * 16777619u;
    return hash;
}

/* A has an admitted, slow response; the user cancels and picks B. Its
   timeout must preserve the list; its explicit retry must publish only B. */
static int test_youtube_interrupted_details(void)
{
    const char *search = "https://m.youtube.com/results?search_query=fixture";
    const char *a = "https://m.youtube.com/watch?v=TFTEST00001&bpctr=9999999999&has_verified=1";
    const char *b = "https://m.youtube.com/watch?v=TFTEST00009&bpctr=9999999999&has_verified=1";
    const char *ok = "<script>var ytInitialData={};</script><script>var ytInitialPlayerResponse="
        "{\"videoDetails\":{\"videoId\":\"TFTEST00009\",\"title\":\"Confirmed ninth result\"}};</script>";
    size_t length = 0;
    char *list = youtube_costs_list_page("videoRenderer", "videoId", "TFTEST%05d",
                                          "title", "Entry %d", &length);
    CHECK(list);
    YoutubeReplayRecord records[] = {{"GET",search,"text/html",list,length},
        {"GET",a,"text/html","",0}, {"GET",b,"text/html","",0}, {"GET",b,"text/html",ok,strlen(ok)}};
    const unsigned delays[] = {0,80,10,2}, statuses[] = {200,503,UINT_MAX,200};
    char dir[128], error[256] = {0}, title[48], channel[24];
    CHECK(write_youtube_records_replay_delayed(dir,records,3,delays,statuses)
        && fetch_trace_replay_begin_response_keyed(dir,error,sizeof(error)));
    BrowserDeviceProfile profile; BrowserConfig config;
    browser_device_profile_psp3000(&profile); browser_config_init(&config,&profile);
    config.resources.enabled = config.resources.web_fonts_enabled = false;
    BrowserEngine *e = browser_engine_create(&config,error,sizeof(error));
    CHECK(e && browser_engine_load_url(e,search,true));
    ControllerAction action = {0};
    CHECK(browser_engine_focus_node(e,youtube_journey_link(e,"TFTEST00001","details"))
        && browser_engine_activate(e,&action)
        && browser_engine_youtube_details_summary(e,&action,title,sizeof(title),channel,sizeof(channel))
        && !strcmp(title,"Entry 1")
        && browser_engine_begin_navigation_action(e,&action,MIB,3000));
    for (unsigned i = 0; i < 5; ++i) CHECK(browser_engine_pump_navigation(e,NULL) == BROWSER_NAVIGATION_JOB_PENDING);
    ControllerAction early = {0};
    CHECK(browser_engine_youtube_preview_play(e, &early) && early.prefer_native_media
        && early.type == CONTROLLER_ACTION_NAVIGATE && !strcmp(early.method, "GET")
        && !strcmp(early.url, action.url));
    browser_engine_cancel_navigation(e,"Back");
    CHECK(!browser_engine_youtube_preview_play(e, &early));
    lxb_dom_node_t *selected = youtube_journey_link(e,"TFTEST00009","details");
    CHECK(selected && browser_engine_scroll_by(e,500) && browser_engine_focus_node(e,selected) && browser_engine_render_frame(e,NULL));
    BrowserViewSnapshot before, after;
    CHECK(browser_engine_view_snapshot(e,&before) && before.scroll_y > 0);
    CHECK(!browser_engine_youtube_details_summary(e,&action,title,sizeof(title),channel,sizeof(channel)));
    CHECK(browser_engine_activate(e,&action));
    CHECK(browser_engine_youtube_details_summary(e,&action,title,sizeof(title),channel,sizeof(channel))
        && !strcmp(title,"Entry 9"));
    CHECK(browser_engine_begin_navigation_action(e,&action,MIB,3000));
    CHECK(browser_engine_youtube_preview_play(e, &early) && strstr(early.url,"TFTEST00009"));
    CHECK(youtube_journey_finish(e) == BROWSER_NAVIGATION_JOB_FAILED);
    CHECK(browser_engine_view_snapshot(e,&after) && !after.loading && !strcmp(before.url,after.url)
        && before.scroll_y == after.scroll_y && controller_focused_node(browser_engine_controller(e)) == selected);
    FetchTraceReplayStats stats = {0};
    CHECK(fetch_trace_replay_stats(&stats));
    if (stats.matched_request_count != 3 || stats.unmatched_request_count)
        fprintf(stderr,"Details replay matched=%zu unmatched=%zu served=%zu: %s\n",stats.matched_request_count,
                stats.unmatched_request_count,fetch_trace_replay_served_count(),browser_engine_last_error(e));
    CHECK(stats.matched_request_count == 3 && !stats.unmatched_request_count);
    fetch_trace_end(); remove_youtube_records_replay(dir,3);
    /* Make the good response available only after explicit Retry. A keyed
       replay can otherwise prefer it over the matching transport failure. */
    CHECK(write_youtube_records_replay(dir,records+3,1)
        && fetch_trace_replay_begin_response_keyed(dir,error,sizeof(error)));
    CHECK(browser_engine_activate(e,&action) && browser_engine_begin_navigation_action(e,&action,MIB,3000)
        && youtube_journey_finish(e) == BROWSER_NAVIGATION_JOB_SUCCEEDED);
    /* The new video, not the retired A or a stale play action, can be
       selected on its very first frame; no image or idle tick is needed. */
    lxb_dom_node_t *play = youtube_journey_link(e, "TFTEST00009", "watch-target");
    CHECK(play && browser_engine_render_frame(e, NULL) && browser_engine_focus_node(e, play)
        && browser_engine_activate(e, &action) && action.prefer_native_media
        && strstr(action.url, "TFTEST00009"));
    for (unsigned i = 0; i < 100; ++i) (void)browser_engine_run_idle_work(e,NULL);
    CHECK(strstr(browser_engine_navigation(e)->page.document_url,"TFTEST00009")
        && fetch_trace_replay_served_count() == 1
        && browser_engine_begin_navigation_history(e,false,MIB,3000)
        && youtube_journey_finish(e) == BROWSER_NAVIGATION_JOB_SUCCEEDED && browser_engine_render_frame(e,NULL)
        && browser_engine_view_snapshot(e,&after) && after.scroll_y == before.scroll_y
        && controller_focused_node(browser_engine_controller(e)) == youtube_journey_link(e,"TFTEST00009","details"));
    Budget *owned = browser_engine_budget(e);
    CHECK(browser_engine_shutdown(e) && !owned->current && !owned->external_reserved);
    browser_engine_destroy(e); fetch_trace_end(); remove_youtube_records_replay(dir,1); free(list);
    return 0;
}

static int test_youtube_superseded_search_journey(void)
{
    static const char home[] = "https://www.youtube.com/";
    static const char result[] = "https://www.youtube.com/results?search_query=journey-b";
    static const char selected_id[] = "TFNEW000009";
    static const char selected_title[] = "Ninth fixture: a deliberately long selected video title "
        "with enough ordinary words to wrap at each native page zoom setting";
    static const char stale[] = "<script>var ytInitialData={\"contents\":["
        "{\"videoRenderer\":{\"videoId\":\"TFOLD000001\",\"title\":{\"simpleText\":\"Retired result\"}}}]};</script>";
    char list[8192], watch[512];
    size_t used = (size_t) snprintf(list, sizeof(list), "<script>var ytInitialData={\"contents\":[");
    for (unsigned i = 1; i <= 10u; i++) {
        int n = snprintf(list + used, sizeof(list) - used,
            "%s{\"videoRenderer\":{\"videoId\":\"TFNEW%06u\",\"title\":{\"simpleText\":\"%s\"},"
            "\"shortBylineText\":{\"simpleText\":\"Fixture studio\"}}}",
            i == 1u ? "" : ",", i, i == 9u ? selected_title : "Other results");
        CHECK(n > 0 && (size_t)n < sizeof(list) - used);
        used += (size_t)n;
    }
    int n = snprintf(list + used, sizeof(list) - used, "]};</script>");
    CHECK(n > 0 && (size_t)n < sizeof(list) - used);
    n = snprintf(watch, sizeof(watch), "<script>var ytInitialData={};</script>"
        "<script>var ytInitialPlayerResponse={\"videoDetails\":{\"videoId\":\"%s\","
        "\"title\":\"%s\",\"author\":\"Fixture studio\",\"lengthSeconds\":\"42\"}};</script>",
        selected_id, selected_title);
    CHECK(n > 0 && (size_t)n < sizeof(watch));
    /* A solid 4x4 PNG: the provider uses a .jpg URL, but the image pipeline
       recognizes the payload's format and preserves the declared box. */
    static const char thumb[] =
        "\x89\x50\x4e\x47\x0d\x0a\x1a\x0a\x00\x00\x00\x0d\x49\x48\x44\x52"
        "\x00\x00\x00\x04\x00\x00\x00\x04\x08\x02\x00\x00\x00\x26\x93\x09"
        "\x29\x00\x00\x00\x10\x49\x44\x41\x54\x78\x9c\x63\x38\x63\x6c\x0c"
        "\x47\x0c\xc4\x71\x00\xfb\x13\x13\x21\x2c\x5d\xcd\x23\x00\x00\x00"
        "\x00\x49\x45\x4e\x44\xae\x42\x60\x82";
    char *feed_identity = NULL, *feed = NULL;
    YoutubeReplayRecord records[] = {
        youtube_fixture_record("GET", "https://m.youtube.com/", "text/html",
            "http-youtube-personalized/0000.body", &feed_identity),
        {"GET", "https://m.youtube.com/results?search_query=journey-a", "text/html", stale, strlen(stale)},
        {"GET", "https://m.youtube.com/results?search_query=journey-b", "text/html", list, strlen(list)},
        {"GET", "https://m.youtube.com/watch?v=TFNEW000009&bpctr=9999999999&has_verified=1", "text/html", watch, strlen(watch)},
        {"GET", "https://i.ytimg.com/vi/TFNEW000009/mqdefault.jpg", "image/png", thumb, sizeof(thumb) - 1u},
        youtube_fixture_record("POST", "https://m.youtube.com/youtubei/v1/browse?key=fixture-key&prettyPrint=false",
            "application/json", "http-youtube-personalized/0001.body", &feed),
    };
    CHECK(feed_identity && feed);
    const unsigned delays[] = {0u, 80u, 5u, 0u, 20u, 80u};
    static const int zooms[] = {100, 125, 150};
    for (unsigned lane = 0; lane < 3u; lane++) {
        const unsigned old_status = lane == 1u ? 503u : 200u;
        const unsigned statuses[] = {200u, old_status, 200u, 200u, 200u, old_status};
        char directory[128], error[256] = {0};
        CHECK(write_youtube_records_replay_delayed(directory, records, 6, delays, statuses)
            && fetch_trace_replay_begin_response_keyed(directory, error, sizeof(error)));
        BrowserDeviceProfile profile;
        BrowserConfig config;
        browser_device_profile_psp3000(&profile);
        browser_config_init(&config, &profile);
        config.resources.enabled = true;
        config.resources.web_fonts_enabled = false;
        BrowserEngine *engine = browser_engine_create(&config, error, sizeof(error));
        CHECK(engine && browser_session_cookie_set_http(browser_engine_session(engine),
            "https://m.youtube.com/", "SAPISID=synthetic-journey; Secure; Domain=youtube.com; Path=/")
            && browser_engine_load_url(engine, home, true)
            && browser_engine_render_frame(engine, NULL));
        ControllerAction action = {0};
        CHECK(browser_engine_focus_node(engine, youtube_journey_id(engine, "yt-feed-refresh"))
            && browser_engine_activate(engine, &action));
        tilefinch_test_faults()->replayed_request_count = 0;
        bool feed_requested = false;
        for (unsigned i = 0; i < 64u && !feed_requested; i++) {
            (void) browser_engine_run_idle_work(engine, NULL);
            const TilefinchTestFaults *observed = tilefinch_test_faults();
            for (unsigned j = 0; j < observed->replayed_request_count && j < 16u; j++)
                if (strstr(observed->replayed_requests[j].url, "/youtubei/v1/browse")) feed_requested = true;
        }
        CHECK(feed_requested);
        CHECK(browser_engine_youtube_feed_state(engine) == BROWSER_YOUTUBE_FEED_LOADING);
        CHECK(youtube_journey_search(engine, "journey-a"));
        for (unsigned i = 0; i < 6u; i++) CHECK(browser_engine_pump_navigation(engine, NULL) == BROWSER_NAVIGATION_JOB_PENDING);
        browser_engine_cancel_navigation(engine, "superseded by the second search");
        BrowserViewSnapshot pending = {0};
        CHECK(browser_engine_apply_page_zoom(engine, zooms[lane])
            && youtube_journey_search(engine, "journey-b")
            && browser_engine_view_snapshot(engine, &pending) && pending.loading
            && !strcmp(pending.url, result)
            && youtube_journey_finish(engine) == BROWSER_NAVIGATION_JOB_SUCCEEDED);
        NavigationSession *nav = browser_engine_navigation(engine);
        CHECK(!strcmp(nav->page.document_url, result) && !youtube_journey_link(engine, "TFOLD000001", "card"));
        lxb_dom_node_t *initial = youtube_journey_link(engine, "TFNEW000001", "card");
        CHECK(initial && controller_focused_node(browser_engine_controller(engine)) == initial);
        /* The thumbnail takes its fixed share; short text and long titles
           receive the same remaining column without measuring their contents
           to choose a flex basis. Exercise the actual provider at each zoom. */
        const LayoutNodeBox *row_link = layout_box_for_node(&nav->page.layout, initial);
        const LayoutNodeBox *thumb = layout_box_for_node(&nav->page.layout, initial->first_child);
        const LayoutNodeBox *info = layout_box_for_node(&nav->page.layout, initial->last_child);
        CHECK(row_link && thumb && info && info->x >= thumb->x + thumb->width
            && info->width > 0 && info->x + info->width == row_link->x + row_link->width);
        if (lane == 1u) {
            /* With no user input the authored first-result autofocus still
               owns the initial view, including after the early idle retry. */
            CHECK(browser_engine_render_frame(engine, NULL));
            size_t moves = browser_engine_controller(engine)->focus_moves;
            (void)browser_engine_run_idle_work(engine, NULL);
            CHECK(controller_focused_node(browser_engine_controller(engine)) == initial
                && browser_engine_controller(engine)->focus_moves == moves);
        } else if (lane == 2u) {
            /* Scrolling alone, without selecting or activating any control,
               also takes ownership away from the deferred autofocus retry. */
            int start_scroll = navigation_current(nav)->scroll_y;
            CHECK(browser_engine_scroll_by(engine, 48));
            int manual_scroll = navigation_current(nav)->scroll_y;
            CHECK(manual_scroll > start_scroll && browser_engine_render_frame(engine, NULL));
            (void)browser_engine_run_idle_work(engine, NULL);
            CHECK(navigation_current(nav)->scroll_y == manual_scroll
                && controller_focused_node(browser_engine_controller(engine)) == initial);
        }
        lxb_dom_node_t *card = youtube_journey_link(engine, selected_id, "card");
        lxb_dom_node_t *details = youtube_journey_link(engine, selected_id, "details");
        CHECK(youtube_journey_title_is(card, selected_title) && details && browser_engine_scroll_by(engine, 800)
            && browser_engine_focus_node(engine, card)
            && browser_engine_render_frame(engine, NULL));
        lxb_dom_node_t *image = card->first_child->first_child;
        CHECK(image && image->local_name == LXB_TAG_IMG
            && !image_resource_available(images_find_node(&nav->page.images, image)));
        uint32_t cold_ink = youtube_journey_thumb_ink(engine, image);
        CHECK(cold_ink);
        const int selected_scroll = navigation_current(nav)->scroll_y;
        uint32_t warm_ink = cold_ink;
        for (unsigned i = 0; i < 320u && warm_ink == cold_ink; i++) {
            (void)browser_engine_prepare_focused_provider_media_thumbnail(engine);
            bool changed = false;
            (void)browser_engine_run_idle_work(engine, &changed);
            if (changed) {
                CHECK(browser_engine_render_frame(engine, NULL));
                if (image_resource_available(images_find_node(&nav->page.images, image)))
                    warm_ink = youtube_journey_thumb_ink(engine, image);
            }
            if (controller_focused_node(browser_engine_controller(engine)) != card
                || navigation_current(nav)->scroll_y != selected_scroll)
                fprintf(stderr, "thumbnail pump %u moved the selected row: scroll %d -> %d\n",
                    i, selected_scroll, navigation_current(nav)->scroll_y);
            CHECK(controller_focused_node(browser_engine_controller(engine)) == card
                && navigation_current(nav)->scroll_y == selected_scroll);
        }
        CHECK(image_resource_available(images_find_node(&nav->page.images, image))
            && browser_engine_render_frame(engine, NULL));
        CHECK(warm_ink && warm_ink != cold_ink);
        CHECK(browser_engine_focus_direction(engine, CONTROLLER_FOCUS_RIGHT)
            && controller_focused_node(browser_engine_controller(engine)) == details
            && browser_engine_render_frame(engine, NULL));
        BrowserViewSnapshot before = {0}, after = {0};
        CHECK(browser_engine_view_snapshot(engine, &before) && before.has_focus
            && before.scroll_y > 0 && before.focus_width > 0 && before.focus_height > 0);
        CHECK(browser_engine_activate(engine, &action) && strstr(action.url, selected_id));
        char preview_title[48], preview_channel[24];
        CHECK(browser_engine_youtube_details_summary(engine, &action, preview_title,
            sizeof(preview_title), preview_channel, sizeof(preview_channel))
            && !strncmp(selected_title, preview_title, sizeof(preview_title) - 1u)
            && !strcmp(preview_channel, "Fixture studio"));
        char selected_details_url[NAVIGATION_URL_LIMIT];
        snprintf(selected_details_url, sizeof(selected_details_url), "%s", action.url);
        CHECK(browser_engine_begin_navigation_action(engine, &action, MIB, 1000)
            && youtube_journey_finish(engine) == BROWSER_NAVIGATION_JOB_SUCCEEDED
            && strstr(nav->page.document_url, selected_id)
            && browser_engine_begin_navigation_history(engine, false, MIB, 1000)
            && youtube_journey_finish(engine) == BROWSER_NAVIGATION_JOB_SUCCEEDED);
        /* The old records outlive Back in the replay, but cancellation destroys
           their scheduler owners, so neither HTTP outcome reaches the page. */
        for (unsigned i = 0; i < 112u; i++) (void)browser_engine_run_idle_work(engine, NULL);
        card = youtube_journey_link(engine, selected_id, "card");
        CHECK(youtube_journey_title_is(card, selected_title));
        image = card->first_child->first_child;
        CHECK(browser_engine_render_frame(engine, NULL)
            && browser_engine_view_snapshot(engine, &after) && !after.loading
            && !strcmp(after.url, result) && after.scroll_y == before.scroll_y
            && image_resource_available(images_find_node(&nav->page.images, image))
            && nav->page.images.stats.decoded_cache_hits > 0
            && after.focus_x == before.focus_x && after.focus_y == before.focus_y
            && after.focus_width == before.focus_width && after.focus_height == before.focus_height
            && controller_focused_node(browser_engine_controller(engine))
                == youtube_journey_link(engine, selected_id, "details")
            && browser_engine_activate(engine, &action)
            && !strcmp(action.url, selected_details_url)
            && browser_engine_focus_direction(engine, CONTROLLER_FOCUS_LEFT));
        CHECK(controller_focused_node(browser_engine_controller(engine))
                == youtube_journey_link(engine, selected_id, "card")
            && browser_engine_activate(engine, &action) && strstr(action.url, selected_id));
        Budget *owned = browser_engine_budget(engine);
        CHECK(browser_engine_shutdown(engine) && owned->current == 0);
        browser_engine_destroy(engine);
        fetch_trace_end();
        remove_youtube_records_replay(directory, 6);
    }
    free(feed_identity);
    free(feed);
    return 0;
}

static bool youtube_journey_play_card(BrowserEngine *engine, const char *id,
                                      ControllerAction *action)
{
    lxb_dom_node_t *card = youtube_journey_link(engine, id, "card");
    return card && browser_engine_focus_node(engine, card)
        && browser_engine_render_frame(engine, NULL)
        && browser_engine_activate(engine, action)
        && action->prefer_native_media && strstr(action->url, id);
}

static int test_youtube_search_admits_request_before_bold_faces(void)
{
    static const char body[] = "<script>var ytInitialData={\"contents\":["
        "{\"videoRenderer\":{\"videoId\":\"TFTEST00001\","
        "\"title\":{\"simpleText\":\"First result\"}}}]};</script>";
    const YoutubeReplayRecord record = {"GET", "https://m.youtube.com/results?search_query=fixture",
                                        "text/html", body, sizeof(body) - 1};
    const unsigned delay = 12;
    /* Cancel on either side of the font handoff. Even after an incumbent
       relayout, the submitted text remains editable and its button focused. */
    for (unsigned lane = 0; lane < 3; lane++) {
        char dir[128], error[256] = {0};
        CHECK(write_youtube_records_replay_delayed(dir, &record, 1, &delay, NULL)
            && fetch_trace_replay_begin_response_keyed(dir, error, sizeof(error)));
        BrowserDeviceProfile profile;
        BrowserConfig config;
        browser_device_profile_psp3000(&profile);
        browser_config_init(&config, &profile);
#define YT_FONT(name) TILEFINCH_TEST_SOURCE_DIR "/fonts/" name ".ttf"
        CHECK(browser_config_set_font_paths(&config, YT_FONT("DejaVuSans-Latin"),
            YT_FONT("DejaVuSerif-Latin"), YT_FONT("DejaVuSans-Oblique-Latin"),
            YT_FONT("DejaVuSans-Bold-Latin"), YT_FONT("DejaVuSerif-Bold-Latin"),
            YT_FONT("TilefinchSans-Regular"), YT_FONT("TilefinchSans-Bold"), 1536u * 1024u));
#undef YT_FONT
        browser_config_set_staged_font_loading(&config, true);
        BrowserEngine *e = browser_engine_create(&config, error, sizeof(error));
        CHECK(e && browser_engine_load_url(e, "https://www.youtube.com/", true)
            && youtube_journey_search(e, "fixture"));
        const FontSet *fonts = browser_engine_fonts(e);
        CHECK(fonts && !fonts->sans_bold.loaded && !fonts->metric_sans_bold.loaded);
        if (lane != 1)
            CHECK(browser_engine_pump_navigation(e, NULL) == BROWSER_NAVIGATION_JOB_PENDING
                && fonts->sans_bold.loaded && fonts->metric_sans_bold.loaded);
        if (lane != 0) {
            browser_engine_cancel_navigation(e, "cancel search");
            lxb_dom_node_t *field = youtube_journey_id(e, "yt-search");
            char value[16]; size_t length = 0;
            CHECK(!browser_engine_navigation_pending(e) && field && field->parent
                && controller_focused_node(browser_engine_controller(e)) == field->parent->last_child
                && fonts->sans_bold.loaded == (lane == 2)
                && fonts->metric_sans_bold.loaded == (lane == 2)
                && browser_engine_focus_node(e, field)
                && browser_engine_text_value(e, value, sizeof(value), &length)
                && length == 7 && !strcmp(value, "fixture") && browser_engine_render_frame(e, NULL));
        } else {
            CHECK(youtube_journey_finish(e) == BROWSER_NAVIGATION_JOB_SUCCEEDED
                && youtube_journey_link(e, "TFTEST00001", "card"));
        }
        Budget *budget = browser_engine_budget(e);
        CHECK(browser_engine_shutdown(e) && budget->current == 0);
        browser_engine_destroy(e); fetch_trace_end(); remove_google_identity_replay(dir, 1);
    }
    return 0;
}

static int test_youtube_results_player_handoff(void)
{
    CHECK(test_youtube_search_admits_request_before_bold_faces() == 0);
    const char *results = "https://www.youtube.com/results?search_query=fixture";
    const char *player = "https://www.youtube.com/youtubei/v1/player?key=fixture-key&prettyPrint=false";
    static const char available_b[] =
        "{\"playabilityStatus\":{\"status\":\"OK\"},"
        "\"videoDetails\":{\"videoId\":\"TFTEST00009\",\"title\":\"Selected B\"},"
        "\"streamingData\":{\"formats\":["
        "{\"itag\":18,\"width\":320,\"height\":240,\"bitrate\":200,"
        "\"contentLength\":\"2000\",\"approxDurationMs\":\"18950\","
        "\"mimeType\":\"video/mp4; codecs=\\\"avc1.42001e, mp4a.40.2\\\"\","
        "\"url\":\"https://rr2.googlevideo.com/videoplayback?expire=4102444800&itag=18&id=B\"}]}}";
    static const char unavailable[] =
        "{\"playabilityStatus\":{\"status\":\"UNPLAYABLE\","
        "\"reason\":\"This video is unavailable in your country\"}}";
    size_t search_size = 0;
    char *search = youtube_costs_list_page("videoRenderer", "videoId", "TFTEST%05d",
        "title", "Fixture entry %d", &search_size);
    char *watch = NULL, *next = NULL;
    YoutubeReplayRecord records[] = {
        {"GET", "https://m.youtube.com/results?search_query=fixture",
            "text/html", search, search_size},
        youtube_fixture_record("GET",
            "https://m.youtube.com/watch?v=TFTEST00001&bpctr=9999999999&has_verified=1",
            "text/html", "http-site-adapter-youtube/0000.body", &watch),
        youtube_fixture_record("POST",
            "https://m.youtube.com/youtubei/v1/next?key=fixture-key&prettyPrint=false",
            "application/json", "http-site-adapter-youtube/0001.body", &next),
        {"POST", player, "application/json", youtube_costs_player_response,
            sizeof(youtube_costs_player_response) - 1u},
        {"POST", player, "application/json", unavailable, sizeof(unavailable) - 1u},
        {"POST", player, "application/json", available_b, sizeof(available_b) - 1u}
    };
    CHECK(search && watch && next);
    for (unsigned lane = 0; lane < 4; lane++) {
        /* A was admitted, but its HTTP result must never own B. Exercise
           both possible outcomes and fresh ownership on each repetition. */
        const unsigned statuses[] = {200u, 200u, 200u, lane % 2 ? 503u : 200u, 200u, 200u};
        char directory[128], error[256] = {0};
        CHECK(write_youtube_records_replay_delayed(directory, records, 6, NULL, statuses)
            && fetch_trace_replay_begin_response_keyed(directory, error, sizeof(error)));
        BrowserDeviceProfile profile;
        BrowserConfig config;
        browser_device_profile_psp3000(&profile);
        browser_config_init(&config, &profile);
        config.resources.enabled = config.resources.web_fonts_enabled = false;
        BrowserEngine *engine = browser_engine_create(&config, error, sizeof(error));
        CHECK(engine);
        Budget *budget = browser_engine_budget(engine);
        BrowserSession *session = browser_engine_session(engine);
        /* The enriched player identity is learned through the real details adapter. */
        CHECK(browser_engine_load_url(engine,
            "https://www.youtube.com/watch?v=TFTEST00001&tilefinch_view=comments", true)
            && browser_engine_load_url(engine, results, true));
        NavigationSession *nav = browser_engine_navigation(engine);
        const uint64_t generation = browser_engine_page_generation(engine);
        ControllerAction a = {0}, b = {0};
        CHECK(youtube_journey_play_card(engine, "TFTEST00001", &a));
        (void) browser_engine_hold_page_network(engine, "native player A");
        YoutubeResolveJob *retired = youtube_resolve_job_begin(
            budget, session, a.url, 360, 30000, NULL, NULL);
        YoutubeResolveJobMetrics in_flight = {0};
        CHECK(retired && browser_engine_page_network_held(engine)
            && youtube_resolve_job_pump(retired) == YOUTUBE_RESOLVE_JOB_PENDING
            && youtube_resolve_job_metrics(retired, &in_flight)
            && in_flight.request_active && in_flight.request_chunks == 0u
            && fetch_background_transport_active_operations() == 1u);
        youtube_resolve_job_cancel(retired, "Circle canceled A");
        CHECK(fetch_background_transport_active_operations() == 0u);
        browser_engine_release_page_network(engine);
        CHECK(browser_engine_scroll_by(engine, 800)
            && youtube_journey_play_card(engine, "TFTEST00009", &b));
        BrowserViewSnapshot before = {0}, after = {0};
        CHECK(browser_engine_view_snapshot(engine, &before)
            && before.has_focus && before.scroll_y > 0 && !strcmp(before.url, results));
        lxb_dom_node_t *selected = controller_focused_node(browser_engine_controller(engine));
        (void) browser_engine_hold_page_network(engine, "native player B");
        YoutubeResolveJob *current = youtube_resolve_job_begin(
            budget, session, b.url, 360, 30000, NULL, NULL);
        YoutubeStream stream = {0};
        CHECK(current && youtube_costs_resolve(current) == YOUTUBE_RESOLVE_JOB_FAILED
            && strstr(youtube_resolve_job_error(current), "unavailable in your country")
            && !youtube_resolve_job_take(current, &stream));
        youtube_resolve_job_destroy(current);

        /* Retry keeps the results surface frozen and selects B again. */
        current = youtube_resolve_job_begin(budget, session, b.url, 360, 30000, NULL, NULL);
        CHECK(current && youtube_resolve_job_pump(current) == YOUTUBE_RESOLVE_JOB_PENDING
            && youtube_resolve_job_pump(retired) == YOUTUBE_RESOLVE_JOB_FAILED
            && !youtube_resolve_job_take(retired, &stream));
        youtube_resolve_job_destroy(retired);
        CHECK(fetch_background_transport_active_operations() == 1u);
        CHECK(youtube_costs_resolve(current) == YOUTUBE_RESOLVE_JOB_COMPLETE
            && youtube_resolve_job_take(current, &stream)
            && !strcmp(stream.video_id, "TFTEST00009")
            && !strcmp(stream.title, "Selected B") && strstr(stream.media_url, "&id=B"));
        youtube_resolve_job_destroy(current);
        CHECK(fetch_background_transport_active_operations() == 0u);
        browser_engine_release_page_network(engine);
        CHECK(browser_engine_render_frame(engine, NULL)
            && browser_engine_view_snapshot(engine, &after)
            && !after.loading && !browser_engine_page_network_held(engine)
            && browser_engine_page_generation(engine) == generation
            && !strcmp(nav->page.document_url, results) && !strcmp(after.url, before.url)
            && after.scroll_y == before.scroll_y && after.focus_y == before.focus_y
            && controller_focused_node(browser_engine_controller(engine)) == selected
            && browser_engine_activate(engine, &a) && a.prefer_native_media
            && !strcmp(a.url, b.url));
        CHECK(browser_engine_shutdown(engine) && !budget->current && !budget->external_reserved);
        browser_engine_destroy(engine);
        fetch_trace_end();
        remove_youtube_records_replay(directory, 6);
    }
    free(search);
    free(watch);
    free(next);
    return 0;
}

static int test_youtube_signout_retires_ready_recommendations(void)
{
    static const char home[] = "https://www.youtube.com/";
    BrowserDeviceProfile profile;
    BrowserConfig config;
    browser_device_profile_psp3000(&profile);
    browser_config_init(&config, &profile);
    config.resources.enabled = config.resources.web_fonts_enabled = false;
    char error[256] = {0}, report[2048] = {0};
    CHECK(fetch_trace_replay_begin_response_keyed(TILEFINCH_TEST_SOURCE_DIR
        "/tests/fixtures/http-youtube-personalized", error, sizeof(error)));
    BrowserEngine *engine = browser_engine_create(&config, error, sizeof(error));
    CHECK(engine);
    browser_engine_set_youtube_signin_diagnostics(engine, true);
    BrowserSession *session = browser_engine_session(engine);
    CHECK(browser_session_cookie_set_http(session, "https://m.youtube.com/",
        "SAPISID=synthetic-signout-journey; Secure; Domain=youtube.com; Path=/")
        && browser_engine_load_url(engine, home, true)
        && browser_engine_render_frame(engine, NULL)
        && browser_engine_scroll_by(engine, 1));
    for (unsigned i = 0; i < 1024u && browser_engine_youtube_feed_state(engine) != BROWSER_YOUTUBE_FEED_READY; i++) {
        bool changed = false;
        (void) browser_engine_run_idle_work(engine, &changed);
        if (changed) CHECK(browser_engine_render_frame(engine, NULL));
    }
    CHECK(browser_engine_youtube_feed_state(engine) == BROWSER_YOUTUBE_FEED_READY
        && browser_engine_youtube_signin_report(engine, report, sizeof(report))
        && strstr(report, "pending-publication=1")
        && !youtube_journey_link(engine, "TFTEST00001", "card"));
    CHECK(browser_engine_focus_node(engine, youtube_journey_id(engine, "yt-account-action")));
    ControllerAction action = {0};
    CHECK(browser_engine_activate(engine, &action)
        && browser_engine_focus_node(engine, youtube_journey_id(engine, "yt-signout-open"))
        && browser_engine_activate(engine, &action)
        && !strcmp(action.url, YOUTUBE_LITE_SIGNOUT_CONFIRM_URL)
        && browser_engine_begin_navigation_action(engine, &action, MIB, 1000)
        && youtube_journey_finish(engine) == BROWSER_NAVIGATION_JOB_SUCCEEDED
        && browser_engine_focus_node(engine, youtube_journey_id(engine, "yt-signout-action"))
        && browser_engine_activate(engine, &action) && !strcmp(action.url, home)
        && !youtube_lite_session_signed_in(session)
        && browser_engine_begin_navigation_action(engine, &action, MIB, 1000)
        && youtube_journey_finish(engine) == BROWSER_NAVIGATION_JOB_SUCCEEDED);
    for (unsigned i = 0; i < 112u; i++) (void)browser_engine_run_idle_work(engine, NULL);
    BrowserViewSnapshot view = {0};
    lxb_dom_node_t *signin = youtube_journey_id(engine, "yt-account-action");
    size_t href_length = 0;
    const char *href = signin ? document_attribute(signin, "href", &href_length) : NULL;
    CHECK(browser_engine_render_frame(engine, NULL)
        && browser_engine_view_snapshot(engine, &view) && !view.loading && !strcmp(view.url, home));
    CHECK(browser_engine_youtube_signin_report(engine, report, sizeof(report))
        && strstr(report, "pending-publication=0")
        && !browser_engine_youtube_login_verified(engine));
    CHECK(!youtube_journey_id(engine, "yt-account-menu")
        && !youtube_journey_link(engine, "TFTEST00001", "card"));
    CHECK(href && href_length > 0 && !strncmp(href, "https://", 8u));
    Budget *budget = browser_engine_budget(engine);
    CHECK(browser_engine_shutdown(engine) && budget->current == 0);
    browser_engine_destroy(engine);
    fetch_trace_end();
    return 0;
}

static int test_youtube_history_reordered_results(void)
{
    static const char result[] = "https://www.youtube.com/results?search_query=reorder";
    static const char request[] = "https://m.youtube.com/results?search_query=reorder";
    static const char video[] = "TFORDER0001";
#define REORDER_ROW(id) "{\"videoRenderer\":{\"videoId\":\"TFORDER000" id "\",\"title\":{\"simpleText\":\"Neighbor\"}}},"
    static const char old[] = "<script>var ytInitialData={\"contents\":["
        REORDER_ROW("2") REORDER_ROW("3") REORDER_ROW("4") REORDER_ROW("5") REORDER_ROW("6") REORDER_ROW("7")
        "{\"videoRenderer\":{\"videoId\":\"TFORDER0001\",\"title\":{\"simpleText\":\"Chosen\"},"
        "\"navigationEndpoint\":{\"watchEndpoint\":{\"videoId\":\"TFORDER0001\",\"startTimeSeconds\":11}}}}]};</script>";
    static const char reordered[] = "<script>var ytInitialData={\"contents\":["
        "{\"videoRenderer\":{\"videoId\":\"TFORDER0001\",\"title\":{\"simpleText\":\"Chosen\"},"
        "\"navigationEndpoint\":{\"watchEndpoint\":{\"videoId\":\"TFORDER0001\",\"startTimeSeconds\":23}}}},"
        REORDER_ROW("2") REORDER_ROW("3") REORDER_ROW("4") REORDER_ROW("5") REORDER_ROW("6")
        "{\"videoRenderer\":{\"videoId\":\"TFORDER0007\",\"title\":{\"simpleText\":\"Neighbor\"}}}]};</script>";
#undef REORDER_ROW
    static const char watch[] = "<script>var ytInitialData={};</script>"
        "<script>var ytInitialPlayerResponse={\"videoDetails\":{\"videoId\":\"TFORDER0001\","
        "\"title\":\"Chosen\",\"author\":\"Fixture studio\",\"lengthSeconds\":\"42\"}};</script>";
    YoutubeReplayRecord records[] = {
        {"GET", request, "text/html", old, sizeof(old) - 1u},
        {"GET", request, "text/html", reordered, sizeof(reordered) - 1u},
        {"GET", "https://m.youtube.com/watch?v=TFORDER0001&bpctr=9999999999&has_verified=1", "text/html", watch, sizeof(watch) - 1u}
    };
    for (unsigned pass = 0; pass < 2u; pass++) {
        char directory[128], error[256] = {0};
        CHECK(write_youtube_records_replay(directory, records, 3u)
            && fetch_trace_replay_begin_response_keyed(directory, error, sizeof(error)));
        BrowserDeviceProfile profile;
        BrowserConfig config;
        browser_device_profile_psp3000(&profile);
        browser_config_init(&config, &profile);
        BrowserEngine *engine = browser_engine_create(&config, error, sizeof(error));
        CHECK(engine && browser_engine_load_url(engine, result, true));
        const char *role = pass == 0 ? "details" : "card";
        lxb_dom_node_t *chosen = youtube_journey_link(engine, video, role);
        ControllerAction action = {0};
        CHECK(chosen && browser_engine_focus_node(engine, chosen)
            && browser_engine_activate(engine, &action) && strstr(action.url, "&t=11")
            && browser_engine_begin_navigation_action(engine, &action, MIB, 1000)
            && youtube_journey_finish(engine) == BROWSER_NAVIGATION_JOB_SUCCEEDED);
        browser_session_site_adapter_document_cache_clear(browser_engine_session(engine));
        CHECK(browser_engine_begin_navigation_history(engine, false, MIB, 1000)
            && youtube_journey_finish(engine) == BROWSER_NAVIGATION_JOB_SUCCEEDED);
        lxb_dom_node_t *new_chosen = youtube_journey_link(engine, video, role);
        size_t length = 0;
        const char *href = document_attribute(new_chosen, "href", &length);
        CHECK(new_chosen && href && strstr(href, "&t=23"));
        CHECK(controller_focused_node(browser_engine_controller(engine)) == new_chosen);
        BrowserViewSnapshot view = {0};
        CHECK(browser_engine_view_snapshot(engine, &view) && view.has_focus
            && view.focus_y < profile.framebuffer_height && view.focus_y + view.focus_height > 0
            && browser_engine_activate(engine, &action)
            && strstr(action.url, video) && strstr(action.url, "&t=23"));
        Budget *owned = browser_engine_budget(engine);
        CHECK(browser_engine_shutdown(engine) && owned->current == 0);
        browser_engine_destroy(engine);
        fetch_trace_end();
        remove_youtube_records_replay(directory, 3u);
    }
    return 0;
}

static int test_youtube_history_keeps_duplicate_playlist_occurrence(void)
{
    static const char url[] = "https://www.youtube.com/playlist?list=PLfixture";
    static const char playlist[] = "<script>var ytInitialData={\"contents\":["
        "{\"playlistVideoRenderer\":{\"videoId\":\"TFORDER0001\",\"title\":{\"simpleText\":\"First occurrence\"},\"isPlayable\":true}},"
        "{\"playlistVideoRenderer\":{\"videoId\":\"TFORDER0002\",\"title\":{\"simpleText\":\"Another video\"},\"isPlayable\":true}},"
        "{\"playlistVideoRenderer\":{\"videoId\":\"TFORDER0001\",\"title\":{\"simpleText\":\"Second occurrence\"},\"isPlayable\":true}}]};</script>";
    static const char watch[] = "<script>var ytInitialData={};</script>"
        "<script>var ytInitialPlayerResponse={\"videoDetails\":{\"videoId\":\"TFORDER0001\","
        "\"title\":\"Chosen\",\"lengthSeconds\":\"42\"}};</script>";
    YoutubeReplayRecord records[] = {
        {"GET", "https://m.youtube.com/playlist?list=PLfixture", "text/html", playlist, sizeof(playlist) - 1u},
        {"GET", "https://m.youtube.com/watch?v=TFORDER0001&bpctr=9999999999&has_verified=1", "text/html", watch, sizeof(watch) - 1u},
    };
    char directory[128], error[256] = {0};
    CHECK(write_youtube_records_replay(directory, records, 2u)
        && fetch_trace_replay_begin_response_keyed(directory, error, sizeof(error)));
    BrowserDeviceProfile profile;
    BrowserConfig config;
    browser_device_profile_psp3000(&profile);
    browser_config_init(&config, &profile);
    BrowserEngine *engine = browser_engine_create(&config, error, sizeof(error));
    CHECK(engine && browser_engine_load_url(engine, url, true));
    const char *selected = "v=TFORDER0001&tilefinch_queue=2";
    lxb_dom_node_t *first = youtube_journey_link(engine, "v=TFORDER0001&tilefinch_queue=0", "card");
    lxb_dom_node_t *second = youtube_journey_link(engine, selected, "card");
    CHECK(first && second && first != second);
    ControllerAction action = {0};
    CHECK(browser_engine_focus_node(engine, second)
        && browser_engine_activate(engine, &action) && strstr(action.url, selected)
        && browser_engine_begin_navigation_action(engine, &action, MIB, 1000)
        && youtube_journey_finish(engine) == BROWSER_NAVIGATION_JOB_SUCCEEDED);
    browser_session_site_adapter_document_cache_clear(browser_engine_session(engine));
    CHECK(browser_engine_begin_navigation_history(engine, false, MIB, 1000)
        && youtube_journey_finish(engine) == BROWSER_NAVIGATION_JOB_SUCCEEDED);
    second = youtube_journey_link(engine, selected, "card");
    CHECK(second && controller_focused_node(browser_engine_controller(engine)) == second
        && browser_engine_activate(engine, &action) && strstr(action.url, selected));
    Budget *owned = browser_engine_budget(engine);
    CHECK(browser_engine_shutdown(engine) && owned->current == 0);
    browser_engine_destroy(engine);
    fetch_trace_end();
    remove_youtube_records_replay(directory, 2u);
    return 0;
}

static int test_youtube_refresh_does_not_own_a_later_search_edit(void)
{
    BrowserDeviceProfile profile;
    BrowserConfig config;
    browser_device_profile_psp3000(&profile);
    browser_config_init(&config, &profile);
    char error[256] = {0};
    CHECK(fetch_trace_replay_begin_response_keyed(TILEFINCH_TEST_SOURCE_DIR
        "/tests/fixtures/http-youtube-personalized", error, sizeof(error)));
    BrowserEngine *engine = browser_engine_create(&config, error, sizeof(error));
    CHECK(engine && browser_session_cookie_set_http(browser_engine_session(engine),
        "https://m.youtube.com/", "SAPISID=synthetic-edit-journey; Secure; Domain=youtube.com; Path=/")
        && browser_engine_load_url(engine, "https://www.youtube.com/", true)
        && browser_engine_render_frame(engine, NULL));
    lxb_dom_node_t *search = youtube_journey_id(engine, "yt-search");
    ControllerAction action = {0};
    CHECK(browser_engine_focus_node(engine, search)
        && browser_engine_replace_text(engine, "draft!", 6u)
        && browser_engine_focus_node(engine, youtube_journey_id(engine, "yt-feed-refresh"))
        && browser_engine_activate(engine, &action));
    (void)browser_engine_run_idle_work(engine, NULL);
    CHECK(browser_engine_youtube_feed_state(engine) == BROWSER_YOUTUBE_FEED_LOADING
        && browser_engine_focus_node(engine, search) && browser_engine_backspace(engine));
    for (unsigned i = 0; i < 256u && browser_engine_youtube_feed_state(engine) == BROWSER_YOUTUBE_FEED_LOADING; i++)
        (void)browser_engine_run_idle_work(engine, NULL);
    size_t length = 0;
    lxb_dom_node_t *now = youtube_journey_id(engine, "yt-search");
    const char *value = document_control_value(now, &length);
    if (!value) value = document_attribute(now, "value", &length);
    CHECK(value && length == 5u && !memcmp(value, "draft", length));
    CHECK(browser_engine_youtube_feed_state(engine) == BROWSER_YOUTUBE_FEED_READY
        && controller_focused_node(browser_engine_controller(engine)) == now);
    CHECK(browser_engine_focus_node(engine, youtube_journey_id(engine, "yt-feed-refresh"))
        && browser_engine_activate(engine, &action));
    (void)browser_engine_run_idle_work(engine, NULL);
    CHECK(browser_engine_youtube_feed_state(engine) == BROWSER_YOUTUBE_FEED_CURRENT
        && youtube_journey_link(engine, "TFTEST00001", "card"));
    Budget *owned = browser_engine_budget(engine);
    CHECK(browser_engine_shutdown(engine) && owned->current == 0);
    browser_engine_destroy(engine);
    fetch_trace_end();
    return 0;
}

static int test_youtube_refresh_defers_to_later_browsing(void)
{
    char *identity = NULL, *body = NULL;
    char directory[128], error[256] = {0};
    YoutubeReplayRecord replacement[] = {
        youtube_fixture_record("GET", "https://m.youtube.com/", "text/html",
            "http-youtube-personalized/0000.body", &identity),
        youtube_fixture_record("POST", "https://m.youtube.com/youtubei/v1/browse?key=fixture-key&prettyPrint=false",
            "application/json", "http-youtube-personalized/0001.body", &body),
    };
    CHECK(identity && body);
    char *third = strstr(body, "TFTEST00003");
    CHECK(third);
    memcpy(third, "TFNEXT00003", sizeof("TFNEXT00003") - 1u);
    const unsigned delays[] = {3u, 3u};
    CHECK(write_youtube_records_replay_delayed(directory, replacement, 2u, delays, NULL));
    free(identity);
    free(body);

    for (unsigned lane = 0; lane < 2u; lane++) {
        BrowserDeviceProfile profile;
        BrowserConfig config;
        browser_device_profile_psp3000(&profile);
        browser_config_init(&config, &profile);
        CHECK(fetch_trace_replay_begin_response_keyed(TILEFINCH_TEST_SOURCE_DIR
            "/tests/fixtures/http-youtube-personalized", error, sizeof(error)));
        BrowserEngine *engine = browser_engine_create(&config, error, sizeof(error));
        CHECK(engine && browser_session_cookie_set_http(browser_engine_session(engine),
            "https://m.youtube.com/", "SAPISID=test; Secure; HttpOnly; Path=/")
            && browser_engine_load_url(engine, "https://www.youtube.com/", true)
            && browser_engine_render_frame(engine, NULL));
        for (unsigned i = 0; i < 256u && browser_engine_youtube_feed_state(engine) != BROWSER_YOUTUBE_FEED_CURRENT; i++)
            (void)browser_engine_run_idle_work(engine, NULL);
        CHECK(browser_engine_youtube_feed_state(engine) == BROWSER_YOUTUBE_FEED_CURRENT);
        fetch_trace_end();
        CHECK(fetch_trace_replay_begin_response_keyed(directory, error, sizeof(error)));

        ControllerAction action = {0};
        CHECK(browser_engine_focus_node(engine, youtube_journey_id(engine, "yt-feed-refresh")));
        if (lane == 1u) CHECK(browser_engine_scroll_by(engine, 32));
        const NavigationSession *nav = browser_engine_navigation(engine);
        const int before = navigation_current(nav)->scroll_y;
        CHECK(lane == 0u || before > 0);
        CHECK(browser_engine_activate(engine, &action)
            && action.type == CONTROLLER_ACTION_CONTROL
            && browser_engine_render_frame(engine, NULL));
        for (unsigned i = 0; i < 64u && browser_engine_youtube_feed_state(engine) != BROWSER_YOUTUBE_FEED_LOADING; i++)
            (void)browser_engine_run_idle_work(engine, NULL);
        CHECK(browser_engine_youtube_feed_state(engine) == BROWSER_YOUTUBE_FEED_LOADING);
        if (lane == 0u) {
            CHECK(browser_engine_focus_node(engine, youtube_journey_link(engine, "TFTEST00003", "card")));
        } else {
            CHECK(browser_engine_scroll_by(engine, -8)
                && navigation_current(nav)->scroll_y < before);
        }
        const int scroll = navigation_current(nav)->scroll_y;
        for (unsigned i = 0; i < 256u && browser_engine_youtube_feed_state(engine) == BROWSER_YOUTUBE_FEED_LOADING; i++)
            (void)browser_engine_run_idle_work(engine, NULL);
        lxb_dom_node_t *card = youtube_journey_link(engine, "TFTEST00003", "card");
        CHECK(browser_engine_youtube_feed_state(engine) == BROWSER_YOUTUBE_FEED_READY
            && card && !youtube_journey_link(engine, "TFNEXT00003", "card")
            && navigation_current(nav)->scroll_y == scroll
            && (lane != 0u || controller_focused_node(browser_engine_controller(engine)) == card));

        CHECK(browser_engine_focus_node(engine, youtube_journey_id(engine, "yt-feed-refresh"))
            && browser_engine_scroll_by(engine, 16)
            && navigation_current(nav)->scroll_y > 0
            && browser_engine_activate(engine, &action));
        (void)browser_engine_run_idle_work(engine, NULL);
        CHECK(browser_engine_youtube_feed_state(engine) == BROWSER_YOUTUBE_FEED_CURRENT
            && !youtube_journey_link(engine, "TFTEST00003", "card")
            && youtube_journey_link(engine, "TFNEXT00003", "card"));
        Budget *owned = browser_engine_budget(engine);
        CHECK(browser_engine_shutdown(engine) && owned->current == 0);
        browser_engine_destroy(engine);
        fetch_trace_end();
    }
    remove_youtube_records_replay(directory, 2u);
    return 0;
}

static int test_youtube_header_refresh_keeps_recommendation_focus(void)
{
    char directory[128], error[256] = {0};
    CHECK(fetch_trace_replay_begin(TILEFINCH_TEST_SOURCE_DIR
        "/tests/fixtures/http-youtube-personalized", error, sizeof(error)));
    BrowserDeviceProfile profile;
    BrowserConfig config;
    browser_device_profile_psp3000(&profile);
    browser_config_init(&config, &profile);
    BrowserEngine *engine = browser_engine_create(&config, error, sizeof(error));
    CHECK(engine && browser_session_cookie_set_http(browser_engine_session(engine),
        "https://m.youtube.com/", "SAPISID=synthetic-header-journey; Secure; Domain=youtube.com; Path=/")
        && browser_engine_load_url(engine, "https://www.youtube.com/", true)
        && browser_engine_render_frame(engine, NULL));
    ControllerAction action = {0};
    CHECK(browser_engine_focus_node(engine, youtube_journey_id(engine, "yt-feed-refresh"))
        && browser_engine_activate(engine, &action));
    for (unsigned i = 0; i < 256u && browser_engine_youtube_feed_state(engine) != BROWSER_YOUTUBE_FEED_CURRENT; i++) {
        bool changed = false;
        (void)browser_engine_run_idle_work(engine, &changed);
        if (changed) CHECK(browser_engine_render_frame(engine, NULL));
    }
    CHECK(browser_engine_youtube_feed_state(engine) == BROWSER_YOUTUBE_FEED_CURRENT
        && youtube_journey_id(engine, "yt-account-menu"));
    fetch_trace_end();
    const GoogleIdentityRecord rejected[] = {
        {.url="https://m.youtube.com/youtubei/v1/browse?key=fixture-key&prettyPrint=false",
         .status=200, .method="POST", .content_type="application/json",
         .body="{\"responseContext\":{\"mainAppWebResponseContext\":{\"loggedOut\":true}}}"},
        {.url="https://m.youtube.com/", .status=200, .content_type="text/html",
         .body="<script>ytcfg.set({\"LOGGED_IN\":false});</script>"},
    };
    CHECK(write_google_identity_replay(directory, rejected, 2u)
        && fetch_trace_replay_begin(directory, error, sizeof(error)));
    CHECK(browser_engine_focus_node(engine, youtube_journey_id(engine, "yt-feed-refresh"))
        && browser_engine_activate(engine, &action)
        && browser_engine_focus_node(engine, youtube_journey_link(engine, "TFTEST00002", "card")));
    int scroll = navigation_current(browser_engine_navigation(engine))->scroll_y;
    for (unsigned i = 0; i < 256u && youtube_journey_id(engine, "yt-account-menu"); i++)
        (void)browser_engine_run_idle_work(engine, NULL);
    CHECK(!youtube_journey_id(engine, "yt-account-menu"));
    lxb_dom_node_t *chosen = youtube_journey_link(engine, "TFTEST00002", "card");
    CHECK(chosen && controller_focused_node(browser_engine_controller(engine)) == chosen
        && navigation_current(browser_engine_navigation(engine))->scroll_y == scroll
        && browser_engine_render_frame(engine, NULL)
        && browser_engine_activate(engine, &action) && strstr(action.url, "TFTEST00002"));
    Budget *owned = browser_engine_budget(engine);
    CHECK(browser_engine_shutdown(engine) && owned->current == 0);
    browser_engine_destroy(engine);
    fetch_trace_end();
    remove_google_identity_replay(directory, 2u);
    return 0;
}

static int test_youtube_provider_layout_states(void)
{
    static const char watch[] = "<script>var ytInitialData='{}';</script>"
        "<script>var ytInitialPlayerResponse={\"videoDetails\":{\"videoId\":\"TFTEST00001\","
        "\"channelId\":\"UCabcdefghijklmnopqrstuv\",\"title\":\"Big Buck Bunny - Official Short Film\","
        "\"author\":\"Fixture studio\",\"lengthSeconds\":\"596\",\"viewCount\":\"9428016\"},"
        "\"microformat\":{\"playerMicroformatRenderer\":{\"publishDate\":\"2021-10-29\","
        "\"category\":\"Film & Animation\"}}};</script>";
    int title_height = 0;
    for (unsigned signed_in = 0; signed_in < 2u; signed_in++) {
        char fixture[128], error[256] = {0};
        CHECK(write_document_replay(fixture,
            "https://m.youtube.com/watch?v=TFTEST00001&bpctr=9999999999&has_verified=1",
            watch, sizeof(watch)-1u, 200, "")
            && fetch_trace_replay_begin(fixture, error, sizeof(error)));
        BrowserDeviceProfile device; browser_device_profile_psp3000(&device);
        BrowserConfig config; browser_config_init(&config, &device);
        config.resources.enabled = config.resources.web_fonts_enabled = false;
        CHECK(browser_config_set_font_paths(&config,
            TILEFINCH_TEST_SOURCE_DIR "/fonts/DejaVuSans-Latin.ttf",
            TILEFINCH_TEST_SOURCE_DIR "/fonts/DejaVuSerif-Latin.ttf",
            TILEFINCH_TEST_SOURCE_DIR "/fonts/DejaVuSans-Oblique-Latin.ttf",
            TILEFINCH_TEST_SOURCE_DIR "/fonts/DejaVuSans-Bold-Latin.ttf",
            TILEFINCH_TEST_SOURCE_DIR "/fonts/DejaVuSerif-Bold-Latin.ttf",
            TILEFINCH_TEST_SOURCE_DIR "/fonts/TilefinchSans-Regular.ttf",
            TILEFINCH_TEST_SOURCE_DIR "/fonts/TilefinchSans-Bold.ttf", 1536u * 1024u));
        BrowserEngine *engine = browser_engine_create(&config, error, sizeof(error));
        CHECK(engine);
        if (signed_in) CHECK(browser_session_cookie_set_http(browser_engine_session(engine),
            "https://m.youtube.com/", "SAPISID=mock-session; Secure; HttpOnly; Path=/"));
        CHECK(browser_engine_load_url(engine,
            "https://www.youtube.com/watch?v=TFTEST00001&tilefinch_view=details", true));
        while (!browser_engine_baseline_fonts_ready(engine)) CHECK(browser_engine_pump_baseline_fonts(engine));
        NavigationSession *nav = browser_engine_navigation(engine);
        lxb_dom_node_t *root = lxb_dom_interface_node(nav->page.document.html);
        const LayoutNodeBox *title = NULL;
        for (size_t i = 0; i < nav->page.layout.node_box_count; i++)
            if (nav->page.layout.node_boxes[i].node->local_name == LXB_TAG_H1)
                title = &nav->page.layout.node_boxes[i];
        CHECK(title && title->width > 200);
        if (!signed_in) title_height = title->height;
        else CHECK(title->height == title_height);
        lxb_dom_node_t *actions = test_reader_find_id(root, "yt-watch-actions");
        const LayoutNodeBox *box = layout_box_for_node(&nav->page.layout, actions);
        CHECK(box && box->width >= 450 && box->y > title->y + title->height);
        CHECK((test_reader_find_id(root, "yt-like-action") != NULL) == (signed_in != 0));
        /* Signed out, the header's Sign in is the page's only one. */
        CHECK(test_reader_find_id(root, "yt-watch-signin") == NULL);
        lxb_dom_node_t *offline = test_reader_find_id(root, "yt-offline-save");
        const LayoutNodeBox *offline_box = layout_box_for_node(&nav->page.layout, offline);
        CHECK(offline_box && offline_box->width == 40 && offline_box->height == 36);
        printf("detail actions: signed-in=%u y=%d height=%d\n",
            signed_in, offline_box->y, offline_box->height);
        if (offline_box->y + offline_box->height > 266)
            fprintf(stderr, "detail actions clipped: signed-in=%u y=%d height=%d\n",
                signed_in, offline_box->y, offline_box->height);
        CHECK(offline_box->y + offline_box->height <= 266);
        size_t label_length = 0;
        const char *label = document_attribute(offline, "aria-label", &label_length);
        CHECK(label && label_length == strlen("Save video offline")
            && !memcmp(label, "Save video offline", label_length));
        size_t text_length = 0;
        lxb_char_t *text = lxb_dom_node_text_content(
            lxb_dom_interface_node(nav->page.document.html->body), &text_length);
        CHECK(text && !strstr((const char *)text, "Select the thumbnail to retry"));
        /* The category closes the details, after the actions and before
           the comments link, not on the first screen. */
        const char *published = strstr((const char *)text, "Published");
        const char *category = strstr((const char *)text, "Category: Film & Animation");
        const char *comments_link = strstr((const char *)text, "View comments");
        CHECK(published && category && comments_link
            && published < category && category < comments_link);
        lxb_dom_document_destroy_text(root->owner_document, text);
        if (signed_in) {
            CHECK(youtube_header_baselines_match(engine));
            lxb_dom_node_t *menu = test_reader_find_id(root, "yt-watch-account");
            lxb_dom_node_t *unlike = test_reader_find_id(root, "yt-unlike-action");
            CHECK(menu && unlike && !layout_box_for_node(&nav->page.layout, unlike));
            static const char *const primary_ids[] = {"yt-subscribe-action", "yt-like-action",
                "yt-watch-later-add", "yt-playlist-save", "yt-offline-save"};
            int previous_right = -1;
            for (unsigned i = 0; i < 5u; i++) {
                lxb_dom_node_t *node = test_reader_find_id(root, primary_ids[i]);
                const LayoutNodeBox *action_box = layout_box_for_node(&nav->page.layout, node);
                CHECK(action_box && action_box->y == offline_box->y
                    && action_box->width == 40 && action_box->height == 36
                    && action_box->x >= previous_right && action_box->x + action_box->width <= 470);
                previous_right = action_box->x + action_box->width;
                label = document_attribute(node, "aria-label", &label_length);
                CHECK(label && label_length != 0);
            }
            const LayoutNodeBox *menu_box = layout_box_for_node(&nav->page.layout, menu->first_child);
            CHECK(menu_box && menu_box->y == offline_box->y && menu_box->x >= previous_right);
            ControllerAction action = {0};
            CHECK(browser_engine_focus_node(engine, menu->first_child)
                && browser_engine_activate(engine, &action)
                && layout_box_for_node(&nav->page.layout, unlike)
                && browser_engine_focus_node(engine, unlike));
            CHECK(browser_engine_focus_node(engine, menu->first_child)
                && browser_engine_activate(engine, &action)
                && !layout_box_for_node(&nav->page.layout, unlike));
            /* When zoomed in the channel, actions and account menu
               must be reachable without a horizontal pan. Check the control
               boxes themselves, not just whether a focus index exists. */
            CHECK(browser_engine_apply_page_zoom(engine, 125));
            const LayoutNodeBox *mid = layout_box_for_node(&nav->page.layout,
                test_reader_find_id(root,"yt-watch-channel"));
            CHECK(mid && mid->width >= 90);
            CHECK(browser_engine_apply_page_zoom(engine, 150));
            root = lxb_dom_interface_node(nav->page.document.html);
            const LayoutNodeBox *channel = layout_box_for_node(&nav->page.layout,
                test_reader_find_id(root,"yt-watch-channel"));
            CHECK(channel && channel->width >= 90);
            int actions_bottom = 0;
            for (unsigned i = 0; i <= 5; ++i) {
                lxb_dom_node_t *target = i == 5 ? menu->first_child : test_reader_find_id(root,primary_ids[i]);
                const LayoutNodeBox *b = layout_box_for_node(&nav->page.layout,target);
                CHECK(b && b->x >= 0 && b->x+b->width <= nav->viewport.css_width);
                actions_bottom = b->y + b->height;
                CHECK((i ? browser_engine_focus_direction(engine,CONTROLLER_FOCUS_RIGHT)
                         : browser_engine_focus_node(engine,target))
                      && controller_focused_node(browser_engine_controller(engine)) == target
                      && browser_engine_render_frame(engine,NULL));
            }
            CHECK(browser_engine_activate(engine,&action));
            const LayoutNodeBox *expanded = layout_box_for_node(&nav->page.layout,unlike);
            CHECK(expanded && expanded->y >= actions_bottom && browser_engine_focus_node(engine,unlike));
            CHECK(browser_engine_apply_page_zoom(engine,100));
        }
        CHECK(browser_engine_render_frame(engine, NULL));
        Budget *owned = browser_engine_budget(engine);
        CHECK(browser_engine_shutdown(engine) && owned->current == 0);
        browser_engine_destroy(engine); fetch_trace_end(); remove_google_identity_replay(fixture, 1u);
    }
    return 0;
}

static bool youtube_journey_plays_at(BrowserEngine *engine, const char *role,
                                     uint64_t seconds)
{
    lxb_dom_node_t *link = youtube_journey_link(engine, "TFTEST00001", role);
    char focused_url[NAVIGATION_URL_LIMIT];
    ControllerAction action = {0};
    return link && browser_engine_focus_node(engine, link)
        && browser_engine_focused_provider_media_url(engine, focused_url, sizeof(focused_url))
        && browser_engine_activate(engine, &action)
        && action.type == CONTROLLER_ACTION_NAVIGATE && action.prefer_native_media
        && !strcmp(focused_url, action.url)
        && youtube_watch_url_start_time_us(action.url) == seconds * UINT64_C(1000000);
}

static int test_youtube_watch_timestamps(void)
{
    static const char watch[] = "<script>var ytInitialData={\"chapters\":["
        "{\"chapterRenderer\":{\"title\":{\"simpleText\":\"One minute\"},\"timeRangeStartMillis\":60000}}]};</script>"
        "<script>var ytInitialPlayerResponse={\"videoDetails\":{\"videoId\":\"TFTEST00001\","
        "\"title\":\"Timestamp fixture\",\"lengthSeconds\":\"200\"}};</script>";
    static const char plain[] = "https://www.youtube.com/watch?v=TFTEST00001";
    static const char timed[] = "https://www.youtube.com/watch?v=TFTEST00001#t=1m30s";
    char tiny[] = "keep";
    CHECK(!youtube_lite_watch_playback_url(timed, NULL, sizeof(tiny))
        && !youtube_lite_watch_playback_url(timed, tiny, 0)
        && !youtube_lite_watch_playback_url(timed, tiny, sizeof(tiny))
        && !strcmp(tiny, "keep"));
    char fixture[128], error[256] = {0};
    CHECK(write_document_replay(fixture,
        "https://m.youtube.com/watch?v=TFTEST00001&bpctr=9999999999&has_verified=1",
        watch, sizeof(watch) - 1u, 200, "")
        && fetch_trace_replay_begin(fixture, error, sizeof(error)));
    BrowserDeviceProfile device;
    BrowserConfig config;
    browser_device_profile_psp3000(&device);
    browser_config_init(&config, &device);
    config.resources.enabled = config.resources.web_fonts_enabled = false;
    BrowserEngine *engine = browser_engine_create(&config, error, sizeof(error));
    CHECK(engine && browser_engine_load_url(engine, plain, true));
    CHECK(youtube_journey_plays_at(engine, "watch-target", 0));
    uint64_t generation = browser_engine_page_generation(engine);
    ControllerAction fragment = {.type = CONTROLLER_ACTION_NAVIGATE};
    snprintf(fragment.url, sizeof(fragment.url), "%s", timed);
    CHECK(browser_engine_begin_navigation_action(engine, &fragment, MIB, 1000)
        && youtube_journey_finish(engine) == BROWSER_NAVIGATION_JOB_SUCCEEDED
        && browser_engine_page_generation(engine) == generation);
    CHECK(youtube_journey_plays_at(engine, "watch-target", 90));
    lxb_dom_node_t *root = lxb_dom_interface_node(browser_engine_navigation(engine)->page.document.html);
    lxb_dom_node_t *chapters = test_reader_find_id(root, "yt-chapters");
    ControllerAction open = {0};
    CHECK(chapters && browser_engine_focus_node(engine, chapters->first_child)
        && browser_engine_activate(engine, &open)
        && youtube_journey_plays_at(engine, "chapter", 60));
    CHECK(browser_engine_history_move(engine, false)
        && youtube_journey_plays_at(engine, "watch-target", 0)
        && browser_engine_history_move(engine, true)
        && youtube_journey_plays_at(engine, "watch-target", 90)
        && browser_engine_page_generation(engine) == generation);
    snprintf(fragment.url, sizeof(fragment.url), "%s#not-a-time", plain);
    CHECK(browser_engine_begin_navigation_action(engine, &fragment, MIB, 1000)
        && youtube_journey_finish(engine) == BROWSER_NAVIGATION_JOB_SUCCEEDED
        && youtube_journey_plays_at(engine, "watch-target", 0));
    static const struct { const char *url; unsigned seconds; } shares[] = {
        {"https://www.youtube.com/embed/TFTEST00001#t=1m30s", 90},
        {"https://www.youtube.com/live/TFTEST00001?si=fixture#t=1m30s", 90},
        {"https://www.youtube.com/embed/TFTEST00001?start=45#t=1m30s", 45},
    };
    for (size_t i = 0; i < sizeof(shares) / sizeof(shares[0]); i++) {
        CHECK(browser_engine_load_url(engine, shares[i].url, true)
            && youtube_journey_plays_at(engine, "watch-target", shares[i].seconds));
    }
    Budget *owned = browser_engine_budget(engine);
    CHECK(browser_engine_shutdown(engine) && owned->current == 0);
    browser_engine_destroy(engine);
    fetch_trace_end(); remove_google_identity_replay(fixture, 1u);
    return 0;
}

static int test_youtube_filters_and_chapters(void)
{
    static const char search[] = "<script>var ytInitialData='{\"contents\":["
        "{\"videoRenderer\":{\"videoId\":\"TFTEST00001\",\"title\":{\"simpleText\":\"Search fixture\"}}},"
        "{\"continuationItemRenderer\":{\"continuationEndpoint\":{\"continuationCommand\":{\"token\":\"fixture-next\"}}}}]}';</script>";
    /* Literal request encodings pin every supported combination, independently
       of the provider's serializer. Empty/default searches stay unchanged. */
    static const char *const params[] = {
        "", "CAM%3D", "EgIYBA%3D%3D", "CAMSAhgE", "EgIYBQ%3D%3D", "CAMSAhgF", "EgIYAg%3D%3D", "CAMSAhgC",
        "EgIIAg%3D%3D", "CAMSAggC", "EgQIAhgE", "CAMSBAgCGAQ%3D", "EgQIAhgF", "CAMSBAgCGAU%3D", "EgQIAhgC", "CAMSBAgCGAI%3D",
        "EgIIAw%3D%3D", "CAMSAggD", "EgQIAxgE", "CAMSBAgDGAQ%3D", "EgQIAxgF", "CAMSBAgDGAU%3D", "EgQIAxgC", "CAMSBAgDGAI%3D",
        "EgIIBA%3D%3D", "CAMSAggE", "EgQIBBgE", "CAMSBAgEGAQ%3D", "EgQIBBgF", "CAMSBAgEGAU%3D", "EgQIBBgC", "CAMSBAgEGAI%3D",
        "EgIIBQ%3D%3D", "CAMSAggF", "EgQIBRgE", "CAMSBAgFGAQ%3D", "EgQIBRgF", "CAMSBAgFGAU%3D", "EgQIBRgC", "CAMSBAgFGAI%3D"
    };
    Budget budget; budget_init(&budget, 16u * MIB);
    BrowserSession session;
    CHECK(browser_session_init(&session, &budget, 512u * 1024u));
    char error[256] = {0};
    for (unsigned i = 0; i < 40; i++) {
        unsigned date = i / 8, duration = (i / 2) % 4, sort = i % 2;
        char url[256], request[256], fixture[128], selected[64];
        snprintf(url, sizeof(url), "https://www.youtube.com/results?search_query=fixture&tilefinch_date=%u&tilefinch_duration=%u&tilefinch_sort=%u",
            date, duration, sort);
        snprintf(request, sizeof(request), "https://m.youtube.com/results?search_query=fixture%s%s",
            params[i][0] ? "&sp=" : "", params[i]);
        CHECK(write_document_replay(fixture, request, search, strlen(search), 200, "")
            && fetch_trace_replay_begin(fixture, error, sizeof(error)));
        YoutubeLiteDocument document = {0};
        YoutubeLiteLoadJob *job = youtube_lite_load_begin(&budget, &session, url,
            1u * MIB, 1000, error, sizeof(error));
        CHECK(job != NULL);
        YoutubeLiteLoadStatus status = YOUTUBE_LITE_LOAD_PENDING;
        for (unsigned turn = 0; turn < 256 && status == YOUTUBE_LITE_LOAD_PENDING; turn++)
            status = youtube_lite_load_pump(job, NULL);
        CHECK(status == YOUTUBE_LITE_LOAD_SUCCEEDED
            && youtube_lite_load_take_document(job, &document)
            && strstr(document.html, "id=yt-filter-panel") != NULL);
        CHECK((strstr(document.html, "id=yt-filter-reset") != NULL) == (i != 0));
        if (i != 0) {
            const char *reset = strstr(document.html, "id=yt-filter-reset");
            const char *end = strstr(reset, "</a>");
            const char *target = strstr(reset, "search_query=fixture\"");
            CHECK(end && target && target < end);
            const char *summary = strstr(document.html, "<summary>Filters");
            CHECK(summary);
            const char *separator = strstr(summary, " &middot; ");
            const char *summary_end = strstr(summary, "</summary>");
            CHECK(separator && summary_end && separator < summary_end);
        }
        static const char *const fields[] = {"tilefinch_date", "tilefinch_duration", "tilefinch_sort"};
        unsigned values[] = {date, duration, sort};
        for (unsigned field = 0; field < 3u; field++) {
            char opening[64];
            snprintf(opening, sizeof(opening), "<select id=%s ", fields[field]);
            const char *start = strstr(document.html, opening);
            CHECK(start);
            const char *end = strstr(start, "</select>");
            snprintf(selected, sizeof(selected), "<option value=%u selected>", values[field]);
            const char *option = strstr(start, selected);
            CHECK(end && option && option < end);
        }
        if (i != 0) {
            char suffix[96];
            snprintf(suffix, sizeof(suffix), "tilefinch_date=%u&amp;tilefinch_duration=%u&amp;tilefinch_sort=%u&amp;tilefinch_token=fixture-next",
                date, duration, sort);
            CHECK(strstr(document.html, suffix));
        }
        youtube_lite_document_destroy(&document);
        youtube_lite_load_destroy(job);
        fetch_trace_end(); remove_google_identity_replay(fixture, 1u);
        browser_session_site_adapter_document_cache_clear(&session);
    }
    /* Invalid local enum values and unowned service parameters are inert. */
    char fixture[128];
    CHECK(write_document_replay(fixture, "https://m.youtube.com/results?search_query=fixture",
        search, strlen(search), 200, "") && fetch_trace_replay_begin(fixture, error, sizeof(error)));
    YoutubeLiteDocument filters = {0};
    CHECK(youtube_lite_load(&budget, &session,
        "https://www.youtube.com/results?search_query=fixture&tilefinch_date=99&tilefinch_duration=-1&tilefinch_sort=1x&sp=arbitrary",
        1u * MIB, 1000, &filters, error, sizeof(error)));
    fetch_trace_end(); remove_google_identity_replay(fixture, 1u);

    static const char watch[] = "<script>var ytInitialData='{\"chapters\":["
        "{\"notchapterRenderer\":{\"title\":{\"simpleText\":\"Suffix only\"},\"timeRangeStartMillis\":0}},"
        "{\"chapterRenderer\" \t : {\"title\":{\"simpleText\":\"Intro\"},\"timeRangeStartMillis\":0}},"
        "{\"chapterRenderer\":{\"title\":{\"simpleText\":\"One & two\"},\"timeRangeStartMillis\":60000}},"
        "{\"chapterRenderer\":{\"title\":{\"simpleText\":\"Duplicate\"},\"timeRangeStartMillis\":60000}},"
        "{\"chapterRenderer\":{\"title\":{\"simpleText\":\"Negative\"},\"timeRangeStartMillis\":-1}},"
        "{\"macroMarkersListItemRenderer\":{\"title\":{\"simpleText\":\"Other video\"},"
        "\"onTap\":{\"watchEndpoint\":{\"videoId\":\"TFTEST00002\",\"startTimeSeconds\":80}}}},"
        "{\"macroMarkersListItemRenderer\":{\"title\":{\"runs\":[{\"text\":\"Final <chapter>\"}]},"
        "\"onTap\":{\"watchEndpoint\":{\"videoId\":\"TFTEST00001\",\"startTimeSeconds\":\"90\"}}}},"
        "{\"chapterRenderer\":{\"title\":{\"simpleText\":\"Past end\"},\"timeRangeStartMillis\":200000}}]}';</script>"
        "<script>var ytInitialPlayerResponse={\"videoDetails\":{\"videoId\":\"TFTEST00001\","
        "\"title\":\"Long fixture\",\"lengthSeconds\":\"200\"}};</script>";
    const char *watch_url = "https://www.youtube.com/watch?v=TFTEST00001&tilefinch_view=details";
    YoutubeLiteDocument chapters = {0}, cooperative = {0};
    CHECK(youtube_lite_build_document(&budget, watch_url, watch, strlen(watch),
        &chapters, error, sizeof(error))
        && strstr(chapters.html, "id=yt-chapters")
        && strstr(chapters.html, "&amp;t=60\">1:00 &mdash; One &amp; two")
        && strstr(chapters.html, "&amp;t=90\">1:30 &mdash; Final &lt;chapter&gt;")
        && !strstr(chapters.html, "Duplicate") && !strstr(chapters.html, "Other video")
        && !strstr(chapters.html, "Past end") && !strstr(chapters.html, "Negative"));
    CHECK(write_document_replay(fixture,
        "https://m.youtube.com/watch?v=TFTEST00001&bpctr=9999999999&has_verified=1",
        watch, strlen(watch), 200, "") && fetch_trace_replay_begin(fixture, error, sizeof(error)));
    CHECK(youtube_lite_load(&budget, &session, watch_url, 1u * MIB, 1000,
        &cooperative, error, sizeof(error))
        && chapters.html_length == cooperative.html_length
        && !memcmp(chapters.html, cooperative.html, chapters.html_length));
    youtube_lite_document_destroy(&cooperative);
    MediaChapter player_chapters[MEDIA_CHAPTER_LIMIT];
    size_t retained = budget.current;
    CHECK(youtube_lite_copy_chapters(&session,
        "https://www.youtube.com/watch?v=TFTEST00001&t=60", player_chapters,
        MEDIA_CHAPTER_LIMIT) == 3u
        && player_chapters[1].seconds == 60u
        && !strcmp(player_chapters[1].title, "One & two")
        && player_chapters[2].seconds == 90u && budget.current == retained);
    CHECK(youtube_lite_copy_chapters(&session,
        "https://www.youtube.com/watch?v=TFTEST00002", player_chapters,
        MEDIA_CHAPTER_LIMIT) == 0u);
    CHECK(youtube_lite_load(&budget, &session,
        "https://www.youtube.com/watch?v=TFTEST00001&tilefinch_view=description",
        1u * MIB, 1000, &cooperative, error, sizeof(error))
        && strstr(cooperative.html, "&amp;t=60\">1:00 &mdash; One &amp; two"));
    youtube_lite_document_destroy(&cooperative);
    fetch_trace_end(); remove_google_identity_replay(fixture, 1u);

    char many[8192];
    size_t at = (size_t) snprintf(many, sizeof(many), "<script>var ytInitialData='{\"chapters\":[");
    for (unsigned i = 0; i < 40; i++) {
        int n = snprintf(many + at, sizeof(many) - at,
            "%s{\"chapterRenderer\":{\"title\":{\"simpleText\":\"Chapter %u\"},\"timeRangeStartMillis\":%u}}",
            i ? "," : "", i, i * 10000);
        CHECK(n > 0 && (size_t) n < sizeof(many) - at); at += (size_t) n;
    }
    int n = snprintf(many + at, sizeof(many) - at, "]}';</script>");
    CHECK(n > 0 && (size_t) n < sizeof(many) - at); at += (size_t) n;
    CHECK(youtube_lite_build_document(&budget, watch_url, many, at, &cooperative, error, sizeof(error)));
    size_t count = 0;
    for (const char *p = cooperative.html; (p = strstr(p, "class=chapter ")) != NULL; p++) count++;
    CHECK(count == 24 && !strstr(cooperative.html, "Chapter 24"));
    youtube_lite_document_destroy(&cooperative);
    size_t owned = budget.current, ceiling = budget.limit;
    budget.limit = owned;
    CHECK(!youtube_lite_build_document(&budget, watch_url, watch, strlen(watch),
        &cooperative, error, sizeof(error)) && budget.current == owned);
    budget.limit = ceiling;

    BrowserDeviceProfile device; browser_device_profile_psp3000(&device);
    BrowserConfig config; browser_config_init(&config, &device);
    config.resources.enabled = config.resources.web_fonts_enabled = false;
    BrowserEngine *engine = browser_engine_create(&config, error, sizeof(error));
    CHECK(engine && browser_engine_commit_html(engine,
        "https://www.youtube.com/results?search_query=fixture", filters.html, filters.html_length, true));
    NavigationSession *nav = browser_engine_navigation(engine);
    lxb_dom_node_t *root = lxb_dom_interface_node(nav->page.document.html);
    lxb_dom_node_t *panel = test_reader_find_id(root, "yt-filter-panel");
    CHECK(panel && !lxb_dom_element_has_attribute(lxb_dom_interface_element(panel),
        (const lxb_char_t *) "open", 4u));
    /* Closed, Filters is one padded line under the heading. */
    ComputedStyle summary_style = style_for_node(&nav->page.stylesheet, panel->first_child, NULL);
    CHECK(summary_style.padding.top >= 3 && summary_style.padding.left >= 12);
    CHECK(!layout_box_for_node(&nav->page.layout,
        test_reader_find_id(root, "tilefinch_date")));
    ControllerAction action = {0};
    CHECK(panel && browser_engine_focus_node(engine, panel->first_child)
        && browser_engine_activate(engine, &action)
        && action.type == CONTROLLER_ACTION_CONTROL);
    static const char *const select_ids[] = {"tilefinch_date", "tilefinch_duration", "tilefinch_sort"};
    CHECK(browser_engine_focus_node(engine, panel->first_child)
        && browser_engine_focus_direction(engine, CONTROLLER_FOCUS_DOWN));
    lxb_dom_node_t *first_field = controller_focused_node(browser_engine_controller(engine));
    CHECK(first_field == test_reader_find_id(root, select_ids[0])
        || first_field == test_reader_find_id(root, select_ids[1])
        || first_field == test_reader_find_id(root, select_ids[2]));
    for (unsigned i = 0; i < 3; i++) {
        root = lxb_dom_interface_node(nav->page.document.html);
        ComputedStyle field_style = style_for_node(&nav->page.stylesheet,
            test_reader_find_id(root, select_ids[i]), NULL);
        CHECK(field_style.padding.top >= 4 && field_style.padding.left >= 8);
        CHECK(browser_engine_focus_node(engine, test_reader_find_id(root, select_ids[i]))
            && browser_engine_activate(engine, &action));
    }
    root = lxb_dom_interface_node(nav->page.document.html);
    CHECK(browser_engine_focus_node(engine, test_reader_find_id(root, "yt-filter-apply"))
        && browser_engine_activate(engine, &action)
        && action.type == CONTROLLER_ACTION_FORM_SUBMIT
        && strstr(action.url, "search_query=fixture")
        && strstr(action.url, "tilefinch_date=1")
        && strstr(action.url, "tilefinch_duration=1")
        && strstr(action.url, "tilefinch_sort=1"));
    /* Compact filters now fit on the first screen. Explicitly scroll so the
       history assertion still exercises a nonzero results position. */
    CHECK(browser_engine_scroll_by(engine, 50));
    ControllerFocusKind results_focus_kind = browser_engine_controller(engine)->focus_kind;
    size_t results_focus_index = browser_engine_controller(engine)->focus_index;
    int results_scroll_y = navigation_current(nav)->scroll_y;
    CHECK(results_scroll_y > 0);
    CHECK(browser_engine_focus_node(engine, test_reader_find_id(root, "yt-search"))
        && browser_engine_replace_text(engine, "edited fixture", 14u));
    /* Its form's only text field: the keyboard's Done searches (Q7). */
    CHECK(browser_engine_keyboard_done_submits(engine));
    CHECK(browser_engine_focus_node(engine, test_reader_find_id(root, "yt-filter-apply")));
    if (navigation_current(nav)->scroll_y != results_scroll_y)
        CHECK(browser_engine_scroll_by(engine,
            results_scroll_y - navigation_current(nav)->scroll_y));
    CHECK(browser_engine_commit_html(engine, watch_url, chapters.html, chapters.html_length, true));
    root = lxb_dom_interface_node(nav->page.document.html);
    panel = test_reader_find_id(root, "yt-chapters");
    CHECK(panel && browser_engine_focus_node(engine, panel->first_child)
        && browser_engine_activate(engine, &action));
    lxb_dom_node_t *link = panel->first_child->next;
    CHECK(link && link->next && browser_engine_focus_node(engine, link->next)
        && browser_engine_activate(engine, &action)
        && action.type == CONTROLLER_ACTION_NAVIGATE && action.prefer_native_media
        && youtube_watch_url_start_time_us(action.url) == UINT64_C(60000000));
    /* Returning from the native player/detail page must keep the results
       focus and edited search text, even when the provider rebuilds the page. */
    CHECK(write_document_replay(fixture,
        "https://m.youtube.com/results?search_query=fixture",
        search, strlen(search), 200, "")
        && fetch_trace_replay_begin(fixture, error, sizeof(error))
        && browser_engine_history_move(engine, false));
    CHECK(browser_engine_controller(engine)->focus_kind == results_focus_kind
        && browser_engine_controller(engine)->focus_index == results_focus_index
        && navigation_current(nav)->scroll_y == results_scroll_y);
    root = lxb_dom_interface_node(nav->page.document.html);
    size_t restored_text_length = 0;
    const char *restored_text = document_control_value(
        test_reader_find_id(root, "yt-search"), &restored_text_length);
    CHECK(restored_text != NULL && restored_text_length == 14u
        && !memcmp(restored_text, "edited fixture", 14u));
    CHECK(browser_engine_activate(engine, &action)
        && action.type == CONTROLLER_ACTION_FORM_SUBMIT
        && strstr(action.url, "tilefinch_date=1")
        && strstr(action.url, "tilefinch_duration=1")
        && strstr(action.url, "tilefinch_sort=1"));
    fetch_trace_end(); remove_google_identity_replay(fixture, 1u);
    /* Exercise Apply through the native navigation receiver, not just its
       serialized URL. Only the exact combined filter request can replay. */
    CHECK(write_document_replay(fixture,
        "https://m.youtube.com/results?search_query=fixture&sp=CAMSBAgCGAQ%3D",
        search, strlen(search), 200, "")
        && fetch_trace_replay_begin(fixture, error, sizeof(error))
        && browser_engine_begin_navigation_action(engine, &action,
            config.maximum_document_bytes, config.navigation_timeout_ms));
    for (unsigned turn = 0; turn < 512u && browser_engine_navigation_pending(engine); turn++)
        (void) browser_engine_pump_navigation(engine, NULL);
    CHECK(!browser_engine_navigation_pending(engine) && nav->page.loaded
        && nav->page.runtime == NULL && fetch_trace_replay_served_count() == 1u);
    root = lxb_dom_interface_node(nav->page.document.html);
    panel = test_reader_find_id(root, "yt-filter-panel");
    CHECK(panel && !lxb_dom_element_has_attribute(lxb_dom_interface_element(panel),
        (const lxb_char_t *) "open", 4u)
        && !layout_box_for_node(&nav->page.layout, test_reader_find_id(root, "tilefinch_date")));
    static const char *const expected_selection[] = {"01000", "0100", "01"};
    for (unsigned i = 0; i < 3u; i++) {
        char state[8];
        size_t count = controller_select_history(test_reader_find_id(root, select_ids[i]),
            state, sizeof(state), false);
        CHECK(count == strlen(expected_selection[i])
            && !memcmp(state, expected_selection[i], count));
    }
    fetch_trace_end(); remove_google_identity_replay(fixture, 1u);
    CHECK(browser_engine_focus_node(engine, panel->first_child)
        && browser_engine_activate(engine, &action));
    for (unsigned i = 0; i < 3u; i++) {
        CHECK(browser_engine_focus_node(engine, test_reader_find_id(root, select_ids[i])));
        for (size_t cycle = 1u; cycle < strlen(expected_selection[i]); cycle++)
            CHECK(browser_engine_activate(engine, &action)
                && action.type == CONTROLLER_ACTION_CONTROL);
    }
    CHECK(browser_engine_focus_node(engine, test_reader_find_id(root, "yt-filter-apply"))
        && browser_engine_activate(engine, &action)
        && action.type == CONTROLLER_ACTION_FORM_SUBMIT);
    CHECK(write_document_replay(fixture,
        "https://m.youtube.com/results?search_query=fixture",
        search, strlen(search), 200, "")
        && fetch_trace_replay_begin(fixture, error, sizeof(error))
        && browser_engine_begin_navigation_action(engine, &action,
            config.maximum_document_bytes, config.navigation_timeout_ms));
    for (unsigned turn = 0; turn < 512u && browser_engine_navigation_pending(engine); turn++)
        (void) browser_engine_pump_navigation(engine, NULL);
    CHECK(!browser_engine_navigation_pending(engine) && nav->page.loaded
        && fetch_trace_replay_served_count() == 1u);
    root = lxb_dom_interface_node(nav->page.document.html);
    panel = test_reader_find_id(root, "yt-filter-panel");
    CHECK(panel && !lxb_dom_element_has_attribute(lxb_dom_interface_element(panel),
        (const lxb_char_t *) "open", 4u));
    for (unsigned i = 0; i < 3u; i++) {
        char state[8];
        size_t count = controller_select_history(test_reader_find_id(root, select_ids[i]),
            state, sizeof(state), false);
        CHECK(count == strlen(expected_selection[i]) && state[0] == '1');
        for (size_t option = 1; option < count; option++) CHECK(state[option] == '0');
    }
    fetch_trace_end(); remove_google_identity_replay(fixture, 1u);
    static const char select_fixture[] = "<form><select id=s multiple>"
        "<optgroup><option value=same selected>A</option>"
        "<option value=same disabled>B</option></optgroup>"
        "<option value=same selected>C</option></select></form>"
        "<div style='height:900px'></div>";
    CHECK(browser_engine_commit_html(engine, "https://select-history.test/",
        select_fixture, sizeof(select_fixture) - 1, true));
    root = lxb_dom_interface_node(nav->page.document.html);
    lxb_dom_node_t *select = test_reader_find_id(root, "s");
    char selected[128], observed[128];
    CHECK(controller_select_history(select, selected, sizeof(selected), false) == 3
        && !memcmp(selected, "101", 3));
    CHECK(controller_select_history(select, selected, 3, true) == 0);
    memcpy(observed, "010", 3);
    CHECK(controller_select_history(select, observed, 2, true) == 0);
    CHECK(controller_select_history(select, observed, 3, true) == 3);
    CHECK(controller_select_history(select, observed, sizeof(observed), false) == 3
        && !memcmp(observed, "010", 3));
    char invalid[3] = {'1', 'X', '1'};
    CHECK(controller_select_history(select, invalid, 3, true) == SIZE_MAX);
    CHECK(controller_select_history(select, observed, sizeof(observed), false) == 3
        && !memcmp(observed, "010", 3));
    CHECK(controller_select_history(select, selected, 3, true) == 3
        && controller_select_history(select, observed, sizeof(observed), false) == 3
        && !memcmp(observed, "101", 3));
    /* Offline history supplies authenticated HTML rather than a network
       response. It must use the same pre-paint control restoration. */
    char local_selection[] = "010";
    CHECK(controller_select_history(select, local_selection, 3, true) == 3
        && browser_engine_focus_node(engine, select)
        && navigation_set_scroll(nav, 200));
    CHECK(browser_engine_commit_html(engine, "https://other-local.test/",
        "<p>Other page", 13, true));
    const NavigationEntry *local_entry = NULL;
    CHECK(navigation_back(nav, &local_entry) && local_entry != NULL);
    CHECK(!browser_engine_commit_history_html(engine, "https://select-history.test/",
        NULL, 0) && nav->page.loaded
        && !strcmp(nav->page.document_url, "https://other-local.test/"));
    CHECK(browser_engine_commit_history_html(engine, "https://select-history.test/",
        select_fixture, sizeof(select_fixture) - 1));
    root = lxb_dom_interface_node(nav->page.document.html);
    select = test_reader_find_id(root, "s");
    CHECK(controller_select_history(select, observed, sizeof(observed), false) == 3
        && !memcmp(observed, "010", 3)
        && browser_engine_controller(engine)->focus_kind == CONTROLLER_FOCUS_CONTROL
        && browser_engine_controller(engine)->focus_index == 0
        && navigation_current(nav)->scroll_y == 200);
    /* Hydration may insert links and serialize relative hrefs as absolute.
       Back must prefer the resolved destination to an old link ordinal. */
    static const char focus_original[] = "<a href='/first'>First</a>"
        "<a href='/chosen?part=2'>Chosen</a><div style='height:900px'></div>";
    static const char focus_hydrated[] = "<a href='/new'>New</a>"
        "<a href='/first'>First</a><a href='https://focus-history.test/chosen?part=2'>Chosen</a>"
        "<div style='height:900px'></div>";
    CHECK(browser_engine_commit_html(engine, "https://focus-history.test/",
        focus_original, sizeof(focus_original) - 1, true));
    CHECK(browser_engine_focus_node(engine, nav->page.layout.links[1].node));
    CHECK(browser_engine_commit_html(engine, "https://other-focus.test/",
        "<p>Other", 8, true));
    CHECK(navigation_back(nav, &local_entry));
    CHECK(browser_engine_commit_history_html(engine, "https://focus-history.test/",
        focus_hydrated, sizeof(focus_hydrated) - 1));
    BrowserController *restored_controller = browser_engine_controller(engine);
    CHECK(restored_controller->focus_kind == CONTROLLER_FOCUS_LINK);
    CHECK(!strcmp(nav->page.layout.links[restored_controller->focus_index].url,
        "https://focus-history.test/chosen?part=2"));
    static const char focus_base_original[] =
        "<base href='https://destination.test/folder/'>"
        "<a href='first'>First</a><a href='../chosen?part=3'>Chosen</a>";
    static const char focus_base_hydrated[] =
        "<base href='https://destination.test/folder/'>"
        "<a href='new'>New</a><a href='first'>First</a>"
        "<a href='https://destination.test/chosen?part=3'>Chosen</a>";
    CHECK(browser_engine_commit_html(engine, "https://base-focus.test/",
        focus_base_original, sizeof(focus_base_original) - 1, true));
    CHECK(browser_engine_focus_node(engine, nav->page.layout.links[1].node));
    CHECK(browser_engine_commit_html(engine, "https://other-base.test/", "<p>Other", 8, true));
    CHECK(navigation_back(nav, &local_entry));
    CHECK(browser_engine_commit_history_html(engine, "https://base-focus.test/",
        focus_base_hydrated, sizeof(focus_base_hydrated) - 1));
    restored_controller = browser_engine_controller(engine);
    CHECK(restored_controller->focus_kind == CONTROLLER_FOCUS_LINK
        && !strcmp(nav->page.layout.links[restored_controller->focus_index].url,
            "https://destination.test/chosen?part=3"));
    browser_engine_destroy(engine);
    youtube_lite_document_destroy(&filters); youtube_lite_document_destroy(&chapters);
    browser_session_destroy(&session);
    CHECK(budget.current == 0);
    return 0;
}
