/* Watch-next cards, the watch page's channel link and the History page.
   Every fixture is synthetic: invented IDs, titles and response shapes. */

#define WATCH_NEXT_CHANNEL "UCabcdefghijklmnopqrstuv"

static int watch_next_build(const char *initial, const char *channel_id,
                            YoutubeLiteDocument *document)
{
    char source[8192], error[256] = {0};
    int length = snprintf(source, sizeof(source),
        "<script>var ytInitialData='%s';</script>"
        "<script>var ytInitialPlayerResponse={\"videoDetails\":{\"videoId\":\"TFTEST00001\","
        "\"channelId\":\"%s\",\"author\":\"Fixture <Channel>\",\"title\":\"Watch fixture\","
        "\"shortDescription\":\"About the fixture\",\"lengthSeconds\":\"200\"}};</script>",
        initial, channel_id);
    Budget *budget = malloc(sizeof(*budget));
    if (budget == NULL || length <= 0 || (size_t) length >= sizeof(source)) return 1;
    budget_init(budget, 4u * MIB);
    bool built = youtube_lite_build_document(budget,
        "https://www.youtube.com/watch?v=TFTEST00001&tilefinch_view=details",
        source, (size_t) length, document, error, sizeof(error));
    if (!built) fprintf(stderr, "watch-next build failed: %s\n", error);
    return built ? 0 : 1;
}

static int watch_next_release(YoutubeLiteDocument *document)
{
    Budget *budget = document->budget;
    youtube_lite_document_destroy(document);
    bool released = budget != NULL && budget->current == 0;
    free(budget);
    return released ? 0 : 1;
}

static int test_youtube_watch_next_cards(void)
{
    static const char related[] = "{\"contents\":["
        "{\"slimOwnerRenderer\":{\"thumbnail\":{\"thumbnails\":[{\"url\":\"//yt3.ggpht.com/fixture-avatar=s48\"}]},"
        "\"title\":{\"runs\":[{\"text\":\"Owner title\"}]},\"navigationEndpoint\":{\"browseEndpoint\":{"
        "\"browseId\":\"" WATCH_NEXT_CHANNEL "\",\"canonicalBaseUrl\":\"/@fixture.handle\"}}}},"
        "{\"slimOwnerRenderer\":{\"navigationEndpoint\":{\"browseEndpoint\":{"
        "\"browseId\":\"UCzzzzzzzzzzzzzzzzzzzzzz\",\"canonicalBaseUrl\":\"/@second\"}}}},"
        "{\"videoWithContextRenderer\":{\"videoId\":\"TFTEST00002\",\"headline\":{\"runs\":[{\"text\":\"First suggestion\"}]},"
        "\"shortBylineText\":{\"runs\":[{\"text\":\"Other channel\"}]},\"lengthText\":{\"runs\":[{\"text\":\"4:05\"}]}}},"
        "{\"adSlotRenderer\":{\"fulfillmentContent\":{\"videoWithContextRenderer\":{\"videoId\":\"TFTEST00009\","
        "\"headline\":{\"runs\":[{\"text\":\"Sponsored fixture\"}]}}}}},"
        "{\"reelShelfRenderer\":{\"items\":[{\"videoWithContextRenderer\":{\"videoId\":\"TFTEST00008\","
        "\"headline\":{\"runs\":[{\"text\":\"Short fixture\"}]}}}]}},"
        "{\"lockupViewModel\":{\"contentType\":\"LOCKUP_CONTENT_TYPE_PLAYLIST\",\"contentId\":\"PLfixturelist\","
        "\"metadata\":{\"lockupMetadataViewModel\":{\"title\":{\"content\":\"Fixture & list\"},"
        "\"metadata\":{\"contentMetadataViewModel\":{\"metadataRows\":[{\"metadataParts\":[{\"text\":{\"content\":\"List owner\"}}]}]}}}},"
        "\"contentImage\":{\"thumbnailBadgeViewModel\":{\"text\":\"12 videos\"}}}},"
        "{\"lockupViewModel\":{\"contentType\":\"LOCKUP_CONTENT_TYPE_PLAYLIST\",\"contentId\":\"RDTFTEST00002\","
        "\"metadata\":{\"lockupMetadataViewModel\":{\"title\":{\"content\":\"Mix fixture\"}}}}},"
        "{\"compactRadioRenderer\":{\"playlistId\":\"RDTFTEST00003\",\"title\":{\"simpleText\":\"Radio fixture\"},"
        "\"videoId\":\"TFTEST00007\"}},"
        "{\"compactPlaylistRenderer\":{\"playlistId\":\"PLfixturelist\",\"title\":{\"simpleText\":\"Duplicate list\"}}},"
        "{\"compactPlaylistRenderer\":{\"playlistId\":\"PLsecondlist\",\"title\":{\"simpleText\":\"Second list\"}}},"
        "{\"compactVideoRenderer\":{\"videoId\":\"TFTEST00003\",\"title\":{\"simpleText\":\"Compact suggestion\"}}},"
        "{\"lockupViewModel\":{\"contentType\":\"LOCKUP_CONTENT_TYPE_VIDEO\",\"contentId\":\"TFTEST00004\","
        "\"metadata\":{\"lockupMetadataViewModel\":{\"title\":{\"content\":\"Lockup suggestion\"}}}}},"
        "{\"videoWithContextRenderer\":{\"videoId\":\"TFTEST00002\",\"headline\":{\"runs\":[{\"text\":\"Repeated\"}]}}}"
        "]}";
    YoutubeLiteDocument document = {0};
    CHECK(watch_next_build(related, WATCH_NEXT_CHANNEL, &document) == 0);
    const char *html = document.html;
    const char *heading = strstr(html, "<h2>Up next</h2>");
    const char *first = strstr(html, "watch?v=TFTEST00002\"");
    const char *list = strstr(html, "playlist?list=PLfixturelist\"");
    const char *second = strstr(html, "playlist?list=PLsecondlist\"");
    const char *compact = strstr(html, "watch?v=TFTEST00003\"");
    const char *lockup = strstr(html, "watch?v=TFTEST00004\"");
    CHECK(heading && first && list && second && compact && lockup
        && heading < first && first < list && list < second
        && second < compact && compact < lockup);
    /* Description and the comments entry stay above the bounded list. */
    const char *description = strstr(html, "id=description");
    const char *comments = strstr(html, "tilefinch_view=comments");
    CHECK(description && comments && description < heading && comments < heading);
    CHECK(strstr(html, "Fixture &amp; list</span><span class=meta>12 videos</span>")
        && strstr(html, "list=PLfixturelist\"><span class=thumb-wrap><span class=duration>Playlist</span>")
        && strstr(html, "Second list") && !strstr(html, "Duplicate list")
        && !strstr(html, "Sponsored fixture") && !strstr(html, "TFTEST00009")
        && !strstr(html, "Short fixture") && !strstr(html, "TFTEST00008")
        && !strstr(html, "Mix fixture") && !strstr(html, "RDTFTEST")
        && !strstr(html, "Radio fixture") && !strstr(html, "TFTEST00007")
        && !strstr(html, "Repeated"));
    CHECK(document.result_count == 5u);
    /* The owner card links by handle, with its avatar; the second card
       (another channel) is ignored and the player's author names it. */
    CHECK(strstr(html, "<div class=watch-action-row><a id=yt-watch-channel class=channel-link "
        "href=\"https://www.youtube.com/@fixture.handle\">"
        "<img class=channel-avatar src=\"https://yt3.ggpht.com/fixture-avatar=s48\" "
        "width=24 height=24 loading=lazy alt=\"\">Fixture &lt;Channel&gt;</a><a id=yt-offline-save")
        && !strstr(html, "<p class=watch-meta>Fixture")
        && !strstr(html, "@second") && !strstr(html, "Owner title"));

    /* d-pad: down from the play target reaches the channel link, which
       leads the action row; right moves on to the next action. */
    BrowserDeviceProfile device;
    browser_device_profile_psp3000(&device);
    BrowserConfig config;
    browser_config_init(&config, &device);
    config.memory_limit = 16u * MIB;
    config.resources.enabled = false;
    config.resources.web_fonts_enabled = false;
    char error[256] = {0};
    BrowserEngine *engine = browser_engine_create(&config, error, sizeof(error));
    CHECK(engine != NULL && browser_engine_commit_html(engine,
        "https://www.youtube.com/watch?v=TFTEST00001&tilefinch_view=details",
        document.html, document.html_length, true));
    CHECK(browser_engine_render_frame(engine, NULL));
    NavigationSession *nav = browser_engine_navigation(engine);
    lxb_dom_node_t *root = lxb_dom_interface_node(nav->page.document.html);
    lxb_dom_node_t *channel = test_reader_find_id(root, "yt-watch-channel");
    lxb_dom_node_t *actions = test_reader_find_id(root, "yt-watch-actions");
    CHECK(channel != NULL && actions != NULL);
#define WATCH_NEXT_FOCUSED() controller_focused_node(browser_engine_controller(engine))
    /* channel-link < div.watch-action-row < section.watch-actions, which
       follows section.watch-hero, whose first child is the play target. */
    lxb_dom_node_t *play = channel->parent->parent->prev->first_child;
    CHECK(browser_engine_focus_node(engine, play) && WATCH_NEXT_FOCUSED() == play);
    size_t class_length = 0;
    const char *play_class = play == NULL ? NULL : document_attribute(play, "class", &class_length);
    CHECK(play_class != NULL && class_length >= 12u && memcmp(play_class, "watch-target", 12u) == 0);
    CHECK(browser_engine_focus_direction(engine, CONTROLLER_FOCUS_DOWN)
        && WATCH_NEXT_FOCUSED() == channel);
    CHECK(browser_engine_focus_direction(engine, CONTROLLER_FOCUS_RIGHT)
        && WATCH_NEXT_FOCUSED() == test_reader_find_id(root, "yt-offline-save"));
    const LayoutNodeBox *link_box = layout_box_for_node(&nav->page.layout, channel);
    const LayoutNodeBox *row_box = layout_box_for_node(&nav->page.layout, actions);
    CHECK(link_box && row_box && link_box->height == 36 && link_box->width <= 140
        && link_box->x >= row_box->x && link_box->y >= row_box->y);
#undef WATCH_NEXT_FOCUSED
    CHECK(browser_engine_shutdown(engine));
    browser_engine_destroy(engine);
    CHECK(watch_next_release(&document) == 0);

    /* An owner card for another channel never supplies the link or avatar. */
    static const char foreign[] = "{\"contents\":[{\"videoOwnerRenderer\":{"
        "\"thumbnail\":{\"thumbnails\":[{\"url\":\"https://yt3.ggpht.com/other=s48\"}]},"
        "\"navigationEndpoint\":{\"browseEndpoint\":{\"browseId\":\"UCzzzzzzzzzzzzzzzzzzzzzz\","
        "\"canonicalBaseUrl\":\"/@other\"}}}}]}";
    CHECK(watch_next_build(foreign, WATCH_NEXT_CHANNEL, &document) == 0);
    CHECK(strstr(document.html, "href=\"https://www.youtube.com/channel/" WATCH_NEXT_CHANNEL "\">Fixture &lt;Channel&gt;</a>")
        && !strstr(document.html, "@other") && !strstr(document.html, "<img class=channel-avatar")
        && !strstr(document.html, "<h2>Up next</h2>") && document.result_count == 0u);
    CHECK(watch_next_release(&document) == 0);

    /* Unsafe handles and avatar hosts fall back to the ID link, no image. */
    static const char unsafe[] = "{\"contents\":[{\"slimOwnerRenderer\":{"
        "\"thumbnail\":{\"thumbnails\":[{\"url\":\"https://evil.test/avatar.png\"}]},"
        "\"navigationEndpoint\":{\"browseEndpoint\":{\"browseId\":\"" WATCH_NEXT_CHANNEL "\","
        "\"canonicalBaseUrl\":\"/@bad\\\\\"handle\"}}}}]}";
    CHECK(watch_next_build(unsafe, WATCH_NEXT_CHANNEL, &document) == 0);
    CHECK(strstr(document.html, "href=\"https://www.youtube.com/channel/" WATCH_NEXT_CHANNEL "\">")
        && !strstr(document.html, "evil.test") && !strstr(document.html, "<img class=channel-avatar"));
    CHECK(watch_next_release(&document) == 0);

    /* Without a channel ID the name stays plain text. */
    CHECK(watch_next_build("{}", "", &document) == 0);
    CHECK(!strstr(document.html, "yt-watch-channel")
        && strstr(document.html, "<p class=watch-meta>Fixture &lt;Channel&gt;"));
    CHECK(watch_next_release(&document) == 0);

    /* The list holds every retained entry, up to the provider's bound. */
    char many[6144] = "{\"contents\":[";
    size_t at = strlen(many);
    for (unsigned i = 0; i < YOUTUBE_LITE_MAXIMUM_RESULTS + 3u; i++) {
        int n = snprintf(many + at, sizeof(many) - at,
            "%s{\"videoWithContextRenderer\":{\"videoId\":\"TFMANY%05u\","
            "\"headline\":{\"runs\":[{\"text\":\"Many %u\"}]}}}", i ? "," : "", i, i);
        CHECK(n > 0 && (size_t) n < sizeof(many) - at);
        at += (size_t) n;
    }
    CHECK(at + 3u < sizeof(many));
    memcpy(many + at, "]}", 3u);
    CHECK(watch_next_build(many, WATCH_NEXT_CHANNEL, &document) == 0);
    CHECK(document.result_count == YOUTUBE_LITE_MAXIMUM_RESULTS
        && strstr(document.html, "Many 11<") && !strstr(document.html, "Many 12<"));
    CHECK(watch_next_release(&document) == 0);
    return 0;
}

/* An engine configured for page images, committing provider documents
   the way the adapter path does: a static, browser-authored commit. */
static BrowserEngine *watch_commit_engine(void)
{
    BrowserDeviceProfile profile;
    browser_device_profile_psp3000(&profile);
    BrowserConfig config;
    browser_config_init(&config, &profile);
    config.memory_limit = 16u * MIB;
    config.resources.enabled = true;
    config.resources.web_fonts_enabled = false;
    char error[256] = {0};
    BrowserEngine *engine = browser_engine_create(&config, error, sizeof(error));
    if (engine == NULL) fprintf(stderr, "watch commit engine: %s\n", error);
    return engine;
}

static bool watch_commit_static(BrowserEngine *engine, const char *url,
                                const YoutubeLiteDocument *document)
{
    NavigationSession *navigation = browser_engine_navigation(engine);
    if (navigation == NULL) return false;
    uint64_t generation = navigation_begin(navigation);
    return navigation_commit_static_html(
        navigation, generation, url, document->html, document->html_length,
        480, NULL, NULL, true);
}

/* The owner card of the watch page, with its avatar. */
static const char watch_commit_related[] = "{\"contents\":[{\"slimOwnerRenderer\":{"
    "\"thumbnail\":{\"thumbnails\":[{\"url\":\"//yt3.ggpht.com/fixture-avatar=s48\"}]},"
    "\"navigationEndpoint\":{\"browseEndpoint\":{\"browseId\":\"" WATCH_NEXT_CHANNEL "\","
    "\"canonicalBaseUrl\":\"/@fixture.handle\"}}}}]}";
/* A 4x4 PNG. */
static const char watch_commit_avatar[] =
    "\x89\x50\x4e\x47\x0d\x0a\x1a\x0a\x00\x00\x00\x0d\x49\x48\x44\x52"
    "\x00\x00\x00\x04\x00\x00\x00\x04\x08\x02\x00\x00\x00\x26\x93\x09"
    "\x29\x00\x00\x00\x10\x49\x44\x41\x54\x78\x9c\x63\x38\x63\x6c\x0c"
    "\x47\x0c\xc4\x71\x00\xfb\x13\x13\x21\x2c\x5d\xcd\x23\x00\x00\x00"
    "\x00\x49\x45\x4e\x44\xae\x42\x60\x82";

/* perf-1110 R2: the channel avatar was the watch page's only eager visible
   image, so the commit fetched it (on a live network, from a host the
   session has no connection to) between its layout passes. It waits in
   the idle image queue now, like the thumbnails and the poster, and loads
   from there after the first frame. */
static int test_youtube_watch_commit_defers_avatar(void)
{
    YoutubeLiteDocument document = {0};
    CHECK(watch_next_build(watch_commit_related, WATCH_NEXT_CHANNEL, &document) == 0
        && strstr(document.html, "<img class=channel-avatar") != NULL);
    YoutubeReplayRecord record = {
        "GET", "https://yt3.ggpht.com/fixture-avatar=s48", "image/png",
        watch_commit_avatar, sizeof(watch_commit_avatar) - 1u
    };
    char directory[128], error[256] = {0};
    CHECK(write_youtube_records_replay(directory, &record, 1)
          && fetch_trace_replay_begin_response_keyed(directory, error, sizeof(error)));
    BrowserEngine *engine = watch_commit_engine();
    CHECK(engine != NULL && watch_commit_static(engine,
        "https://www.youtube.com/watch?v=TFTEST00001&tilefinch_view=details", &document));
    /* navigation_begin zeroed the tallies: this is the commit's count. */
    TilefinchWorkCount commit_passes = tilefinch_work_tally.layout_passes;
    NavigationSession *navigation = browser_engine_navigation(engine);
    /* perf-1110 R3: the action row's inline SVG icons are published before
       the image-planning layout, so that layout is the page's final one:
       the commit lays the watch page out once, not twice. */
    size_t svg_published = 0;
    for (size_t i = 0; i < navigation->page.images.count; i++) {
        const ImageResource *image = &navigation->page.images.items[i];
        svg_published += image->pixels != NULL && image->node != NULL
            && image->node->type == LXB_DOM_NODE_TYPE_ELEMENT
            && lxb_dom_element_local_name(lxb_dom_interface_element(image->node), NULL) != NULL
            && strcmp((const char *) lxb_dom_element_local_name(
                   lxb_dom_interface_element(image->node), NULL), "svg") == 0;
    }
    if (commit_passes != 1 || svg_published == 0
        || navigation->performance.static_image_layout_adoptions != 1)
        fprintf(stderr, "watch commit layout passes=%llu svg=%zu adoptions=%zu\n",
                (unsigned long long) commit_passes, svg_published,
                navigation->performance.static_image_layout_adoptions);
    CHECK(commit_passes == 1 && svg_published != 0
        && navigation->performance.static_image_layout_adoptions == 1);
    FetchTraceReplayStats replay = {0};
    CHECK(fetch_trace_replay_stats(&replay));
    lxb_dom_node_t *channel = test_reader_find_id(
        lxb_dom_interface_node(navigation->page.document.html), "yt-watch-channel");
    lxb_dom_node_t *avatar = channel == NULL ? NULL : channel->first_child;
    CHECK(avatar != NULL && avatar->type == LXB_DOM_NODE_TYPE_ELEMENT);
    bool avatar_queued = false;
    for (size_t i = 0; i < navigation->page.deferred_image_count; i++)
        avatar_queued |= navigation->page.deferred_image_targets[i].node == avatar;
    if (replay.request_count != 0 || !avatar_queued)
        fprintf(stderr, "watch commit requests=%zu avatar-queued=%d\n",
                replay.request_count, avatar_queued);
    /* No request inside the commit; the avatar waits in the idle queue. */
    CHECK(replay.request_count == 0 && replay.claimed_record_count == 0
        && navigation->page.loaded && avatar_queued
        && images_find_node(&navigation->page.images, avatar) == NULL
        && navigation_background_resources_pending(navigation));
    size_t pumps = 0;
    while (navigation_background_resources_pending(navigation) && pumps++ < 64u)
        if (!navigation_run_background_resources(navigation)) break;
    /* The idle queue requests it after the commit. */
    CHECK(fetch_trace_replay_stats(&replay) && replay.claimed_record_count == 1
        && !navigation_background_resources_pending(navigation));
    fetch_trace_end();
    CHECK(browser_engine_shutdown(engine));
    browser_engine_destroy(engine);
    remove_youtube_records_replay(directory, 1);
    CHECK(watch_next_release(&document) == 0);
    return 0;
}

static int history_load(Budget *budget, BrowserSession *session, const char *url,
                        YoutubeLiteDocument *document)
{
    char error[256] = {0};
    bool loaded = youtube_lite_load(budget, session, url, 1u * MIB, 1000,
                                    document, error, sizeof(error));
    if (!loaded) fprintf(stderr, "history load failed: %s\n", error);
    return loaded ? 0 : 1;
}

static int test_youtube_history_page(void)
{
    CHECK(youtube_lite_route("https://www.youtube.com/feed/history") == YOUTUBE_LITE_ROUTE_HISTORY
        && youtube_lite_route("https://m.youtube.com/feed/history?tilefinch_token=x") == YOUTUBE_LITE_ROUTE_HISTORY
        && youtube_lite_navigation_requires_network("https://www.youtube.com/feed/history"));
    static const char bootstrap[] = "<script>ytcfg.set({\"INNERTUBE_API_KEY\":\"fixture-key\","
        "\"INNERTUBE_CONTEXT_CLIENT_VERSION\":\"fixture-version\",\"INNERTUBE_CLIENT_NAME\":\"MWEB\","
        "\"VISITOR_DATA\":\"fixture-visitor\",\"SESSION_INDEX\":\"0\",\"LOGGED_IN\":true});</script>"
        "<script>var ytInitialData='{}';</script>";
    static const char first_page[] = "{\"responseContext\":{\"mainAppWebResponseContext\":{\"loggedOut\":false}},"
        "\"contents\":{\"sectionListRenderer\":{\"contents\":[{\"itemSectionRenderer\":{\"contents\":["
        "{\"videoRenderer\":{\"videoId\":\"TFHIST00001\",\"title\":{\"simpleText\":\"Watched <one>\"},"
        "\"navigationEndpoint\":{\"watchEndpoint\":{\"videoId\":\"TFHIST00001\",\"startTimeSeconds\":75}},"
        "\"thumbnailOverlays\":[{\"thumbnailOverlayResumePlaybackProgressRenderer\":{\"percentDurationWatched\":30}}]}},"
        "{\"reelShelfRenderer\":{\"items\":[{\"videoWithContextRenderer\":{\"videoId\":\"TFHIST00009\","
        "\"headline\":{\"runs\":[{\"text\":\"Short in history\"}]}}}]}},"
        "{\"lockupViewModel\":{\"contentType\":\"LOCKUP_CONTENT_TYPE_VIDEO\",\"contentId\":\"TFHIST00002\","
        "\"metadata\":{\"lockupMetadataViewModel\":{\"title\":{\"content\":\"Watched two\"}}},"
        "\"contentImage\":{\"thumbnailViewModel\":{\"overlays\":[{\"thumbnailBottomOverlayViewModel\":{"
        "\"progressBar\":{\"thumbnailOverlayProgressBarViewModel\":{\"startPercent\":60}}}}]}}}}]}},"
        "{\"continuationItemRenderer\":{\"continuationEndpoint\":{\"continuationCommand\":{"
        "\"token\":\"history-next\"}}}}]}}}";
    static const char second_page[] = "{\"responseContext\":{},\"onResponseReceivedActions\":[{"
        "\"appendContinuationItemsAction\":{\"continuationItems\":[{\"itemSectionRenderer\":{\"contents\":["
        "{\"videoRenderer\":{\"videoId\":\"TFHIST00003\",\"title\":{\"simpleText\":\"Watched three\"}}}]}}]}}]}";
    const char *browse = "https://m.youtube.com/youtubei/v1/browse?key=fixture-key&prettyPrint=false";
    GoogleIdentityRecord records[] = {
        {.url="https://m.youtube.com/feed/history", .status=200, .content_type="text/html", .body=bootstrap},
        {.url=browse, .status=200, .content_type="application/json", .body=first_page, .method="POST"},
        {.url=browse, .status=200, .content_type="application/json", .body=second_page, .method="POST"}
    };
    char fixture[128], error[256] = {0};
    CHECK(write_google_identity_replay(fixture, records, 3u)
        && fetch_trace_replay_begin(fixture, error, sizeof(error)));
    Budget budget;
    budget_init(&budget, 8u * MIB);
    BrowserSession session;
    CHECK(browser_session_init(&session, &budget, 1024));
    CHECK(browser_session_cookie_set_http(&session, "https://m.youtube.com/",
        "SAPISID=mock-session; Secure; HttpOnly; Path=/"));
    YoutubeLiteDocument document = {0};
    CHECK(history_load(&budget, &session, "https://www.youtube.com/feed/history", &document) == 0);
    const char *html = document.html;
    CHECK(document.route == YOUTUBE_LITE_ROUTE_HISTORY && document.result_count == 2u);
    CHECK(strstr(html, "<title>History</title>") && strstr(html, "<h1>History</h1>")
        && strstr(html, "href=\"https://www.youtube.com/playlist?list=WL\">Watch Later</a>"));
    /* Resume positions and progress, from both card shapes. */
    CHECK(strstr(html, "watch?v=TFHIST00001&amp;t=75\"") && strstr(html, "Watched &lt;one&gt;")
        && strstr(html, "30 percent watched") && strstr(html, "watch?v=TFHIST00002\"")
        && strstr(html, "60 percent watched") && !strstr(html, "TFHIST00009"));
    CHECK(strstr(html, "href=\"https://www.youtube.com/feed/history?tilefinch_token=history-next\""));
    youtube_lite_document_destroy(&document);
    /* The next page reuses the cached identity: one signed browse request. */
    CHECK(history_load(&budget, &session,
        "https://www.youtube.com/feed/history?tilefinch_token=history-next", &document) == 0);
    CHECK(document.result_count == 1u && strstr(document.html, "Watched three")
        && !strstr(document.html, "tilefinch_token="));
    youtube_lite_document_destroy(&document);
    /* The account menu links History beside Watch Later. */
    CHECK(history_load(&budget, &session, "https://www.youtube.com/", &document) == 0);
    CHECK(strstr(document.html, "{{") == NULL
        && strstr(document.html, "list=WL\">Watch Later</a><a href=\"https://www.youtube.com/feed/history\">History</a>"));
    youtube_lite_document_destroy(&document);
    fetch_trace_end();
    remove_google_identity_replay(fixture, 3u);

    /* Signed out: no request at all (an empty replay refuses any), only a
       pointer to sign-in. */
    CHECK(browser_session_cookie_clear_domain(&session, "https://youtube.com/"));
    CHECK(write_google_identity_replay(fixture, records, 0u)
        && fetch_trace_replay_begin(fixture, error, sizeof(error)));
    CHECK(history_load(&budget, &session, "https://www.youtube.com/feed/history", &document) == 0);
    CHECK(document.result_count == 0u
        && strstr(document.html, "<p class=empty>Sign in to view your history.</p>")
        && strstr(document.html, "id=yt-history-signin autofocus")
        && strstr(document.html, "href=\"" YOUTUBE_LITE_SIGNIN_HTML_URL "\">Sign in</a>")
        && !strstr(document.html, "No entries were returned"));
    youtube_lite_document_destroy(&document);
    fetch_trace_end();
    remove_google_identity_replay(fixture, 0u);
    browser_session_site_adapter_document_cache_clear(&session);
    browser_session_destroy(&session);
    CHECK(budget.current == 0 && budget.external_reserved == 0);
    return 0;
}

#undef WATCH_NEXT_CHANNEL
