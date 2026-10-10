/* Synthetic service responses only: never capture account credentials. */
static int test_youtube_login_journey(void)
{
    static const char fixture[] = TILEFINCH_TEST_SOURCE_DIR "/tests/fixtures/http-youtube-login";
    static const char form[] = "<form method=post action='https://accounts.google.com/tilefinch-test-signin'>"
        "<input id=user name=user><input id=password type=password name=password>"
        "<button id=submit>Sign in</button></form>";
    BrowserDeviceProfile device; browser_device_profile_psp3000(&device);
    BrowserConfig config; browser_config_init(&config, &device);
    config.memory_limit = 16u * MIB; config.resources.web_fonts_enabled = false;
    char error[256] = {0};
    for (unsigned lane = 0; lane < 3u; lane++) {
        CHECK(fetch_trace_replay_begin(fixture, error, sizeof(error)));
        BrowserEngine *engine = browser_engine_create(&config, error, sizeof(error)); CHECK(engine);
        CHECK(browser_engine_set_javascript_enabled(engine, false));
        CHECK(browser_engine_load_url(engine, "https://www.youtube.com/", true));
        NavigationSession *nav = browser_engine_navigation(engine);
        CHECK(nav->page.runtime == NULL);
        CHECK(strstr(nav->page.layout.links[1].url, "https://accounts.google.com/ServiceLogin?") == nav->page.layout.links[1].url);
        CHECK(browser_engine_commit_html(engine, "https://accounts.google.com/tilefinch-test-signin", form, sizeof(form) - 1u, false));
        lxb_dom_node_t *root = lxb_dom_interface_node(nav->page.document.html);
        CHECK(browser_engine_focus_node(engine, test_reader_find_id(root, "user")));
        CHECK(browser_engine_replace_text(engine, "synthetic-user", 14));
        CHECK(browser_engine_focus_node(engine, test_reader_find_id(root, "password")));
        CHECK(browser_engine_replace_text(engine, "synthetic-password", 18));
        CHECK(browser_engine_focus_node(engine, test_reader_find_id(root, "submit")));
        ControllerAction action = {0}; CHECK(browser_engine_activate(engine, &action));
        CHECK(action.type == CONTROLLER_ACTION_FORM_SUBMIT && strcmp(action.method, "POST") == 0);
        CHECK(browser_engine_set_javascript_enabled(engine, true));
        if (lane == 2u) {
            CHECK(browser_engine_load_url(engine, "https://accounts.google.com/tilefinch-test-signin", false));
        } else CHECK(browser_engine_begin_navigation_action(engine, &action, config.maximum_document_bytes, config.navigation_timeout_ms));
        if (lane == 1u) {
            browser_engine_cancel_navigation(engine, "login cancelled");
            CHECK(browser_engine_take_youtube_login_events(engine) == 0u);
            CHECK(strcmp(nav->page.document_url, "https://accounts.google.com/tilefinch-test-signin") == 0);
        } else {
            for (unsigned tick = 0; browser_engine_navigation_pending(engine) && tick < 512u; tick++)
                (void) browser_engine_pump_navigation(engine, NULL);
            CHECK(!browser_engine_navigation_pending(engine));
            CHECK(nav->page.loaded && nav->page.runtime == NULL);
            CHECK(strcmp(nav->page.document_url, "https://www.youtube.com/") == 0);
            CHECK(nav->page.runtime == NULL);
            /* Cookie eligibility alone must not emit verified-login consent. */
            CHECK(browser_engine_take_youtube_login_events(engine) == 0u);
        }
        CHECK(browser_engine_shutdown(engine)); browser_engine_destroy(engine); fetch_trace_end();
    }
    /* Unsupported paths and HTTP entry points must stay native too. */
    CHECK(youtube_lite_route("https://www.youtube.com/signin?next=%2F") == YOUTUBE_LITE_ROUTE_HOME);
    CHECK(youtube_lite_route("https://www.youtube.com/account") == YOUTUBE_LITE_ROUTE_HOME);
    CHECK(youtube_lite_route("http://www.youtube.com/unsupported") == YOUTUBE_LITE_ROUTE_HOME);
    CHECK(youtube_lite_route("https://notyoutube.com/") == YOUTUBE_LITE_ROUTE_NONE);
    CHECK(fetch_trace_replay_begin(fixture, error, sizeof(error)));
    BrowserEngine *framed = browser_engine_create(&config, error, sizeof(error)); CHECK(framed);
    CHECK(browser_engine_set_javascript_enabled(framed, true));
    static const char framed_html[] = "<iframe src='https://accounts.google.com/tilefinch-test-frame'></iframe>";
    CHECK(browser_engine_commit_html(framed, "https://accounts.google.com/", framed_html, sizeof(framed_html) - 1u, true));
    NavigationSession *framed_nav = browser_engine_navigation(framed);
    for (unsigned tick = 0; tick < 128u && framed_nav->frames_failed == 0; tick++)
        CHECK(browser_engine_advance_runtime(framed, 16u, 8u, NULL));
    CHECK(framed_nav->page.loaded && framed_nav->page.frame_count == 1u);
    CHECK(framed_nav->frames_failed == 1u && framed_nav->frames_loaded == 0u);
    CHECK(framed_nav->page.frames[0].runtime == NULL && !framed_nav->page.frames[0].loaded);
    CHECK(browser_engine_shutdown(framed)); browser_engine_destroy(framed); fetch_trace_end();
    BrowserEngine *engine = browser_engine_create(&config, error, sizeof(error)); CHECK(engine);
    BrowserSession *session = browser_engine_session(engine);
    for (unsigned lane = 0; lane < 4u; lane++) {
        CHECK(fetch_trace_replay_begin(TILEFINCH_TEST_SOURCE_DIR "/tests/fixtures/http-youtube-personalized", error, sizeof(error)));
        if (lane == 0u || lane == 2u) CHECK(browser_session_cookie_set_http(session,
            "https://m.youtube.com/", lane == 0u ? "SAPISID=account-a; Secure; Path=/" : "SAPISID=account-b; Secure; Path=/"));
        if (lane == 3u) CHECK(browser_session_cookie_set_http(session, "https://m.youtube.com/", "visitor=renewed; Secure; Path=/"));
        CHECK(browser_engine_load_url(engine, "https://www.youtube.com/", true));
        CHECK(browser_engine_render_frame(engine, NULL));
        for (unsigned tick = 0; tick < 512u; tick++) {
            bool visual = false; (void) browser_engine_run_idle_work(engine, &visual);
            if (browser_engine_youtube_feed_state(engine) == BROWSER_YOUTUBE_FEED_CURRENT) break;
        }
        CHECK(browser_engine_youtube_feed_state(engine) == BROWSER_YOUTUBE_FEED_CURRENT);
        CHECK(browser_engine_take_youtube_login_events(engine) == (lane == 0u || lane == 2u ? BROWSER_YOUTUBE_LOGIN_VERIFIED : 0u));
        fetch_trace_end();
    }
    CHECK(browser_engine_shutdown(engine)); browser_engine_destroy(engine);
    return 0;
}
