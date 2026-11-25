/* Exercise the shipping queue activation, tick, menu projection and intent
   handlers. Replace the device-specific route open/close, not the provider,
   resolver, controller-token checks, playlist, or retained browser page. */
#include "../../src/psp_media_session_internal.h"
#include "tilefinch/ui_language.h"

static PspMediaEvent playlist_service_event(PspMediaSession *m, PspMediaEventType type)
{
    return (PspMediaEvent){
        .type = type, .service_epoch = m->service.epoch, .service_command = m->service.command};
}

static void playlist_finish_quiesce(PspMediaSession *m)
{
    if (m->machine.state != PSP_MEDIA_SESSION_QUIESCING) return;
    const PspMediaEventType done[] = {PSP_MEDIA_EVENT_ADMISSION_STOPPED,
        PSP_MEDIA_EVENT_TRANSPORT_CANCELLED, PSP_MEDIA_EVENT_BACKEND_QUIESCED};
    for (size_t i = 0; i < sizeof(done) / sizeof(done[0]); i++) {
        PspMediaEvent ev = playlist_service_event(m, done[i]);
        (void)psp_media_commit_controller_event(m, &ev);
    }
}

static void playlist_host_close(PspMediaSession *m)
{
    youtube_playlist_destroy(m->playlist);
    m->playlist = NULL;
    youtube_resolve_job_destroy(m->resolver_job);
    m->resolver_job = NULL;
    (void)psp_media_commit_controller_event(m, &(PspMediaEvent){.type = PSP_MEDIA_EVENT_CLOSE});
    playlist_finish_quiesce(m);
    m->ui.visible = m->ui_presentation.queue_active = false;
    m->source[0] = 0;
    browser_engine_release_page_network(m->platform.context);
}

static bool playlist_host_open(PspMediaSession *m, const char *url, uint64_t generation)
{
    if (generation != browser_engine_page_generation(m->platform.context)) return false;
    m->resolver_job = youtube_resolve_job_begin(m->budget, m->session, url, 360, 30000, NULL, NULL);
    if (!m->resolver_job) return false;
    snprintf(m->source, sizeof(m->source), "%s", url);
    (void)browser_engine_hold_page_network(m->platform.context, "playlist playback");
    (void)psp_media_commit_controller_event(
        m, &(PspMediaEvent){.type = PSP_MEDIA_EVENT_OPEN, .autoplay = true});
    psp_ui_media_set_resolving(&m->ui, "YouTube video");
    return true;
}

static void playlist_host_error(PspMediaSession *m, const char *message, const char *reason)
{
    psp_ui_media_set_error(&m->ui, reason ? reason : message);
}

#define psp_media_close playlist_host_close
#define psp_media_open_provider_route playlist_host_open
#define psp_media_raise_error playlist_host_error
#include "../../src/psp_media_playlist.inc"
#undef psp_media_raise_error
#undef psp_media_open_provider_route
#undef psp_media_close

static PspUiMediaIntent playlist_press(PspMediaSession *m, unsigned button)
{
    return psp_ui_media_update(
        &m->ui, &(PspUiInput){.pressed = button, .analog_x = 128, .analog_y = 128});
}

static bool playlist_resolves_to(
    PspMediaSession *m, const char *id, const char *title, const char *media_id)
{
    YoutubeStream stream = {0};
    return m->resolver_job && youtube_costs_resolve(m->resolver_job) == YOUTUBE_RESOLVE_JOB_COMPLETE
        && youtube_resolve_job_take(m->resolver_job, &stream) && !strcmp(stream.video_id, id)
        && !strcmp(stream.title, title) && strstr(stream.media_url, media_id);
}

static int test_youtube_interrupted_playlist(void)
{
    static const char playlist_url[] = "https://www.youtube.com/playlist?list=PLfixture";
    static const char player[] =
        "https://www.youtube.com/youtubei/v1/player?key=fixture-key&prettyPrint=false";
    static const char blocked[] = "{\"playabilityStatus\":{\"status\":\"UNPLAYABLE\","
                                  "\"reason\":\"This video is unavailable in your country\"}}";
    char responses[3][1024];
    const unsigned playable[] = {10, 12, 11}; /* 11 initially fails; 12 is on the next page. */
    for (size_t i = 0; i < sizeof(playable) / sizeof(playable[0]); i++)
        snprintf(responses[i], sizeof(responses[i]),
            "{\"playabilityStatus\":{\"status\":\"OK\"},"
            "\"videoDetails\":{\"videoId\":\"TFTEST%05u\",\"title\":\"Item %u\"},"
            "\"streamingData\":{\"formats\":[{\"itag\":18,\"width\":320,\"height\":240,\"bitrate\":200,"
            "\"contentLength\":\"2000\",\"approxDurationMs\":\"18950\","
            "\"mimeType\":\"video/mp4; codecs=\\\"avc1.42001e, mp4a.40.2\\\"\","
            "\"url\":\"https://rr2.googlevideo.com/videoplayback?expire=4102444800&itag=18&id=%u\"}]}}",
            playable[i], playable[i], playable[i]);
    size_t length = 0;
    char *list = youtube_costs_list_page(
        "playlistVideoRenderer", "videoId", "TFTEST%05d", "title", "Entry %d", &length);
    static const char identity[] = "<script>ytcfg.set({\"INNERTUBE_API_KEY\":\"fixture-key\","
                                   "\"INNERTUBE_CONTEXT_CLIENT_VERSION\":\"2.20260724.00.00\","
                                   "\"VISITOR_DATA\":\"fixture-visitor\"});</script>";
    char page[16 * 1024];
    CHECK(list && length < sizeof(page) - sizeof(identity));
    memcpy(page, identity, sizeof(identity) - 1);
    memcpy(page + sizeof(identity) - 1, list, length);
    free(list);
    YoutubeReplayRecord r[] = {{"GET", "https://m.youtube.com/playlist?list=PLfixture", "text/html",
                                   page, sizeof(identity) - 1 + length},
        {"POST", player, "application/json", responses[0], strlen(responses[0])},
        {"POST", player, "application/json", blocked, sizeof(blocked) - 1u},
        {"POST", player, "application/json", responses[1], strlen(responses[1])},
        {"POST", player, "application/json", responses[2], strlen(responses[2])}};
    for (unsigned cancel = 0; cancel <= 1; cancel++) {
        size_t count = cancel ? 3 : sizeof(r) / sizeof(r[0]);
        char directory[128], error[256] = {0};
        CHECK(write_youtube_records_replay(directory, r, count)
            && fetch_trace_replay_begin_response_keyed(directory, error, sizeof(error)));
        BrowserDeviceProfile profile;
        BrowserConfig config;
        browser_device_profile_psp3000(&profile);
        browser_config_init(&config, &profile);
        config.resources.enabled = config.resources.web_fonts_enabled = false;
        BrowserEngine *engine = browser_engine_create(&config, error, sizeof(error));
        CHECK(engine && browser_engine_load_url(engine, playlist_url, true)
            && browser_engine_scroll_by(engine, 700));
        ControllerAction action;
        CHECK(youtube_journey_play_card(engine, "TFTEST00010", &action));
        BrowserViewSnapshot before, after;
        CHECK(browser_engine_view_snapshot(engine, &before) && before.has_focus
            && before.scroll_y > 0);
        lxb_dom_node_t *selected = controller_focused_node(browser_engine_controller(engine));
        const uint64_t generation = browser_engine_page_generation(engine);
        static PspMediaSession m;
        memset(&m, 0, sizeof(m));
        m.machine = psp_media_machine_initial();
        psp_ui_media_init(&m.ui);
        psp_ui_media_bind_presentation(&m.ui, &m.ui_presentation);
        m.budget = browser_engine_budget(engine);
        m.session = browser_engine_session(engine);
        m.platform.context = engine;
        CHECK(psp_media_open_playlist_route(&m, action.url, playlist_url, generation));
        for (unsigned i = 0; i < 512 && !m.resolver_job; i++)
            (void)psp_media_playlist_tick(&m);
        CHECK(m.ui_presentation.queue_position == 10u);
        CHECK(playlist_resolves_to(&m, "TFTEST00010", "Item 10", "&id=10"));
        /* Supply the decoder's end notification after enabling Auto next
           through the real queue shortcut. No simulated playback timing. */
        CHECK(playlist_press(&m, PSP_UI_BUTTON_TOOLBAR).visual_changed
            && m.ui_presentation.track_menu_open);
        PspUiMediaIntent toggle = playlist_press(&m, PSP_UI_BUTTON_RELOAD);
        CHECK(toggle.action == PSP_UI_MEDIA_ACTION_AUTOMATIC_NEXT
            && psp_media_playlist_intent(&m, toggle));
        m.ui.ended = true;
        CHECK(psp_media_playlist_tick(&m) && m.ui_presentation.automatic_next);
        CHECK(m.ui_presentation.queue_position == 11u && strstr(m.source, "TFTEST00011"));
        CHECK(m.resolver_job && youtube_costs_resolve(m.resolver_job) == YOUTUBE_RESOLVE_JOB_FAILED
            && strstr(youtube_resolve_job_error(m.resolver_job), "unavailable in your country"));
        PspMediaEvent late = playlist_service_event(&m, PSP_MEDIA_EVENT_OPEN_PHASE_COMPLETE);
        PspMediaEvent failure = playlist_service_event(&m, PSP_MEDIA_EVENT_OPEN_FAILED);
        CHECK(late.service_epoch && psp_media_commit_controller_event(&m, &failure));
        playlist_finish_quiesce(&m);
        psp_ui_media_set_error(&m.ui, youtube_resolve_job_error(m.resolver_job));
        CHECK(m.machine.state == PSP_MEDIA_SESSION_FAILED && m.ui_presentation.queue_next);
        if (!cancel) {
            PspUiMediaIntent skip = playlist_press(&m, PSP_UI_BUTTON_PAGE_DOWN);
            CHECK(skip.action == PSP_UI_MEDIA_ACTION_NEXT && psp_media_playlist_intent(&m, skip));
            for (unsigned i = 0; i < 512u && !strstr(m.source,"TFTEST00012"); ++i)
                (void)psp_media_playlist_tick(&m);
            CHECK(m.ui_presentation.queue_position == 12u
                && !strcmp(m.ui_presentation.queue_titles[0], "Entry 12"));
            CHECK(m.resolver_job
                && youtube_resolve_job_pump(m.resolver_job) == YOUTUBE_RESOLVE_JOB_PENDING);
        } else {
            CHECK(playlist_press(&m, PSP_UI_BUTTON_CANCEL).action == PSP_UI_MEDIA_ACTION_CLOSE);
            playlist_host_close(&m);
        }
        /* A stale worker notification may arrive after its owner has been
           freed. Deliver the stamp, never invoke the freed resolver. */
        CHECK(!psp_media_commit_controller_event(&m, &late)
            && !psp_media_commit_controller_event(&m, &failure)
            && m.stale_service_completions == 2u);
        if (!cancel) {
            CHECK(m.machine.state == PSP_MEDIA_SESSION_OPENING
                && browser_engine_page_network_held(engine));
            CHECK(playlist_resolves_to(&m, "TFTEST00012", "Item 12", "&id=12"));
            for (unsigned i = 0; i < 8; ++i) (void)psp_media_playlist_tick(&m);
            CHECK(m.ui_presentation.queue_position == 12u && !m.ui.ended);
            PspUiMediaIntent previous = playlist_press(&m, PSP_UI_BUTTON_PAGE_UP);
            CHECK(previous.action == PSP_UI_MEDIA_ACTION_PREVIOUS && psp_media_playlist_intent(&m,previous)
                && psp_media_playlist_tick(&m) && m.ui_presentation.queue_position == 11u
                && playlist_resolves_to(&m,"TFTEST00011","Item 11","&id=11"));
            CHECK(playlist_press(&m, PSP_UI_BUTTON_CANCEL).action == PSP_UI_MEDIA_ACTION_CLOSE);
            playlist_host_close(&m);
        }
        CHECK(!m.playlist && !m.resolver_job && !m.ui.visible && !m.ui_presentation.queue_active);
        CHECK(!fetch_background_transport_active_operations()
            && !browser_engine_page_network_held(engine));
        CHECK(browser_engine_render_frame(engine, NULL)
            && browser_engine_view_snapshot(engine, &after));
        CHECK(browser_engine_page_generation(engine) == generation
            && !strcmp(after.url, playlist_url));
        CHECK(after.scroll_y == before.scroll_y && after.has_focus
            && after.focus_x == before.focus_x && after.focus_y == before.focus_y
            && after.focus_width == before.focus_width && after.focus_height == before.focus_height
            && controller_focused_node(browser_engine_controller(engine)) == selected);
        CHECK(browser_engine_activate(engine, &action) && action.prefer_native_media
            && strstr(action.url, "TFTEST00010"));
        /* A locally paged continuation fetches the source for the next
           slice. Neither the late service event nor idle ticks may replay
           an additional player request. */
        if (fetch_trace_replay_served_count() != count + !cancel)
            fprintf(stderr,"playlist requests: got %zu, expected %zu (cancel=%u)\n",
                    fetch_trace_replay_served_count(),count + !cancel,cancel);
        CHECK(fetch_trace_replay_served_count() == count + !cancel);
        CHECK(
            browser_engine_shutdown(engine) && !m.budget->current && !m.budget->external_reserved);
        browser_engine_destroy(engine);
        fetch_trace_end();
        remove_youtube_records_replay(directory, count);
    }
    return 0;
}
