static bool test_cancellation_callback(void *opaque)
{
    return tilefinch_cancellation_requested(opaque);
}

static bool test_psp_network_reinitialization_policy(void)
{
    static const struct {
        PspNetworkInitService service;
        int existing;
    } cases[] = {
        {PSP_NETWORK_INIT_CORE, (int) PSP_NETWORK_CORE_NOT_TERMINATED},
        {PSP_NETWORK_INIT_INET,
         (int) PSP_NETWORK_INET_ALREADY_INITIALIZED},
        {PSP_NETWORK_INIT_RESOLVER,
         (int) PSP_NETWORK_RESOLVER_NOT_TERMINATED},
        {PSP_NETWORK_INIT_APCTL,
         (int) PSP_NETWORK_APCTL_ALREADY_INITIALIZED}
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        bool adopted = true;
        if (!psp_network_init_result_usable(cases[i].service, 0, &adopted)
            || adopted) return false;
        adopted = false;
        if (!psp_network_init_result_usable(
                cases[i].service, cases[i].existing, &adopted)
            || !adopted) return false;
        adopted = true;
        if (psp_network_init_result_usable(
                cases[i].service, -123, &adopted) || adopted) return false;
        for (size_t other = 0;
             other < sizeof(cases) / sizeof(cases[0]); other++) {
            if (other == i) continue;
            if (psp_network_init_result_usable(
                    cases[i].service, cases[other].existing, NULL))
                return false;
        }
    }
    if (psp_network_apctl_state_is_associating(0)
        || psp_network_apctl_state_is_associating(1)
        || !psp_network_apctl_state_is_associating(2)
        || !psp_network_apctl_state_is_associating(3)
        || psp_network_apctl_state_is_associating(4)
        || !psp_network_apctl_state_is_associating(5)
        || !psp_network_apctl_state_is_associating(6)
        || psp_network_apctl_state_is_associating(7)) return false;
    return true;
}

static bool test_youtube_player_response_parser(void)
{
    if (!youtube_watch_url_supported(
            "https://www.youtube.com/watch?v=TFTEST00001")
        || !youtube_watch_url_supported(
            "https://youtu.be/TFTEST00001?t=3")
        || youtube_watch_url_supported("http://youtu.be/TFTEST00001")
        || youtube_watch_url_supported("https://youtube.com/")
        || youtube_watch_url_supported(
               "https://notyoutube.com/watch?v=TFTEST00001")) {
        return false;
    }
    if (!youtube_media_url_supported(
            "https://rr1---sn.example.googlevideo.com/videoplayback")
        || !youtube_media_url_supported(
            "https://googlevideo.com/videoplayback")
        || youtube_media_url_supported(
            "http://rr1---sn.example.googlevideo.com/videoplayback")
        || youtube_media_url_supported(
            "https://evilgooglevideo.com/videoplayback")
        || youtube_media_url_supported(
            "https://googlevideo.com.evil.test/videoplayback")) {
        return false;
    }
    YoutubeStream direct = {
        .content_length = 2u * 1024u * 1024u,
        .audio_content_length = 2u * 1024u * 1024u,
        .split_streams = true
    };
    if (!youtube_direct_delivery_admitted("IOS", &direct)
        || !youtube_direct_delivery_admitted("ANDROID_VR", &direct)
        || !youtube_direct_delivery_admitted(
               "WEB_EMBEDDED_PLAYER", &direct)
        || youtube_direct_delivery_admitted("UNKNOWN", &direct)) {
        return false;
    }
    direct.content_length++;
    if (youtube_direct_delivery_admitted("IOS", &direct)
        || youtube_direct_delivery_admitted("ANDROID_VR", &direct)
        || !youtube_direct_delivery_admitted(
               "WEB_EMBEDDED_PLAYER", &direct)) {
        return false;
    }
    direct.content_length--;
    direct.audio_content_length++;
    if (youtube_direct_delivery_admitted("IOS", &direct)
        || youtube_direct_delivery_admitted("ANDROID_VR", &direct)) {
        return false;
    }
    if (youtube_playability_is_globally_terminal(
            YOUTUBE_PLAYABILITY_UNAVAILABLE)
        || youtube_playability_is_globally_terminal(
               YOUTUBE_PLAYABILITY_LOGIN_REQUIRED)
        || youtube_playability_is_globally_terminal(
               YOUTUBE_PLAYABILITY_AGE_RESTRICTED)
        || !youtube_playability_is_globally_terminal(
               YOUTUBE_PLAYABILITY_REGION_BLOCKED)
        || !youtube_playability_is_globally_terminal(
               YOUTUBE_PLAYABILITY_LIVE_UNSUPPORTED)
        || !youtube_playability_is_globally_terminal(
               YOUTUBE_PLAYABILITY_UPCOMING_UNSUPPORTED)) {
        return false;
    }
    Budget cancellation_budget;
    budget_init(&cancellation_budget, 256 * 1024);
    BrowserSession cancellation_session;
    TilefinchCancellation cancellation;
    tilefinch_cancellation_init(&cancellation);
    tilefinch_cancellation_request(&cancellation);
    YoutubeStream cancelled_stream = {0};
    char cancelled_error[128] = {0};
    bool cancellation_session_ready = browser_session_init(
        &cancellation_session, &cancellation_budget, 4096);
    if (!cancellation_session_ready
        || youtube_resolve_progressive_mp4_cancelable(
               &cancellation_budget, &cancellation_session,
               "https://www.youtube.com/watch?v=TFTEST00001",
               240, 30000, test_cancellation_callback, &cancellation,
               &cancelled_stream, cancelled_error,
               sizeof(cancelled_error))
        || strstr(cancelled_error, "cancelled") == NULL) {
        if (cancellation_session_ready)
            browser_session_destroy(&cancellation_session);
        return false;
    }
    browser_session_destroy(&cancellation_session);
    if (cancellation_budget.current != 0) return false;
    static const char response[] =
        "{\"videoDetails\":{\"title\":\"Fixture \\u0076ideo\","
        "\"isLiveContent\":true},"
        "\"playabilityStatus\":{\"status\":\"OK\"},"
        "\"streamingData\":{\"hlsManifestUrl\":"
        "\"https://manifest.googlevideo.com/archive.m3u8\","
        "\"formats\":["
        "{\"height\":144,\"url\":\"https:\\/\\/rr1.googlevideo.com\\/low.mp4?"
        "a=1\\u0026b=2\",\"mimeType\":\"video\\/mp4; codecs=\\\""
        "avc1.4d400c, mp4a.40.2\\\"\",\"width\":256,\"itag\":17,"
        "\"bitrate\":100,\"approxDurationMs\":\"19000\","
        "\"contentLength\":\"1000\"},"
        "{\"contentLength\":\"2000\",\"bitrate\":200,\"itag\":18,"
        "\"mimeType\":\"video/mp4; codecs=\\\"avc1.42001e, "
        "mp4a.40.2\\\"\",\"url\":\"https://rr2.googlevideo.com/chosen.mp4\","
        "\"approxDurationMs\":\"18950\",\"height\":240,\"width\":320},"
        "{\"itag\":22,\"width\":1280,\"height\":720,\"bitrate\":900,"
        "\"mimeType\":\"video/mp4; codecs=\\\"avc1.64001f, "
        "mp4a.40.2\\\"\",\"url\":\"https://rr3.googlevideo.com/too-high.mp4\"},"
        "{\"itag\":134,\"width\":320,\"height\":240,\"bitrate\":300,"
        "\"mimeType\":\"video/mp4; codecs=\\\"avc1.4d4015\\\"\","
        "\"url\":\"https://rr4.googlevideo.com/video-only.mp4\"}]}}";
    YoutubeStream stream;
    char error[256];
    if (!youtube_parse_player_response(
            response, strlen(response), "TFTEST00001", 360,
            &stream, error, sizeof(error))) {
        fprintf(stderr, "youtube parser: %s\n", error);
        return false;
    }
    if (strcmp(stream.video_id, "TFTEST00001") != 0
        || strcmp(stream.title, "Fixture video") != 0
        || strcmp(stream.media_url,
                  "https://rr2.googlevideo.com/chosen.mp4") != 0
        || stream.itag != 18 || stream.width != 320 || stream.height != 240
        || stream.duration_ms != 18950 || stream.content_length != 2000
        || stream.bitrate != 200) return false;

    static const char adaptive[] =
        "{\"playabilityStatus\":{\"status\":\"OK\"},"
        "\"videoDetails\":{\"title\":\"Fixture artist (테스트)\"},"
        "\"streamingData\":{\"formats\":["
        "{\"itag\":18,\"width\":540,\"height\":360,\"bitrate\":700,"
        "\"mimeType\":\"video/mp4; codecs=\\\"avc1.42001e, "
        "mp4a.40.2\\\"\",\"url\":\"https://rr1.googlevideo.com/360.mp4\"}],"
        "\"adaptiveFormats\":["
        "{\"itag\":133,\"width\":360,\"height\":240,\"bitrate\":260,"
        "\"approxDurationMs\":\"19000\",\"contentLength\":\"3100\","
        "\"mimeType\":\"video/mp4; codecs=\\\"avc1.4d400d\\\"\","
        "\"url\":\"https://rr2.googlevideo.com/video-240.mp4\"},"
        "{\"itag\":134,\"width\":640,\"height\":360,\"bitrate\":620,"
        "\"approxDurationMs\":\"19000\",\"contentLength\":\"5100\","
        "\"mimeType\":\"video/mp4; codecs=\\\"avc1.4d401e\\\"\","
        "\"url\":\"https://rr2.googlevideo.com/video-360.mp4\"},"
        "{\"itag\":136,\"width\":640,\"height\":360,\"bitrate\":900,"
        "\"approxDurationMs\":\"19000\",\"contentLength\":\"5200\","
        "\"mimeType\":\"video/mp4; codecs=\\\"avc1.64001e\\\"\","
        "\"url\":\"https://rr2.googlevideo.com/high-360.mp4\"},"
        "{\"itag\":137,\"width\":640,\"height\":360,\"bitrate\":800,"
        "\"approxDurationMs\":\"19000\",\"contentLength\":\"5200\","
        "\"mimeType\":\"video/mp4; codecs=\\\"avc1.42001e\\\"\","
        "\"url\":\"https://rr2.googlevideo.com/baseline-wide.mp4\"},"
        "{\"itag\":138,\"width\":640,\"height\":360,\"bitrate\":700,"
        "\"approxDurationMs\":\"19000\",\"contentLength\":\"5200\","
        "\"mimeType\":\"video/mp4; codecs=\\\"avc1.4d401f\\\"\","
        "\"url\":\"https://rr2.googlevideo.com/level31-360.mp4\"},"
        "{\"itag\":139,\"width\":640,\"height\":360,\"bitrate\":1000,"
        "\"approxDurationMs\":\"19000\",\"contentLength\":\"5200\","
        "\"mimeType\":\"video/mp4; codecs=\\\"avc1.4d\\\"\","
        "\"url\":\"https://rr2.googlevideo.com/truncated-codec.mp4\"},"
        "{\"itag\":135,\"width\":640,\"height\":360,\"bitrate\":999,"
        "\"approxDurationMs\":\"19000\","
        "\"mimeType\":\"video/mp4; codecs=\\\"avc1.4d401e\\\"\","
        "\"url\":\"https://rr2.googlevideo.com/unsized-video.mp4\"},"
        "{\"itag\":258,\"bitrate\":384000,\"audioChannels\":6,"
        "\"approxDurationMs\":\"19040\",\"contentLength\":\"2400\","
        "\"mimeType\":\"audio/mp4; codecs=\\\"mp4a.40.2\\\"\","
        "\"url\":\"https://rr3.googlevideo.com/surround.m4a\"},"
        "{\"itag\":139,\"bitrate\":256000,\"audioChannels\":2,"
        "\"approxDurationMs\":\"19040\",\"contentLength\":\"1800\","
        "\"mimeType\":\"audio/mp4; codecs=\\\"mp4a.40.5\\\"\","
        "\"url\":\"https://rr3.googlevideo.com/he-aac.m4a\"},"
        "{\"itag\":141,\"bitrate\":384000,\"audioChannels\":2,"
        "\"approxDurationMs\":\"19040\",\"contentLength\":\"2000\","
        "\"mimeType\":\"audio/mp4; codecs=\\\"mp4a.40.29\\\"\","
        "\"url\":\"https://rr3.googlevideo.com/he-aac-v2.m4a\"},"
        "{\"itag\":140,\"bitrate\":129000,\"approxDurationMs\":\"19040\","
        "\"contentLength\":\"1200\","
        "\"mimeType\":\"audio/mp4; codecs=\\\"mp4a.40.2\\\"\","
        "\"url\":\"https://rr3.googlevideo.com/audio.m4a\"}]}}";
    if (!youtube_parse_player_response(
            adaptive, strlen(adaptive), "-bC4iak3kxg", 240,
            &stream, error, sizeof(error))
        || !stream.split_streams || stream.itag != 133
        || stream.audio_itag != 140 || stream.width != 360
        || stream.height != 240 || stream.duration_ms != 19040
        || stream.content_length != 3100
        || stream.audio_content_length != 1200
        || strcmp(
               stream.media_url,
               "https://rr2.googlevideo.com/video-240.mp4") != 0
        || strcmp(
               stream.audio_url,
               "https://rr3.googlevideo.com/audio.m4a") != 0) return false;
    if (!youtube_parse_player_response(
            adaptive, strlen(adaptive), "-bC4iak3kxg", 360,
            &stream, error, sizeof(error))
        || !stream.split_streams || stream.itag != 134
        || stream.audio_itag != 140 || stream.width != 640
        || stream.height != 360 || stream.content_length != 5100
        || strcmp(
               stream.media_url,
               "https://rr2.googlevideo.com/video-360.mp4") != 0)
        return false;

    static const char unsized_only[] =
        "{\"playabilityStatus\":{\"status\":\"OK\"},"
        "\"streamingData\":{\"formats\":["
        "{\"itag\":18,\"width\":640,\"height\":360,\"bitrate\":700,"
        "\"mimeType\":\"video/mp4; codecs=\\\"avc1.42001e, "
        "mp4a.40.2\\\"\","
        "\"url\":\"https://rr1.googlevideo.com/unsized.mp4\"}]}}";
    if (youtube_parse_player_response(
            unsized_only, strlen(unsized_only), "unsizedvideo", 360,
            &stream, error, sizeof(error))
        || strstr(error, "no unciphered AVC") == NULL) {
        return false;
    }

    static const char long_inventory[] =
        "{\"playabilityStatus\":{\"status\":\"OK\"},"
        "\"videoDetails\":{\"title\":\"Long video\"},"
        "\"streamingData\":{\"formats\":["
        "{\"itag\":18,\"width\":640,\"height\":360,\"bitrate\":500,"
        "\"approxDurationMs\":\"1200000\",\"contentLength\":\"9000\","
        "\"mimeType\":\"video/mp4; codecs=\\\"avc1.42001e, "
        "mp4a.40.2\\\"\",\"url\":\"https://rr1.googlevideo.com/"
        "progressive-long.mp4\"}],\"adaptiveFormats\":["
        "{\"itag\":134,\"width\":640,\"height\":360,\"bitrate\":700,"
        "\"approxDurationMs\":\"1200000\",\"contentLength\":\"7000\","
        "\"mimeType\":\"video/mp4; codecs=\\\"avc1.4d401e\\\"\","
        "\"url\":\"https://rr2.googlevideo.com/adaptive-long.mp4\"},"
        "{\"itag\":258,\"bitrate\":384000,\"audioChannels\":6,"
        "\"approxDurationMs\":\"1200000\",\"contentLength\":\"6000\","
        "\"mimeType\":\"audio/mp4; codecs=\\\"mp4a.40.2\\\"\","
        "\"url\":\"https://rr3.googlevideo.com/surround-long.m4a\"},"
        "{\"itag\":140,\"bitrate\":129000,"
        "\"audioChannels\":2,"
        "\"approxDurationMs\":\"1200000\",\"contentLength\":\"3000\","
        "\"mimeType\":\"audio/mp4; codecs=\\\"mp4a.40.2\\\"\","
        "\"url\":\"https://rr3.googlevideo.com/audio-long.m4a\"}]}}";
    if (!youtube_parse_player_response(
            long_inventory, strlen(long_inventory), "longvideo01", 360,
            &stream, error, sizeof(error))
        || !stream.split_streams || stream.itag != 134
        || stream.audio_itag != 140
        || stream.duration_ms != 1200000
        || strcmp(
               stream.media_url,
               "https://rr2.googlevideo.com/adaptive-long.mp4") != 0
        || strcmp(
               stream.audio_url,
               "https://rr3.googlevideo.com/audio-long.m4a") != 0) {
        return false;
    }

    static const char adaptive_only[] =
        "{\"status\":\"UNRELATED_TOP_LEVEL_VALUE\","
        "\"adaptiveFormats\":[],"
        "\"playabilityStatus\":{\"status\":\"OK\"},"
        "\"videoDetails\":{\"title\":\"Adaptive only\"},"
        "\"streamingData\":{\"adaptiveFormats\":["
        "{\"itag\":133,\"width\":426,\"height\":240,\"bitrate\":260,"
        "\"contentLength\":\"3100\","
        "\"mimeType\":\"video/mp4; codecs=\\\"avc1.4d400d\\\"\","
        "\"url\":\"https://rr2.googlevideo.com/video-240.mp4\"},"
        "{\"itag\":140,\"bitrate\":129000,\"contentLength\":\"1200\","
        "\"mimeType\":\"audio/mp4; codecs=\\\"mp4a.40.2\\\"\","
        "\"url\":\"https://rr3.googlevideo.com/audio.m4a\"}]}}";
    YoutubePlayability playability = YOUTUBE_PLAYABILITY_UNKNOWN;
    if (!youtube_parse_player_response_diagnostic(
            adaptive_only, strlen(adaptive_only), "-bC4iak3kxg", 240,
            &playability, &stream, error, sizeof(error))
        || playability != YOUTUBE_PLAYABILITY_OK
        || !stream.split_streams || stream.itag != 133
        || stream.audio_itag != 140) {
        return false;
    }

    static const char age_restricted[] =
        "{\"playabilityStatus\":{\"status\":\"LOGIN_REQUIRED\","
        "\"reason\":\"Sign in to confirm your age\"}}";
    if (youtube_parse_player_response_diagnostic(
            age_restricted, strlen(age_restricted), "age", 240,
            &playability, &stream, error, sizeof(error))
        || playability != YOUTUBE_PLAYABILITY_AGE_RESTRICTED
        || strstr(error, "playability: age-restricted") == NULL) {
        return false;
    }
    static const char bot_login[] =
        "{\"playabilityStatus\":{\"status\":\"LOGIN_REQUIRED\","
        "\"reason\":\"Sign in to confirm you are not a bot\"}}";
    if (youtube_parse_player_response_diagnostic(
            bot_login, strlen(bot_login), "bot", 240,
            &playability, &stream, error, sizeof(error))
        || playability != YOUTUBE_PLAYABILITY_LOGIN_REQUIRED) {
        return false;
    }
    static const char live[] =
        "{\"playabilityStatus\":{\"status\":\"OK\"},"
        "\"videoDetails\":{\"isLiveContent\":true},"
        "\"streamingData\":{\"hlsManifestUrl\":"
        "\"https://manifest.googlevideo.com/live.m3u8\"}}";
    if (youtube_parse_player_response_diagnostic(
            live, strlen(live), "live", 240,
            &playability, &stream, error, sizeof(error))
        || playability != YOUTUBE_PLAYABILITY_LIVE_UNSUPPORTED
        || strstr(error, "live streams") == NULL) {
        return false;
    }

    static const char untrusted[] =
        "{\"playabilityStatus\":{\"status\":\"OK\"},"
        "\"streamingData\":{\"formats\":[{\"itag\":18,\"width\":320,"
        "\"height\":240,\"contentLength\":\"2000\","
        "\"mimeType\":\"video/mp4; codecs=\\\""
        "avc1.42001e, mp4a.40.2\\\"\","
        "\"url\":\"https://googlevideo.com.attacker.test/video.mp4\"}]}}";
    if (youtube_parse_player_response(
            untrusted, strlen(untrusted), "TFTEST00001", 360,
            &stream, error, sizeof(error))
        || strstr(error, "untrusted media") == NULL) return false;

    static const char ciphered[] =
        "{\"playabilityStatus\":{\"status\":\"OK\"},"
        "\"streamingData\":{\"formats\":[{\"itag\":18,\"width\":320,"
        "\"height\":240,\"mimeType\":\"video/mp4; codecs=\\\""
        "avc1.42001e, mp4a.40.2\\\"\","
        "\"signatureCipher\":\"url=x%3A%2F%2Fy\"}]}}";
    return !youtube_parse_player_response(
        ciphered, strlen(ciphered), "TFTEST00001", 360,
        &stream, error, sizeof(error))
        && (strstr(error, "adaptive inventory") != NULL
            || strstr(error, "unciphered AVC") != NULL);
}

typedef struct {
    size_t calls;
    size_t cancel_at;
} DocumentParseCooperateProbe;

static bool test_document_parse_cooperate(
    void *context, const char *phase, size_t completed_work_units)
{
    DocumentParseCooperateProbe *probe = context;
    /* The statistics walk checkpoints inside the same call; only the feed
       loop's own phase is under test. */
    if (strcmp(phase, "document-parse") != 0 || completed_work_units == 0)
        return true;
    probe->calls++;
    return probe->cancel_at == 0 || probe->calls < probe->cancel_at;
}

static bool test_youtube_lite_commit_cooperates(Budget *budget)
{
    if (budget == NULL) return false;
    size_t entry_baseline = budget->current;
    YoutubeLiteDocument document = {0};
    char error[256] = {0};
    /*
     * A search-results page is committed by the site adapter, which hands the
     * whole document to the navigation pump in one piece. The parse of that
     * document is the head of the commit, so it is the one stage during which
     * cancel cannot be sampled and the supervisor cannot paint unless the
     * parse takes checkpoints of its own.
     *
     * The source below is a synthetic response containing the fields the
     * builder reads: twelve videoWithContextRenderer rows whose
     * descriptionSnippet arrives split at the query matches. It is here
     * rather than a hand-sized page because the checkpoint is per window of
     * fed bytes, and a real result page is the smallest document the shipping
     * routes produce that crosses one.
     */
    enum { MANY_SOURCE_CAPACITY = 16384 };
    char *many_source = budget_malloc(budget, MANY_SOURCE_CAPACITY);
    if (many_source == NULL) return false;
    int many_used = snprintf(
        many_source, MANY_SOURCE_CAPACITY,
        "<script>var ytInitialData = '\\x7b\\x22contents\\x22:\\x5b");
    for (size_t i = 0; i < YOUTUBE_LITE_MAXIMUM_RESULTS; i++) {
        int written = snprintf(
            many_source + many_used,
            (size_t) (MANY_SOURCE_CAPACITY - many_used),
            "%s\\x7b\\x22videoWithContextRenderer\\x22:\\x7b"
            "\\x22headline\\x22:\\x7b\\x22runs\\x22:\\x5b\\x7b"
            "\\x22text\\x22:\\x22Animal result %02zu\\x22\\x7d\\x5d\\x7d,"
            "\\x22shortBylineText\\x22:\\x7b\\x22runs\\x22:\\x5b\\x7b"
            "\\x22text\\x22:\\x22Nature Channel\\x22\\x7d\\x5d\\x7d,"
            "\\x22lengthText\\x22:\\x7b\\x22runs\\x22:\\x5b\\x7b"
            "\\x22text\\x22:\\x223:11\\x22\\x7d\\x5d\\x7d,"
            "\\x22shortViewCountText\\x22:\\x7b\\x22runs\\x22:\\x5b\\x7b"
            "\\x22text\\x22:\\x2239M views\\x22\\x7d\\x5d\\x7d,"
            "\\x22publishedTimeText\\x22:\\x7b\\x22runs\\x22:\\x5b\\x7b"
            "\\x22text\\x22:\\x222 weeks ago\\x22\\x7d\\x5d\\x7d,"
            "\\x22descriptionSnippet\\x22:\\x7b\\x22runs\\x22:\\x5b\\x7b"
            "\\x22text\\x22:\\x22Welcome to the wild! Watch amazing "
            "\\x22\\x7d,\\x7b\\x22text\\x22:\\x22animal\\x22\\x7d,\\x7b"
            "\\x22text\\x22:\\x22 footage from every continent\\x22\\x7d"
            "\\x5d\\x7d,"
            "\\x22videoId\\x22:\\x22animal_%03zu\\x22\\x7d\\x7d",
            i == 0 ? "" : ",", i, i);
        if (written <= 0 || written >= MANY_SOURCE_CAPACITY - many_used) {
            budget_free(budget, many_source);
            return false;
        }
        many_used += written;
    }
    int many_tail = snprintf(
        many_source + many_used,
            (size_t) (MANY_SOURCE_CAPACITY - many_used),
        "\\x5d\\x7d';</script>");
    if (many_tail <= 0 || many_tail >= MANY_SOURCE_CAPACITY - many_used) {
        budget_free(budget, many_source);
        return false;
    }
    many_used += many_tail;
    if (!youtube_lite_build_document(
            budget, "https://www.youtube.com/results?search_query=animal",
            many_source, (size_t) many_used,
            &document, error, sizeof(error))) {
        fprintf(stderr, "youtube lite cooperative search: %s\n", error);
        budget_free(budget, many_source);
        return false;
    }
    budget_free(budget, many_source);
    size_t parse_baseline = budget->current;
    DocumentParseCooperateProbe parse_probe = {0};
    TilefinchPlatformServices parse_services = {
        .context = &parse_probe,
        .cooperate = test_document_parse_cooperate
    };
    tilefinch_platform_set_services(&parse_services);
    PocDocument committed = {0};
    bool parsed = document_parse(
        &committed, budget, document.html, document.html_length, 512);
    document_destroy(&committed);
    DocumentParseCooperateProbe cancel_probe = {.cancel_at = 1};
    parse_services.context = &cancel_probe;
    tilefinch_platform_set_services(&parse_services);
    PocDocument abandoned = {0};
    bool cancelled = !document_parse(
        &abandoned, budget, document.html, document.html_length, 512);
    document_destroy(&abandoned);
    tilefinch_platform_set_services(NULL);
    bool cooperative_commit_ok =
        document.result_count == YOUTUBE_LITE_MAXIMUM_RESULTS
        /* The checkpoint is only owed once a document crosses a window; a
           fixture that stopped crossing one would pass silently. */
        && document.html_length > 8u * 1024u
        && parsed && parse_probe.calls >= 1
        && cancelled && cancel_probe.calls == 1
        && budget->current == parse_baseline;
    if (!cooperative_commit_ok) {
        fprintf(stderr,
                "youtube lite cooperative commit: results=%zu html=%zu "
                "parsed=%d calls=%zu cancelled=%d cancel-calls=%zu "
                "retained=%zu/%zu\n",
                document.result_count, document.html_length, (int) parsed,
                parse_probe.calls, (int) cancelled, cancel_probe.calls,
                budget->current, parse_baseline);
    }
    youtube_lite_document_destroy(&document);
    return cooperative_commit_ok && budget->current == entry_baseline;
}

static bool test_youtube_lite_document_builder(Budget *layout_budget)
{
    char video_id[YOUTUBE_VIDEO_ID_CAPACITY] = {0};
    if (youtube_lite_route("https://www.youtube.com/")
            != YOUTUBE_LITE_ROUTE_HOME
        || youtube_lite_route(
               "https://m.youtube.com/results?search_query=psp")
            != YOUTUBE_LITE_ROUTE_SEARCH
        || youtube_lite_route(
               "https://youtu.be/TFTEST00001?t=3")
            != YOUTUBE_LITE_ROUTE_WATCH
        || youtube_lite_route(
               "https://www.youtube.com/watch?v=TFTEST00001"
               "&tilefinch_view=comments")
            != YOUTUBE_LITE_ROUTE_WATCH
        || youtube_lite_route("https://www.youtube.com/@fixturechannel")
            != YOUTUBE_LITE_ROUTE_CHANNEL
        || youtube_watch_url_supported(
               "https://www.youtube.com/watch?v=TFTEST00001"
               "&tilefinch_view=comments")
        || youtube_watch_url_supported(
               "https://www.youtube.com/watch?v=TFTEST00001"
               "&tilefinch_view=description")
        || youtube_lite_route("http://www.youtube.com/")
            != YOUTUBE_LITE_ROUTE_NONE
        || youtube_lite_route("https://notyoutube.com/")
            != YOUTUBE_LITE_ROUTE_NONE
        || youtube_watch_url_video_id(NULL, video_id)
        || youtube_watch_url_video_id(
               "https://youtu.be/TFTEST00001", NULL)
        || !youtube_watch_url_video_id(
               "https://www.youtube.com/watch?v=TFTEST00001&list=x",
               video_id)
        || strcmp(video_id, "TFTEST00001") != 0) return false;

    static const char search_source[] =
        "<!doctype html><script>var ytInitialData = '"
        "\\x7b\\x22contents\\x22:\\x5b"
        "\\x7b\\x22videoWithContextRenderer\\x22:\\x7b"
        "\\x22headline\\x22:\\x7b\\x22runs\\x22:\\x5b\\x7b"
        "\\x22text\\x22:\\x22PSP & Guide\\x22\\x7d\\x5d\\x7d,"
        "\\x22shortBylineText\\x22:\\x7b\\x22runs\\x22:\\x5b\\x7b"
        "\\x22text\\x22:\\x22Channel One\\x22\\x7d\\x5d\\x7d,"
        "\\x22lengthText\\x22:\\x7b\\x22runs\\x22:\\x5b\\x7b"
        "\\x22text\\x22:\\x223:14\\x22\\x7d\\x5d\\x7d,"
        "\\x22shortViewCountText\\x22:\\x7b\\x22runs\\x22:\\x5b\\x7b"
        "\\x22text\\x22:\\x2212K views\\x22\\x7d\\x5d\\x7d,"
        "\\x22publishedTimeText\\x22:\\x7b\\x22runs\\x22:\\x5b\\x7b"
        "\\x22text\\x22:\\x222 days ago\\x22\\x7d\\x5d\\x7d,"
        /* The mobile page splits a description snippet across runs at every
           query match. */
        "\\x22descriptionSnippet\\x22:\\x7b\\x22runs\\x22:\\x5b\\x7b"
        "\\x22text\\x22:\\x22Hands-on with the \\x22\\x7d,\\x7b"
        "\\x22text\\x22:\\x22PSP\\x22\\x7d,\\x7b"
        "\\x22text\\x22:\\x22 & homebrew\\x22\\x7d\\x5d\\x7d,"
        "\\x22videoId\\x22:\\x22abc_DEF-123\\x22\\x7d\\x7d,"
        "\\x7b\\x22videoRenderer\\x22:\\x7b"
        "\\x22title\\x22:\\x7b\\x22runs\\x22:\\x5b\\x7b"
        "\\x22text\\x22:\\x22Second Video\\x22\\x7d\\x5d\\x7d,"
        /* The desktop shape wraps the same text one level deeper. */
        "\\x22detailedMetadataSnippets\\x22:\\x5b\\x7b"
        "\\x22snippetText\\x22:\\x7b\\x22runs\\x22:\\x5b\\x7b"
        "\\x22text\\x22:\\x22Desktop snippet shape\\x22\\x7d\\x5d\\x7d\\x7d"
        "\\x5d,"
        "\\x22videoId\\x22:\\x22zyx_987-ABC\\x22\\x7d\\x7d"
        "\\x5d\\x7d';</script>";

    Budget budget;
    budget_init(&budget, 2 * MIB);
    YoutubeLiteDocument document = {0};
    char error[256] = {0};
    if (!youtube_lite_build_document(
            &budget, "https://www.youtube.com/",
            search_source, sizeof(search_source) - 1,
            &document, error, sizeof(error))) {
        fprintf(stderr, "youtube lite home: %s\n", error);
        return false;
    }
    bool home_ok =
        document.route == YOUTUBE_LITE_ROUTE_HOME
        && strstr(document.html, "public mobile-page data") == NULL
        && strstr(document.html, "Lightweight view") == NULL
        && strstr(
               document.html,
               "id=yt-search name=search_query type=search "
               "aria-label=\"Search YouTube\" "
               "placeholder=\"Search YouTube\" value=\"\" autofocus")
               != NULL;
    youtube_lite_document_destroy(&document);
    if (!home_ok || budget.current != 0) return false;

    if (!youtube_lite_build_document(
            &budget,
            "https://www.youtube.com/results?"
            "search_query=PSP+%26+homebrew",
            search_source, sizeof(search_source) - 1,
            &document, error, sizeof(error))) {
        fprintf(stderr, "youtube lite search: %s\n", error);
        return false;
    }
    bool search_ok =
        document.route == YOUTUBE_LITE_ROUTE_SEARCH
        && document.result_count == 2
        && document.source_bytes == sizeof(search_source) - 1
        && document.html != NULL && document.html_length != 0
        && strstr(document.html, "name=search_query") != NULL
        && strstr(document.html, "<a class=card autofocus") != NULL
        && strstr(document.html, "class=compact-results") == NULL
        && strstr(document.html, "view-switch") == NULL
        && strstr(document.html, "tilefinch_layout") == NULL
        && strstr(document.html, "value=\"PSP &amp; homebrew\" autofocus")
               == NULL
        && strstr(document.html, "PSP &amp; homebrew") != NULL
        && strstr(document.html, "PSP &amp; Guide") != NULL
        && strstr(document.html,
                  "watch?v=abc_DEF-123&amp;search_query="
                  "PSP%20%26%20homebrew") != NULL
        && strstr(document.html,
                  "i.ytimg.com/vi/abc_DEF-123/mqdefault.jpg") != NULL
        && strstr(document.html, "Channel One") != NULL
        && strstr(document.html, "<span class=channel>Channel One</span>")
               != NULL
        && strstr(document.html, "<a class=channel") == NULL
        && strstr(document.html, "Second Video") != NULL
        /* A result row carries its description under the title, styled as a
           single clipped line so the row cannot grow without bound. */
        && strstr(document.html,
                  ".snippet{display:block;font-size:11px") != NULL
        && strstr(document.html,
                  "white-space:nowrap;overflow:clip;"
                  "text-overflow:ellipsis}") != NULL
        && strstr(document.html,
                  "<span class=snippet>Hands-on with the PSP "
                  "&amp; homebrew</span>") != NULL
        && strstr(document.html,
                  "<span class=snippet>Desktop snippet shape</span>")
               != NULL
        && strstr(document.html, "<span class=snippet></span>") == NULL
        && strstr(document.html, "<script") == NULL;
    youtube_lite_document_destroy(&document);
    if (!search_ok || budget.current != 0) return false;

    if (!youtube_lite_build_document(
            &budget,
            "https://www.youtube.com/results?"
            "search_query=PSP+%26+homebrew&tilefinch_layout=compact",
            search_source, sizeof(search_source) - 1,
            &document, error, sizeof(error))) {
        fprintf(stderr, "youtube lite compact search: %s\n", error);
        return false;
    }
    bool compact_body =
        strstr(document.html, "<body class=compact-results>") != NULL;
    bool compact_has_no_page_switch =
        strstr(document.html, "view-switch") == NULL
        && strstr(document.html, "tilefinch_layout") == NULL;
    bool compact_card_css = strstr(document.html,
                  ".compact-results .card{min-height:60px;"
                  "margin-bottom:6px;gap:8px}") != NULL;
    bool compact_thumb_css = strstr(document.html,
                  ".compact-results .thumb-wrap{width:106px;height:60px}")
               != NULL;
    bool compact_hide_css = strstr(document.html,
                  ".compact-results .snippet{display:none}") != NULL;
    bool compact_duration_overlay = strstr(document.html,
                  "<span class=duration>3:14</span>") != NULL
        && strstr(document.html,
                  ".duration{position:absolute;right:5px;bottom:4px;")
               != NULL;
    bool compact_search_ok =
        document.route == YOUTUBE_LITE_ROUTE_SEARCH
        && document.result_count == 2 && compact_body
        && compact_has_no_page_switch && compact_card_css && compact_thumb_css
        && compact_hide_css && compact_duration_overlay
        && strstr(document.html, "<a class=card autofocus") != NULL;
    if (!compact_search_ok) {
        fprintf(stderr, "youtube compact markup: body=%d no-switch=%d "
                "card=%d thumb=%d hide=%d duration=%d\n",
                compact_body, compact_has_no_page_switch,
                compact_card_css, compact_thumb_css, compact_hide_css,
                compact_duration_overlay);
    }
    PocDocument compact_document = {0};
    Stylesheet compact_sheet = {0};
    LayoutDocument compact_layout = {0};
    bool compact_parsed = compact_search_ok && document_parse(
        &compact_document, layout_budget, document.html,
        document.html_length, 17);
    bool compact_styled = compact_parsed && stylesheet_build(
        &compact_sheet, layout_budget, &compact_document, 480);
    bool compact_laid_out = compact_styled && layout_build(
        &compact_layout, layout_budget, &compact_document,
        &compact_sheet, NULL, NULL, 480);
    compact_search_ok = compact_search_ok && compact_parsed
        && compact_styled && compact_laid_out;
    const LinkRegion *compact_first = NULL;
    const LinkRegion *compact_second = NULL;
    static const char watch_prefix[] =
        "https://www.youtube.com/watch?v=";
    for (size_t i = 0; compact_search_ok
         && i < compact_layout.link_count; i++) {
        const LinkRegion *link = &compact_layout.links[i];
        if (link->url_length < sizeof(watch_prefix) - 1u
            || memcmp(link->url, watch_prefix,
                      sizeof(watch_prefix) - 1u) != 0) continue;
        if (compact_first == NULL) compact_first = link;
        else if (link->y != compact_first->y) {
            compact_second = link;
            break;
        }
    }
    compact_search_ok = compact_search_ok
        && compact_first != NULL && compact_second != NULL
        && compact_first->height <= 64
        && compact_second->y - compact_first->y <= 68;
    if (!compact_search_ok) {
        fprintf(
            stderr,
            "youtube compact layout: links=%zu first=%p second=%p "
            "first-y=%d first-h=%d second-y=%d parse=%d style=%d layout=%d\n",
            compact_layout.link_count, (void *) compact_first,
            (void *) compact_second,
            compact_first == NULL ? -1 : compact_first->y,
            compact_first == NULL ? -1 : compact_first->height,
            compact_second == NULL ? -1 : compact_second->y,
            compact_parsed, compact_styled, compact_laid_out);
    } else {
        printf("youtube compact layout: first-y=%d height=%d step=%d\n",
               compact_first->y, compact_first->height,
               compact_second->y - compact_first->y);
    }
    layout_destroy(&compact_layout);
    stylesheet_destroy(&compact_sheet);
    document_destroy(&compact_document);
    youtube_lite_document_destroy(&document);
    if (!compact_search_ok || budget.current != 0) return false;

    /*
     * The snippet runs well past the 96 byte visible budget and places a
     * two-byte character across the cut, so the clamp has to fall back to the
     * preceding boundary rather than emit half a sequence.
     */
    static const char continuation_source[] =
        "{\"onResponseReceivedCommands\":[{\"appendContinuationItemsAction\":"
        "{\"continuationItems\":[{\"videoRenderer\":{"
        "\"title\":{\"runs\":[{\"text\":\"Third Video\"}]},"
        "\"descriptionSnippet\":{\"runs\":[{\"text\":"
        "\"Bounded snippets never copy an unbounded description, "
        "so this one runs far past the row budgets"
        "\xc3\xbc" "ber alles and onwards\"}]},"
        "\"videoId\":\"third_1234\"}},{\"continuationItemRenderer\":{"
        "\"continuationEndpoint\":{\"continuationCommand\":{"
        "\"token\":\"NEXT+/=\"}}}}]}}]}";
    if (!youtube_lite_build_document_with_comments(
            &budget,
            "https://www.youtube.com/results?search_query=PSP"
            "&tilefinch_token=FIRST",
            search_source, sizeof(search_source) - 1,
            continuation_source, sizeof(continuation_source) - 1,
            &document, error, sizeof(error))) {
        fprintf(stderr, "youtube lite search continuation: %s\n", error);
        return false;
    }
    bool continuation_ok =
        document.result_count == 1
        && strstr(document.html, "Third Video") != NULL
        && strstr(document.html, "<a class=card autofocus") != NULL
        && strstr(document.html, "Load more results") != NULL
        && strstr(document.html, "NEXT%2B%2F%3D") != NULL
        && strstr(document.html,
                  "<span class=snippet>Bounded snippets never copy an "
                  "unbounded description, so this one runs far past the "
                  "row budgets" "\xe2\x80\xa6" "</span>") != NULL
        && strstr(document.html, "\xc3\xbc") == NULL
        && strstr(document.html, "ber alles") == NULL
        && strstr(document.html, "PSP &amp; Guide") == NULL;
    youtube_lite_document_destroy(&document);
    if (!continuation_ok || budget.current != 0) return false;

    if (!youtube_lite_build_document_with_comments(
            &budget,
            "https://www.youtube.com/results?search_query=PSP"
            "&tilefinch_layout=compact&tilefinch_token=FIRST",
            search_source, sizeof(search_source) - 1,
            continuation_source, sizeof(continuation_source) - 1,
            &document, error, sizeof(error))) {
        fprintf(stderr, "youtube lite compact continuation: %s\n", error);
        return false;
    }
    bool compact_continuation_ok =
        document.result_count == 1
        && strstr(document.html, "<body class=compact-results>") != NULL
        && strstr(document.html,
                  "results?search_query=PSP&amp;"
                  "tilefinch_token=NEXT%2B%2F%3D") != NULL
        && strstr(document.html, "tilefinch_layout") == NULL;
    youtube_lite_document_destroy(&document);
    if (!compact_continuation_ok || budget.current != 0) return false;

    if (!youtube_lite_build_document(
            &budget, "https://www.youtube.com/@fixturechannel",
            search_source, sizeof(search_source) - 1,
            &document, error, sizeof(error))) {
        fprintf(stderr, "youtube lite channel: %s\n", error);
        return false;
    }
    bool channel_ok =
        document.route == YOUTUBE_LITE_ROUTE_CHANNEL
        && strstr(document.html, "Channel videos") != NULL
        && strstr(document.html, "PSP &amp; Guide") != NULL;
    youtube_lite_document_destroy(&document);
    if (!channel_ok || budget.current != 0) return false;

    static const char watch_source[] =
        "<script>var ytInitialPlayerResponse = null;</script>"
        "<script>var ytInitialPlayerResponse = {"
        "\"videoDetails\":{\"videoId\":\"TFTEST00001\","
        "\"title\":\"Fixture \\u0076ideo & friends\","
        "\"author\":\"Fixture channel\",\"lengthSeconds\":\"19\","
        "\"viewCount\":\"350000000\","
        "\"shortDescription\":\"The first line.\\nSecond <line>.\"},"
        "\"microformat\":{\"playerMicroformatRenderer\":{"
        "\"publishDate\":\"2005-04-23\",\"category\":\"People & Blogs\""
        "}}};</script>";
    if (!youtube_lite_build_document(
            &budget,
            "https://www.youtube.com/watch?v=TFTEST00001"
            "&search_query=PSP+%26+homebrew",
            watch_source, sizeof(watch_source) - 1,
            &document, error, sizeof(error))) {
        fprintf(stderr, "youtube lite watch: %s\n", error);
        return false;
    }
    bool watch_ok =
        document.route == YOUTUBE_LITE_ROUTE_WATCH
        && document.result_count == 0
        && strstr(document.html, "Fixture video &amp; friends") != NULL
        && strstr(document.html, "Fixture channel") != NULL
        && strstr(document.html, "0:19") != NULL
        && strstr(document.html, "Published 2005-04-23") != NULL
        && strstr(document.html, "Category: People &amp; Blogs") != NULL
        && strstr(document.html,
                  "The first line.<br>Second &lt;line&gt;.") != NULL
        && strstr(document.html, "View full description") != NULL
        && strstr(document.html, "View comments") != NULL
        && strstr(document.html, "Play in native player") != NULL
        && strstr(
               document.html,
               "tilefinch.local/offline/youtube?id=TFTEST00001") != NULL
        && strstr(document.html, "value=\"PSP &amp; homebrew\"") != NULL
        && strstr(document.html, "<section class=watch-hero>") != NULL
        && strstr(
               document.html, "<a class=watch-target autofocus") != NULL
        /*
         * The overlay badge is painted by the block decoration path, not
         * decoded as an image.  Pin the whole recipe -- 50x38 at radius 12 in
         * #25282b/76%, centred on left 50% / top 64px -- and pin that no
         * inline <svg> comes back: an inline SVG only paints when the image
         * pipeline runs, so it disappeared under an image cap.
         */
        && strstr(document.html,
                  ".play-icon{display:block;position:absolute;"
                  "left:50%;top:64px;width:50px;height:38px;"
                  "margin-left:-25px;margin-top:-19px;border-radius:12px;"
                  "background:rgba(37,40,43,.76);z-index:1}") != NULL
        && strstr(document.html,
                  "<span class=play-icon aria-hidden=true></span>") != NULL
        && strstr(document.html, "<svg") == NULL
        && strstr(document.html, "fill-opacity") == NULL
        && strstr(document.html,
                  "<span class=play-glyph aria-hidden=true>"
                  "&#9658;</span>") != NULL
        && strstr(document.html, "<span class=mark>") == NULL
        && strstr(document.html,
                  "line-height:32px;text-align:center;") != NULL
        && strstr(document.html,
                  "background:#f00;border:2px solid #fff") == NULL
        && strstr(document.html,
                  "i.ytimg.com/vi/TFTEST00001/hqdefault.jpg") != NULL;
    youtube_lite_document_destroy(&document);
    if (!watch_ok || budget.current != 0) return false;

    if (!youtube_lite_build_document(
            &budget,
            "https://www.youtube.com/watch?v=TFTEST00001"
            "&tilefinch_view=description",
            watch_source, sizeof(watch_source) - 1,
            &document, error, sizeof(error))) {
        fprintf(stderr, "youtube lite description: %s\n", error);
        return false;
    }
    bool description_ok =
        strstr(document.html,
               "The first line.<br>Second &lt;line&gt;.") != NULL
        && strstr(document.html, "Back to video") != NULL
        && strstr(document.html, "Play in native player") == NULL
        && strstr(document.html, "View comments") == NULL;
    youtube_lite_document_destroy(&document);
    if (!description_ok || budget.current != 0) return false;

    static const char comments_source[] =
        "{\"onResponseReceivedEndpoints\":[{"
        "\"commentsHeaderRenderer\":{\"commentsCount\":{\"runs\":["
        "{\"text\":\"10M\"}]}},\"commentThreadRenderer\":{\"comment\":{"
        "\"commentRenderer\":{\"authorText\":{\"runs\":[{\"text\":"
        "\"@visitor\"}]},\"contentText\":{\"runs\":["
        "{\"text\":\"First & \"},{\"text\":\"best <comment>\"}]},"
        "\"publishedTimeText\":{\"runs\":[{\"text\":\"today\"}]},"
        "\"voteCount\":{\"runs\":[{\"text\":\"42\"}]}}}}},"
        "\"continuationItemRenderer\":{\"continuationEndpoint\":{"
        "\"continuationCommand\":{\"token\":\"MORE+/=\"}}}}]}";
    if (!youtube_lite_build_document_with_comments(
            &budget,
            "https://www.youtube.com/watch?v=TFTEST00001"
            "&tilefinch_view=comments",
            watch_source, sizeof(watch_source) - 1,
            comments_source, sizeof(comments_source) - 1,
            &document, error, sizeof(error))) {
        fprintf(stderr, "youtube lite comments: %s\n", error);
        return false;
    }
    bool comments_ok =
        document.result_count == 0
        && strstr(document.html, "Comments (10M)") != NULL
        && strstr(document.html, "@visitor") != NULL
        && strstr(document.html,
                  "First &amp; best &lt;comment&gt;") != NULL
        && strstr(document.html, "42 likes") != NULL
        && strstr(document.html, "Load more comments") != NULL
        && strstr(document.html, "MORE%2B%2F%3D") != NULL
        && strstr(document.html, "View comments") == NULL;
    youtube_lite_document_destroy(&document);
    return comments_ok && budget.current == 0;
}

static int test_foundation(void)
{
    puts("test: budget");
    Budget budget;
    budget_init(&budget, 16 * MIB);
    budget_install_lexbor(&budget);
    CHECK(strcmp(budget_category_name((BudgetCategory) -1),
                 "uncategorized") == 0
          && strcmp(budget_pressure_reason_name((BudgetPressureReason) -1),
                    "invalid") == 0);

    puts("test: PSP network reinitialization policy");
    CHECK(test_psp_network_reinitialization_policy());
    puts("test: bounded YouTube player response parser");
    CHECK(test_youtube_player_response_parser());
    puts("test: bounded YouTube lightweight page builder");
    CHECK(test_youtube_lite_document_builder(&budget));
    puts("test: YouTube adapter commit parses cooperatively");
    CHECK(test_youtube_lite_commit_cooperates(&budget));
    CHECK(budget.current == 0);

    puts("test: incremental HTML parser boundary equivalence");
    CHECK(test_incremental_parser_boundaries(&budget));
    CHECK(test_large_parser_feed_is_cooperative(&budget));
    CHECK(budget.current == 0);

    puts("test: scripting-aware noscript parsing");
    CHECK(test_parser_scripting_noscript_model(&budget));
    CHECK(budget.current == 0);

    puts("test: parser lifecycle close-element checkpoints");
    CHECK(test_parser_lifecycle_hook(&budget));
    CHECK(budget.current == 0);
    puts("test: parser-blocking innerHTML reentrancy");
    CHECK(test_parser_reentrant_inner_html(&budget));
    CHECK(budget.current == 0);
    puts("test: reentrant incumbent DOM mutation owner isolation");
    CHECK(test_reentrant_incumbent_owner_isolation(&budget));

    puts("test: constant-stack deep document and image traversal");
    CHECK(test_deep_document_resource_walkers(&budget));

    puts("test: bounded deterministic response streaming");
    CHECK(test_streaming_replay(&budget));
    CHECK(budget.current == 0);

    puts("test: lab-only response-keyed visual replay");
    CHECK(test_response_keyed_visual_replay(&budget));
    CHECK(budget.current == 0);

    puts("test: streaming base, viewport, and referrer discovery");
    CHECK(test_streaming_navigation_metadata(&budget));
    CHECK(budget.current == 0);

    puts("test: streaming parser-blocking script lifecycle");
    CHECK(test_streaming_navigation_lifecycle(&budget));
    CHECK(budget.current == 0);

    puts("test: streaming stylesheet checkpoint reuse");
    CHECK(test_streaming_stylesheet_checkpoint_reuse(&budget));
    CHECK(budget.current == 0);

    puts("test: streaming allocation-phase recovery sweep");
    CHECK(test_streaming_navigation_allocation_sweep(&budget));
    CHECK(budget.current == 0);

    puts("test: bounded image-node cap retains allocation ownership");
    CHECK(test_image_node_cap_ownership(&budget));
    CHECK(test_image_refresh_transfers_shared_ownership(&budget));
    CHECK(test_decoded_media_surface_ownership(&budget));
    CHECK(test_decoded_media_surface_replaces_poster(&budget));
    CHECK(test_mutable_surface_cache_invalidation(&budget));
    CHECK(test_picture_source_media_selection(&budget));
    puts("test: bounded inline SVG raster resource");
    CHECK(test_inline_svg_resource(&budget));
    CHECK(test_visible_inline_svg_priority_resource(&budget));
    CHECK(test_bounded_large_data_svg_resource(&budget));
    puts("test: bounded extreme SVG raster edge");
    CHECK(test_inline_svg_extreme_edge_resource(&budget));
    puts("test: SVG presentation cascade and generic use targets");
    CHECK(test_inline_svg_presentation_resource(&budget));
    puts("test: bounded inline SVG symbol/use raster resource");
    CHECK(test_inline_svg_symbol_use_resource(&budget));
    CHECK(test_external_svg_symbol_use_resource(&budget));
    CHECK(test_stylesheet_shared_cache_body(&budget));
    CHECK(test_stylesheet_transient_retry_recovers(&budget));
    CHECK(test_stylesheet_transient_retry_cap(&budget));
    CHECK(test_script_shared_cache_lifetimes(&budget));
    CHECK(test_image_virtual_cancel_bound(&budget));
    CHECK(test_image_request_no_progress_bound(&budget));
    CHECK(test_image_request_policy_and_cookies(&budget));
    CHECK(budget.current == 0);

    puts("test: installable platform services");
    uint64_t platform_clock = 123456;
    TilefinchPlatformServices platform_services = {
        .context = &platform_clock,
        .wall_time_ns = test_platform_wall,
        .monotonic_time_ns = test_platform_monotonic,
        .monotonic_time_us = test_platform_monotonic_us,
        .secure_random = test_platform_random,
        .read_asset = test_platform_asset,
        .poll_input = test_platform_input,
        .present_rgb565 = test_platform_present,
        .cooperate = test_platform_cooperate
    };
    tilefinch_platform_set_services(&platform_services);
    unsigned char random_bytes[4] = {0};
    unsigned char *asset_bytes = NULL;
    size_t asset_length = 0;
    TilefinchPlatformInput platform_input = {0};
    uint16_t platform_pixel = 0xbeef;
    CHECK(tilefinch_platform_wall_time_ns() == platform_clock
          && tilefinch_platform_monotonic_time_ns() == platform_clock + 1
          && tilefinch_platform_monotonic_time_us() == platform_clock + 2
          && tilefinch_platform_secure_random(random_bytes,
                                           sizeof(random_bytes))
          && random_bytes[0] == 0xa5 && random_bytes[3] == 0xa5
          && tilefinch_platform_read_asset(&budget, "test.asset", 3,
                                        &asset_bytes, &asset_length)
          && asset_length == 3 && memcmp(asset_bytes, "PSP", 3) == 0
          && tilefinch_platform_poll_input(&platform_input)
          && platform_input.buttons == 0x1234
          && platform_input.analog_x == -7 && platform_input.analog_y == 9
          && tilefinch_platform_present_rgb565(&platform_pixel, 1, 1, 1)
          && tilefinch_platform_cooperate("test", 7)
          && !tilefinch_platform_cooperate("wrong", 7));
    budget_free(&budget, asset_bytes);
    tilefinch_platform_set_services(NULL);
    CHECK(budget.current == 0);

    puts("test: division-free pixel and checked viewport math are exact");
    for (unsigned code = 0; code < 32u; code++) {
        CHECK(tilefinch_rgb5_to_u8(code) == code * 255u / 31u);
    }
    for (unsigned code = 0; code < 64u; code++) {
        CHECK(tilefinch_rgb6_to_u8(code) == code * 255u / 63u);
    }
    for (uint32_t value = 0; value <= UINT16_MAX; value++) {
        CHECK(tilefinch_div255_u16(value) == value / 255u);
    }
    for (uint32_t value = 0; value <= UINT32_C(100000); value++) {
        uint32_t root = tilefinch_isqrt_u32(value);
        CHECK((uint64_t) root * root <= value
              && (uint64_t) (root + 1u) * (root + 1u) > value);
    }
    static const uint32_t sqrt_edges[] = {
        UINT32_C(1000000), UINT32_C(2147395600), UINT32_MAX
    };
    for (size_t i = 0; i < sizeof(sqrt_edges) / sizeof(sqrt_edges[0]); i++) {
        uint32_t root = tilefinch_isqrt_u32(sqrt_edges[i]);
        CHECK((uint64_t) root * root <= sqrt_edges[i]
              && (uint64_t) (root + 1u) * (root + 1u) > sqrt_edges[i]);
    }
    static const int scale_values[] = {
        INT_MIN, -1000000000, -983, -481, -1, 0, 1, 479, 980,
        1000000000, INT_MAX
    };
    static const int scale_numerators[] = {1, 3, 480, 983, INT_MAX};
    static const int scale_denominators[] = {1, 2, 480, 983, INT_MAX};
    for (size_t value_index = 0;
         value_index < sizeof(scale_values) / sizeof(scale_values[0]);
         value_index++) {
        for (size_t numerator_index = 0;
             numerator_index < sizeof(scale_numerators)
                               / sizeof(scale_numerators[0]);
             numerator_index++) {
            for (size_t denominator_index = 0;
                 denominator_index < sizeof(scale_denominators)
                                     / sizeof(scale_denominators[0]);
                 denominator_index++) {
                int value = scale_values[value_index];
                int numerator = scale_numerators[numerator_index];
                int denominator = scale_denominators[denominator_index];
                int64_t product = (int64_t) value * numerator;
                int64_t floor64 = product >= 0
                    ? product / denominator
                    : -((-product + denominator - 1) / denominator);
                int64_t ceil64 = product >= 0
                    ? (product + denominator - 1) / denominator
                    : -((-product) / denominator);
                if (floor64 < INT_MIN || floor64 > INT_MAX
                    || ceil64 < INT_MIN || ceil64 > INT_MAX) continue;
                int expected_floor = (int) floor64;
                int expected_ceil = (int) ceil64;
                CHECK(viewport_scale_floor(value, numerator, denominator)
                          == expected_floor
                      && viewport_scale_ceil(value, numerator, denominator)
                          == expected_ceil);
            }
        }
    }

    void *probe = budget_malloc(&budget, 64);
    CHECK(probe != NULL);
    probe = budget_realloc(&budget, probe, 128);
    CHECK(probe != NULL && budget_usable_size(probe) == 128);
    CHECK(budget_malloc(&budget, 32 * MIB) == NULL);
    budget_free(&budget, probe);
    unsigned char *zero_calloc = budget_calloc(&budget, 0, 32);
    CHECK(zero_calloc != NULL && budget_usable_size(zero_calloc) == 1
          && zero_calloc[0] == 0);
    budget_free(&budget, zero_calloc);

    puts("test: empty-ledger recovery remains diagnosable");
    Budget repair_budget;
    budget_init(&repair_budget, 1024);
    void *repair_probe = budget_malloc(&repair_budget, 32);
    CHECK(repair_probe != NULL);
    repair_budget.current++;
    budget_free(&repair_budget, repair_probe);
    CHECK(repair_budget.current == 0
          && repair_budget.accounting_repair_count == 1
          && !budget_categories_reconcile(&repair_budget));

    puts("test: allocator ownership rejects cross-budget operations");
    Budget owner_a, owner_b;
    budget_init(&owner_a, 1024);
    budget_init(&owner_b, 1024);
    unsigned char *owned = budget_malloc(&owner_a, 32);
    CHECK(owned != NULL && budget_owns(&owner_a, owned)
          && !budget_owns(&owner_b, owned)
          && !budget_owns(NULL, owned)
          && !budget_owns(&owner_a, NULL));
    memset(owned, 0x5a, 32);
    size_t owner_a_current = owner_a.current;
    size_t owner_a_allocations = owner_a.allocation_count;
    budget_free(&owner_b, owned);
    CHECK(budget_owns(&owner_a, owned)
          && owner_a.current == owner_a_current
          && owner_a.allocation_count == owner_a_allocations
          && owner_a.free_count == 0
          && owner_b.current == 0 && owner_b.allocation_count == 0
          && owner_b.free_count == 0 && owned[0] == 0x5a
          && owned[31] == 0x5a);
    CHECK(budget_realloc(&owner_b, owned, 64) == NULL
          && budget_owns(&owner_a, owned)
          && budget_usable_size(owned) == 32
          && owner_a.current == owner_a_current
          && owner_a.allocation_count == owner_a_allocations
          && owner_a.free_count == 0
          && owner_b.current == 0 && owner_b.allocation_count == 0
          && owner_b.free_count == 0 && owner_b.failure_count == 0
          && owned[0] == 0x5a && owned[31] == 0x5a);
    CHECK(budget_realloc(&owner_b, owned, 0) == NULL
          && budget_owns(&owner_a, owned)
          && budget_usable_size(owned) == 32
          && owner_a.current == owner_a_current
          && owner_a.free_count == 0 && owner_b.free_count == 0);
    budget_free(&owner_a, owned);
    CHECK(owner_a.current == 0 && owner_a.free_count == 1
          && owner_b.current == 0 && owner_b.free_count == 0);

    puts("test: process-global Lexbor allocator ownership is non-replacing");
    Budget lexbor_a, lexbor_b;
    budget_init(&lexbor_a, 4 * MIB);
    budget_init(&lexbor_b, 4 * MIB);
    PocDocument live_lexbor_document = {0};
    static const char live_lexbor_html[] =
        "<!doctype html><title>owned</title><body>live";
    CHECK(budget_install_lexbor(&lexbor_a)
          && budget_lexbor_is_installed(&lexbor_a));
    CHECK(document_parse(&live_lexbor_document, &lexbor_a,
                         live_lexbor_html,
                         sizeof(live_lexbor_html) - 1, 16));
    CHECK(!budget_install_lexbor(&lexbor_b));
    CHECK(!budget_uninstall_lexbor(&lexbor_a)
          && budget_lexbor_is_installed(&lexbor_a));
    DocumentParser wrong_owner_parser;
    CHECK(!document_parser_begin(&wrong_owner_parser, &lexbor_b)
          && wrong_owner_parser.budget == NULL
          && lexbor_b.current == 0);
    document_parser_abort(&wrong_owner_parser);
    document_destroy(&live_lexbor_document);
    CHECK(lexbor_a.current == 0
          && budget_install_lexbor(&lexbor_b)
          && budget_lexbor_is_installed(&lexbor_b));
    PocDocument second_owner_document = {0};
    CHECK(document_parse(&second_owner_document, &lexbor_b,
                         live_lexbor_html,
                         sizeof(live_lexbor_html) - 1, 16));
    document_destroy(&second_owner_document);
    CHECK(lexbor_b.current == 0
          && budget_uninstall_lexbor(&lexbor_b)
          && !budget_lexbor_is_installed(&lexbor_b)
          && budget_install_lexbor(&budget));

    puts("test: allocation category reconciliation");
    void *dom_probe = budget_malloc_category(
        &budget, BUDGET_CATEGORY_DOM, 17);
    void *layout_probe = budget_calloc_category(
        &budget, BUDGET_CATEGORY_LAYOUT, 3, 11);
    CHECK(dom_probe != NULL && layout_probe != NULL
          && budget_categories_reconcile(&budget)
          && budget.categories[BUDGET_CATEGORY_DOM].active_allocations == 1
          && budget.categories[BUDGET_CATEGORY_LAYOUT].active_allocations == 1
          && budget.categories[BUDGET_CATEGORY_DOM].current > 17
          && budget.categories[BUDGET_CATEGORY_LAYOUT].current > 33);
    size_t dom_charge = budget.categories[BUDGET_CATEGORY_DOM].current;
    dom_probe = budget_realloc_category(
        &budget, BUDGET_CATEGORY_STYLE, dom_probe, 49);
    CHECK(dom_probe != NULL && budget_categories_reconcile(&budget)
          && budget.categories[BUDGET_CATEGORY_DOM].current
             == dom_charge + 32
          && budget.categories[BUDGET_CATEGORY_STYLE].current == 0);
    budget_free(&budget, layout_probe);
    budget_free(&budget, dom_probe);
    CHECK(budget.current == 0 && budget_categories_reconcile(&budget)
          && budget.categories[BUDGET_CATEGORY_DOM].active_allocations == 0
          && budget.categories[BUDGET_CATEGORY_LAYOUT].active_allocations == 0);
    puts("test: category-scoped parser rollback preserves session ownership");
    uint64_t parser_checkpoint = budget_checkpoint(&budget);
    char *parser_owned = budget_malloc_category(
        &budget, BUDGET_CATEGORY_DOM, 32);
    char *session_owned = budget_malloc_category(
        &budget, BUDGET_CATEGORY_SESSION, 32);
    CHECK(parser_owned != NULL && session_owned != NULL);
    memcpy(session_owned, "session-survives", 17);
    budget_rollback_category(&budget, parser_checkpoint,
                             BUDGET_CATEGORY_DOM);
    CHECK(strcmp(session_owned, "session-survives") == 0
          && budget.categories[BUDGET_CATEGORY_DOM].active_allocations == 0
          && budget.categories[BUDGET_CATEGORY_SESSION].active_allocations == 1
          && budget_categories_reconcile(&budget));
    budget_free(&budget, session_owned);
    CHECK(budget.current == 0 && budget_categories_reconcile(&budget));
    puts("test: allocation-owner rollback isolates overlapping DOM work");
    BudgetAllocationOwner parser_owner = BUDGET_ALLOCATION_OWNER_NONE;
    CHECK(budget_allocation_owner_create(&budget, &parser_owner));
    BudgetAllocationOwner previous_owner = budget_allocation_owner_enter(
        &budget, parser_owner);
    char *candidate_dom = budget_malloc_category(
        &budget, BUDGET_CATEGORY_DOM, 32);
    budget_allocation_owner_leave(&budget, previous_owner);
    candidate_dom = budget_realloc_category(
        &budget, BUDGET_CATEGORY_DOM, candidate_dom, 64);
    char *incumbent_dom = budget_malloc_category(
        &budget, BUDGET_CATEGORY_DOM, 32);
    CHECK(candidate_dom != NULL && incumbent_dom != NULL);
    memcpy(incumbent_dom, "incumbent-survives", 19);
    budget_rollback_owner_category(&budget, parser_owner,
                                   BUDGET_CATEGORY_DOM);
    CHECK(strcmp(incumbent_dom, "incumbent-survives") == 0
          && budget.active_allocation_owner
                 == BUDGET_ALLOCATION_OWNER_NONE
          && budget.categories[BUDGET_CATEGORY_DOM].active_allocations == 1
          && budget_categories_reconcile(&budget));
    budget_free(&budget, incumbent_dom);
    CHECK(budget.current == 0 && budget_categories_reconcile(&budget));
    puts("test: allocation-free pressure telemetry");
    size_t allocation_count_before_pressure = budget.allocation_count;
    CHECK(!budget_pressure_required(&budget, 1 * MIB, 1 * MIB));
    budget_record_pressure(&budget, BUDGET_PRESSURE_TILE, 4096, 1024);
    budget_record_pressure(&budget, BUDGET_PRESSURE_TILE, 2048, 0);
    budget_record_pressure(&budget, BUDGET_PRESSURE_CACHE, 8192, 4096);
    CHECK(budget.pressure_decisions == 3
          && budget.pressure_avoided_bytes == 14336
          && budget.pressure_saved_bytes == 5120
          && budget.pressure[BUDGET_PRESSURE_TILE].decisions == 2
          && budget.pressure[BUDGET_PRESSURE_TILE].avoided_bytes == 6144
          && budget.pressure[BUDGET_PRESSURE_CACHE].saved_bytes == 4096
          && budget.allocation_count == allocation_count_before_pressure
          && budget.current == 0);
    puts("test: deterministic allocator failure and recovery");
    size_t injected_before = budget.injected_failure_count;
    budget_inject_failure_after(&budget, 1);
    void *injection_first = budget_malloc(&budget, 24);
    CHECK(injection_first != NULL && budget_malloc(&budget, 24) == NULL
          && budget.injected_failure_count == injected_before + 1);
    void *injection_recovered = budget_malloc(&budget, 24);
    CHECK(injection_recovered != NULL);
    budget_inject_failure_after(&budget, 0);
    CHECK(budget_realloc(&budget, injection_recovered, 48) == NULL
          && budget_usable_size(injection_recovered) == 24);
    injection_recovered = budget_realloc(&budget, injection_recovered, 48);
    CHECK(injection_recovered != NULL
          && budget_usable_size(injection_recovered) == 48);
    budget_free(&budget, injection_first);
    budget_free(&budget, injection_recovered);
    CHECK(budget.current == 0);

    puts("test: deterministic transport failure reporting");
    static const FetchInjectedFailure transport_failures[] = {
        FETCH_INJECT_TIMEOUT, FETCH_INJECT_TLS, FETCH_INJECT_REDIRECT,
        FETCH_INJECT_TRUNCATED, FETCH_INJECT_CANCELLED
    };
    static const char *transport_errors[] = {
        "injected transport timeout", "injected TLS failure",
        "injected redirect failure",
        "injected truncated response", "injected request cancellation"
    };
    for (size_t i = 0; i < sizeof(transport_failures)
                           / sizeof(transport_failures[0]); i++) {
        FetchResult injected = {0};
        fetch_inject_failure_once(transport_failures[i]);
        CHECK(!fetch_url(&budget, "https://injected.invalid/", 1024, 100,
                         &injected)
              && strcmp(injected.error, transport_errors[i]) == 0
              && injected.timed_out
                 == (transport_failures[i] == FETCH_INJECT_TIMEOUT));
        fetch_result_destroy(&injected);
    }
    CHECK(budget.current == 0);

    puts("test: bounded HTTP cache policy, eviction, and OOM recovery");
    BrowserSession cache_policy_session;
    static const unsigned char cache_old[] = "old-data";
    static const unsigned char cache_new[] = "new-data";
    CHECK(browser_session_init(&cache_policy_session, &budget, 24)
          && browser_session_cache_put_http(
              &cache_policy_session, "https://cache.test/fresh", cache_old,
              sizeof(cache_old) - 1, "v1", NULL, "text/plain",
              "public, max-age=10", "accept-encoding", UINT64_C(100)));
    const BrowserCacheEntry *cache_match = NULL;
    CHECK(browser_session_cache_match_http(
              &cache_policy_session, "https://cache.test/fresh",
              UINT64_C(9000000100), &cache_match) == BROWSER_CACHE_FRESH
          && cache_match != NULL
          && browser_session_cache_match_http(
              &cache_policy_session, "https://cache.test/fresh",
              UINT64_C(11000000100), &cache_match) == BROWSER_CACHE_STALE
          && browser_session_cache_revalidate(
              &cache_policy_session, "https://cache.test/fresh", "", "",
              UINT64_C(11000000100))
          && browser_session_cache_match_http(
              &cache_policy_session, "https://cache.test/fresh",
              UINT64_C(20000000100), &cache_match) == BROWSER_CACHE_FRESH);
    budget_inject_failure_after(&budget, 0);
    CHECK(!browser_session_cache_put_http(
              &cache_policy_session, "https://cache.test/fresh", cache_new,
              sizeof(cache_new) - 1, "v2", NULL, "text/plain",
              "max-age=20", NULL, UINT64_C(200))
          && browser_session_cache_get(
              &cache_policy_session, "https://cache.test/fresh", NULL, NULL)
          && cache_match != NULL
          && memcmp(cache_match->data, cache_old, sizeof(cache_old) - 1) == 0
          && browser_session_cache_put_http(
              &cache_policy_session, "https://cache.test/fresh", cache_new,
              sizeof(cache_new) - 1, "v2", NULL, "text/plain",
              "no-cache, must-revalidate", NULL, UINT64_C(200))
          && browser_session_cache_match_http(
              &cache_policy_session, "https://cache.test/fresh",
              UINT64_C(201), &cache_match) == BROWSER_CACHE_STALE);
    CHECK(!browser_session_cache_put_http(
              &cache_policy_session, "https://cache.test/fresh", cache_old,
              sizeof(cache_old) - 1, NULL, NULL, NULL, "no-store", NULL, 0)
          && !browser_session_cache_get(
              &cache_policy_session, "https://cache.test/fresh", NULL, NULL)
          && !browser_session_cache_put_http(
              &cache_policy_session, "https://cache.test/vary", cache_old,
              sizeof(cache_old) - 1, NULL, NULL, NULL, "max-age=10", "*", 0)
          && !browser_session_cache_put_http(
              &cache_policy_session, "https://cache.test/vary-origin",
              cache_old, sizeof(cache_old) - 1, NULL, NULL, NULL,
              "max-age=10", "origin", 0));
    unsigned char *classic_origin_data =
        budget_malloc(&budget, sizeof(cache_old) - 1);
    CHECK(classic_origin_data != NULL);
    memcpy(classic_origin_data, cache_old, sizeof(cache_old) - 1);
    BrowserSharedBody *classic_origin_body = browser_shared_body_take(
        &budget, classic_origin_data, sizeof(cache_old) - 1);
    TilefinchRequestContext classic_context = {
        .target_url = "https://cache.test/classic-vary-origin",
        .initiator_url = "https://page.test/",
        .top_level_url = "https://page.test/",
        .method = "GET", .mode = TILEFINCH_REQUEST_MODE_NO_CORS,
        .credentials = TILEFINCH_CREDENTIALS_INCLUDE,
        .destination = TILEFINCH_DESTINATION_SCRIPT
    };
    TilefinchResourceGrant classic_grant = {
        .destination = TILEFINCH_DESTINATION_SCRIPT,
        .mode = TILEFINCH_REQUEST_MODE_NO_CORS,
        .credentials = TILEFINCH_CREDENTIALS_INCLUDE,
        .corp = TILEFINCH_CORP_UNSPECIFIED
    };
    CHECK(classic_origin_body != NULL
          && browser_session_cache_put_http_shared_classic_script(
              &cache_policy_session,
              "https://cache.test/classic-vary-origin",
              classic_origin_body, NULL, NULL, "text/javascript",
              "max-age=10", "Accept-Encoding, Origin", 0,
              &classic_context, &classic_grant));
    browser_shared_body_release(classic_origin_body);
    CHECK(browser_session_cache_match_http(
              &cache_policy_session,
              "https://cache.test/classic-vary-origin", 1,
              &cache_match) == BROWSER_CACHE_MISS
          && browser_session_cache_match_classic_script(
              &cache_policy_session,
              "https://cache.test/classic-vary-origin", &classic_context, 1,
              &cache_match) == BROWSER_CACHE_FRESH
          && cache_match != NULL
          && browser_session_classic_script_bytecode_put(
              &cache_policy_session,
              "https://cache.test/classic-vary-origin",
              cache_old, sizeof(cache_old) - 1,
              cache_new, sizeof(cache_new) - 1));
    BrowserSharedBody *classic_bytecode =
        browser_session_classic_script_bytecode_acquire(
            &cache_policy_session,
            "https://cache.test/classic-vary-origin",
            cache_old, sizeof(cache_old) - 1);
    CHECK(classic_bytecode != NULL
          && classic_bytecode->length == sizeof(cache_new) - 1
          && memcmp(classic_bytecode->data, cache_new,
                    sizeof(cache_new) - 1) == 0);
    browser_shared_body_release(classic_bytecode);
    CHECK(browser_session_cache_put_http(
              &cache_policy_session, "https://cache.test/a", cache_old,
              sizeof(cache_old) - 1, NULL, NULL, NULL, "immutable", NULL, 0)
          && browser_session_cache_put_http(
              &cache_policy_session, "https://cache.test/b", cache_old,
              sizeof(cache_old) - 1, NULL, NULL, NULL, "immutable", NULL, 0)
          && browser_session_cache_put_http(
              &cache_policy_session, "https://cache.test/c", cache_old,
              sizeof(cache_old) - 1, NULL, NULL, NULL, "immutable", NULL, 0)
          && browser_session_cache_put_http(
              &cache_policy_session, "https://cache.test/d", cache_old,
              sizeof(cache_old) - 1, NULL, NULL, NULL, "immutable", NULL, 0)
          && cache_policy_session.cache_bytes <= 24
          && cache_policy_session.cache_evictions >= 1);
    browser_session_destroy(&cache_policy_session);
    CHECK(budget.current == 0);
    size_t pool_baseline = budget.current;
    BudgetQuickJSPool *quickjs_pool = budget_quickjs_pool_create(&budget);
    CHECK(quickjs_pool != NULL);
    JSRuntime *pool_runtime = JS_NewRuntime2(
        budget_quickjs_pool_allocator(), quickjs_pool);
    CHECK(pool_runtime != NULL);
    JS_SetMemoryLimit(pool_runtime, 4 * MIB);
    JSContext *pool_context = JS_NewContext(pool_runtime);
    CHECK(pool_context != NULL);
    static const char pool_source[] =
        "let total=0;for(let i=0;i<1000;i++){const f=x=>x+i;total+=f(1)};"
        "let truth=0;for(let i=0;i<100000;i++){if(!![])truth++;"
        "if(!{})truth=-1000000;}globalThis.poolSummary=total+truth;";
    JSValue pool_value = JS_Eval(pool_context, pool_source,
                                 sizeof(pool_source) - 1,
                                 "<pool-test>", JS_EVAL_TYPE_GLOBAL);
    CHECK(!JS_IsException(pool_value));
    JS_FreeValue(pool_context, pool_value);
    JSValue pool_global = JS_GetGlobalObject(pool_context);
    JSValue pool_summary = JS_GetPropertyStr(pool_context, pool_global,
                                             "poolSummary");
    int64_t pool_summary_value = 0;
    CHECK(JS_ToInt64(pool_context, &pool_summary_value, pool_summary) == 0
          && pool_summary_value == 600500);
    JS_FreeValue(pool_context, pool_summary);
    JS_FreeValue(pool_context, pool_global);
    CHECK(budget_quickjs_pool_reserved_peak(quickjs_pool) > 0);
    JS_FreeContext(pool_context);
    JS_FreeRuntime(pool_runtime);
    size_t pool_before_trim = budget.current;
    CHECK(budget_quickjs_pool_trim(quickjs_pool, 0) > 0
          && budget.current < pool_before_trim);
    CHECK(budget_quickjs_pool_destroy(quickjs_pool));
    CHECK(budget.current == pool_baseline);
#if defined(PSP_BROWSER_BELLARD_QUICKJS)
    puts("test: exported Bellard allocator enforces the configured limit");
    const JSMallocFunctions *plain_allocator = budget_quickjs_allocator();
    JSMallocState plain_state = {
        .malloc_limit = 100,
        .opaque = &budget
    };
    void *plain_value = plain_allocator->js_malloc(&plain_state, 64);
    CHECK(plain_value != NULL && plain_state.malloc_count == 1
          && plain_state.malloc_size == 64
          && plain_allocator->js_malloc(&plain_state, 37) == NULL
          && plain_allocator->js_realloc(
                 &plain_state, plain_value, 101) == NULL);
    plain_value = plain_allocator->js_realloc(
        &plain_state, plain_value, 32);
    CHECK(plain_value != NULL && plain_state.malloc_count == 1
          && plain_state.malloc_size == 32);
    plain_allocator->js_free(&plain_state, plain_value);
    CHECK(plain_state.malloc_count == 0 && plain_state.malloc_size == 0
          && budget.current == pool_baseline);

    puts("test: Bellard QuickJS pool enforces the configured active limit");
    BudgetQuickJSPool *limited_pool = budget_quickjs_pool_create(&budget);
    const JSMallocFunctions *limited_allocator =
        budget_quickjs_pool_allocator();
    JSMallocState limited_state = {
        .malloc_limit = 100,
        .opaque = limited_pool
    };
    CHECK(limited_pool != NULL && limited_allocator != NULL);
    void *limited_value = limited_allocator->js_malloc(&limited_state, 64);
    CHECK(limited_value != NULL && limited_state.malloc_count == 1
          && limited_state.malloc_size == 64);
    void *limited_resized = limited_allocator->js_realloc(
        &limited_state, limited_value, 96);
    CHECK(limited_resized != NULL && limited_state.malloc_count == 1
          && limited_state.malloc_size == 96);
    limited_value = limited_resized;
    CHECK(limited_allocator->js_realloc(
              &limited_state, limited_value, 101) == NULL
          && limited_state.malloc_count == 1
          && limited_state.malloc_size == 96);
    CHECK(limited_allocator->js_malloc(&limited_state, 5) == NULL
          && limited_state.malloc_count == 1
          && limited_state.malloc_size == 96);
    limited_resized = limited_allocator->js_realloc(
        &limited_state, limited_value, 32);
    CHECK(limited_resized != NULL && limited_state.malloc_count == 1
          && limited_state.malloc_size == 32);
    limited_value = limited_resized;
    void *limited_second = limited_allocator->js_realloc(
        &limited_state, NULL, 68);
    CHECK(limited_second != NULL && limited_state.malloc_count == 2
          && limited_state.malloc_size == 100);
    CHECK(limited_allocator->js_realloc(
              &limited_state, limited_second, 69) == NULL
          && limited_state.malloc_count == 2
          && limited_state.malloc_size == 100);
    limited_allocator->js_free(&limited_state, limited_second);
    CHECK(limited_state.malloc_count == 1 && limited_state.malloc_size == 32);
    CHECK(limited_allocator->js_realloc(
              &limited_state, limited_value, 0) == NULL
          && limited_state.malloc_count == 0
          && limited_state.malloc_size == 0
          && limited_allocator->js_malloc(&limited_state, 0) == NULL
          && limited_allocator->js_realloc(
                 &limited_state, NULL, 0) == NULL);
    puts("test: QuickJS pool preserves deterministic injected OOM");
    limited_state.malloc_limit = 1024;
    size_t pool_injected_before = budget.injected_failure_count;
    budget_inject_failure_after(&budget, 0);
    CHECK(limited_allocator->js_malloc(&limited_state, 257) == NULL
          && budget.injected_failure_count == pool_injected_before + 1
          && limited_state.malloc_count == 0);
    void *injected_pool_value =
        limited_allocator->js_malloc(&limited_state, 257);
    CHECK(injected_pool_value != NULL && limited_state.malloc_count == 1);
    limited_allocator->js_free(&limited_state, injected_pool_value);
    pool_injected_before = budget.injected_failure_count;
    budget_inject_failure_after(&budget, 0);
    CHECK(limited_allocator->js_malloc(&limited_state, 257) == NULL
          && budget.injected_failure_count == pool_injected_before + 1
          && limited_state.malloc_count == 0);
    injected_pool_value = limited_allocator->js_malloc(
        &limited_state, 257);
    CHECK(injected_pool_value != NULL && limited_state.malloc_count == 1);
    limited_allocator->js_free(&limited_state, injected_pool_value);
    CHECK(budget_quickjs_pool_trim(limited_pool, 0) > 0
          && budget_quickjs_pool_destroy(limited_pool)
          && budget.current == pool_baseline);
#endif
    char resolved[256];
    CHECK(fetch_resolve_url("https://en.wikipedia.org/wiki/PSP",
                            "../wiki/Sony", resolved, sizeof(resolved)));
    CHECK(strcmp(resolved, "https://en.wikipedia.org/wiki/Sony") == 0);
    CHECK(fetch_resolve_url("https://en.wikipedia.org/wiki/PSP",
                            "//commons.wikimedia.org/example", resolved,
                            sizeof(resolved)));
    CHECK(strcmp(resolved, "https://commons.wikimedia.org/example") == 0);
    CHECK(fetch_resolve_url("https://example.com/a/b?old=1", "/root?q=2",
                            resolved, sizeof(resolved)));
    CHECK(strcmp(resolved, "https://example.com/root?q=2") == 0);
    char referrer[256];
    CHECK(fetch_compute_referrer(
              "https://example.com/path?x=1#fragment",
              "https://other.test/resource", "same-origin",
              referrer, sizeof(referrer))
          && referrer[0] == '\0');
    CHECK(fetch_compute_referrer(
              "https://example.com/path?x=1#fragment",
              "https://example.com/resource", "same-origin",
              referrer, sizeof(referrer))
          && strcmp(referrer, "https://example.com/path?x=1") == 0);
    CHECK(fetch_compute_referrer(
              "https://example.com/path?x=1",
              "https://other.test/resource", "",
              referrer, sizeof(referrer))
          && strcmp(referrer, "https://example.com/") == 0);
    CHECK(fetch_compute_referrer(
              "https://example.com/path", "http://other.test/resource",
              "strict-origin", referrer, sizeof(referrer))
          && referrer[0] == '\0');
    FetchResult header_probe = {0};
    static const char header_block[] =
        "content-type: text/html\nreferrer-policy: same-origin\n";
    memcpy(header_probe.response_headers, header_block,
           sizeof(header_block) - 1);
    header_probe.response_headers_length = sizeof(header_block) - 1;
    CHECK(fetch_response_header_value(
              &header_probe, "Referrer-Policy",
              referrer, sizeof(referrer))
          && strcmp(referrer, "same-origin") == 0);

    puts("test: external stylesheet link media filtering");
    static const char media_link_page[] =
        "<!doctype html><meta name=viewport content='width=device-width'>"
        "<link rel=stylesheet media='screen and (min-width: 768px)' "
        "href='https://fixture.test/desktop.css'><body>MEDIA</body>";
    static const unsigned char media_link_css[] =
        "body{color:#ff0000}";
    PocDocument media_link_document;
    Stylesheet media_link_sheet;
    ExternalStylesheetStats media_link_stats;
    BrowserSession media_link_session;
    CHECK(document_parse(&media_link_document, &budget, media_link_page,
                         strlen(media_link_page), 8)
          && stylesheet_build(&media_link_sheet, &budget,
                              &media_link_document, 480)
          && browser_session_init(&media_link_session, &budget, 16 * 1024)
          && test_cache_put_stylesheet(
              &media_link_session,
              "https://fixture.test/desktop.css",
              media_link_css, sizeof(media_link_css) - 1,
              NULL)
          && stylesheets_load_external(
              &media_link_document, &media_link_sheet, &budget,
              "https://fixture.test/", 2, 64u * 1024u, 64u * 1024u, 1000,
              NULL, &media_link_session, &media_link_stats)
          && media_link_stats.discovered == 1
          && media_link_stats.attempted == 1
          && media_link_stats.loaded == 1
          && media_link_stats.cache_hits == 1
          && media_link_stats.skipped_media == 1
          && media_link_stats.rules_added == 0);
    browser_session_destroy(&media_link_session);
    stylesheet_destroy(&media_link_sheet);
    document_destroy(&media_link_document);
    CHECK(budget.current == 0);

    puts("test: cooperative resource discovery and cancellation");
    char resource_page[16384] = "<!doctype html><body>";
    size_t resource_length = strlen(resource_page);
    for (size_t i = 0; i < 600; i++) {
        int written = snprintf(
            resource_page + resource_length,
            sizeof(resource_page) - resource_length,
            "<div>%zu</div>", i);
        CHECK(written > 0
              && (size_t) written
                 < sizeof(resource_page) - resource_length);
        resource_length += (size_t) written;
    }
    CHECK(resource_length + sizeof("</body>") < sizeof(resource_page));
    memcpy(resource_page + resource_length, "</body>", sizeof("</body>"));
    resource_length += sizeof("</body>") - 1;
    PocDocument resource_document;
    Stylesheet resource_sheet;
    CHECK(document_parse(&resource_document, &budget, resource_page,
                         resource_length, 31)
          && stylesheet_build(&resource_sheet, &budget,
                              &resource_document, 480));
    size_t resource_baseline = budget.current;
    LayoutCooperateProbe resource_probe = {.cancel_resource_at = 1};
    TilefinchPlatformServices resource_services = {
        .context = &resource_probe,
        .cooperate = test_layout_cooperate
    };
    tilefinch_platform_set_services(&resource_services);
    ExternalStylesheetStats resource_stats;
    CHECK(!stylesheets_load_external(
              &resource_document, &resource_sheet, &budget,
              "https://resource.test/", 2, 64u * 1024u, 64u * 1024u,
              1000, NULL, NULL, &resource_stats)
          && resource_probe.resource_calls == 1
          && budget.current == resource_baseline);
    resource_probe = (LayoutCooperateProbe) {0};
    CHECK(stylesheets_load_external(
              &resource_document, &resource_sheet, &budget,
              "https://resource.test/", 2, 64u * 1024u, 64u * 1024u,
              1000, NULL, NULL, &resource_stats)
          && resource_probe.resource_calls >= 2
          && resource_stats.cooperative_yields
             == resource_probe.resource_calls
          && resource_stats.work_units > 600
          && resource_stats.max_slice_work_units <= 256
          && budget.current == resource_baseline);
    tilefinch_platform_set_services(NULL);
    stylesheet_destroy(&resource_sheet);
    document_destroy(&resource_document);
    CHECK(budget.current == 0);

    puts("test: mobile viewport policy and root horizontal overflow");
    static const char declared_viewport_page[] =
        "<!doctype html><meta name=VIEWPORT "
        "content='initial-scale=1,width = device-width'><body></body>";
    static const char numeric_viewport_page[] =
        "<!doctype html><meta name=viewport content='width=720'><body></body>";
    static const char legacy_viewport_page[] =
        "<!doctype html><style>body{margin:0}#wide{min-width:900px;height:10px}"
        "</style><body><div id=wide></div></body>";
    PocDocument declared_viewport_document;
    PocDocument numeric_viewport_document;
    PocDocument legacy_viewport_document;
    MobileViewport viewport;
    CHECK(document_parse(&declared_viewport_document, &budget,
                         declared_viewport_page,
                         strlen(declared_viewport_page), 17)
          && document_mobile_viewport(&declared_viewport_document, 480, 980,
                                      &viewport)
          && viewport.declared && viewport.device_width
          && viewport.layout_width == 480
          && viewport.scale_numerator == 1
          && viewport.scale_denominator == 1);
    CHECK(document_parse(&numeric_viewport_document, &budget,
                         numeric_viewport_page,
                         strlen(numeric_viewport_page), 17)
          && document_mobile_viewport(&numeric_viewport_document, 480, 980,
                                      &viewport)
          && viewport.declared && !viewport.device_width
          && viewport.layout_width == 720
          && viewport.scale_numerator == 480
          && viewport.scale_denominator == 720);
    CHECK(document_parse(&legacy_viewport_document, &budget,
                         legacy_viewport_page,
                         strlen(legacy_viewport_page), 17)
          && document_mobile_viewport(&legacy_viewport_document, 480, 980,
                                      &viewport)
          && !viewport.declared && viewport.layout_width == 980
          && viewport.scale_numerator == 480
          && viewport.scale_denominator == 980);
    Stylesheet legacy_viewport_sheet;
    LayoutDocument legacy_viewport_layout;
    CHECK(stylesheet_build(&legacy_viewport_sheet, &budget,
                           &legacy_viewport_document, 480)
          && layout_build(&legacy_viewport_layout, &budget,
                          &legacy_viewport_document, &legacy_viewport_sheet,
                          NULL, NULL, 480)
          && legacy_viewport_layout.width == 480
          && legacy_viewport_layout.scroll_width >= 900);
    layout_destroy(&legacy_viewport_layout);
    stylesheet_destroy(&legacy_viewport_sheet);
    document_destroy(&legacy_viewport_document);
    document_destroy(&numeric_viewport_document);
    document_destroy(&declared_viewport_document);

    puts("test: dual CSS and visual viewport integration");
    static const char dual_viewport_page[] =
        "<!doctype html><style>body{margin:0;background:#ffffff}"
        "#target{display:block;margin-left:700px;margin-top:40px;width:100px;"
        "height:40px;background:#aa0000}"
        "@media (min-width:900px){#target{background:#0000aa}}"
        "#vh-probe{display:block;height:50vh;width:11px}"
        "@media (min-height:500px){#vh-probe{width:33px}}"
        "#fixed{position:fixed;left:0;bottom:0;width:980px;height:20px;"
        "background:#00aa00}#tail{height:1200px}</style><body>"
        "<a id=target href=/scaled>scaled target</a><div id=vh-probe></div>"
        "<div id=tail></div>"
        "<div id=fixed></div><script>globalThis.pocSummary=[innerWidth,"
        "innerHeight,screen.width,screen.height,visualViewport.width,"
        "visualViewport.height,visualViewport.scale.toFixed(6),"
        "document.documentElement.clientWidth,"
        "document.documentElement.clientHeight,"
        "matchMedia('(min-width:900px)').matches,"
        "matchMedia('(min-height:500px)').matches].join(':')</script></body>";
    NavigationSession dual_navigation;
    CHECK(navigation_init(&dual_navigation, &budget, 2));
    navigation_enable_scripts(&dual_navigation, 2 * MIB, 1000);
    uint64_t dual_generation = navigation_begin(&dual_navigation);
    CHECK(navigation_commit_html(
              &dual_navigation, dual_generation,
              "https://viewport.test/", dual_viewport_page,
              strlen(dual_viewport_page), 480, NULL, NULL, true)
          && dual_navigation.page.layout.width == 980
          && dual_navigation.page.layout.viewport.css_height == 556
          && dual_navigation.page.layout.viewport.device_width == 480
          && dual_navigation.page.layout.viewport.device_height == 272
          && dual_navigation.page.layout.viewport.scale_numerator == 480
          && dual_navigation.page.layout.viewport.scale_denominator == 980
          && strcmp(dual_navigation.page.script_result.summary,
                    "980:556:480:272:980:556:0.489796:980:556:true:true")
             == 0
          && dual_navigation.page.layout.link_count == 1);
    lxb_dom_node_t *dual_vh_node = find_id(
        lxb_dom_interface_node(dual_navigation.page.document.html),
        "vh-probe");
    const LayoutNodeBox *dual_vh_box = layout_box_for_node(
        &dual_navigation.page.layout, dual_vh_node);
    CHECK(dual_vh_box != NULL && dual_vh_box->height == 278
          && dual_vh_box->width == 33);
    TileCache dual_cache;
    uint16_t *dual_frame = budget_malloc(
        &budget, 480u * 272u * sizeof(*dual_frame));
    CHECK(dual_frame != NULL
          && tile_cache_init(&dual_cache, &budget,
                             &dual_navigation.page.layout, 8)
          && dual_cache.owns_visual_layout
          && dual_cache.layout == &dual_cache.visual_layout
          && dual_cache.layout->width == 480
          && dual_cache.layout->viewport.css_height == 272
          && tile_cache_set_frame(&dual_cache, dual_frame, 480u * 272u)
          && tile_cache_render_frame(&dual_cache, 0, 480, 272, NULL));
    const LinkRegion *dual_visual_link = &dual_cache.layout->links[0];
    CHECK(dual_visual_link->x == layout_css_to_visual(
              &dual_navigation.page.layout,
              dual_navigation.page.layout.links[0].x)
          && dual_frame[(size_t) (dual_visual_link->y
                                  + dual_visual_link->height - 3) * 480u
                        + (size_t) (dual_visual_link->x
                                   + dual_visual_link->width - 3)]
             == test_rgb565(0x0000aa)
          && dual_frame[271u * 480u + 10u] == test_rgb565(0x00aa00));
    BrowserController dual_controller;
    CHECK(controller_init(&dual_controller, &dual_navigation)
          && dual_controller.viewport_height == 556
          && controller_focus_at(
              &dual_controller, dual_visual_link->x + 2,
              dual_visual_link->y + 2)
          && dual_controller.focus_kind == CONTROLLER_FOCUS_LINK);
    int dual_scroll_delta = layout_visual_to_css(
        &dual_navigation.page.layout, 100);
    CHECK(controller_scroll_by(&dual_controller, 100, 272)
          && navigation_current(&dual_navigation)->scroll_y
             == dual_scroll_delta
          && tile_cache_render_frame(
              &dual_cache, navigation_current(&dual_navigation)->scroll_y,
              480, 272, NULL)
          && dual_frame[271u * 480u + 10u] == test_rgb565(0x00aa00)
          && controller_scroll_to_bottom(&dual_controller, 272)
          && navigation_current(&dual_navigation)->scroll_y
             == dual_navigation.page.layout.height - 556);
    const LayoutDocument *dual_cached_layout = dual_cache.layout;
    const LayoutDocument *dual_cached_source = dual_cache.source_layout;
    size_t dual_invalidations = dual_cache.invalidations;
    size_t dual_budget_limit = budget.limit;
    size_t dual_failures = budget.failure_count;
    budget.limit = budget.current;
    CHECK(!tile_cache_replace_layout(&dual_cache,
                                     &dual_navigation.page.layout)
          && dual_cache.layout == dual_cached_layout
          && dual_cache.source_layout == dual_cached_source
          && dual_cache.invalidations == dual_invalidations
          && budget.failure_count > dual_failures);
    budget.limit = dual_budget_limit;
    CHECK(tile_cache_render_frame(
        &dual_cache, navigation_current(&dual_navigation)->scroll_y,
        480, 272, NULL));
    tile_cache_destroy(&dual_cache);
    budget_free(&budget, dual_frame);
    navigation_destroy(&dual_navigation);
    CHECK(budget.current == 0);

    CHECK(fetch_resolve_url("https://example.com/a/b?old=1", "?new=2",
                            resolved, sizeof(resolved)));
    CHECK(strcmp(resolved, "https://example.com/a/b?new=2") == 0);
    size_t scheduler_baseline = budget.current;
    FetchScheduler *empty_scheduler = fetch_scheduler_create(
        &budget, 4, 2 * MIB);
    CHECK(empty_scheduler != NULL
          && fetch_scheduler_pending(empty_scheduler) == 0);
    FetchRequest scheduled_request = {.method = "GET",
                                      .allow_http_errors = true};
    size_t scheduler_owned_baseline = budget.current;
    FetchResult *rejected_heap_result = fetch_result_create(&budget);
    CHECK(rejected_heap_result != NULL
          && !fetch_scheduler_request(
                 empty_scheduler, "https://oversized.invalid/",
                 &scheduled_request, 3 * MIB, 100,
                 rejected_heap_result)
          && rejected_heap_result->budget == &budget);
    fetch_result_free(rejected_heap_result);
    CHECK(budget.current == scheduler_owned_baseline);
    fetch_inject_failure_once(FETCH_INJECT_TLS);
    uint64_t injected_scheduled_id = fetch_scheduler_enqueue(
        empty_scheduler, "https://injected.invalid/", &scheduled_request,
        1024, 100);
    bool injected_scheduled_success = true;
    FetchResult injected_scheduled_result = {0};
    CHECK(injected_scheduled_id != 0
          && fetch_scheduler_pending(empty_scheduler) == 0
          && fetch_scheduler_take(empty_scheduler, injected_scheduled_id,
                                  &injected_scheduled_success,
                                  &injected_scheduled_result)
          && !injected_scheduled_success
          && strcmp(injected_scheduled_result.error,
                    "injected TLS failure") == 0);
    fetch_result_destroy(&injected_scheduled_result);
    uint64_t scheduled_ids[4];
    for (size_t i = 0; i < 4; i++) {
        scheduled_ids[i] = fetch_scheduler_enqueue(
            empty_scheduler, "https://example.invalid/api",
            &scheduled_request, 512u * 1024u, 5000);
        CHECK(scheduled_ids[i] != 0);
    }
    CHECK(fetch_scheduler_pending(empty_scheduler) == 4
          && fetch_scheduler_enqueue(
              empty_scheduler, "https://example.invalid/api",
              &scheduled_request, 512u * 1024u, 5000) == 0);
    CHECK(fetch_scheduler_cancel(empty_scheduler, scheduled_ids[1],
                                 "request aborted")
          && fetch_scheduler_pending(empty_scheduler) == 3);
    bool cancelled_success = true;
    FetchResult cancelled_result = {0};
    CHECK(fetch_scheduler_take(empty_scheduler, scheduled_ids[1],
                               &cancelled_success, &cancelled_result)
          && !cancelled_success
          && strcmp(cancelled_result.error, "request aborted") == 0);
    fetch_result_destroy(&cancelled_result);
    CHECK(!fetch_scheduler_cancel(empty_scheduler, scheduled_ids[1],
                                  "duplicate cancellation"));
    fetch_scheduler_destroy(empty_scheduler);
    CHECK(budget.current == scheduler_baseline);

    PocDocument document;
    puts("test: document");
    CHECK(document_parse(&document, &budget, page, strlen(page), 17));
    CHECK(document.node_count > 8 && document.body_text == NULL
          && document.body_text_length == 0);
    CHECK(strcmp(document.title, "Test") == 0);
    const char *materialized_body = document_body_text(&document);
    CHECK(materialized_body != NULL
          && materialized_body == document_body_text(&document)
          && document.body_text_length == strlen(materialized_body)
          && strstr(materialized_body, "still parses") != NULL);

    /* A delayed native mutation must not turn stale aggregate counts into an
       unsafe body-text allocation. The materializer bounds its first pass,
       refreshes the live DOM metadata, and retries exactly once. */
    static const char stale_page[] =
        "<!doctype html><title>Stale body stats</title><body>tiny</body>";
    static const lxb_char_t stale_tag[] = "p";
    static const lxb_char_t stale_text[] =
        "body text added after stale metadata must be materialized safely";
    PocDocument stale_document = {0};
    CHECK(document_parse(&stale_document, &budget, stale_page,
                         sizeof(stale_page) - 1, 11));
    size_t stale_text_bytes = stale_document.text_bytes;
    BudgetAllocationOwner stale_owner =
        document_allocation_owner_enter(&stale_document);
    lxb_dom_element_t *stale_element = lxb_dom_document_create_element(
        &stale_document.html->dom_document, stale_tag,
        sizeof(stale_tag) - 1, NULL);
    lxb_dom_text_t *stale_text_node = lxb_dom_document_create_text_node(
        &stale_document.html->dom_document, stale_text,
        sizeof(stale_text) - 1);
    lxb_dom_node_t *stale_body = document_body_node(&stale_document);
    bool stale_mutated = stale_element != NULL && stale_text_node != NULL
        && stale_body != NULL
        && lxb_dom_node_append_child(
               lxb_dom_interface_node(stale_element),
               lxb_dom_interface_node(stale_text_node))
               == LXB_DOM_EXCEPTION_OK
        && lxb_dom_node_append_child(
               stale_body, lxb_dom_interface_node(stale_element))
               == LXB_DOM_EXCEPTION_OK;
    document_allocation_owner_leave(&stale_document, stale_owner);
    const char *recovered_body = stale_mutated
        ? document_body_text(&stale_document) : NULL;
    CHECK(recovered_body != NULL
          && strstr(recovered_body, (const char *) stale_text) != NULL
          && stale_document.text_bytes > stale_text_bytes
          && stale_document.body_text_length == strlen(recovered_body));
    document_destroy(&stale_document);

    puts("test: cooperative layout work boundaries and cancellation");
    char cooperative_page[16384] =
        "<!doctype html><style>";
    size_t cooperative_length = strlen(cooperative_page);
    for (size_t i = 0; i < 32; i++) {
        int written = snprintf(
            cooperative_page + cooperative_length,
            sizeof(cooperative_page) - cooperative_length,
            "body main div{color:#%06zx}", i + 1u);
        CHECK(written > 0
              && (size_t) written
                 < sizeof(cooperative_page) - cooperative_length);
        cooperative_length += (size_t) written;
    }
    static const char cooperative_body[] = "</style><body><main>";
    CHECK(cooperative_length + sizeof(cooperative_body)
          < sizeof(cooperative_page));
    memcpy(cooperative_page + cooperative_length, cooperative_body,
           sizeof(cooperative_body));
    cooperative_length += sizeof(cooperative_body) - 1;
    for (size_t i = 0; i < 192; i++) {
        int written = snprintf(
            cooperative_page + cooperative_length,
            sizeof(cooperative_page) - cooperative_length,
            "<div>%zu</div>", i);
        CHECK(written > 0
              && (size_t) written
                 < sizeof(cooperative_page) - cooperative_length);
        cooperative_length += (size_t) written;
    }
    CHECK(cooperative_length + sizeof("</main></body>")
          < sizeof(cooperative_page));
    memcpy(cooperative_page + cooperative_length, "</main></body>",
           sizeof("</main></body>"));
    cooperative_length += sizeof("</main></body>") - 1;
    PocDocument cooperative_document;
    Stylesheet cooperative_sheet;
    CHECK(document_parse(&cooperative_document, &budget, cooperative_page,
                         cooperative_length, 31)
          && stylesheet_build(&cooperative_sheet, &budget,
                              &cooperative_document, 480));
    size_t cooperative_baseline = budget.current;
    LayoutCooperateProbe cooperate_probe = {0};
    TilefinchPlatformServices cooperate_services = {
        .context = &cooperate_probe,
        .cooperate = test_layout_cooperate
    };
    CHECK(setenv("TILEFINCH_SELECTOR_COOPERATE_VISITS", "64", 1) == 0);
    tilefinch_platform_set_services(&cooperate_services);
    LayoutDocument cooperative_layout;
    bool cooperative_ok = layout_build(
        &cooperative_layout, &budget, &cooperative_document,
        &cooperative_sheet, NULL, NULL, 480);
    if (!cooperative_ok || cooperate_probe.calls < 1
        || cooperate_probe.style_calls < 1
        || cooperate_probe.index_calls < 1
        || cooperative_layout.cooperative_yields != cooperate_probe.calls
        || cooperative_layout.layout_work_units < 192
        || cooperative_layout.max_work_slice_units > 4096) {
        fprintf(stderr,
                "cooperative layout: ok=%d calls=%zu style=%zu index=%zu "
                "yields=%zu work=%zu "
                "max-work=%zu\n",
                cooperative_ok, cooperate_probe.calls,
                cooperate_probe.style_calls,
                cooperate_probe.index_calls,
                cooperative_layout.cooperative_yields,
                cooperative_layout.layout_work_units,
                cooperative_layout.max_work_slice_units);
    }
    CHECK(cooperative_ok
          && cooperate_probe.calls >= 1
          && cooperate_probe.style_calls >= 1
          && cooperate_probe.index_calls >= 1
          && cooperative_layout.cooperative_yields == cooperate_probe.calls
          && cooperative_layout.layout_work_units >= 192
          /* Tree work yields every 16 nodes; the linear radix/index phases
           * use a separately calibrated 4096-entry batch. Work-unit counts
           * are phase-relative, while elapsed slice telemetry is comparable
           * across phases. */
          && cooperative_layout.max_work_slice_units <= 4096);
    layout_destroy(&cooperative_layout);
    /* The first selector-rich build may populate retained stylesheet
       indexes. Cancellation checks compare against that warmed baseline. */
    cooperative_baseline = budget.current;
    cooperate_probe = (LayoutCooperateProbe) {.cancel_index_at = 1};
    CHECK(!layout_build(&cooperative_layout, &budget, &cooperative_document,
                        &cooperative_sheet, NULL, NULL, 480)
          && cooperate_probe.index_calls == 1
          && cooperative_layout.budget == NULL
          && budget.current == cooperative_baseline);
    cooperate_probe = (LayoutCooperateProbe) {.cancel_at = 1};
    CHECK(!layout_build(&cooperative_layout, &budget, &cooperative_document,
                        &cooperative_sheet, NULL, NULL, 480)
          && cooperate_probe.calls == 1 && cooperative_layout.budget == NULL
          && budget.current == cooperative_baseline);
    cooperate_probe = (LayoutCooperateProbe) {.cancel_style_at = 1};
    CHECK(!layout_build(&cooperative_layout, &budget, &cooperative_document,
                        &cooperative_sheet, NULL, NULL, 480)
          && cooperate_probe.style_calls == 1
          && cooperative_layout.budget == NULL
          && budget.current == cooperative_baseline);
    tilefinch_platform_set_services(NULL);
    CHECK(unsetenv("TILEFINCH_SELECTOR_COOPERATE_VISITS") == 0);

    puts("test: transactional layout allocation-failure sweep");
    size_t injected_layout_failures = 0;
    size_t uninjected_layout_builds = 0;
    for (size_t fail_after = 0; fail_after < 96; fail_after++) {
        size_t injected_before_layout = budget.injected_failure_count;
        budget_inject_failure_after(&budget, fail_after);
        bool allocation_layout_ok = layout_build(
            &cooperative_layout, &budget, &cooperative_document,
            &cooperative_sheet, NULL, NULL, 480);
        bool injected = budget.injected_failure_count
                        != injected_before_layout;
        budget_clear_failure_injection(&budget);
        if (allocation_layout_ok) layout_destroy(&cooperative_layout);
        CHECK(budget.current == cooperative_baseline);
        if (injected) {
            injected_layout_failures++;
            uninjected_layout_builds = 0;
        } else if (++uninjected_layout_builds == 2) {
            break;
        }
    }
    CHECK(injected_layout_failures >= 8 && uninjected_layout_builds == 2);
    stylesheet_destroy(&cooperative_sheet);
    document_destroy(&cooperative_document);

    Stylesheet sheet;
