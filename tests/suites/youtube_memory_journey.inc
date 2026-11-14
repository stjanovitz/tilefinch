/* Synthetic response-keyed journeys: no account or live network required. */
#include "tilefinch/youtube_playlist.h"

static int test_youtube_repeated_journey_ownership(void)
{
    char *search = NULL, *watch = NULL, *next = NULL;
    size_t search_size = 0;
    search = youtube_costs_list_page("videoRenderer", "videoId", "TFTEST%05d",
        "title", "Fixture entry %d", &search_size);
    static const char playlist[] = "<script>var ytInitialData={\"contents\":["
        "{\"playlistVideoRenderer\":{\"videoId\":\"TFTEST00001\","
        "\"title\":{\"simpleText\":\"First\"},\"isPlayable\":true}},"
        "{\"playlistVideoRenderer\":{\"videoId\":\"TFTEST00002\","
        "\"title\":{\"simpleText\":\"Second\"},\"isPlayable\":true}}]};</script>";
    const char *results_url = "https://www.youtube.com/results?search_query=fixture";
    const char *details_url = "https://www.youtube.com/watch?v=TFTEST00001&tilefinch_view=details";
    const char *comments_url = "https://www.youtube.com/watch?v=TFTEST00001&tilefinch_view=comments";
    const char *playlist_url = "https://www.youtube.com/playlist?list=PLfixture";
    YoutubeReplayRecord records[4] = {
        {"GET", "https://m.youtube.com/results?search_query=fixture",
         "text/html", search, search_size}
    };
    records[1] = youtube_fixture_record("GET",
        "https://m.youtube.com/watch?v=TFTEST00001&bpctr=9999999999&has_verified=1",
        "text/html", "http-site-adapter-youtube/0000.body", &watch);
    records[2] = youtube_fixture_record("POST",
        "https://m.youtube.com/youtubei/v1/next?key=fixture-key&prettyPrint=false",
        "application/json", "http-site-adapter-youtube/0001.body", &next);
    records[3] = (YoutubeReplayRecord){"GET",
        "https://m.youtube.com/playlist?list=PLfixture", "text/html",
        playlist, sizeof(playlist) - 1u};
    char directory[128], error[256] = {0};
    CHECK(search && watch && next
          && write_youtube_records_replay(directory, records, 4));
    for (unsigned signed_in = 0; signed_in < 2; signed_in++) {
        BrowserDeviceProfile profile;
        browser_device_profile_psp3000(&profile);
        BrowserConfig config;
        browser_config_init(&config, &profile);
        config.memory_limit = 16u * MIB;
        config.history_capacity = BROWSER_PSP_APP_HISTORY_CAPACITY;
        config.resources.enabled = config.resources.web_fonts_enabled = false;
        BrowserEngine *engine = browser_engine_create(&config, error, sizeof(error));
        CHECK(engine && fetch_trace_replay_begin_response_keyed(
            directory, error, sizeof(error)));
        Budget *budget = browser_engine_budget(engine);
        BrowserSession *session = browser_engine_session(engine);
        if (signed_in) CHECK(browser_session_cookie_set_http(session,
            "https://m.youtube.com/", "SAPISID=synthetic-session; Secure; HttpOnly; Path=/"));
        size_t plateau = 0, allocations = 0;
        for (unsigned cycle = 0; cycle < 16; cycle++) {
            CHECK(browser_engine_load_url(engine, results_url, true));
            NavigationSession *nav = browser_engine_navigation(engine);
            lxb_dom_node_t *root = lxb_dom_interface_node(nav->page.document.html);
            CHECK(browser_engine_focus_node(engine, test_reader_find_id(root, "yt-search"))
                  && browser_engine_replace_text(engine, "edited fixture", 14u)
                  && browser_engine_scroll_by(engine, 50));
            int scroll = navigation_current(nav)->scroll_y;
            size_t focus = browser_engine_controller(engine)->focus_index;
            CHECK(browser_engine_load_url(engine, details_url, true)
                  && browser_engine_load_url(engine, comments_url, true)
                  && browser_engine_history_move(engine, false)
                  && browser_engine_history_move(engine, false));
            CHECK(navigation_current(nav)->scroll_y == scroll
                  && browser_engine_controller(engine)->focus_index == focus);
            root = lxb_dom_interface_node(nav->page.document.html);
            size_t length = 0;
            const char *value = document_control_value(
                test_reader_find_id(root, "yt-search"), &length);
            CHECK(value && length == 14u && !memcmp(value, "edited fixture", 14u));

            YoutubePlaylistQueue *queue = youtube_playlist_begin(
                budget, session, playlist_url, 0, true, NULL);
            CHECK(queue);
            for (unsigned pump = 0; pump < 512 && youtube_playlist_loading(queue); pump++)
                (void) youtube_playlist_pump(queue, NULL);
            YoutubePlaylistEntry rows[6];
            char video[256];
            CHECK(!youtube_playlist_failed(queue) && !youtube_playlist_loading(queue)
                  && youtube_playlist_view(queue, rows, 6) == 2
                  && youtube_playlist_take_video(queue, video, sizeof(video))
                  && youtube_playlist_step(queue, 1)
                  && youtube_playlist_take_video(queue, video, sizeof(video))
                  && strstr(video, "TFTEST00002"));
            youtube_playlist_destroy(queue);

            /* Retire queue loads and page candidates at different pump boundaries. */
            static const unsigned cuts[] = {0, 1, 4, 16};
            unsigned cut = cuts[cycle % 4];
            youtube_lite_forget_playlist_page(session, playlist_url);
            queue = youtube_playlist_begin(budget, session, playlist_url, 0, true, NULL);
            CHECK(queue);
            for (unsigned pump = 0; pump < cut; pump++)
                (void) youtube_playlist_pump(queue, NULL);
            youtube_playlist_destroy(queue);
            CHECK(browser_engine_begin_navigation_url(engine, details_url, MIB, 1000, true));
            BrowserNavigationJobStatus status = BROWSER_NAVIGATION_JOB_PENDING;
            for (unsigned pump = 0; pump < cut
                 && status == BROWSER_NAVIGATION_JOB_PENDING; pump++)
                status = browser_engine_pump_navigation(engine, NULL);
            browser_engine_cancel_navigation(engine, "synthetic journey cancellation");
            if (status == BROWSER_NAVIGATION_JOB_SUCCEEDED)
                CHECK(browser_engine_history_move(engine, false));
            CHECK(!strcmp(nav->page.document_url, results_url));
            CHECK(navigation_current(nav)->scroll_y == scroll
                  && browser_engine_controller(engine)->focus_index == focus);
            size_t before_refusal = budget->current;
            budget_inject_failure_after(budget, cycle % 4);
            YoutubeLiteLoadJob *refused = youtube_lite_load_begin(
                budget, session, details_url, MIB, 1000, error, sizeof(error));
            budget_clear_failure_injection(budget);
            youtube_lite_load_destroy(refused);
            CHECK(budget->current == before_refusal);
            /* Cache replacement and history are warm by cycle four. Compare
               matching cancellation boundaries rather than unlike live phases. */
            if (cycle % 4 == 3) {
                if (!plateau) {
                    plateau = budget->current;
                    allocations = budget_active_allocations(budget, NULL);
                } else {
                    printf("youtube ownership signed-in=%u cycle=%u bytes=%zu baseline=%zu allocations=%zu\n",
                        signed_in, cycle, budget->current, plateau,
                        budget_active_allocations(budget, NULL));
                    CHECK(budget->current == plateau
                          && budget_active_allocations(budget, NULL) == allocations);
                }
                CHECK(budget_categories_reconcile(budget));
            }
        }
        FetchTraceReplayStats replay = {0};
        CHECK(fetch_trace_replay_stats(&replay)
              && replay.unmatched_request_count == 0
              && replay.conflicting_request_count == 0);
        fetch_trace_end();
        CHECK(browser_engine_shutdown(engine));
        CHECK(budget->current == 0 && budget->external_reserved == 0);
        browser_engine_destroy(engine);
    }
    remove_youtube_records_replay(directory, 4);
    free(search); free(watch); free(next);
    return 0;
}
