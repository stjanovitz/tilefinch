/* The provider's page builder on edge shapes of its input: display strings
   longer than their fields, unplayable playlist entries, link forms YouTube
   serves besides the canonical one, single-row lockup metadata, scheduled
   broadcasts, and a next page that cannot be fetched. The JSON is
   synthetic, minimized provider data written for these tests. */

/* True when text[0, length) is well-formed UTF-8. */
static bool page_builder_utf8_valid(const char *text, size_t length)
{
    const unsigned char *at = (const unsigned char *) text;
    const unsigned char *end = at + length;
    while (at < end) {
        size_t count = *at < 0x80u ? 1u : (*at & 0xe0u) == 0xc0u ? 2u
            : (*at & 0xf0u) == 0xe0u ? 3u : (*at & 0xf8u) == 0xf0u ? 4u : 0u;
        if (count == 0u || (size_t) (end - at) < count) return false;
        for (size_t i = 1; i < count; i++)
            if ((at[i] & 0xc0u) != 0x80u) return false;
        at += count;
    }
    return true;
}

static int page_builder_build(Budget *budget, const char *url,
                              const char *source, YoutubeLiteDocument *document)
{
    char error[256] = {0};
    if (!youtube_lite_build_document(budget, url, source, strlen(source),
                                     document, error, sizeof(error))) {
        fprintf(stderr, "page builder %s: %s\n", url, error);
        return 1;
    }
    CHECK(page_builder_utf8_valid(document->html, document->html_length));
    return 0;
}

/* Titles, channel names and watch titles longer than their fields keep a
   prefix cut on a character boundary: a 100-character CJK title is 300
   bytes. Before, a card title fell back to "YouTube video" and the others
   were cut inside a character. */
static int test_youtube_page_builder_long_strings(void)
{
    enum { REPEAT = 12 };
    /* "日本語のタイトル": 8 characters, 24 bytes; 12 copies are 288 bytes. */
    static const char phrase[] = "\xe6\x97\xa5\xe6\x9c\xac\xe8\xaa\x9e\xe3\x81\xae"
        "\xe3\x82\xbf\xe3\x82\xa4\xe3\x83\x88\xe3\x83\xab";
    /* "チャンネル名前": 7 characters, 21 bytes; 7 copies are 147 bytes. */
    static const char name[] = "\xe3\x83\x81\xe3\x83\xa3\xe3\x83\xb3\xe3\x83\x8d"
        "\xe3\x83\xab\xe5\x90\x8d\xe5\x89\x8d";
    char title[REPEAT * sizeof(phrase)] = {0}, channel[7 * sizeof(name)] = {0};
    for (unsigned i = 0; i < REPEAT; i++) strcat(title, phrase);
    for (unsigned i = 0; i < 7u; i++) strcat(channel, name);
    char source[2048];
    snprintf(source, sizeof(source),
        "<script>var ytInitialData = {\"contents\":[{\"videoWithContextRenderer\":"
        "{\"videoId\":\"LONGTITLE01\",\"headline\":{\"runs\":[{\"text\":\"%s\"}]},"
        "\"shortBylineText\":{\"runs\":[{\"text\":\"%s\"}]},"
        "\"lengthText\":{\"runs\":[{\"text\":\"3:21\"}]}}}]};</script>",
        title, channel);
    Budget budget;
    budget_init(&budget, 4u * MIB);
    YoutubeLiteDocument document = {0};
    CHECK(page_builder_build(&budget,
        "https://www.youtube.com/results?search_query=long", source, &document) == 0);
    /* The longest whole-character prefix of each field: 255 bytes of title
       hold 85 characters, 127 bytes of channel 42. */
    char expected[512];
    snprintf(expected, sizeof(expected), "<span class=title>%.255s</span>", title);
    CHECK(strstr(document.html, expected) != NULL
        && strstr(document.html, "YouTube video") == NULL);
    snprintf(expected, sizeof(expected), "<span class=channel>%.126s</span>", channel);
    CHECK(strstr(document.html, expected) != NULL);
    youtube_lite_document_destroy(&document);

    snprintf(source, sizeof(source),
        "<script>var ytInitialPlayerResponse = {\"videoDetails\":{\"videoId\":"
        "\"LONGTITLE01\",\"title\":\"A%s\",\"author\":\"%s\",\"channelId\":"
        "\"UCaaaaaaaaaaaaaaaaaaaaaa\",\"lengthSeconds\":\"100\",\"viewCount\":\"5\"}};"
        "</script><script>var ytInitialData = {\"contents\":{}};</script>",
        title, channel);
    CHECK(page_builder_build(&budget,
        "https://www.youtube.com/watch?v=LONGTITLE01", source, &document) == 0);
    /* "A" and 84 characters fill 253 of the 255 bytes. */
    snprintf(expected, sizeof(expected), "<h1>A%.252s</h1>", title);
    CHECK(strstr(document.html, expected) != NULL);
    youtube_lite_document_destroy(&document);
    CHECK(budget.current == 0);
    return 0;
}

/* A private or deleted entry is a dimmed row with YouTube's own title: no
   link, thumbnail or player, no queue position, and Play all starts on a
   playable entry. On a playlist its owner can edit, the row keeps its
   Remove action. */
static int test_youtube_page_builder_unplayable_entries(void)
{
    static const char entries[] =
        "{\"playlistVideoRenderer\":{\"videoId\":\"PRIVATE0001\",\"title\":{\"runs\":"
        "[{\"text\":\"[Private video]\"}]},\"isPlayable\":false}},"
        "{\"playlistVideoRenderer\":{\"videoId\":\"PLAYABLE001\",\"title\":{\"runs\":"
        "[{\"text\":\"Good\"}]},\"isPlayable\":true,\"lengthText\":{\"simpleText\":\"1:00\"}}},"
        "{\"playlistVideoRenderer\":{\"videoId\":\"DELETED0001\",\"title\":{\"runs\":"
        "[{\"text\":\"[Deleted video]\"}]},\"isPlayable\":false}},"
        "{\"playlistVideoRenderer\":{\"videoId\":\"PLAYABLE002\",\"title\":{\"runs\":"
        "[{\"text\":\"Also good\"}]}}}";
    char source[2048];
    Budget budget;
    budget_init(&budget, 4u * MIB);
    YoutubeLiteDocument document = {0};
    for (unsigned owned = 0; owned < 2u; owned++) {
        snprintf(source, sizeof(source),
            "<script>var ytInitialData = {\"contents\":{\"playlistVideoListRenderer\":"
            "{\"playlistId\":\"%s\",\"isEditable\":%s,\"contents\":[%s]}},"
            "\"metadata\":{\"playlistMetadataRenderer\":{\"title\":\"Mixed list\"}}};</script>",
            owned ? "WL" : "PLfixture01", owned ? "true" : "false", entries);
        CHECK(page_builder_build(&budget, owned
            ? "https://www.youtube.com/playlist?list=WL"
            : "https://www.youtube.com/playlist?list=PLfixture01", source, &document) == 0);
        const char *html = document.html;
        CHECK(document.result_count == 4u && document.playlist != NULL
            && document.playlist->count == 2u
            && strcmp(document.playlist->entries[0].video_id, "PLAYABLE001") == 0
            && strcmp(document.playlist->entries[1].video_id, "PLAYABLE002") == 0);
        CHECK(strstr(html, "<div class=\"result-row unplayable\"><span class=info>"
                "<span class=title>[Private video]</span></span></div>") != NULL
            && strstr(html, "<span class=title>[Deleted video]</span>") != NULL
            && strstr(html, "watch?v=PRIVATE0001") == NULL
            && strstr(html, "vi/PRIVATE0001") == NULL
            && strstr(html, "watch?v=DELETED0001") == NULL
            && strstr(html, "watch?v=PLAYABLE001&amp;tilefinch_queue=all") != NULL
            && strstr(html, "watch?v=PLAYABLE001&amp;tilefinch_queue=0") != NULL
            && strstr(html, "watch?v=PLAYABLE002&amp;tilefinch_queue=1") != NULL);
        const char *remove = strstr(html, "data-yt-target=\"WL\" data-yt-video=\"PRIVATE0001\"");
        CHECK(owned ? remove != NULL
            && strstr(html, "data-yt-target=\"WL\" data-yt-video=\"DELETED0001\"") != NULL
            : strstr(html, "data-yt-video=\"PRIVATE0001\"") == NULL);
        youtube_lite_document_destroy(&document);
    }
    CHECK(budget.current == 0);
    return 0;
}

/* Every link form YouTube serves opens its page, never Home: http, /live/
   and /embed/ watch links, and paths with one trailing slash. */
static int test_youtube_page_builder_link_forms(void)
{
    static const struct {
        const char *url;
        YoutubeLiteRoute route;
    } cases[] = {
        {"http://www.youtube.com/watch?v=TFTEST00001", YOUTUBE_LITE_ROUTE_WATCH},
        {"http://m.youtube.com/shorts/TFTEST00001", YOUTUBE_LITE_ROUTE_WATCH},
        {"http://youtu.be/TFTEST00001", YOUTUBE_LITE_ROUTE_WATCH},
        {"https://youtu.be/TFTEST00001/", YOUTUBE_LITE_ROUTE_WATCH},
        {"https://www.youtube.com/live/TFTEST00001?si=x", YOUTUBE_LITE_ROUTE_WATCH},
        {"https://www.youtube.com/embed/TFTEST00001", YOUTUBE_LITE_ROUTE_WATCH},
        {"https://www.youtube.com/shorts/TFTEST00001/", YOUTUBE_LITE_ROUTE_WATCH},
        {"https://www.youtube.com/results/?search_query=psp", YOUTUBE_LITE_ROUTE_SEARCH},
        {"https://www.youtube.com/feed/history/", YOUTUBE_LITE_ROUTE_HISTORY},
        {"https://www.youtube.com/feed/playlists/", YOUTUBE_LITE_ROUTE_PLAYLISTS},
        {"https://www.youtube.com/playlist/?list=PLfixture01", YOUTUBE_LITE_ROUTE_PLAYLIST},
        {"https://www.youtube.com/@fixture/videos/", YOUTUBE_LITE_ROUTE_CHANNEL},
        {"http://www.youtube.com/results?search_query=psp", YOUTUBE_LITE_ROUTE_SEARCH},
        /* Not a video: still the provider's, never the full application. */
        {"https://www.youtube.com/live/", YOUTUBE_LITE_ROUTE_HOME},
        {"https://www.youtube.com/embed/TFTEST00001/extra", YOUTUBE_LITE_ROUTE_HOME},
        {"https://www.youtube.com/gaming", YOUTUBE_LITE_ROUTE_HOME},
        {"http://notyoutube.com/live/TFTEST00001", YOUTUBE_LITE_ROUTE_NONE},
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        if (youtube_lite_route(cases[i].url) != cases[i].route)
            fprintf(stderr, "route %s: %d\n", cases[i].url,
                    (int) youtube_lite_route(cases[i].url));
        CHECK(youtube_lite_route(cases[i].url) == cases[i].route);
    }

    /* A /live/ link builds the watch page of its video. */
    static const char watch[] =
        "<script>var ytInitialPlayerResponse = {\"videoDetails\":{\"videoId\":"
        "\"TFTEST00001\",\"title\":\"Station view\",\"author\":\"Fixture\","
        "\"lengthSeconds\":\"0\",\"viewCount\":\"5\"}};</script>"
        "<script>var ytInitialData = {\"contents\":{}};</script>";
    Budget budget;
    budget_init(&budget, 4u * MIB);
    YoutubeLiteDocument document = {0};
    CHECK(page_builder_build(&budget,
        "http://www.youtube.com/live/TFTEST00001", watch, &document) == 0);
    CHECK(document.route == YOUTUBE_LITE_ROUTE_WATCH
        && strstr(document.html, "<h1>Station view</h1>") != NULL
        && strstr(document.html, "watch?v=TFTEST00001") != NULL);
    youtube_lite_document_destroy(&document);

    /* A channel tab with a trailing slash selects that tab. */
    static const char channel[] =
        "<script>var ytInitialData = {\"contents\":{},\"metadata\":"
        "{\"channelMetadataRenderer\":{\"title\":\"Fixture\","
        "\"externalId\":\"UCaaaaaaaaaaaaaaaaaaaaaa\"}}};</script>";
    CHECK(page_builder_build(&budget,
        "https://www.youtube.com/@fixture/videos/", channel, &document) == 0);
    CHECK(strstr(document.html, "<a class=\"comments-link on\" aria-current=page "
        "href=\"https://www.youtube.com/@fixture/videos\">") != NULL);
    youtube_lite_document_destroy(&document);
    CHECK(budget.current == 0);
    return 0;
}

/* A query longer than the search box holds is cut on a character boundary,
   and the box, heading and next-page link all show that same text. */
static int test_youtube_page_builder_long_query(void)
{
    /* "a" then 100 x "日" (%E6%97%A5): 301 bytes decoded. */
    char url[1024] = "https://www.youtube.com/results?search_query=a";
    for (unsigned i = 0; i < 100u; i++) strcat(url, "%E6%97%A5");
    char kept[256] = "a";
    for (unsigned i = 0; i < 84u; i++) strcat(kept, "\xe6\x97\xa5");
    static const char source[] =
        "<script>var ytInitialData = {\"contents\":[{\"videoWithContextRenderer\":"
        "{\"videoId\":\"QUERYCARD01\",\"headline\":{\"runs\":[{\"text\":\"Card\"}]}}},"
        "{\"continuationItemRenderer\":{\"continuationEndpoint\":{\"continuationCommand\":"
        "{\"token\":\"NEXTPAGE\"}}}}]};</script>";
    Budget budget;
    budget_init(&budget, 4u * MIB);
    YoutubeLiteDocument document = {0};
    CHECK(page_builder_build(&budget, url, source, &document) == 0);
    char expected[512];
    snprintf(expected, sizeof(expected), "value=\"%s\"", kept);
    CHECK(strstr(document.html, expected) != NULL);
    snprintf(expected, sizeof(expected), "&ldquo;%s&rdquo;", kept);
    CHECK(strstr(document.html, expected) != NULL);
    /* The next page asks for the same, cut query: "a" and 84 characters. */
    char encoded[1024] = "search_query=a";
    for (unsigned i = 0; i < 84u; i++) strcat(encoded, "%E6%97%A5");
    strcat(encoded, "&amp;");
    CHECK(strstr(document.html, encoded) != NULL);
    youtube_lite_document_destroy(&document);
    CHECK(budget.current == 0);
    return 0;
}

/* A lockup with one metadata row (a channel's own tab) shows it as views
   and age, not as the channel; two rows are channel, then views and age. */
static int test_youtube_page_builder_lockup_rows(void)
{
    static const char source[] =
        "<script>var ytInitialData = {\"contents\":["
        "{\"lockupViewModel\":{\"contentId\":\"ONEROWCARD1\","
        "\"contentType\":\"LOCKUP_CONTENT_TYPE_VIDEO\",\"metadata\":{"
        "\"lockupMetadataViewModel\":{\"title\":{\"content\":\"One row\"},"
        "\"metadata\":{\"contentMetadataViewModel\":{\"metadataRows\":["
        "{\"metadataParts\":[{\"text\":{\"content\":\"1.2M views\"}},"
        "{\"text\":{\"content\":\"3 days ago\"}}]}]}}}}}},"
        "{\"lockupViewModel\":{\"contentId\":\"TWOROWCARD1\","
        "\"contentType\":\"LOCKUP_CONTENT_TYPE_VIDEO\",\"metadata\":{"
        "\"lockupMetadataViewModel\":{\"title\":{\"content\":\"Two rows\"},"
        "\"metadata\":{\"contentMetadataViewModel\":{\"metadataRows\":["
        "{\"metadataParts\":[{\"text\":{\"content\":\"Some Channel\"}}]},"
        "{\"metadataParts\":[{\"text\":{\"content\":\"5K views\"}},"
        "{\"text\":{\"content\":\"1 year ago\"}}]}]}}}}}}]};</script>";
    Budget budget;
    budget_init(&budget, 4u * MIB);
    YoutubeLiteDocument document = {0};
    CHECK(page_builder_build(&budget,
        "https://www.youtube.com/results?search_query=rows", source, &document) == 0);
    const char *one = strstr(document.html, "watch?v=ONEROWCARD1");
    const char *two = strstr(document.html, "watch?v=TWOROWCARD1");
    CHECK(one != NULL && two != NULL && one < two);
    const char *views = strstr(one, "1.2M views");
    const char *age = strstr(one, "3 days ago");
    CHECK(views != NULL && views < two && age != NULL && age < two
        && strstr(document.html, "<span class=channel>1.2M views") == NULL);
    const char *channel = strstr(two, "<span class=channel>Some Channel</span>");
    CHECK(channel != NULL && strstr(two, "5K views") != NULL
        && strstr(two, "1 year ago") != NULL);
    youtube_lite_document_destroy(&document);
    CHECK(budget.current == 0);
    return 0;
}

/* YouTube's scheduled line is "Scheduled for DATE_PLACEHOLDER", completed by
   its page script from startTime. The date takes the token's place; without
   a start time the line is dropped, never shown with the token. */
static int test_youtube_page_builder_scheduled_date(void)
{
    static const char source[] =
        "<script>var ytInitialData = {\"contents\":["
        "{\"videoWithContextRenderer\":{\"videoId\":\"SCHEDULED01\","
        "\"headline\":{\"runs\":[{\"text\":\"Launch\"}]},"
        "\"upcomingEventData\":{\"startTime\":\"1793269800\",\"isReminderSet\":false,"
        "\"upcomingEventText\":{\"runs\":[{\"text\":\"Scheduled for \"},"
        "{\"text\":\"DATE_PLACEHOLDER\"}]}}}},"
        "{\"videoWithContextRenderer\":{\"videoId\":\"SCHEDULED02\","
        "\"headline\":{\"runs\":[{\"text\":\"Undated\"}]},"
        "\"upcomingEventData\":{\"upcomingEventText\":{\"runs\":["
        "{\"text\":\"Scheduled for \"},{\"text\":\"DATE_PLACEHOLDER\"}]}}}}]};</script>";
    Budget budget;
    budget_init(&budget, 4u * MIB);
    YoutubeLiteDocument document = {0};
    CHECK(page_builder_build(&budget,
        "https://www.youtube.com/results?search_query=launch", source, &document) == 0);
    const char *second = strstr(document.html, "watch?v=SCHEDULED02");
    const char *date = strstr(document.html, "Scheduled for 2026/10/29");
    CHECK(second != NULL && date != NULL && date < second
        && strstr(document.html, "DATE_PLACEHOLDER") == NULL
        && strstr(second, "Scheduled for") == NULL);
    youtube_lite_document_destroy(&document);
    CHECK(budget.current == 0);
    return 0;
}

/* A next page that cannot be fetched says so and offers the same link again.
   Before, page one was built under the next page's link, whose "Load more"
   led back to the same page. */
static int test_youtube_page_builder_failed_next_page(void)
{
    static const char page[] =
        "<script>var ytInitialData = {\"contents\":[{\"videoWithContextRenderer\":"
        "{\"videoId\":\"FIRSTPAGE01\",\"headline\":{\"runs\":[{\"text\":\"Page one\"}]}}},"
        "{\"continuationItemRenderer\":{\"continuationEndpoint\":{\"continuationCommand\":"
        "{\"token\":\"NEXTPAGE\"}}}}]};</script>";
    static const char url[] =
        "https://www.youtube.com/results?search_query=psp&tilefinch_token=NEXTPAGE";
    Budget budget;
    budget_init(&budget, 8u * MIB);
    YoutubeLiteDocument document = {0};
    CHECK(page_data_load(&budget, url, "https://m.youtube.com/results?search_query=psp",
        page, sizeof(page) - 1u, &document) == 0);
    CHECK(strstr(document.html, "<p class=empty>More results could not load.</p>") != NULL
        && strstr(document.html, "id=yt-continuation-retry autofocus class=comments-link "
            "href=\"https://www.youtube.com/results?search_query=psp&amp;tilefinch_token=NEXTPAGE\">"
            "Try again</a>") != NULL
        && strstr(document.html, "FIRSTPAGE01") == NULL
        && strstr(document.html, "value=\"psp\"") != NULL);
    youtube_lite_document_destroy(&document);
    CHECK(budget.current == 0);
    return 0;
}

/* Signed out, a watch page has one Sign in, the header's. Empty states use
   plain wording. */
static int test_youtube_page_builder_plain_pages(void)
{
    static const char watch[] =
        "<script>var ytInitialPlayerResponse = {\"videoDetails\":{\"videoId\":"
        "\"TFTEST00001\",\"title\":\"Plain\",\"author\":\"Fixture\","
        "\"channelId\":\"UCaaaaaaaaaaaaaaaaaaaaaa\",\"lengthSeconds\":\"60\"}};</script>"
        "<script>var ytInitialData = {\"contents\":{}};</script>";
    Budget budget;
    budget_init(&budget, 4u * MIB);
    YoutubeLiteDocument document = {0};
    CHECK(page_builder_build(&budget,
        "https://www.youtube.com/watch?v=TFTEST00001", watch, &document) == 0);
    const char *first = strstr(document.html, ">Sign in</a>");
    CHECK(first != NULL && strstr(first + 1, ">Sign in</a>") == NULL
        && strstr(document.html, "id=yt-account-action") != NULL
        && strstr(document.html, "yt-watch-signin") == NULL);
    youtube_lite_document_destroy(&document);
    static const char empty[] = "<script>var ytInitialData = {\"contents\":[]};</script>";
    CHECK(page_builder_build(&budget,
        "https://www.youtube.com/results?search_query=nothing", empty, &document) == 0);
    CHECK(strstr(document.html, "<p class=empty>No videos found. Try another search.</p>")
        && !strstr(document.html, "bounded"));
    youtube_lite_document_destroy(&document);
    CHECK(budget.current == 0);
    return 0;
}

/* Signed in, Home's Refresh suggestions sits under the status line, below
   the hero box. Its corner place, beside a feed's heading, overlapped the
   box. */
static int test_youtube_page_builder_refresh_layout(void)
{
    Budget budget;
    budget_init(&budget, 4u * MIB);
    YoutubeLiteDocument document = {0};
    CHECK(youtube_lite_test_build_signed_in(
        &budget, "https://www.youtube.com/", NULL, 0, &document));
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
        "https://www.youtube.com/", document.html, document.html_length, true));
    CHECK(browser_engine_render_frame(engine, NULL));
    NavigationSession *nav = browser_engine_navigation(engine);
    lxb_dom_node_t *root = lxb_dom_interface_node(nav->page.document.html);
    lxb_dom_node_t *status = test_reader_find_id(root, "yt-feed-status");
    lxb_dom_node_t *refresh = test_reader_find_id(root, "yt-feed-refresh");
    CHECK(status != NULL && refresh != NULL && status->prev != NULL);
    /* The hero section directly precedes the status line. */
    const LayoutNodeBox *hero = layout_box_for_node(&nav->page.layout, status->prev);
    const LayoutNodeBox *line = layout_box_for_node(&nav->page.layout, status);
    const LayoutNodeBox *link = layout_box_for_node(&nav->page.layout, refresh);
    CHECK(hero != NULL && line != NULL && link != NULL
        && link->y >= hero->y + hero->height && link->y >= line->y + line->height);
    /* The focused search box draws the page's own 3 px white ring, like
       the cards, instead of the browser's thin native one. */
    CHECK(strstr(document.html, "input:focus,button:focus{outline:3px solid #fff;outline-offset:1px}")
        && strstr(document.html, ".card:focus,.details:focus{outline:3px solid #fff;outline-offset:1px}"));
    CHECK(browser_engine_shutdown(engine));
    youtube_lite_document_destroy(&document);
    CHECK(budget.current == 0);
    return 0;
}

/* Home says what to do first; Sign out sits in the Account panel and opens
   a local page that asks, with Cancel focused. Only that page carries the
   control bound to the sign-out command. Signed out, the question is never
   asked and Home is shown instead. */
static int test_youtube_page_builder_signout_confirm(void)
{
    Budget budget;
    budget_init(&budget, 4u * MIB);
    YoutubeLiteDocument document = {0};
    static const char empty[] = "<!doctype html>";
    const char *confirm = YOUTUBE_LITE_SIGNOUT_CONFIRM_URL;
    CHECK(youtube_lite_signout_confirmation(confirm)
        && !youtube_lite_signout_confirmation("https://www.youtube.com/")
        && !youtube_lite_signout_confirmation("https://www.youtube.com/results?tilefinch_signout=1")
        && !youtube_lite_navigation_requires_network(confirm));
    for (unsigned lane = 0; lane < 2u; lane++) {
        const char *url = lane ? confirm : "https://www.youtube.com/";
        CHECK(page_builder_build(&budget, url, empty, &document) == 0);
        /* Signed out, Home is the header and its focused search box. */
        CHECK(!strstr(document.html, "class=hero")
            && !strstr(document.html, "Watch YouTube on this device")
            && strstr(document.html, " autofocus>")
            && !strstr(document.html, "yt-signout"));
        youtube_lite_document_destroy(&document);
    }
    CHECK(youtube_lite_test_build_signed_in(&budget, "https://www.youtube.com/", NULL, 0, &document));
    const char *panel = strstr(document.html, "id=yt-account-panel");
    const char *open = strstr(document.html, "<a id=yt-signout-open href=\"" YOUTUBE_LITE_SIGNOUT_CONFIRM_URL "\">Sign out</a>");
    CHECK(panel && open && panel < open && open < strstr(document.html, "</details>")
        && !strstr(document.html, "yt-signout-action"));
    /* Signed in, Home keeps its heading while suggestions load. */
    CHECK(strstr(document.html, "<section class=hero><h1>Watch YouTube on this device</h1></section>"));
    youtube_lite_document_destroy(&document);
    CHECK(youtube_lite_test_build_signed_in(&budget, confirm, NULL, 0, &document));
    CHECK(strstr(document.html, "<h1>Sign out of YouTube and your Google account on this PSP?</h1>")
        && strstr(document.html, "<a id=yt-signout-cancel autofocus class=comments-link href=\"https://www.youtube.com/\">Cancel</a>")
        && strstr(document.html, "<a id=yt-signout-action class=comments-link href=\"" YOUTUBE_LITE_FRAGMENT_SIGNOUT "\">Sign out</a>")
        && !strstr(document.html, "\" autofocus>")
        && !strstr(document.html, "yt-feed-status")
        && !strstr(document.html, "Watch YouTube on this device"));
    youtube_lite_document_destroy(&document);
    CHECK(budget.current == 0);
    return 0;
}

static int test_youtube_page_builder(void)
{
    return test_youtube_page_builder_long_strings()
        || test_youtube_page_builder_unplayable_entries()
        || test_youtube_page_builder_link_forms()
        || test_youtube_page_builder_long_query()
        || test_youtube_page_builder_lockup_rows()
        || test_youtube_page_builder_scheduled_date()
        || test_youtube_page_builder_failed_next_page()
        || test_youtube_page_builder_refresh_layout()
        || test_youtube_page_builder_signout_confirm()
        || test_youtube_page_builder_plain_pages();
}
