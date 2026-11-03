/* Channel pages: curated Home shelves, header facts, playlist cards, live
   and upcoming labels, the subscribe control, tabs, and the Videos tab's
   next page. Synthetic, minimized provider data written for these tests. */
static int youtube_channel_build(const char *url, const char *source,
    size_t length, const char *next, size_t next_length, bool signed_in,
    Budget *budget, YoutubeLiteDocument *document)
{
    char error[256] = {0};
    bool built = signed_in
        ? youtube_lite_test_build_signed_in(budget, url, source, length, document)
        : youtube_lite_build_document_with_comments(budget, url, source, length,
              next, next_length, document, error, sizeof(error));
    if (!built) fprintf(stderr, "youtube channel %s: %s\n", url, error);
    return built ? 0 : 1;
}

static const char *youtube_channel_after(const char *html, const char *first,
                                         const char *second)
{
    const char *a = strstr(html, first);
    const char *b = a == NULL ? NULL : strstr(a, second);
    return b;
}

static int test_youtube_lite_channel_pages(void)
{
    /* The metadata object's name also appears earlier as a string value. */
    static const char home[] =
        "<script>var ytInitialData = '{\"responseContext\":{\"serviceTrackingParam"
        "s\":[{\"params\":[{\"key\":\"browse\",\"value\":\"channelMetadataRenderer\"}]}]},"
        "\"contents\":{\"singleColumnBrowseResultsRenderer\":{\"tabs\":[{\"tabRenderer"
        "\":{\"selected\":true,\"content\":{\"sectionListRenderer\":{\"contents\":[{\"ite"
        "mSectionRenderer\":{\"contents\":[{\"channelFeaturedVideoRenderer\":{\"video"
        "Id\":\"FEATURE0001\",\"title\":{\"runs\":[{\"text\":\"Featured trailer\"}]},\"leng"
        "thText\":{\"runs\":[{\"text\":\"0:00\"}]}}}]}},{\"shelfRenderer\":{\"title\":{\"ru"
        "ns\":[{\"text\":\"Coming up\",\"navigationEndpoint\":{\"commandMetadata\":{\"web"
        "CommandMetadata\":{\"url\":\"/@fixture/videos?view=2&live_view=502\"}}}}]},"
        "\"content\":{\"verticalListRenderer\":{\"items\":[{\"compactVideoRenderer\":{\""
        "videoId\":\"UPCOMING001\",\"title\":{\"runs\":[{\"text\":\"Launch soon\"}]},\"shor"
        "tViewCountText\":{\"runs\":[{\"text\":\"15\"},{\"text\":\" waiting\"}]},\"upcoming"
        "EventData\":{\"upcomingEventText\":{\"runs\":[{\"text\":\"Scheduled for \"},{\"t"
        "ext\":\"10/29\"}]}},\"thumbnailOverlays\":[{\"thumbnailOverlayTimeStatusRend"
        "erer\":{\"text\":{\"runs\":[{\"text\":\"Upcoming\"}]},\"style\":\"UPCOMING\"}}]}},{"
        "\"compactVideoRenderer\":{\"videoId\":\"LIVENOW0001\",\"title\":{\"runs\":[{\"tex"
        "t\":\"Station view\"}]},\"shortViewCountText\":{\"runs\":[{\"text\":\"98\"},{\"tex"
        "t\":\" watching\"}]},\"thumbnailOverlays\":[{\"thumbnailOverlayTimeStatusRen"
        "derer\":{\"text\":{\"runs\":[{\"text\":\"LIVE\"}]},\"style\":\"LIVE\"}}]}}]}}}},{\"s"
        "helfRenderer\":{\"title\":{\"runs\":[{\"text\":\"Mission & more\",\"navigationEn"
        "dpoint\":{\"commandMetadata\":{\"webCommandMetadata\":{\"url\":\"/playlist?lis"
        "t=PLfixture01&index=2\"}}}}]},\"content\":{\"verticalListRenderer\":{\"items"
        "\":[{\"compactVideoRenderer\":{\"videoId\":\"VIDEO000001\",\"title\":{\"runs\":[{"
        "\"text\":\"Mission part 1\"}]},\"lengthText\":{\"simpleText\":\"1:01\"}}},{\"comp"
        "actVideoRenderer\":{\"videoId\":\"VIDEO000002\",\"title\":{\"runs\":[{\"text\":\"M"
        "ission part 2\"}]},\"lengthText\":{\"simpleText\":\"1:02\"}}},{\"compactVideoR"
        "enderer\":{\"videoId\":\"VIDEO000003\",\"title\":{\"runs\":[{\"text\":\"Mission pa"
        "rt 3\"}]},\"lengthText\":{\"simpleText\":\"1:03\"}}},{\"compactVideoRenderer\":"
        "{\"videoId\":\"VIDEO000004\",\"title\":{\"runs\":[{\"text\":\"Mission part 4\"}]},"
        "\"lengthText\":{\"simpleText\":\"1:04\"}}},{\"compactVideoRenderer\":{\"videoId"
        "\":\"VIDEO000005\",\"title\":{\"runs\":[{\"text\":\"Mission part 5\"}]},\"lengthTe"
        "xt\":{\"simpleText\":\"1:05\"}}},{\"compactVideoRenderer\":{\"videoId\":\"VIDEO0"
        "00006\",\"title\":{\"runs\":[{\"text\":\"Mission part 6\"}]},\"lengthText\":{\"sim"
        "pleText\":\"1:06\"}}}]}}}},{\"shelfRenderer\":{\"title\":{\"runs\":[{\"text\":\"Th"
        "eir playlists\"}]},\"content\":{\"verticalListRenderer\":{\"items\":[{\"compac"
        "tPlaylistRenderer\":{\"playlistId\":\"PLfixture02\",\"title\":{\"runs\":[{\"text"
        "\":\"Walks\"}]},\"thumbnail\":{\"thumbnails\":[{\"url\":\"https://i.ytimg.com/vi"
        "/THUMB000001/default.jpg\"}]},\"videoCountText\":{\"runs\":[{\"text\":\"18\"},{"
        "\"text\":\" videos\"}]}}},{\"lockupViewModel\":{\"contentType\":\"LOCKUP_CONTEN"
        "T_TYPE_PLAYLIST\",\"contentId\":\"PLfixture03\",\"metadata\":{\"lockupMetadata"
        "ViewModel\":{\"title\":{\"content\":\"Lockup list\"}}}}}]}}}},{\"shelfRenderer"
        "\":{\"title\":{\"runs\":[{\"text\":\"Friends\",\"navigationEndpoint\":{\"showEngag"
        "ementPanelEndpoint\":{\"panel\":{\"url\":\"/@other\"}}}}]},\"content\":{\"vertic"
        "alListRenderer\":{\"items\":[{\"compactChannelRenderer\":{\"channelId\":\"UCzz"
        "zzzzzzzzzzzzzzzzzzzz\"}}]}}}}]}}}}]}},\"header\":{\"pageHeaderRenderer\":{\""
        "content\":{\"pageHeaderViewModel\":{\"image\":{\"decoratedAvatarViewModel\":{"
        "\"avatar\":{\"avatarViewModel\":{\"image\":{\"sources\":[{\"url\":\"https://yt3.g"
        "oogleusercontent.com/avatar=s72\",\"width\":72},{\"url\":\"https://yt3.googl"
        "eusercontent.com/avatar=s160\",\"width\":160}]}}}}},\"metadata\":{\"contentM"
        "etadataViewModel\":{\"metadataRows\":[{\"metadataParts\":[{\"text\":{\"content"
        "\":\"@fixture\"}}]},{\"metadataParts\":[{\"text\":{\"content\":\"1.5K subscriber"
        "s\"}},{\"text\":{\"content\":\"42 videos\"}}]}]}},\"banner\":{\"imageBannerViewM"
        "odel\":{\"image\":{\"sources\":[{\"url\":\"https://yt3.googleusercontent.com/b"
        "anner=w320\",\"width\":320},{\"url\":\"https://yt3.googleusercontent.com/ban"
        "ner=w2120\",\"width\":2120},{\"url\":\"https://yt3.googleusercontent.com/ban"
        "ner=w1060\",\"width\":1060},{\"url\":\"https://example.com/banner=w600\",\"wid"
        "th\":600}]}}}}}}},\"metadata\":{\"channelMetadataRenderer\":{\"title\":\"Fixtu"
        "re <Channel>\",\"externalId\":\"UCabcdefghijklmnopqrstuv\",\"description\":\"L"
        "ine one.\\\\nLine two.\"}}}';</script>";
    static const char signed_home[] =
        "<script>var ytInitialData = '{\"responseContext\":{\"serviceTrackingParam"
        "s\":[{\"params\":[{\"key\":\"browse\",\"value\":\"channelMetadataRenderer\"}]}]},"
        "\"contents\":{\"singleColumnBrowseResultsRenderer\":{\"tabs\":[{\"tabRenderer"
        "\":{\"selected\":true,\"content\":{\"sectionListRenderer\":{\"contents\":[{\"ite"
        "mSectionRenderer\":{\"contents\":[{\"channelFeaturedVideoRenderer\":{\"video"
        "Id\":\"FEATURE0001\",\"title\":{\"runs\":[{\"text\":\"Featured trailer\"}]},\"leng"
        "thText\":{\"runs\":[{\"text\":\"0:00\"}]}}}]}},{\"shelfRenderer\":{\"title\":{\"ru"
        "ns\":[{\"text\":\"Coming up\",\"navigationEndpoint\":{\"commandMetadata\":{\"web"
        "CommandMetadata\":{\"url\":\"/@fixture/videos?view=2&live_view=502\"}}}}]},"
        "\"content\":{\"verticalListRenderer\":{\"items\":[{\"compactVideoRenderer\":{\""
        "videoId\":\"UPCOMING001\",\"title\":{\"runs\":[{\"text\":\"Launch soon\"}]},\"shor"
        "tViewCountText\":{\"runs\":[{\"text\":\"15\"},{\"text\":\" waiting\"}]},\"upcoming"
        "EventData\":{\"upcomingEventText\":{\"runs\":[{\"text\":\"Scheduled for \"},{\"t"
        "ext\":\"10/29\"}]}},\"thumbnailOverlays\":[{\"thumbnailOverlayTimeStatusRend"
        "erer\":{\"text\":{\"runs\":[{\"text\":\"Upcoming\"}]},\"style\":\"UPCOMING\"}}]}},{"
        "\"compactVideoRenderer\":{\"videoId\":\"LIVENOW0001\",\"title\":{\"runs\":[{\"tex"
        "t\":\"Station view\"}]},\"shortViewCountText\":{\"runs\":[{\"text\":\"98\"},{\"tex"
        "t\":\" watching\"}]},\"thumbnailOverlays\":[{\"thumbnailOverlayTimeStatusRen"
        "derer\":{\"text\":{\"runs\":[{\"text\":\"LIVE\"}]},\"style\":\"LIVE\"}}]}}]}}}},{\"s"
        "helfRenderer\":{\"title\":{\"runs\":[{\"text\":\"Mission & more\",\"navigationEn"
        "dpoint\":{\"commandMetadata\":{\"webCommandMetadata\":{\"url\":\"/playlist?lis"
        "t=PLfixture01&index=2\"}}}}]},\"content\":{\"verticalListRenderer\":{\"items"
        "\":[{\"compactVideoRenderer\":{\"videoId\":\"VIDEO000001\",\"title\":{\"runs\":[{"
        "\"text\":\"Mission part 1\"}]},\"lengthText\":{\"simpleText\":\"1:01\"}}},{\"comp"
        "actVideoRenderer\":{\"videoId\":\"VIDEO000002\",\"title\":{\"runs\":[{\"text\":\"M"
        "ission part 2\"}]},\"lengthText\":{\"simpleText\":\"1:02\"}}},{\"compactVideoR"
        "enderer\":{\"videoId\":\"VIDEO000003\",\"title\":{\"runs\":[{\"text\":\"Mission pa"
        "rt 3\"}]},\"lengthText\":{\"simpleText\":\"1:03\"}}},{\"compactVideoRenderer\":"
        "{\"videoId\":\"VIDEO000004\",\"title\":{\"runs\":[{\"text\":\"Mission part 4\"}]},"
        "\"lengthText\":{\"simpleText\":\"1:04\"}}},{\"compactVideoRenderer\":{\"videoId"
        "\":\"VIDEO000005\",\"title\":{\"runs\":[{\"text\":\"Mission part 5\"}]},\"lengthTe"
        "xt\":{\"simpleText\":\"1:05\"}}},{\"compactVideoRenderer\":{\"videoId\":\"VIDEO0"
        "00006\",\"title\":{\"runs\":[{\"text\":\"Mission part 6\"}]},\"lengthText\":{\"sim"
        "pleText\":\"1:06\"}}}]}}}},{\"shelfRenderer\":{\"title\":{\"runs\":[{\"text\":\"Th"
        "eir playlists\"}]},\"content\":{\"verticalListRenderer\":{\"items\":[{\"compac"
        "tPlaylistRenderer\":{\"playlistId\":\"PLfixture02\",\"title\":{\"runs\":[{\"text"
        "\":\"Walks\"}]},\"thumbnail\":{\"thumbnails\":[{\"url\":\"https://i.ytimg.com/vi"
        "/THUMB000001/default.jpg\"}]},\"videoCountText\":{\"runs\":[{\"text\":\"18\"},{"
        "\"text\":\" videos\"}]}}},{\"lockupViewModel\":{\"contentType\":\"LOCKUP_CONTEN"
        "T_TYPE_PLAYLIST\",\"contentId\":\"PLfixture03\",\"metadata\":{\"lockupMetadata"
        "ViewModel\":{\"title\":{\"content\":\"Lockup list\"}}}}}]}}}},{\"shelfRenderer"
        "\":{\"title\":{\"runs\":[{\"text\":\"Friends\",\"navigationEndpoint\":{\"showEngag"
        "ementPanelEndpoint\":{\"panel\":{\"url\":\"/@other\"}}}}]},\"content\":{\"vertic"
        "alListRenderer\":{\"items\":[{\"compactChannelRenderer\":{\"channelId\":\"UCzz"
        "zzzzzzzzzzzzzzzzzzzz\"}}]}}}}]}}}}]}},\"header\":{\"pageHeaderRenderer\":{\""
        "content\":{\"pageHeaderViewModel\":{\"image\":{\"decoratedAvatarViewModel\":{"
        "\"avatar\":{\"avatarViewModel\":{\"image\":{\"sources\":[{\"url\":\"https://yt3.g"
        "oogleusercontent.com/avatar=s72\",\"width\":72},{\"url\":\"https://yt3.googl"
        "eusercontent.com/avatar=s160\",\"width\":160}]}}}}},\"metadata\":{\"contentM"
        "etadataViewModel\":{\"metadataRows\":[{\"metadataParts\":[{\"text\":{\"content"
        "\":\"@fixture\"}}]},{\"metadataParts\":[{\"text\":{\"content\":\"1.5K subscriber"
        "s\"}},{\"text\":{\"content\":\"42 videos\"}}]}]}},\"banner\":{\"imageBannerViewM"
        "odel\":{\"image\":{\"sources\":[{\"url\":\"https://yt3.googleusercontent.com/b"
        "anner=w320\",\"width\":320},{\"url\":\"https://yt3.googleusercontent.com/ban"
        "ner=w2120\",\"width\":2120},{\"url\":\"https://yt3.googleusercontent.com/ban"
        "ner=w1060\",\"width\":1060},{\"url\":\"https://example.com/banner=w600\",\"wid"
        "th\":600}]}}},\"actions\":{\"subscribeButtonViewModel\":{\"subscribeButtonCo"
        "ntent\":{\"subscribed\":true}}}}}}},\"metadata\":{\"channelMetadataRenderer\""
        ":{\"title\":\"Fixture <Channel>\",\"externalId\":\"UCabcdefghijklmnopqrstuv\","
        "\"description\":\"Line one.\\\\nLine two.\"}}}';</script>";
    static const char videos[] =
        "<script>var ytInitialData = '{\"contents\":{\"singleColumnBrowseResultsRe"
        "nderer\":{\"tabs\":[{\"tabRenderer\":{\"selected\":true,\"content\":{\"richGridR"
        "enderer\":{\"contents\":[{\"richItemRenderer\":{\"content\":{\"compactVideoRen"
        "derer\":{\"videoId\":\"LATEST00001\",\"title\":{\"runs\":[{\"text\":\"Newest uploa"
        "d\"}]},\"publishedTimeText\":{\"runs\":[{\"text\":\"1 hour ago\"}]}}}}},{\"richI"
        "temRenderer\":{\"content\":{\"compactVideoRenderer\":{\"videoId\":\"LATEST0000"
        "2\",\"title\":{\"runs\":[{\"text\":\"Older upload\"}]}}}}},{\"continuationItemRe"
        "nderer\":{\"continuationEndpoint\":{\"continuationCommand\":{\"token\":\"NEXT+"
        "/=AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
        "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
        "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
        "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
        "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
        "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
        "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
        "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
        "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
        "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
        "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
        "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
        "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
        "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
        "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
        "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
        "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
        "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
        "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
        "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
        "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
        "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
        "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA\"}}}}]}}"
        "}}]}},\"header\":{\"pageHeaderRenderer\":{\"content\":{\"pageHeaderViewModel\""
        ":{\"description\":{\"descriptionPreviewViewModel\":{\"onTap\":{\"engagementPa"
        "nel\":{\"contents\":[{\"continuationItemRenderer\":{\"continuationEndpoint\":"
        "{\"continuationCommand\":{\"token\":\"ABOUT\"}}}}]}}}}}}}},\"metadata\":{\"chan"
        "nelMetadataRenderer\":{\"title\":\"Fixture <Channel>\",\"externalId\":\"UCabcd"
        "efghijklmnopqrstuv\",\"description\":\"Line one.\\\\nLine two.\"}}}';</script"
        ">";
    static const char page2[] =
        "{\"onResponseReceivedActions\":[{\"appendContinuationItemsAction\":{\"conti"
        "nuationItems\":[{\"richItemRenderer\":{\"content\":{\"compactVideoRenderer\":"
        "{\"videoId\":\"PAGE2000001\",\"title\":{\"runs\":[{\"text\":\"Second page\"}]}}}}}"
        ",{\"continuationItemRenderer\":{\"continuationEndpoint\":{\"continuationCom"
        "mand\":{\"token\":\"THIRD\"}}}}]}}]}";
    static const char shorts[] =
        "<script>var ytInitialData = '{\"contents\":{\"richGridRenderer\":{\"content"
        "s\":[{\"richItemRenderer\":{\"content\":{\"shortsLockupViewModel\":{\"accessib"
        "ilityText\":\"Label clip, again, 2 thousand views - play Short\",\"onTap\":"
        "{\"innertubeCommand\":{\"reelWatchEndpoint\":{\"videoId\":\"SHORT000001\"}}},\""
        "overlayMetadata\":{\"primaryText\":{\"content\":\"Tiny clip\"},\"secondaryText"
        "\":{\"content\":\"5K views\"}}}}}},{\"richItemRenderer\":{\"content\":{\"shortsL"
        "ockupViewModel\":{\"accessibilityText\":\"Label clip, again, 2 thousand vi"
        "ews - play Short\",\"onTap\":{\"innertubeCommand\":{\"reelWatchEndpoint\":{\"v"
        "ideoId\":\"SHORT000002\"}}},\"overlayMetadata\":{\"secondaryText\":{\"content\""
        ":\"2K views\"}}}}}}]}},\"metadata\":{\"channelMetadataRenderer\":{\"title\":\"F"
        "ixture <Channel>\",\"externalId\":\"UCabcdefghijklmnopqrstuv\",\"description"
        "\":\"Line one.\\\\nLine two.\"}}}';</script>";
    static const char playlists[] =
        "<script>var ytInitialData = '{\"contents\":{\"sectionListRenderer\":{\"cont"
        "ents\":[{\"itemSectionRenderer\":{\"contents\":[{\"compactPlaylistRenderer\":"
        "{\"playlistId\":\"PLfixture04\",\"title\":{\"runs\":[{\"text\":\"Tab list\"}]},\"vi"
        "deoCountText\":{\"runs\":[{\"text\":\"3\"},{\"text\":\" videos\"}]}}}]}}]}},\"meta"
        "data\":{\"channelMetadataRenderer\":{\"title\":\"Fixture <Channel>\",\"externa"
        "lId\":\"UCabcdefghijklmnopqrstuv\",\"description\":\"Line one.\\\\nLine two.\"}"
        "}}';</script>";
    /* An About panel response: one link through YouTube's redirect, a
       YouTube path, an unsafe target, and more links than are shown. */
#define CHANNEL_ABOUT_LINK(title, url) \
        "{\"channelExternalLinkViewModel\":{\"title\":{\"content\":\"" title "\"},\"link\":{\"content\":\"" \
        title "\",\"commandRuns\":[{\"onTap\":{\"innertubeCommand\":{\"urlEndpoint\":{\"url\":\"" url "\"}}}}]}}},"
    static const char about[] =
        "{\"responseContext\":{\"webResponseContextExtensionData\":{\"hasDecorated\":true},"
        "\"rendererNames\":[\"aboutChannelViewModel\"]},\"onResponseReceivedEndpoints\":[{\"appendContin"
        "uationItemsAction\":{\"continuationItems\":[{\"aboutChannelRenderer\":{\"metadata\":{\"aboutChann"
        "elViewModel\":{\"description\":\"First <line>.\\n\\nSecond line that is longer than the header sh"
        "ows.\",\"subscriberCountText\":\"1.5K subscribers\",\"viewCountText\":\"1,234 views\",\"joinedDate"
        "Text\":{\"content\":\"Joined Jan 2, 2020\"},\"canonicalChannelUrl\":\"http://www.youtube.com/@fixt"
        "ure\",\"channelId\":\"UCabcdefghijklmnopqrstuv\",\"videoCountText\":\"42 videos\",\"country\":\"Icel"
        "and\",\"links\":["
        "{\"channelExternalLinkViewModel\":{\"title\":{\"content\":\"Site\"},\"link\":{\"content\":\"example.c"
        "om/fixture\",\"commandRuns\":[{\"onTap\":{\"innertubeCommand\":{\"commandMetadata\":{\"webCommandMe"
        "tadata\":{\"url\":\"https:\\/\\/www.youtube.com\\/redirect?event=channel_description\\u0026q=https"
        "%3A%2F%2Fwww.example.com%2Ffixture%3Fa%3D1%26b%3D2\"}}}}}]}}},"
        CHANNEL_ABOUT_LINK("friend", "/@friend")
        CHANNEL_ABOUT_LINK("Unsafe", "javascript:alert(1)")
        CHANNEL_ABOUT_LINK("three", "https://three.example/")
        CHANNEL_ABOUT_LINK("four", "https://four.example/")
        CHANNEL_ABOUT_LINK("five", "https://five.example/")
        CHANNEL_ABOUT_LINK("six", "https://six.example/")
        CHANNEL_ABOUT_LINK("seven", "https://seven.example/")
        CHANNEL_ABOUT_LINK("eight", "https://eight.example/")
        CHANNEL_ABOUT_LINK("nine", "https://nine.example/")
        "{\"channelExternalLinkViewModel\":{}}]}}}}]}}]}";
#undef CHANNEL_ABOUT_LINK
    Budget budget;
    budget_init(&budget, 4 * MIB);
    YoutubeLiteDocument document = {0};
    const char *html;

    /* Home: header, then the curated shelves in the channel's order. */
    CHECK(youtube_channel_build("https://www.youtube.com/@fixture", home,
        sizeof(home) - 1u, NULL, 0, false, &budget, &document) == 0);
    html = document.html;
    CHECK(strstr(html, "<title>Fixture &lt;Channel&gt;</title>") != NULL);
    CHECK(strstr(html, "<h1>Fixture &lt;Channel&gt;</h1>") != NULL);
    CHECK(strstr(html, "1.5K subscribers \xc2\xb7 42 videos") != NULL);
    CHECK(strstr(html, "@fixture</p>") == NULL);
    CHECK(strstr(html, "src=\"https://yt3.googleusercontent.com/avatar=s72\"") != NULL);
    /* The smallest banner at least the screen's width, above the name. */
    CHECK(youtube_channel_after(html, "channel-banner src=\"https://yt3.googleusercontent.com/banner=w1060\"",
                                "<h1>") != NULL);
    CHECK(strstr(html, "Line one. Line two.") != NULL);
    CHECK(strstr(html, "Channel videos") == NULL);
    /* Signed out there is no subscribe or sign-in control on the page. */
    CHECK(strstr(html, "yt-subscribe-action") == NULL);
    CHECK(strstr(html, "yt-unsubscribe-action") == NULL);
    CHECK(strstr(html, "data-yt-action") == NULL);
    CHECK(strstr(html, "channel-actions") == NULL);
    CHECK(strstr(html, "yt-library-status") == NULL);
    /* The description line opens the full description. Without the About
       panel's continuation, that view reads the channel page again. */
    CHECK(youtube_channel_after(html, "</h1>",
        "<a id=yt-channel-about class=channel-about title=\"View full description\" "
        "href=\"https://www.youtube.com/@fixture?tilefinch_view=description#description\">"
        "Line one. Line two.</a>") != NULL);
    CHECK(youtube_channel_after(html, "id=yt-channel-about", "<nav class=channel-tabs>") != NULL);
    CHECK(strstr(html, "<a class=\"comments-link on\" aria-current=page href=\"https://www.youtube.com/@fixture\">Home</a>") != NULL);
    CHECK(strstr(html, "href=\"https://www.youtube.com/@fixture/videos\">Videos</a>") != NULL);
    CHECK(strstr(html, "href=\"https://www.youtube.com/@fixture/shorts\">Shorts</a>") != NULL);
    CHECK(strstr(html, "href=\"https://www.youtube.com/@fixture/playlists\">Playlists</a>") != NULL);
    CHECK(youtube_channel_after(html, "watch?v=FEATURE0001", "<h2>Coming up</h2>") != NULL);
    /* A featured live stream's stated 0:00 length is not shown. */
    CHECK(strstr(html, "0:00") == NULL);
    CHECK(youtube_channel_after(html, "<h2>Coming up</h2>", "<h2>Mission &amp; more</h2>") != NULL);
    CHECK(youtube_channel_after(html, "<h2>Mission &amp; more</h2>", "<h2>Their playlists</h2>") != NULL);
    /* Live and upcoming entries are labelled from the page's own text, not
       with a length; a live label is the shared live badge. */
    CHECK(youtube_channel_after(html, "watch?v=UPCOMING001", "<span class=duration>Upcoming</span>") != NULL);
    CHECK(strstr(html, "15 waiting &middot; Scheduled for 10/29") != NULL);
    CHECK(youtube_channel_after(html, "watch?v=LIVENOW0001", "<span class=\"duration live\">LIVE</span>") != NULL);
    CHECK(strstr(html, "98 watching") != NULL);
    /* See all: a channel tab, and a playlist, each named after its shelf;
       a shelf's entries are bounded. */
    CHECK(youtube_channel_after(html, "watch?v=LIVENOW0001",
        "<a class=\"more see-all\" href=\"https://www.youtube.com/@fixture/videos\">See all: Coming up</a>") != NULL);
    CHECK(strstr(html, "watch?v=VIDEO000004") != NULL);
    CHECK(strstr(html, "VIDEO000005") == NULL);
    CHECK(youtube_channel_after(html, "watch?v=VIDEO000004",
        "<a class=\"more see-all\" href=\"https://www.youtube.com/playlist?list=PLfixture01\">See all: Mission &amp; more</a>") != NULL);
    /* Playlists are playlist cards, never video cards. */
    CHECK(strstr(html, "<a class=card href=\"https://www.youtube.com/playlist?list=PLfixture02\">") != NULL);
    CHECK(youtube_channel_after(html, "list=PLfixture02", "i.ytimg.com/vi/THUMB000001/mqdefault.jpg") != NULL);
    CHECK(youtube_channel_after(html, "list=PLfixture02", "<span class=duration>Playlist</span>") != NULL);
    CHECK(youtube_channel_after(html, "list=PLfixture02", "<span class=meta>18 videos</span>") != NULL);
    CHECK(youtube_channel_after(html, "list=PLfixture03", "Lockup list") != NULL);
    CHECK(strstr(html, "watch?v=PLfixture") == NULL);
    CHECK(strstr(html, "Their playlists</h2><div") != NULL);
    /* A shelf with no entry this page can show has no heading; its panel
       link is not a See all target. Home has no next-page link. */
    CHECK(strstr(html, "Friends") == NULL && strstr(html, "@other") == NULL);
    CHECK(strstr(html, "tilefinch_token") == NULL);
    CHECK(document.result_count == 9u);
    youtube_lite_document_destroy(&document);
    CHECK(budget.current == 0);

    /* Channel banners hidden: the page names no banner image, so none is
       fetched or decoded; the rest of the header is unchanged. */
    CHECK(youtube_channel_build("https://www.youtube.com/@fixture?tilefinch_banners=0", home,
        sizeof(home) - 1u, NULL, 0, false, &budget, &document) == 0);
    html = document.html;
    CHECK(strstr(html, "googleusercontent.com/banner") == NULL
        && strstr(html, "<img class=channel-banner") == NULL
        && strstr(html, "<h1>Fixture &lt;Channel&gt;</h1>") != NULL
        && strstr(html, "src=\"https://yt3.googleusercontent.com/avatar=s72\"") != NULL
        && youtube_channel_after(html, "id=yt-channel-about", "<nav class=channel-tabs>") != NULL
        && document.result_count == 9u);
    youtube_lite_document_destroy(&document);
    CHECK(budget.current == 0);

    /* Signed in with a stated subscription: only the lit bell, which
       unsubscribes, beside the name and before the description and tabs. */
    CHECK(youtube_channel_build("https://www.youtube.com/@fixture", signed_home,
        sizeof(signed_home) - 1u, NULL, 0, true, &budget, &document) == 0);
    html = document.html;
    CHECK(youtube_channel_after(html, "<div class=channel-names><h1>",
        "<div class=channel-actions><a id=\"yt-unsubscribe-action\" class=\"comments-link icon-action\" "
        "data-yt-action=4 data-yt-target=\"UCabcdefghijklmnopqrstuv\"") != NULL);
    CHECK(youtube_channel_after(html, "id=\"yt-unsubscribe-action\"",
        "aria-label=\"Unsubscribe\" title=\"Unsubscribe\"><span aria-hidden=true class=\"action-icon icon-bell-on\">") != NULL);
    CHECK(strstr(html, "yt-subscribe-action") == NULL);
    CHECK(strstr(html, "yt-channel-account") == NULL);
    CHECK(youtube_channel_after(html, "yt-unsubscribe-action", "id=yt-channel-about") != NULL);
    CHECK(strstr(html, ".icon-bell-on .a{") != NULL);
    /* The action result line is the watch page's: reserved and empty
       under the subscriber line until an action paints into it. */
    CHECK(strstr(html, "<p class=hint>1.5K subscribers \xc2\xb7 42 videos</p>"
                       "<p id=yt-library-status class=watch-status>&nbsp;</p></div>") != NULL);
    CHECK(strstr(html, "Select an action.") == NULL);
    youtube_lite_document_destroy(&document);
    /* Signed in without one: the bell subscribes, as on the watch page,
       and Unsubscribe waits in the account menu beside it. */
    CHECK(youtube_channel_build("https://www.youtube.com/@fixture", home,
        sizeof(home) - 1u, NULL, 0, true, &budget, &document) == 0);
    html = document.html;
    CHECK(youtube_channel_after(html, "<div class=channel-actions>",
        "<a id=\"yt-subscribe-action\" class=\"comments-link icon-action\" data-yt-action=3 "
        "data-yt-target=\"UCabcdefghijklmnopqrstuv\" data-yt-video=\"\" href=\""
        YOUTUBE_LITE_FRAGMENT_ACCOUNT_ACTION "\" aria-label=\"Subscribe\" title=\"Subscribe\">"
        "<span aria-hidden=true class=\"action-icon icon-bell\">") != NULL);
    CHECK(youtube_channel_after(html, "id=\"yt-subscribe-action\"",
        "<details id=yt-channel-account class=watch-account-menu>") != NULL);
    CHECK(youtube_channel_after(html, "id=yt-channel-account", "id=\"yt-unsubscribe-action\"") != NULL);
    CHECK(youtube_channel_after(html, "</details></div></div>", "id=yt-channel-about") != NULL);
    youtube_lite_document_destroy(&document);
    CHECK(budget.current == 0);

    /* Watch-page parity: the channel's bell is the watch page's control,
       byte for byte, for the same channel. */
    {
        static const char watch[] =
            "<script>var ytInitialData='{}';</script><script>var ytInitialPlayerResponse="
            "{\"videoDetails\":{\"videoId\":\"TFTEST00001\",\"channelId\":\"UCabcdefghijklmnopqrstuv\","
            "\"author\":\"Fixture\",\"title\":\"Watch fixture\",\"lengthSeconds\":\"200\"}};</script>";
        YoutubeLiteDocument watch_document = {0};
        CHECK(youtube_channel_build("https://www.youtube.com/watch?v=TFTEST00001", watch,
            sizeof(watch) - 1u, NULL, 0, true, &budget, &watch_document) == 0);
        const char *start = strstr(watch_document.html, "<a id=\"yt-subscribe-action\"");
        const char *end = start == NULL ? NULL : strstr(start, "</a>");
        CHECK(start != NULL && end != NULL && strstr(start, "icon-bell\"") != NULL);
        CHECK(youtube_channel_build("https://www.youtube.com/@fixture", home,
            sizeof(home) - 1u, NULL, 0, true, &budget, &document) == 0);
        const char *match = strstr(document.html, "<a id=\"yt-subscribe-action\"");
        CHECK(match != NULL && end != NULL
              && strncmp(match, start, (size_t) (end - start) + 4u) == 0);
        youtube_lite_document_destroy(&document);
        youtube_lite_document_destroy(&watch_document);
        CHECK(budget.current == 0);
    }

    /* The fallback full-description view: the page's whole description,
       no entries, no tab current, and the way back. */
    CHECK(youtube_channel_build("https://www.youtube.com/@fixture?tilefinch_view=description",
        home, sizeof(home) - 1u, NULL, 0, false, &budget, &document) == 0);
    html = document.html;
    CHECK(strstr(html, "<h1>Fixture &lt;Channel&gt;</h1>") != NULL);
    CHECK(youtube_channel_after(html, "<nav class=channel-tabs>",
        "<h2>Description</h2><div id=description class=description>Line one.<br>Line two.</div>") != NULL);
    CHECK(strstr(html, "aria-current=page") == NULL);
    CHECK(strstr(html, "id=yt-channel-about") == NULL);
    CHECK(strstr(html, "watch?v=") == NULL && strstr(html, "<h2>Coming up") == NULL);
    CHECK(strstr(html, "No public video entries") == NULL);
    CHECK(strstr(html, "<p><a class=comments-link href=\"https://www.youtube.com/@fixture\">Back to channel</a></p>") != NULL);
    CHECK(strstr(html, "yt-subscribe-action") == NULL);
    CHECK(document.result_count == 0u);
    youtube_lite_document_destroy(&document);
    CHECK(budget.current == 0);

    /* Videos tab: latest uploads in order, then the next page. */
    CHECK(youtube_channel_build("https://www.youtube.com/@fixture/videos", videos,
        sizeof(videos) - 1u, NULL, 0, false, &budget, &document) == 0);
    html = document.html;
    CHECK(youtube_channel_after(html, "watch?v=LATEST00001", "watch?v=LATEST00002") != NULL);
    CHECK(strstr(html, "<h2>") == NULL);
    CHECK(strstr(html, "<a class=\"comments-link on\" aria-current=page href=\"https://www.youtube.com/@fixture/videos\">Videos</a>") != NULL);
    /* A tab's token is long (about 1.7 KB on YouTube's channel pages). */
    CHECK(strstr(html, "href=\"https://www.youtube.com/@fixture/videos?tilefinch_token=NEXT%2B%2F%3DAAAA") != NULL);
    CHECK(strstr(html, "AAAA\">More videos</a>") != NULL);
    /* The header's About panel continuation is not the next page: it is
       the full-description view's, from the description line. A tab's
       description link opens the channel's own view. */
    CHECK(strstr(html, "tilefinch_token=ABOUT\">") == NULL);
    CHECK(strstr(html, "href=\"https://www.youtube.com/@fixture?tilefinch_view=description&amp;tilefinch_token=ABOUT#description\">Line one. Line two.</a>") != NULL);
    CHECK(document.result_count == 2u);
    youtube_lite_document_destroy(&document);
    CHECK(youtube_channel_build("https://www.youtube.com/@fixture/videos?tilefinch_token=NEXT",
        NULL, 0, page2, sizeof(page2) - 1u, false, &budget, &document) == 0);
    html = document.html;
    CHECK(strstr(html, "watch?v=PAGE2000001") != NULL);
    CHECK(strstr(html, "@fixture/videos?tilefinch_token=THIRD") != NULL);
    CHECK(strstr(html, "href=\"https://www.youtube.com/@fixture/videos\">Videos</a>") != NULL);
    youtube_lite_document_destroy(&document);
    CHECK(budget.current == 0);

    /* The full-description view from the About panel response: the whole
       description, links to the sites the redirects name (bounded at
       eight, unsafe targets dropped), the panel's counts and dates. */
    CHECK(youtube_channel_build(
        "https://www.youtube.com/@fixture?tilefinch_view=description&tilefinch_token=ABOUT",
        NULL, 0, about, sizeof(about) - 1u, false, &budget, &document) == 0);
    html = document.html;
    CHECK(strstr(html, "<title>@fixture</title>") != NULL);
    CHECK(strstr(html, "<h1>@fixture</h1><p class=hint>1.5K subscribers \xc2\xb7 42 videos</p>") != NULL);
    CHECK(youtube_channel_after(html, "<h2>Description</h2>",
        "<div id=description class=description>First &lt;line&gt;.<br><br>Second line that is longer than the header shows.</div>") != NULL);
    CHECK(youtube_channel_after(html, "</div><h2>Links</h2><ul class=channel-links>",
        "<li><a href=\"https://www.example.com/fixture?a=1&amp;b=2\">Site</a> <span class=hint>example.com/fixture</span></li>") != NULL);
    CHECK(strstr(html, "<li><a href=\"https://www.youtube.com/@friend\">friend</a></li>") != NULL);
    CHECK(strstr(html, "javascript") == NULL && strstr(html, "Unsafe") == NULL);
    CHECK(youtube_channel_after(html, "</ul>",
        "<p class=hint>1,234 views \xc2\xb7 Joined Jan 2, 2020 \xc2\xb7 Iceland</p>") != NULL);
    CHECK(strstr(html, "href=\"https://www.youtube.com/@fixture\">Back to channel</a>") != NULL);
    CHECK(strstr(html, "aria-current=page") == NULL && strstr(html, "yt-subscribe-action") == NULL);
    {
        size_t links = 0;
        for (const char *at = html; (at = strstr(at, "<li><a href=")) != NULL; at++) links++;
        CHECK(links == 8u);
    }
    youtube_lite_document_destroy(&document);
    CHECK(budget.current == 0);

    /* A description line cut just after a stop ends in the marker alone,
       never ".…". */
    {
        char words[96], source[1024];
        memset(words, 'w', 95u);
        words[95] = '\0';
        int length = snprintf(source, sizeof(source),
            "<script>var ytInitialData = '{\"contents\":{},\"metadata\":{\"channelMetadataRen"
            "derer\":{\"title\":\"Fixture\",\"externalId\":\"UCabcdefghijklmnopqrstuv\","
            "\"description\":\"%s. Then more after the stop.\"}}}';</script>", words);
        CHECK(length > 0 && (size_t) length < sizeof(source));
        CHECK(youtube_channel_build("https://www.youtube.com/@fixture", source,
            (size_t) length, NULL, 0, false, &budget, &document) == 0);
        CHECK(strstr(document.html, "www\xe2\x80\xa6</a>") != NULL);
        CHECK(strstr(document.html, "w.\xe2\x80\xa6") == NULL);
        youtube_lite_document_destroy(&document);
        CHECK(budget.current == 0);
    }

    /* Shorts and Playlists tabs, on a /channel/ URL. */
    CHECK(youtube_channel_build("https://www.youtube.com/channel/UCabcdefghijklmnopqrstuv/shorts",
        shorts, sizeof(shorts) - 1u, NULL, 0, false, &budget, &document) == 0);
    html = document.html;
    CHECK(youtube_channel_after(html, "watch?v=SHORT000001", "Tiny clip") != NULL);
    CHECK(strstr(html, "5K views") != NULL);
    CHECK(youtube_channel_after(html, "watch?v=SHORT000002",
                                "<span class=title>Label clip, again</span>") != NULL);
    CHECK(strstr(html, "aria-current=page href=\"https://www.youtube.com/channel/UCabcdefghijklmnopqrstuv/shorts\">") != NULL);
    youtube_lite_document_destroy(&document);
    CHECK(youtube_channel_build("https://www.youtube.com/c/fixture/playlists",
        playlists, sizeof(playlists) - 1u, NULL, 0, false, &budget, &document) == 0);
    html = document.html;
    CHECK(strstr(html, "<a class=card href=\"https://www.youtube.com/playlist?list=PLfixture04\">") != NULL);
    CHECK(strstr(html, "3 videos") != NULL);
    CHECK(strstr(html, "aria-current=page href=\"https://www.youtube.com/c/fixture/playlists\">") != NULL);
    CHECK(strstr(html, "href=\"https://www.youtube.com/c/fixture\">Home</a>") != NULL);
    youtube_lite_document_destroy(&document);
    CHECK(budget.current == 0);
    return 0;
}
