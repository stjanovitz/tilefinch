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
        snprintf(selected, sizeof(selected), "<option value=%u selected>", date);
        CHECK(strstr(document.html, selected));
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
        "{\"chapterRenderer\":{\"title\":{\"simpleText\":\"Intro\"},\"timeRangeStartMillis\":0}},"
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
    ControllerAction action = {0};
    CHECK(panel && browser_engine_focus_node(engine, panel->first_child)
        && browser_engine_activate(engine, &action)
        && action.type == CONTROLLER_ACTION_CONTROL);
    static const char *const select_ids[] = {"tilefinch_date", "tilefinch_duration", "tilefinch_sort"};
    for (unsigned i = 0; i < 3; i++) {
        root = lxb_dom_interface_node(nav->page.document.html);
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
    browser_engine_destroy(engine);
    youtube_lite_document_destroy(&filters); youtube_lite_document_destroy(&chapters);
    browser_session_destroy(&session);
    CHECK(budget.current == 0);
    return 0;
}
