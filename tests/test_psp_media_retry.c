#include "../src/psp_media_session_internal.h"
#include <stdio.h>
#include <string.h>

#define CHECK(c) do { if (!(c)) { \
    fprintf(stderr, "retry check failed line %d: %s\n", __LINE__, #c); \
    return 1; } } while (0)

static unsigned destroyed;
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
    media->playback = NULL;
    media->job_phase = PSP_MEDIA_JOB_NONE;
}
void psp_media_session_dispatch_event(PspMediaSession *media,
                                      PspMediaEvent event, const char *where)
{
    (void) where;
    media->machine = psp_media_machine_transition(&media->machine, &event).next;
}
void psp_media_finish_synchronous_quiesce(PspMediaSession *media,
                                         const char *where)
{
    if (media->machine.state != PSP_MEDIA_SESSION_QUIESCING) return;
    const PspMediaEventType events[] = {PSP_MEDIA_EVENT_ADMISSION_STOPPED,
        PSP_MEDIA_EVENT_TRANSPORT_CANCELLED, PSP_MEDIA_EVENT_BACKEND_QUIESCED};
    for (unsigned i = 0; i < 3; i++)
        psp_media_session_dispatch_event(media,
            (PspMediaEvent){.type = events[i]}, where);
}
void psp_media_job_failed(PspMediaSession *media, const char *op, const char *error)
{
    (void) op;
    psp_ui_media_set_error(&media->ui, error);
    psp_media_session_dispatch_event(media,
        (PspMediaEvent){.type = PSP_MEDIA_EVENT_OPEN_FAILED}, "failed");
}

int main(void)
{
    static PspMediaSession media;
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
