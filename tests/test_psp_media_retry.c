#include "../src/psp_media_session_internal.h"
#include <stdio.h>
#include <string.h>

#define CHECK(c) do { if (!(c)) { \
    fprintf(stderr, "retry check failed line %d: %s\n", __LINE__, #c); \
    return 1; } } while (0)

static unsigned destroyed;
static MediaPlayback *last_released;
static bool quarantined;
static bool cancelled;
static PspMediaNetworkStatus network_status;
static unsigned network_checks, network_begins;

bool psp_media_offline_route(const PspMediaSession *media, const char *url)
{
    (void) media;
    return strncmp(url, "file:", 5) == 0;
}
bool psp_media_cancel_requested(const PspMediaSession *media)
{
    (void) media;
    return cancelled;
}
static PspMediaNetworkStatus prepare_network(void *opaque, bool begin)
{
    (void) opaque;
    network_checks++;
    if (begin) network_begins++;
    return network_status;
}

bool media_psp_backend_quarantined(void) { return quarantined; }
uint64_t psp_media_recovery_position_us(const PspMediaSession *media)
{
    return media->clock_us;
}
bool psp_media_machine_wants_playing(const PspMediaSession *media)
{
    return media->machine.resume_target == PSP_MEDIA_RESUME_PLAYING;
}
void psp_media_remember_retry_state(PspMediaSession *media, bool playing)
{
    psp_media_continuation_reopen(&media->continuation, media->clock_us, playing);
}
const char *psp_media_resolving_title(const PspMediaSession *media,
                                      const char *video_title)
{
    return media->audio_only ? "Opening audio" : video_title;
}
void psp_media_pipeline_destroy(PspMediaSession *media)
{
    destroyed++;
    last_released = media->playback;
    media->playback = NULL;
    media->job_phase = PSP_MEDIA_JOB_NONE;
}
void psp_media_session_dispatch_event(PspMediaSession *media,
                                      PspMediaEvent event, const char *where)
{
    (void) where;
    (void) psp_media_commit_controller_event(media, &event);
}
void psp_media_finish_synchronous_quiesce(PspMediaSession *media,
                                         const char *where)
{
    if (media->machine.state != PSP_MEDIA_SESSION_QUIESCING) return;
    const PspMediaEventType events[] = {PSP_MEDIA_EVENT_ADMISSION_STOPPED,
        PSP_MEDIA_EVENT_TRANSPORT_CANCELLED, PSP_MEDIA_EVENT_BACKEND_QUIESCED};
    for (unsigned i = 0; i < 3; i++)
        psp_media_session_dispatch_event(media,
            (PspMediaEvent){.type = events[i], .service_epoch = media->service.epoch,
                .service_command = media->service.command}, where);
}
void psp_media_job_failed(PspMediaSession *media, const char *op, const char *error)
{
    (void) op;
    psp_ui_media_set_error(&media->ui, error);
    psp_media_session_dispatch_event(media,
        (PspMediaEvent){.type = PSP_MEDIA_EVENT_OPEN_FAILED}, "failed");
}

static PspMediaEvent completion(const PspMediaSession *m, PspMediaEventType type)
{
    return (PspMediaEvent){.type = type, .service_command = m->service.command,
                           .service_epoch = m->service.epoch};
}

static int cancelled_open_cannot_publish(PspMediaSession *m)
{
    memset(m, 0, sizeof(*m));
    Budget budget;
    BrowserSession session;
    budget_init(&budget, 8u * 1024u * 1024u);
    CHECK(browser_session_init(&session, &budget, 128u * 1024u));
    m->playlist = youtube_playlist_begin(&budget, &session,
        "https://www.youtube.com/playlist?list=PLfixture", 0, true, NULL);
    YoutubePlaylistPage page = {.count = 2};
    strcpy(page.entries[0].video_id, "TFTEST00001");
    strcpy(page.entries[1].video_id, "TFTEST00002");
    CHECK(m->playlist && youtube_playlist_accept_page(m->playlist, &page)
          && youtube_playlist_take_video(m->playlist, m->source, sizeof(m->source)));
    m->machine = psp_media_machine_initial();
    PspMediaEvent open = {.type = PSP_MEDIA_EVENT_OPEN, .autoplay = true};
    psp_media_session_dispatch_event(m, open, "A");
    psp_media_job_failed(m, "A", "open refused");
    psp_media_finish_synchronous_quiesce(m, "failed open");
    psp_media_request_user_retry(m);
    PspMediaEvent success = completion(m, PSP_MEDIA_EVENT_OPEN_PHASE_COMPLETE);
    PspMediaEvent failure = completion(m, PSP_MEDIA_EVENT_OPEN_FAILED);
    /* Next selects a new queue position during the retry. Use sentinel
       backend owners; the host does not open a firmware decoder. */
    CHECK(youtube_playlist_step(m->playlist, 1)
          && youtube_playlist_take_video(m->playlist, m->source, sizeof(m->source))
          && strstr(m->source, "TFTEST00002"));
    int owner_a, owner_b;
    m->playback = (MediaPlayback *)&owner_a;
    unsigned before = destroyed;
    psp_media_session_dispatch_event(m, (PspMediaEvent){.type=PSP_MEDIA_EVENT_CLOSE}, "cancel A");
    PspMediaEvent cleanup = completion(m, PSP_MEDIA_EVENT_ADMISSION_STOPPED);
    psp_media_pipeline_destroy(m);
    psp_media_finish_synchronous_quiesce(m, "retire A");
    CHECK(m->machine.state == PSP_MEDIA_SESSION_IDLE);
    m->ui.visible = false;
    m->ui.failed = true; /* Close hides the panel but can retain its route. */
    psp_media_request_user_retry(m);
    CHECK(destroyed == before + 1 && m->machine.state == PSP_MEDIA_SESSION_IDLE);
    psp_media_session_dispatch_event(m, open, "B");
    m->playback = (MediaPlayback *)&owner_b;
    PspMediaEvent current = completion(m, PSP_MEDIA_EVENT_OPEN_PHASE_COMPLETE);
    CHECK(current.service_epoch != success.service_epoch && success.service_epoch != 0);
    PspMediaEvent render_a = success;
    render_a.type = PSP_MEDIA_EVENT_PLAYBACK_FAILED;
    const PspMediaEvent late[] = {failure, cleanup, success, render_a};
    for (unsigned i=0; i<sizeof(late)/sizeof(late[0]); i++) {
        psp_media_session_dispatch_event(m, late[i], "retired A");
        CHECK(m->machine.opening_phase == PSP_MEDIA_OPEN_RESOLVING
              && m->machine.state == PSP_MEDIA_SESSION_OPENING
              && psp_media_service_token_matches(&m->service, &current)
              && m->playback == (MediaPlayback *)&owner_b && !m->have_frame
              && strstr(m->source, "TFTEST00002"));
    }
    CHECK(m->stale_service_completions == 4);
    /* Even with B's stamp a notification for the wrong phase must not
       consume the resolver token and strand B's valid completion. */
    psp_media_session_dispatch_event(m, completion(m, PSP_MEDIA_EVENT_BACKEND_QUIESCED), "wrong phase");
    CHECK(psp_media_service_token_matches(&m->service, &current));
    psp_media_session_dispatch_event(m, current, "B ready");
    CHECK(m->machine.opening_phase == PSP_MEDIA_OPEN_DECODER_PREPARE);
    /* Deliver both A outcomes again after B has advanced: neither may
       replace B's next service or publish A's picture. */
    PspMediaServiceToken preparing = m->service;
    psp_media_session_dispatch_event(m, success, "late A success");
    psp_media_session_dispatch_event(m, failure, "late A failure");
    CHECK(m->service.epoch == preparing.epoch && m->service.command == preparing.command
          && !m->have_frame && m->machine.opening_phase == PSP_MEDIA_OPEN_DECODER_PREPARE);
    psp_media_session_dispatch_event(m, current, "duplicate B ready");
    CHECK(m->machine.opening_phase == PSP_MEDIA_OPEN_DECODER_PREPARE
          && m->stale_service_completions == 7);
    for (unsigned i = 0; i < 8 && m->machine.state == PSP_MEDIA_SESSION_OPENING; i++)
        psp_media_session_dispatch_event(m, completion(m, PSP_MEDIA_EVENT_OPEN_PHASE_COMPLETE), "prepare B");
    CHECK(m->machine.state == PSP_MEDIA_SESSION_PRIMING);
    PspMediaEvent prime_b = completion(m, PSP_MEDIA_EVENT_PRIME_READY);
    PspMediaEvent prime_a = success;
    prime_a.type = PSP_MEDIA_EVENT_PRIME_READY;
    psp_media_session_dispatch_event(m, prime_a, "retired first picture");
    CHECK(m->machine.readiness == PSP_MEDIA_PRESENTATION_NEEDS_PRIME
          && m->machine.state == PSP_MEDIA_SESSION_PRIMING && !m->have_frame
          && psp_media_service_token_matches(&m->service, &prime_b));
    psp_media_session_dispatch_event(m, prime_b, "play B");
    psp_media_session_dispatch_event(m, render_a, "A failed after B started");
    CHECK(psp_media_machine_project_ui(&m->machine).playing
          && m->machine.state == PSP_MEDIA_SESSION_PLAYING && m->machine.failure == PSP_MEDIA_FAILURE_NONE);
    CHECK(destroyed == before + 1 && last_released == (MediaPlayback *)&owner_a
          && m->playback == (MediaPlayback *)&owner_b
          && youtube_playlist_position(m->playlist) == 1);
    youtube_playlist_destroy(m->playlist);
    m->playlist = NULL;
    browser_session_destroy(&session);
    CHECK(!budget.current);
    return 0;
}

int main(void)
{
    {
        PspMediaSession m = {0};
        m.playback = (MediaPlayback *)&m;
        m.ui.visible = true;
        m.first_frame_opened_us = 1000;
        CHECK(psp_media_first_picture_poll(&m, 1000) && psp_media_first_picture_poll(&m, 250999));
        CHECK(!psp_media_first_picture_poll(&m, 999) && !psp_media_first_picture_poll(&m, 251000));
        m.have_frame = true; CHECK(!psp_media_first_picture_poll(&m, 1000));
        m.have_frame = false; m.use_swdec = true; CHECK(!psp_media_first_picture_poll(&m, 1000));
        m.use_swdec = false; m.first_frame_opened_us = 0; CHECK(!psp_media_first_picture_poll(&m, 1000));
    }
    /* Only a quick, completed handoff may start another open unit before
       yielding for input. Never poll a pending worker twice in one turn. */
    {
        PspMediaSession m = {0};
        m.ui.visible = true;
        m.machine.state = PSP_MEDIA_SESSION_OPENING;
        m.service.command = PSP_MEDIA_COMMAND_START_OPEN_PHASE;
        m.service.epoch = 1;
        m.job_phase = PSP_MEDIA_JOB_OPEN_DECODER_PREPARE;
        CHECK(psp_media_open_followup_due(&m, PSP_MEDIA_JOB_OPEN_RESOLVE, 500));
        CHECK(!psp_media_open_followup_due(&m, m.job_phase, 0));
        m.job_phase = PSP_MEDIA_JOB_OPEN_VIDEO_RANGE;
        CHECK(psp_media_open_followup_due(&m, PSP_MEDIA_JOB_OPEN_DECODER_PREPARE, 100));
        CHECK(!psp_media_open_followup_due(&m, PSP_MEDIA_JOB_OPEN_DECODER_PREPARE, 5000));
        m.job_phase = PSP_MEDIA_JOB_OPEN_PLAYBACK;
        CHECK(psp_media_open_followup_due(&m, PSP_MEDIA_JOB_OPEN_VIDEO_PRIME, 50));
        CHECK(!psp_media_open_followup_due(&m, PSP_MEDIA_JOB_OPEN_VIDEO_DEMUX, 50));
#define CHECK_EXCLUDED(field, value) do { m.field = value; \
    CHECK(!psp_media_open_followup_due(&m, PSP_MEDIA_JOB_OPEN_VIDEO_PRIME, 50)); } while (0)
        CHECK_EXCLUDED(open_service_pending, true); m.open_service_pending = false;
        CHECK_EXCLUDED(ui.failed, true); m.ui.failed = false;
        CHECK_EXCLUDED(ui.visible, false); m.ui.visible = true;
        CHECK_EXCLUDED(page_source, true); m.page_source = false;
        CHECK_EXCLUDED(offline_source, true); m.offline_source = false;
        CHECK_EXCLUDED(use_swdec, true); m.use_swdec = false;
        CHECK_EXCLUDED(stream.live_hls, true); m.stream.live_hls = false;
        CHECK_EXCLUDED(service.command, PSP_MEDIA_COMMAND_NONE);
        m.service.command = PSP_MEDIA_COMMAND_START_OPEN_PHASE;
        CHECK_EXCLUDED(machine.state, PSP_MEDIA_SESSION_FAILED);
#undef CHECK_EXCLUDED
    }
    static PspMediaSession media;
    CHECK(cancelled_open_cannot_publish(&media) == 0);
    int backend;
    for (unsigned playing = 0; playing < 2; playing++) {
        memset(&media, 0, sizeof(media));
        strcpy(media.source, "https://video.test/playlist.m3u8");
        media.machine = psp_media_machine_initial();
        media.machine.state = playing ? PSP_MEDIA_SESSION_PLAYING
                                      : PSP_MEDIA_SESSION_PAUSED;
        media.machine.pipeline = PSP_MEDIA_PIPELINE_FULL;
        media.machine.has_plan = true;
        media.ui.failed = media.ui.visible = true;
        media.playback = (MediaPlayback *)&backend;
        media.clock_us = 105813333;
        psp_media_continuation_reopen(&media.continuation, media.clock_us,
                                      playing != 0);
        media.transport_reresolve_attempts = 2;
        destroyed = 0;
        psp_media_request_user_retry(&media);
        CHECK(destroyed == 1 && media.playback == NULL);
        CHECK(media.machine.state == PSP_MEDIA_SESSION_OPENING);
        CHECK(media.machine.resume_target == (playing ? PSP_MEDIA_RESUME_PLAYING
                                                      : PSP_MEDIA_RESUME_PAUSED));
        CHECK(media.continuation.position_us == 105813333);
        CHECK(media.continuation.playing == (playing != 0));
        CHECK(media.open_service_pending && !media.ui.failed);
        /* A repeated Retry from the same panel doesn't retire the new open. */
        psp_media_request_user_retry(&media);
        CHECK(destroyed == 1 && media.open_service_pending);
        CHECK(media.transport_reresolve_attempts == 0);
        CHECK(psp_media_machine_violations(&media.machine) == 0);
    }
    /* An open that fails before it builds a pipeline -- a probe failure,
       or the unencrypted-audio question a page station raises -- records
       no continuation. Its Retry ("Play anyway") must still play: opening
       paused left a station with nothing to decode until the first-output
       watchdog ended it. A paused open's Retry stays paused. */
    for (unsigned autoplay = 0; autoplay < 2; autoplay++) {
        memset(&media, 0, sizeof(media));
        strcpy(media.source, "http://127.0.0.1:8000/stream");
        media.audio_only = true;
        media.machine = psp_media_machine_initial();
        psp_media_session_dispatch_event(&media, (PspMediaEvent) {
            .type = PSP_MEDIA_EVENT_OPEN, .autoplay = autoplay != 0,
            .audio_only = true
        }, "route-open");
        psp_media_job_failed(&media, "page audio",
                             "PLAY UNENCRYPTED AUDIO?");
        psp_media_finish_synchronous_quiesce(&media, "failed");
        CHECK(media.machine.state == PSP_MEDIA_SESSION_FAILED);
        CHECK(media.playback == NULL
              && !psp_media_continuation_reopening(&media.continuation));
        media.ui.failed = media.ui.visible = true;
        psp_media_request_user_retry(&media);
        CHECK(media.machine.state == PSP_MEDIA_SESSION_OPENING);
        CHECK(media.machine.audio_only);
        CHECK(media.machine.resume_target == (autoplay
                  ? PSP_MEDIA_RESUME_PLAYING : PSP_MEDIA_RESUME_PAUSED));
        CHECK(psp_media_machine_violations(&media.machine) == 0);
    }
    quarantined = true;
    destroyed = 0;
    media.open_service_pending = false;
    psp_media_request_user_retry(&media);
    CHECK(destroyed == 0 && !media.open_service_pending);
    CHECK(media.ui.retry_unavailable);
    quarantined = false;

    memset(&media, 0, sizeof(media));
    psp_ui_media_bind_presentation(&media.ui, &media.ui_presentation);
    psp_media_continuation_reopen(&media.continuation, 42000000, false);
    media.stream.duration_ms = 597120;
    strcpy(media.stream.media_url, "https://video.test/expired");
    media.ui_presentation.audio_track_count = 2;
    media.ui_presentation.subtitle_track_count = 2;
    psp_media_clear_resolved_stream(&media);
    CHECK(media.stream.duration_ms == 597120);
    CHECK(!media.stream.media_url[0]);
    CHECK(!media.ui_presentation.audio_track_count
          && !media.ui_presentation.subtitle_track_count);
    /* Repeated pending resolver pumps must not erase the known timeline. */
    psp_media_clear_resolved_stream(&media);
    CHECK(media.stream.duration_ms == 597120);
    psp_media_continuation_clear(&media.continuation);
    psp_media_clear_resolved_stream(&media);
    CHECK(media.stream.duration_ms == 0);
    psp_media_continuation_reopen(&media.continuation, 0, true);
    media.stream.duration_ms = 30000;
    media.stream.live_hls = true;
    psp_media_clear_resolved_stream(&media);
    CHECK(media.stream.duration_ms == 0); /* Live window is not a VOD timeline. */

    /* A second audio choice can arrive after the replacement has resolved
       its catalog but before it has a playback owner. Preserve the newest
       seek/pause intent, not the old pipeline's clock or stale job flag. */
    for (unsigned playing = 0; playing < 2; playing++) {
        memset(&media, 0, sizeof(media));
        strcpy(media.source, "https://video.test/watch");
        strcpy(media.track_preferences.audio_track_id, "alternate");
        strcpy(media.track_preferences.caption_track_id, "captions");
        media.machine = psp_media_machine_initial();
        media.machine.state = PSP_MEDIA_SESSION_OPENING;
        media.machine.resume_target = playing ? PSP_MEDIA_RESUME_PLAYING
                                             : PSP_MEDIA_RESUME_PAUSED;
        media.clock_us = 12000000;
        media.job_resume_playing = true; /* Belongs to the retired job. */
        psp_media_continuation_reopen(&media.continuation, 42000000, playing);
        destroyed = 0;
        psp_media_request_track_reopen(&media);
        CHECK(destroyed == 1 && media.open_service_pending);
        CHECK(media.continuation.position_us == 42000000);
        CHECK(media.continuation.playing == (playing != 0));
        CHECK(!strcmp(media.track_preferences.audio_track_id, "alternate"));
        CHECK(!strcmp(media.track_preferences.caption_track_id, "captions"));
        CHECK(psp_media_machine_violations(&media.machine) == 0);

        psp_media_target_commit(&media.pending_target, 67000000, false);
        psp_media_request_track_reopen(&media);
        CHECK(media.continuation.position_us == 67000000);
        CHECK(!media.continuation.playing);
        CHECK(!psp_media_target_commit_pending(&media.pending_target));
    }

    media.platform.prepare_network = prepare_network;
    network_status = PSP_MEDIA_NETWORK_FAILED;
    media.offline_source = true;
    CHECK(psp_media_prepare_open_network(&media, true));
    CHECK(network_checks == 0);
    media.offline_source = false;
    strcpy(media.source, "file:/clip.mp4");
    CHECK(psp_media_prepare_open_network(&media, true));
    CHECK(network_checks == 0);
    strcpy(media.source, "https://video.test/playlist.m3u8");
    CHECK(!psp_media_prepare_open_network(&media, true));
    CHECK(network_checks == 1 && media.ui.failed);
    CHECK(media.machine.state == PSP_MEDIA_SESSION_FAILED);
    /* The user reconnects, then Retry must create another valid open. */
    network_status = PSP_MEDIA_NETWORK_WAIT;
    psp_media_request_user_retry(&media);
    CHECK(media.machine.state == PSP_MEDIA_SESSION_OPENING);
    CHECK(!psp_media_prepare_open_network(&media, true));
    CHECK(media.open_network_pending && !media.ui.failed);
    CHECK(!psp_media_prepare_open_network(&media, false));
    CHECK(network_begins == 2);
    network_status = PSP_MEDIA_NETWORK_READY;
    CHECK(psp_media_prepare_open_network(&media, false));
    CHECK(network_checks == 4 && !media.ui.failed);
    CHECK(!media.open_network_pending);
    cancelled = true;
    CHECK(!psp_media_prepare_open_network(&media, false));
    CHECK(network_checks == 4);
    cancelled = false;
    network_status = PSP_MEDIA_NETWORK_WAIT;
    psp_media_request_user_retry(&media);
    CHECK(!psp_media_prepare_open_network(&media, true));
    cancelled = true;
    CHECK(!psp_media_prepare_open_network(&media, false));
    CHECK(network_checks == 5 && media.ui.failed);
    CHECK(!media.open_network_pending);
    puts("media retry tests passed");
    return 0;
}
