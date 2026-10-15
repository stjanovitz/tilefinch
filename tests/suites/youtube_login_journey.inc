/* Synthetic service responses only: never capture account credentials. */
static int test_youtube_login_unseeded_flow(void)
{
    const char *entry = "https://accounts.google.com/ServiceLogin?service=youtube&continue=https%3A%2F%2Fwww.youtube.com%2F";
    BrowserDeviceProfile device; browser_device_profile_psp3000(&device);
    BrowserConfig config; browser_config_init(&config, &device);
    config.memory_limit = 16u * MIB; config.resources.web_fonts_enabled = false;
    char error[256], report[1024];
    for (unsigned lane = 0; lane < 8; lane++) {
        const char *fixture = lane == 0
            ? TILEFINCH_TEST_SOURCE_DIR "/tests/fixtures/http-youtube-login-flow"
            : lane == 4 ? TILEFINCH_TEST_SOURCE_DIR "/tests/fixtures/http-youtube-login-persistent"
            : TILEFINCH_TEST_SOURCE_DIR "/tests/fixtures/http-youtube-login-recovery";
        CHECK(fetch_trace_replay_begin(fixture, error, sizeof(error)));
        BrowserEngine *engine = browser_engine_create(&config, error, sizeof(error)); CHECK(engine);
        BrowserSession *session = browser_engine_session(engine);
        CHECK(session->cookie_bytes == 0 && !youtube_lite_session_signed_in(session));
        CHECK(!browser_engine_youtube_signin_report(engine, report, sizeof(report)) && report[0] == '\0');
        browser_engine_set_youtube_signin_diagnostics(engine, true);
        browser_engine_set_defer_script_navigation(engine, true);
        CHECK(browser_engine_set_javascript_enabled(engine, true));
        CHECK(browser_engine_load_url(engine, "https://www.youtube.com/", true));
        if (lane == 0) CHECK(browser_session_cookie_set_http(session, "https://www.youtube.com/",
            "strict-probe=synthetic; Secure; SameSite=Strict; Domain=youtube.com; Path=/"));
        tilefinch_test_faults()->replayed_request_count = 0;
        NavigationSession *nav = browser_engine_navigation(engine);
        CHECK(browser_engine_render_frame(engine, NULL));
        CHECK(browser_engine_focus_node(engine, test_reader_find_id(
            lxb_dom_interface_node(nav->page.document.html), "yt-account-action")));
        ControllerAction signin = {0};
        CHECK(browser_engine_activate(engine, &signin)
            && signin.type == CONTROLLER_ACTION_NAVIGATE && strcmp(signin.url, entry) == 0);
        CHECK(browser_engine_begin_navigation_action(engine, &signin,
            config.maximum_document_bytes, config.navigation_timeout_ms));
        CHECK(browser_engine_take_youtube_login_events(engine) == BROWSER_YOUTUBE_LOGIN_STARTED);
        for (unsigned i = 0; i < 512 && browser_engine_navigation_pending(engine); i++)
            (void) browser_engine_pump_navigation(engine, NULL);
        CHECK(!browser_engine_navigation_pending(engine) && nav->page.loaded);
        CHECK(browser_engine_render_frame(engine, NULL));
        if (lane == 0) {
            lxb_dom_node_t *root = lxb_dom_interface_node(nav->page.document.html);
            CHECK(browser_engine_focus_node(engine, test_reader_find_id(root, "user")));
            CHECK(browser_engine_replace_text(engine, "synthetic-private-user", 22));
            CHECK(browser_engine_focus_node(engine, test_reader_find_id(root, "password")));
            CHECK(browser_engine_replace_text(engine, "synthetic-private-password", 26));
            CHECK(browser_engine_focus_node(engine, test_reader_find_id(root, "submit")));
            ControllerAction action = {0}; CHECK(browser_engine_activate(engine, &action));
            CHECK(action.type == CONTROLLER_ACTION_FORM_SUBMIT && strcmp(action.method, "POST") == 0);
            CHECK(browser_engine_begin_navigation_action(engine, &action,
                config.maximum_document_bytes, config.navigation_timeout_ms));
        } else {
            if (lane == 7) CHECK(browser_session_cookie_set_http(session, "https://m.youtube.com/",
                "SAPISID=synthetic-stale-session; Secure; SameSite=Strict; Domain=youtube.com; Path=/"));
            navigation_retire_current_page_scripts(nav);
            nav->page.script_degradation_observed = true;
            if (lane == 5) {
                CHECK(browser_engine_focus_node(engine, test_reader_find_id(
                    lxb_dom_interface_node(nav->page.document.html), "password")));
                CHECK(browser_engine_replace_text(engine, "preserve-this-input", 19));
            }
            if (lane == 2) CHECK(browser_engine_set_javascript_enabled(engine, false));
            if (lane == 3) {
                CHECK(browser_engine_begin_navigation_url(engine, entry,
                    config.maximum_document_bytes, config.navigation_timeout_ms, false));
                browser_engine_cancel_navigation(engine, "synthetic private cancellation");
            }
            bool changed = false;
            if (lane == 6) tilefinch_test_faults()->refuse_youtube_signin_recovery = true;
            (void) browser_engine_run_idle_work(engine, &changed);
            CHECK(browser_engine_navigation_pending(engine) == (lane == 1 || lane == 4 || lane == 7));
            CHECK(browser_engine_youtube_signin_report(engine, report, sizeof(report)));
            CHECK(strstr(report, lane == 1 || lane == 4 || lane == 6 || lane == 7
                ? "recovery-attempts=1" : "recovery-attempts=0"));
            if (lane == 6) CHECK(browser_engine_take_youtube_login_events(engine)
                == (BROWSER_YOUTUBE_LOGIN_RECOVERING | BROWSER_YOUTUBE_LOGIN_FAILED));
            if (lane == 4) {
                for (unsigned i = 0; i < 512 && browser_engine_navigation_pending(engine); i++)
                    (void) browser_engine_pump_navigation(engine, NULL);
                CHECK(!browser_engine_navigation_pending(engine) && nav->page.loaded);
                navigation_retire_current_page_scripts(nav);
                nav->page.script_degradation_observed = true;
                for (unsigned i = 0; i < 8; i++) (void) browser_engine_run_idle_work(engine, &changed);
                CHECK(!browser_engine_navigation_pending(engine));
                CHECK(browser_engine_take_youtube_login_events(engine)
                    == (BROWSER_YOUTUBE_LOGIN_RECOVERING | BROWSER_YOUTUBE_LOGIN_FAILED));
                CHECK(browser_engine_youtube_signin_report(engine, report, sizeof(report))
                    && strstr(report, "stage=6 recovery-attempts=1"));
                const char *retry = browser_engine_youtube_signin_continue_target(engine);
                CHECK(retry && browser_engine_begin_navigation_url(engine, retry,
                    config.maximum_document_bytes, config.navigation_timeout_ms, false));
                CHECK(browser_engine_take_youtube_login_events(engine) == BROWSER_YOUTUBE_LOGIN_STARTED);
                CHECK(browser_engine_youtube_signin_report(engine, report, sizeof(report))
                    && strstr(report, "stage=1 recovery-attempts=0"));
                browser_engine_cancel_navigation(engine, "synthetic fresh-attempt check complete");
            }
            if (lane != 1 && lane != 7) {
                CHECK(!youtube_lite_session_signed_in(session));
                if (lane == 5) {
                    size_t length = 0;
                    const char *value = document_control_value(test_reader_find_id(
                        lxb_dom_interface_node(nav->page.document.html), "password"), &length);
                    CHECK(value && length == 19 && !memcmp(value, "preserve-this-input", 19)
                        && !strstr(report, "preserve-this-input"));
                }
                CHECK(browser_engine_shutdown(engine)); browser_engine_destroy(engine); fetch_trace_end();
                continue;
            }
        }
        bool completed = false;
        for (unsigned i = 0; i < 512 && !completed; i++) {
            if (browser_engine_navigation_pending(engine)) (void) browser_engine_pump_navigation(engine, NULL);
            else {
                char url[NAVIGATION_URL_LIMIT]; bool history = false;
                if (browser_engine_take_script_navigation(engine, url, sizeof(url), &history)) {
                    bool started = browser_engine_begin_navigation_url(engine, url,
                        config.maximum_document_bytes, config.navigation_timeout_ms, history);
                    CHECK(started);
                }
                else {
                    (void) browser_engine_advance_runtime(engine, 16u, 8u, NULL);
                    CHECK(nav->page.loaded && browser_engine_last_error(engine)[0] == '\0');
                }
            }
            completed = !browser_engine_navigation_pending(engine)
                && strcmp(nav->page.document_url, "https://www.youtube.com/") == 0;
        }
        CHECK(completed && youtube_lite_session_signed_in(session) && nav->page.runtime == NULL);
        lxb_dom_node_t *account = test_reader_find_id(
            lxb_dom_interface_node(nav->page.document.html), "yt-account-panel");
        CHECK(account);
        size_t account_text_length = 0;
        lxb_char_t *account_text = lxb_dom_node_text_content(account, &account_text_length);
        CHECK(account_text && !strstr((const char *) account_text, "Signed in on this device."));
        lxb_dom_document_destroy_text(account->owner_document, account_text);
        if (lane == 0) {
            bool found_handoff = false;
            TilefinchTestFaults *faults = tilefinch_test_faults();
            for (unsigned i = 0; i < faults->replayed_request_count && i < 16; i++) {
                if (youtube_lite_signin_handoff_url(faults->replayed_requests[i].url)) {
                    CHECK(!found_handoff && !faults->replayed_requests[i].cookie_present
                        && faults->replayed_requests[i].fetch_site == 4);
                    found_handoff = true;
                }
            }
            CHECK(found_handoff);
        }
        CHECK(browser_engine_take_youtube_login_events(engine) == (lane == 1 || lane == 7
            ? BROWSER_YOUTUBE_LOGIN_RECOVERING : 0u)); /* Not verified by cookie alone. */
        CHECK(browser_engine_youtube_signin_report(engine, report, sizeof(report)));
        CHECK(strstr(report, "server-verified=0") && !strstr(report, "synthetic-private")
            && !strstr(report, "synthetic-flow-session") && !strstr(report, "synthetic-handoff")
            && !strstr(report, "https:") && !strstr(report, "SAPISID"));
        CHECK(strstr(report, "handoff-http-status=200 handoff-service-origin=1"));
        fetch_trace_end();
        CHECK(fetch_trace_replay_begin(TILEFINCH_TEST_SOURCE_DIR "/tests/fixtures/http-youtube-personalized", error, sizeof(error)));
        CHECK(browser_engine_render_frame(engine, NULL));
        for (unsigned i = 0; i < 512 && browser_engine_youtube_feed_state(engine) != BROWSER_YOUTUBE_FEED_CURRENT; i++) {
            bool changed = false; (void) browser_engine_run_idle_work(engine, &changed);
        }
        CHECK(browser_engine_youtube_feed_state(engine) == BROWSER_YOUTUBE_FEED_CURRENT);
        CHECK(browser_engine_take_youtube_login_events(engine) == BROWSER_YOUTUBE_LOGIN_VERIFIED);
        CHECK(browser_engine_youtube_signin_report(engine, report, sizeof(report))
            && strstr(report, "server-verified=1") && strstr(report, "api-authenticated=1")
            && strstr(report, "parsed-results=") && strstr(report, "pending-publication=0"));
        snprintf(nav->page.script_result.error, sizeof(nav->page.script_result.error),
                 "%s", "never-export-private-error");
        size_t limit = session->maximum_cookie_bytes;
        session->maximum_cookie_bytes = session->cookie_bytes;
        CHECK(!browser_session_cookie_set_http(session, "https://accounts.google.com/",
            "extra=never-export-cookie; Secure; Path=/"));
        session->maximum_cookie_bytes = limit;
        CHECK(browser_engine_youtube_signin_report(engine, report, sizeof(report))
            && strstr(report, "byte-refusals=1") && !strstr(report, "never-export"));
        CHECK(!browser_engine_youtube_signin_report(engine, report, 8) && report[0] == '\0');
        browser_engine_set_youtube_signin_diagnostics(engine, false);
        CHECK(!browser_engine_youtube_signin_report(engine, report, sizeof(report)) && report[0] == '\0'
            && !session->cookie_diagnostics.enabled && session->cookie_diagnostics.http_attempts == 0);
        CHECK(browser_engine_shutdown(engine)); browser_engine_destroy(engine); fetch_trace_end();
    }
    return 0;
}

static int test_youtube_login_display_failures(void)
{
    BrowserDeviceProfile device; browser_device_profile_psp3000(&device);
    BrowserConfig config; browser_config_init(&config, &device);
    config.memory_limit = 16u * MIB; config.resources.web_fonts_enabled = false;
    char error[256], report[1024];
    for (unsigned lane = 0; lane < 4; lane++) {
        CHECK(fetch_trace_replay_begin(TILEFINCH_TEST_SOURCE_DIR
            "/tests/fixtures/http-youtube-personalized", error, sizeof(error)));
        BrowserEngine *engine = browser_engine_create(&config, error, sizeof(error)); CHECK(engine);
        BrowserSession *session = browser_engine_session(engine);
        browser_engine_set_youtube_signin_diagnostics(engine, true);
        /* Cookie receipt may happen after the native handoff shell was built.
           Its recovery controls must already exist, without assuming login. */
        CHECK(browser_engine_load_url(engine,
            "https://www.youtube.com/?tilefinch_signin=1", true));
        NavigationSession *nav = browser_engine_navigation(engine);
        lxb_dom_node_t *root = lxb_dom_interface_node(nav->page.document.html);
        CHECK(test_reader_find_id(root, "yt-feed-status")
            && test_reader_find_id(root, "yt-feed-refresh"));
        CHECK(browser_engine_focus_node(engine, test_reader_find_id(root, "yt-search")));
        CHECK(browser_engine_replace_text(engine, "draft query", 11));
        CHECK(browser_session_cookie_set_http(session, "https://m.youtube.com/",
            "SAPISID=synthetic-display-session; Domain=youtube.com; Secure; Path=/"));
        CHECK(browser_engine_render_frame(engine, NULL));
        if (lane == 0) tilefinch_test_faults()->refuse_next_youtube_feed_build = true;
        if (lane == 2) tilefinch_test_faults()->expire_youtube_history = true;
        if (lane == 3) CHECK(browser_engine_scroll_by(engine, 1));
        unsigned events = 0;
        bool done = false;
        bool publication_fault = false;
        for (unsigned tick = 0; tick < 1024 && !done; tick++) {
            bool changed = false; (void) browser_engine_run_idle_work(engine, &changed);
            if (changed) CHECK(browser_engine_render_frame(engine, NULL));
            unsigned turn_events = browser_engine_take_youtube_login_events(engine);
            events |= turn_events;
            if (turn_events & BROWSER_YOUTUBE_LOGIN_VERIFIED) {
                lxb_dom_node_t *search = test_reader_find_id(
                    lxb_dom_interface_node(nav->page.document.html), "yt-search");
                size_t length = 0;
                const char *value = document_control_value(search, &length);
                if (value == NULL) value = document_attribute(search, "value", &length);
                CHECK(controller_focused_node(browser_engine_controller(engine)) == search
                    && value && length == 11 && !memcmp(value, "draft query", 11));
            }
            if (lane == 1 && !publication_fault && (events & BROWSER_YOUTUBE_LOGIN_VERIFIED)) {
                tilefinch_test_faults()->refuse_next_render_shell_init = true;
                publication_fault = true;
            }
            BrowserYoutubeFeedState state = browser_engine_youtube_feed_state(engine);
            done = state == BROWSER_YOUTUBE_FEED_FAILED || state == BROWSER_YOUTUBE_FEED_CURRENT
                || (lane == 3 && state == BROWSER_YOUTUBE_FEED_READY);
        }
        CHECK(done && events == BROWSER_YOUTUBE_LOGIN_VERIFIED
            && browser_engine_youtube_login_verified(engine));
        root = lxb_dom_interface_node(nav->page.document.html);
        CHECK(test_reader_find_id(root, "yt-account-menu") && test_reader_find_id(root, "yt-signout-action"));
        CHECK(browser_engine_youtube_signin_report(engine, report, sizeof(report))
            && strstr(report, "server-verified=1") && strstr(report, "api-authenticated=1")
            && strstr(report, "bootstrap-status=200 api-status=200")
            && !strstr(report, "synthetic-display") && !strstr(report, "https:"));
        CHECK(strstr(report, lane == 0 ? "build-outcome=3 publication-outcome=0"
            : lane == 1 ? "build-outcome=2 publication-outcome=3"
            : lane == 2 ? "build-outcome=2 publication-outcome=2"
            : "build-outcome=2 publication-outcome=4"));
        if (lane == 3) {
            root = lxb_dom_interface_node(nav->page.document.html);
            CHECK(browser_engine_focus_node(engine, test_reader_find_id(root, "yt-feed-refresh")));
            ControllerAction refresh = {0}; CHECK(browser_engine_activate(engine, &refresh));
            bool changed = false; (void) browser_engine_run_idle_work(engine, &changed);
            CHECK(browser_engine_youtube_feed_state(engine) == BROWSER_YOUTUBE_FEED_CURRENT);
        }
        if (lane < 2) {
            /* A display refusal is retryable with the same authenticated
               session. No account page or credential submission is needed. */
            fetch_trace_end();
            CHECK(fetch_trace_replay_begin(TILEFINCH_TEST_SOURCE_DIR
                "/tests/fixtures/http-youtube-personalized", error, sizeof(error)));
            root = lxb_dom_interface_node(nav->page.document.html);
            CHECK(browser_engine_focus_node(engine, test_reader_find_id(root, "yt-feed-refresh")));
            ControllerAction refresh = {0}; CHECK(browser_engine_activate(engine, &refresh));
            for (unsigned i = 0; i < 1024 && browser_engine_youtube_feed_state(engine) != BROWSER_YOUTUBE_FEED_CURRENT; i++) {
                bool changed = false; (void) browser_engine_run_idle_work(engine, &changed);
                if (changed) CHECK(browser_engine_render_frame(engine, NULL));
            }
            CHECK(browser_engine_youtube_feed_state(engine) == BROWSER_YOUTUBE_FEED_CURRENT
                && browser_engine_youtube_login_verified(engine)
                && browser_engine_take_youtube_login_events(engine) == 0);
        }
        /* Proof is bound to the session, not a sticky native boolean. */
        CHECK(browser_session_cookie_set_http(session, "https://m.youtube.com/",
            "SAPISID=synthetic-replacement; Domain=youtube.com; Secure; Path=/"));
        CHECK(!browser_engine_youtube_login_verified(engine));
        CHECK(browser_engine_shutdown(engine)); browser_engine_destroy(engine); fetch_trace_end();
    }
    return 0;
}

static int test_youtube_login_manual_continuation(void)
{
    BrowserDeviceProfile device; browser_device_profile_psp3000(&device);
    BrowserConfig config; browser_config_init(&config, &device);
    config.memory_limit = 16u * MIB; config.resources.web_fonts_enabled = false;
    char error[256];
    CHECK(fetch_trace_replay_begin(TILEFINCH_TEST_SOURCE_DIR
        "/tests/fixtures/http-youtube-login-recovery", error, sizeof(error)));
    BrowserEngine *engine = browser_engine_create(&config, error, sizeof(error)); CHECK(engine);
    browser_engine_set_defer_script_navigation(engine, true);
    CHECK(browser_engine_set_javascript_enabled(engine, true));
    CHECK(!browser_engine_youtube_signin_continue_available(engine));
    CHECK(browser_engine_load_url(engine,
        "https://accounts.google.com/ServiceLogin?service=youtube&continue=https%3A%2F%2Fwww.youtube.com%2F", false));
    CHECK(browser_engine_take_youtube_login_events(engine) == BROWSER_YOUTUBE_LOGIN_STARTED);
    /* A completed account/support surface may have no automatic onward
       navigation and no script degradation. Only an explicit action leaves. */
    static const char account[] = "<p>Account step complete</p>";
    CHECK(browser_engine_commit_html(engine, "https://support.google.com/",
        account, sizeof(account) - 1u, false));
    CHECK(browser_engine_render_frame(engine, NULL));
    CHECK(browser_engine_youtube_signin_continue_available(engine));
    bool changed = false; (void) browser_engine_run_idle_work(engine, &changed);
    CHECK(!browser_engine_navigation_pending(engine));
    const char *target = browser_engine_youtube_signin_continue_target(engine);
    CHECK(target && strcmp(target,
        "https://accounts.google.com/ServiceLogin?service=youtube&continue=https%3A%2F%2Fwww.youtube.com%2F") == 0);
    CHECK(browser_engine_begin_navigation_url(engine, target, config.maximum_document_bytes,
        config.navigation_timeout_ms, true));
    CHECK(browser_engine_take_youtube_login_events(engine) == BROWSER_YOUTUBE_LOGIN_STARTED);
    NavigationSession *nav = browser_engine_navigation(engine);
    bool completed = false;
    for (unsigned i = 0; i < 512 && !completed; i++) {
        if (browser_engine_navigation_pending(engine)) (void) browser_engine_pump_navigation(engine, NULL);
        else {
            char url[NAVIGATION_URL_LIMIT]; bool history = false;
            if (browser_engine_take_script_navigation(engine, url, sizeof(url), &history))
                CHECK(browser_engine_begin_navigation_url(engine, url, config.maximum_document_bytes,
                    config.navigation_timeout_ms, history));
            else CHECK(browser_engine_advance_runtime(engine, 16, 8, NULL));
        }
        completed = !browser_engine_navigation_pending(engine)
            && youtube_lite_route(nav->page.document_url) == YOUTUBE_LITE_ROUTE_HOME;
    }
    CHECK(!browser_engine_navigation_pending(engine) && nav->page.loaded
        && nav->page.runtime == NULL && !browser_engine_youtube_signin_continue_available(engine));
    CHECK(completed && youtube_lite_session_signed_in(browser_engine_session(engine)));
    CHECK(test_reader_find_id(lxb_dom_interface_node(nav->page.document.html), "yt-feed-refresh"));
    /* Authentication can then be bootstrapped and checked natively, without
       loading the service website or repeating the submitted password. */
    CHECK(browser_engine_commit_html(engine, "https://support.google.com/",
        account, sizeof(account) - 1u, false));
    target = browser_engine_youtube_signin_continue_target(engine);
    CHECK(target && strcmp(target,
        "https://accounts.google.com/ServiceLogin?service=youtube&continue=https%3A%2F%2Fwww.youtube.com%2F") == 0);
    CHECK(browser_engine_load_url(engine, "https://www.youtube.com/?tilefinch_signin=1", true));
    fetch_trace_end();
    CHECK(fetch_trace_replay_begin(TILEFINCH_TEST_SOURCE_DIR
        "/tests/fixtures/http-youtube-personalized", error, sizeof(error)));
    CHECK(browser_engine_render_frame(engine, NULL));
    for (unsigned i = 0; i < 1024 && !browser_engine_youtube_login_verified(engine); i++)
        (void) browser_engine_run_idle_work(engine, &changed);
    CHECK(browser_engine_youtube_login_verified(engine)
        && browser_engine_take_youtube_login_events(engine) == BROWSER_YOUTUBE_LOGIN_VERIFIED);
    CHECK(browser_engine_shutdown(engine)); browser_engine_destroy(engine); fetch_trace_end();
    return 0;
}

static int test_youtube_login_failure_diagnostics(void)
{
    static const char bootstrap[] = "<script>ytcfg.set({\"INNERTUBE_API_KEY\":\"fixture-key\","
        "\"INNERTUBE_CONTEXT_CLIENT_VERSION\":\"fixture-version\",\"INNERTUBE_CLIENT_NAME\":\"MWEB\","
        "\"VISITOR_DATA\":\"fixture-visitor\",\"LOGGED_IN\":true});</script>";
    static const char logged_out[] = "<script>ytcfg.set({\"INNERTUBE_API_KEY\":\"fixture-key\","
        "\"INNERTUBE_CONTEXT_CLIENT_VERSION\":\"fixture-version\",\"INNERTUBE_CLIENT_NAME\":\"MWEB\","
        "\"VISITOR_DATA\":\"fixture-visitor\",\"LOGGED_IN\":false});</script>";
    const YoutubeLiteFeedFailure expected[] = {YOUTUBE_FEED_FAILURE_CONFIGURATION,
        YOUTUBE_FEED_FAILURE_AUTHENTICATION, YOUTUBE_FEED_FAILURE_HTTP,
        YOUTUBE_FEED_FAILURE_JSON, YOUTUBE_FEED_FAILURE_AUTHENTICATION,
        YOUTUBE_FEED_FAILURE_SESSION_CHANGED};
    for (unsigned lane = 0; lane < 6; lane++) {
        GoogleIdentityRecord records[] = {
            {.url="https://m.youtube.com/", .status=200, .content_type="text/html",
             .body=lane == 0 ? "<p>Missing configuration</p>" : lane == 1 ? logged_out : bootstrap},
            {.url="https://m.youtube.com/youtubei/v1/browse?key=fixture-key&prettyPrint=false",
             .status=lane == 2 ? 503 : 200, .content_type="application/json", .method="POST",
             .body=lane == 3 ? "{malformed" : lane == 5
                ? "{\"responseContext\":{\"mainAppWebResponseContext\":{\"loggedOut\":false}}}"
                : "{\"responseContext\":{\"mainAppWebResponseContext\":{\"loggedOut\":true}}}"}
        };
        char fixture[128], error[256];
        CHECK(write_google_identity_replay(fixture, records, 2)
            && fetch_trace_replay_begin(fixture, error, sizeof(error)));
        Budget budget; budget_init(&budget, 4u * MIB);
        BrowserSession session; CHECK(browser_session_init(&session, &budget, 1024));
        CHECK(browser_session_cookie_set_http(&session, "https://m.youtube.com/",
            "SAPISID=synthetic-diagnostic-session; Secure; Path=/"));
        YoutubeLiteFeedJob *job = youtube_lite_feed_begin(&budget, &session, false, false, 1000);
        CHECK(job);
        FetchPumpQuota quota = {.maximum_body_callbacks=1, .maximum_body_bytes=4096, .maximum_time_us=1000};
        YoutubeLiteLoadStatus status = YOUTUBE_LITE_LOAD_PENDING;
        bool replaced = false;
        for (unsigned i = 0; i < 512 && status == YOUTUBE_LITE_LOAD_PENDING; i++) {
            status = youtube_lite_feed_pump(job, &quota);
            YoutubeLiteFeedDiagnostics current;
            if (lane == 5 && !replaced && youtube_lite_feed_diagnostics(job, &current)
                && current.phase == 2 && current.request_admitted) {
                CHECK(browser_session_cookie_set_http(&session, "https://m.youtube.com/",
                    "SAPISID=synthetic-inflight-replacement; Secure; Path=/"));
                replaced = true;
            }
        }
        YoutubeLiteFeedDiagnostics diagnostics;
        CHECK(status == YOUTUBE_LITE_LOAD_FAILED && youtube_lite_feed_diagnostics(job, &diagnostics)
            && diagnostics.failure == expected[lane] && diagnostics.bootstrap_status == 200
            && !diagnostics.api_authenticated && youtube_lite_feed_authenticated_identity(job) == 0);
        CHECK(diagnostics.request_admitted == (lane >= 2)
            && diagnostics.request_signed == (lane >= 2));
        /* Replay has no native TLS backend; unknown must not become a
           fabricated certificate result or error code. */
        CHECK(diagnostics.transport_code == 0 && !diagnostics.tls_verify_result_available
            && !diagnostics.tls_verification_failed && !diagnostics.transport_timed_out);
        CHECK(lane != 5 || replaced);
        youtube_lite_feed_destroy(job); browser_session_destroy(&session); fetch_trace_end();
        remove_google_identity_replay(fixture, 2);
        CHECK(budget.current == 0 && budget.external_reserved == 0);
    }
    return 0;
}

static int test_youtube_login_late_session_changes(void)
{
    BrowserDeviceProfile device; browser_device_profile_psp3000(&device);
    BrowserConfig config; browser_config_init(&config, &device);
    config.memory_limit = 16u * MIB; config.resources.web_fonts_enabled = false;
    char error[256], report[1024];
    for (unsigned lane = 0; lane < 2; lane++) {
        CHECK(fetch_trace_replay_begin(TILEFINCH_TEST_SOURCE_DIR "/tests/fixtures/http-youtube-personalized",
            error, sizeof(error)));
        BrowserEngine *engine = browser_engine_create(&config, error, sizeof(error)); CHECK(engine);
        browser_engine_set_youtube_signin_diagnostics(engine, true);
        BrowserSession *session = browser_engine_session(engine);
        CHECK(browser_session_cookie_set_http(session, "https://m.youtube.com/",
            "SAPISID=synthetic-late-a; Secure; Domain=youtube.com; Path=/"));
        CHECK(browser_engine_load_url(engine, "https://www.youtube.com/", true)
            && browser_engine_render_frame(engine, NULL));
        NavigationSession *nav = browser_engine_navigation(engine);
        if (lane == 1) CHECK(browser_engine_scroll_by(engine, 1));
        for (unsigned i = 0; i < 1024; i++) {
            bool changed = false; (void) browser_engine_run_idle_work(engine, &changed);
            if (changed) CHECK(browser_engine_render_frame(engine, NULL));
            if (lane == 0 ? browser_engine_youtube_login_verified(engine)
                : browser_engine_youtube_feed_state(engine) == BROWSER_YOUTUBE_FEED_READY) break;
        }
        uint64_t offered_identity = browser_engine_youtube_login_identity(engine);
        CHECK(offered_identity != 0);
        CHECK(lane != 1 || browser_engine_youtube_feed_state(engine) == BROWSER_YOUTUBE_FEED_READY);
        CHECK(browser_engine_take_youtube_login_events(engine) == BROWSER_YOUTUBE_LOGIN_VERIFIED);
        CHECK(browser_session_cookie_set_http(session, "https://m.youtube.com/",
            "SAPISID=synthetic-late-b; Secure; Domain=youtube.com; Path=/"));
        CHECK(browser_engine_youtube_login_identity(engine) == 0);
        if (lane == 1) {
            lxb_dom_node_t *root = lxb_dom_interface_node(nav->page.document.html);
            CHECK(browser_engine_focus_node(engine, test_reader_find_id(root, "yt-feed-refresh")));
            ControllerAction action; CHECK(browser_engine_activate(engine, &action));
        }
        bool changed = false; (void) browser_engine_run_idle_work(engine, &changed);
        CHECK(browser_engine_youtube_feed_state(engine) == BROWSER_YOUTUBE_FEED_FAILED);
        CHECK(browser_engine_youtube_signin_report(engine, report, sizeof(report))
            && strstr(report, "failure-kind=11") && strstr(report, "pending-publication=0"));
        lxb_dom_node_t *root = document_body_node(&nav->page.document);
        size_t text_length = 0; lxb_char_t *text = lxb_dom_node_text_content(root, &text_length);
        CHECK(text && !strstr((const char *) text, "First mock recommendation"));
        lxb_dom_document_destroy_text(root->owner_document, text);
        CHECK(browser_engine_shutdown(engine)); browser_engine_destroy(engine); fetch_trace_end();
    }
    return 0;
}

static int test_youtube_login_rejection_preserves_failures(void)
{
    static const char bootstrap[] = "<script>ytcfg.set({\"INNERTUBE_API_KEY\":\"fixture-key\","
        "\"INNERTUBE_CONTEXT_CLIENT_VERSION\":\"fixture-version\",\"INNERTUBE_CLIENT_NAME\":\"MWEB\","
        "\"VISITOR_DATA\":\"fixture-visitor\",\"LOGGED_IN\":true});</script>";
    static const char logged_out[] = "<script>ytcfg.set({\"LOGGED_IN\":false});</script>";
    BrowserDeviceProfile device; browser_device_profile_psp3000(&device);
    BrowserConfig config; browser_config_init(&config, &device);
    config.memory_limit = 16u * MIB; config.resources.web_fonts_enabled = false;
    char error[256];
    for (unsigned lane = 0; lane < 5; lane++) {
        CHECK(fetch_trace_replay_begin(TILEFINCH_TEST_SOURCE_DIR "/tests/fixtures/http-youtube-personalized",
            error, sizeof(error)));
        BrowserEngine *engine = browser_engine_create(&config, error, sizeof(error)); CHECK(engine);
        CHECK(browser_session_cookie_set_http(browser_engine_session(engine), "https://m.youtube.com/",
            "SAPISID=synthetic-rejection-session; Secure; Domain=youtube.com; Path=/"));
        CHECK(browser_engine_load_url(engine, "https://www.youtube.com/", true)
            && browser_engine_render_frame(engine, NULL));
        for (unsigned i = 0; i < 1024 && browser_engine_youtube_feed_state(engine) != BROWSER_YOUTUBE_FEED_CURRENT; i++) {
            bool changed = false; (void) browser_engine_run_idle_work(engine, &changed);
            if (changed) CHECK(browser_engine_render_frame(engine, NULL));
        }
        uint64_t offered = browser_engine_youtube_login_identity(engine); CHECK(offered);
        CHECK(browser_engine_take_youtube_login_events(engine) == BROWSER_YOUTUBE_LOGIN_VERIFIED);
        fetch_trace_end();
        GoogleIdentityRecord records[] = {
            {.url="https://m.youtube.com/", .status=200, .content_type="text/html",
                .body=lane == 1 ? logged_out : bootstrap},
            {.url="https://m.youtube.com/youtubei/v1/browse?key=fixture-key&prettyPrint=false",
                .status=lane == 3 ? 503 : 200, .method="POST", .content_type="application/json",
                .body=lane == 2 ? "{malformed" : lane == 4 ? "{\"responseContext\":{}}"
                    : "{\"responseContext\":{\"mainAppWebResponseContext\":{\"loggedOut\":true}}}"}
        };
        char fixture[128]; CHECK(write_google_identity_replay(fixture, records, 2)
            && fetch_trace_replay_begin(fixture, error, sizeof(error)));
        NavigationSession *nav = browser_engine_navigation(engine);
        lxb_dom_node_t *root = lxb_dom_interface_node(nav->page.document.html);
        CHECK(browser_engine_focus_node(engine, test_reader_find_id(root, "yt-feed-refresh")));
        ControllerAction action; CHECK(browser_engine_activate(engine, &action));
        for (unsigned i = 0; i < 1024; i++) {
            bool changed = false; (void) browser_engine_run_idle_work(engine, &changed);
            if (changed) CHECK(browser_engine_render_frame(engine, NULL));
            if (browser_engine_youtube_feed_state(engine) == BROWSER_YOUTUBE_FEED_FAILED) break;
        }
        CHECK(browser_engine_youtube_feed_state(engine) == BROWSER_YOUTUBE_FEED_FAILED);
        CHECK(browser_engine_youtube_login_identity(engine) == (lane < 2 ? 0 : offered));
        CHECK(browser_engine_take_youtube_login_events(engine) == (lane < 2 ? BROWSER_YOUTUBE_LOGIN_FAILED : 0));
        root = lxb_dom_interface_node(nav->page.document.html);
        CHECK((test_reader_find_id(root, "yt-signout-action") != NULL) == (lane >= 2));
        CHECK(browser_engine_shutdown(engine)); browser_engine_destroy(engine); fetch_trace_end();
        remove_google_identity_replay(fixture, 2);
    }
    return 0;
}

static int test_youtube_login_journey(void)
{
    CHECK(test_youtube_login_unseeded_flow() == 0);
    CHECK(test_youtube_login_display_failures() == 0);
    CHECK(test_youtube_login_manual_continuation() == 0);
    CHECK(test_youtube_login_failure_diagnostics() == 0);
    CHECK(test_youtube_login_late_session_changes() == 0);
    CHECK(test_youtube_login_rejection_preserves_failures() == 0);
    CHECK(youtube_lite_signin_handoff_url("https://www.youtube.com/signin?token=synthetic"));
    CHECK(!youtube_lite_signin_handoff_url("https://notyoutube.com/signin?token=synthetic")
        && !youtube_lite_signin_handoff_url("http://www.youtube.com/signin?token=synthetic")
        && !youtube_lite_signin_handoff_url("https://www.youtube.com/signin-extra"));
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
        lxb_dom_node_t *root = lxb_dom_interface_node(nav->page.document.html);
        CHECK(test_reader_find_id(root, "yt-feed-status") == NULL
            && test_reader_find_id(root, "yt-feed-refresh") == NULL);
        CHECK(strstr(nav->page.layout.links[1].url, "https://accounts.google.com/ServiceLogin?") == nav->page.layout.links[1].url);
        CHECK(browser_engine_commit_html(engine, "https://accounts.google.com/tilefinch-test-signin", form, sizeof(form) - 1u, false));
        root = lxb_dom_interface_node(nav->page.document.html);
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
        NavigationSession *nav = browser_engine_navigation(engine);
        lxb_dom_node_t *root = lxb_dom_interface_node(nav->page.document.html);
        CHECK(test_reader_find_id(root, "yt-feed-status") != NULL
            && test_reader_find_id(root, "yt-feed-refresh") != NULL);
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
