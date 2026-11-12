#include "psp_media_session_internal.h"
#include <string.h>

#ifndef __PSP__
/* The host lifecycle test supplies the firmware ownership seam. */
extern bool media_psp_backend_quarantined(void);
#endif

void psp_media_clear_resolved_stream(PspMediaSession *media)
{
    /* Only the known timeline survives a same-video reopen. Resolved URLs,
       formats and track indices must still be refreshed. */
    uint64_t duration_ms = !media->stream.live_hls
        && psp_media_continuation_reopening(&media->continuation)
            ? media->stream.duration_ms : 0;
    memset(&media->stream, 0, sizeof(media->stream));
    media->stream.duration_ms = duration_ms;
    /* Counts revoke the old labels; the next publication overwrites them.
       Avoid repeatedly clearing both label arrays on pending resolver pumps. */
    media->ui_presentation.audio_track_count = 0;
    media->ui_presentation.subtitle_track_count = 0;
    media->ui_presentation.selected_audio_track = -1;
    media->ui_presentation.selected_subtitle_track = -1;
}

void psp_media_request_track_reopen(PspMediaSession *media)
{
    if (media == NULL || media->source[0] == '\0') return;
    /* The catalog can already be available while the new playback owner is
       still opening. Retire that open too, and prefer current user intent to
       clocks/flags left by the pipeline it replaced. */
    bool committed = psp_media_target_commit_pending(&media->pending_target);
    bool reopening = psp_media_continuation_reopening(&media->continuation);
    uint64_t resume_us = committed ? media->pending_target.target_us
        : reopening ? media->continuation.position_us
        : psp_media_recovery_position_us(media);
    bool resume_playing = committed ? media->pending_target.resume_playing
        : reopening ? media->continuation.playing
        : psp_media_scrub_active(&media->scrub) ? media->scrub.was_playing
        : psp_media_machine_wants_playing(media);
    psp_media_session_dispatch_event(media, (PspMediaEvent) {
        .type = PSP_MEDIA_EVENT_CLOSE,
        .retain_pipeline = false
    }, "track-switch-close");
    psp_media_pipeline_destroy(media);
    psp_media_finish_synchronous_quiesce(media, "track-switch-released");
    if (media_psp_backend_quarantined()) {
        psp_media_job_failed(
            media, "track switch", "VIDEO DECODER NEEDS APP RESTART");
        return;
    }
    media->clock_us = resume_us;
    psp_media_continuation_reopen(
        &media->continuation, resume_us, resume_playing);
    if (committed) psp_media_target_clear(&media->pending_target);
    media->reopen_seek_completion_pending = true;
    media->open_service_pending = true;
    psp_media_session_dispatch_event(media, (PspMediaEvent) {
        .type = PSP_MEDIA_EVENT_OPEN,
        .autoplay = resume_playing,
        .has_separate_audio = false,
        .audio_only = media->audio_only
    }, "track-switch-open");
    psp_ui_media_set_resolving(&media->ui, "Switching audio");
}

void psp_media_request_user_retry(PspMediaSession *media)
{
    if (media == NULL || media->source[0] == '\0') return;
    if (media_psp_backend_quarantined()) {
        media->ui.retry_unavailable = true;
        return;
    }
    /* The failed open's own play intent. Its pipeline records it as a
       continuation; an open that failed before it built one -- a resolve or
       probe failure, or the unencrypted-audio question -- has nothing to
       record, and its Retry still answers the press that started it. */
    bool resume_playing = media->ui.playing
        || media->machine.resume_target == PSP_MEDIA_RESUME_PLAYING;
    if (!psp_media_continuation_reopening(&media->continuation)
        && media->playback != NULL)
        psp_media_remember_retry_state(media, resume_playing);
    /* Terminal panels historically retained a paused pipeline and could
       leave the controller Playing. RETRY is valid only from Failed. Retire
       any retained/partial owner before opening a new generation, instead
       of clearing the panel and resuming that dead pipeline. */
    psp_media_session_dispatch_event(media, (PspMediaEvent) {
        .type = PSP_MEDIA_EVENT_CLOSE, .retain_pipeline = false
    }, "retry-close");
    psp_media_pipeline_destroy(media);
    psp_media_finish_synchronous_quiesce(media, "retry-released");
    if (media_psp_backend_quarantined()) {
        psp_media_job_failed(media, "retry", "VIDEO DECODER NEEDS APP RESTART");
        return;
    }
    media->transport_reresolve_attempts = 0;
    media->transport_refresh_rearm_us = 0;
    media->transport_next_expiry_check_us = 0;
    media->failure_report_level = 0;
    media->open_service_pending = true;
    psp_media_session_dispatch_event(media, (PspMediaEvent) {
        .type = PSP_MEDIA_EVENT_OPEN,
        .autoplay = psp_media_continuation_reopening(&media->continuation)
            ? media->continuation.playing : resume_playing,
        .audio_only = media->audio_only
    }, "retry-intent");
    psp_ui_media_set_resolving(
        &media->ui, psp_media_resolving_title(media, "Opening video"));
}

bool psp_media_prepare_open_network(PspMediaSession *media, bool begin)
{
    media->open_network_pending = false;
    if (media->offline_source || psp_media_offline_route(media, media->source)
        || media->platform.prepare_network == NULL) return true;
    PspMediaNetworkStatus status = psp_media_cancel_requested(media)
        ? PSP_MEDIA_NETWORK_FAILED : media->platform.prepare_network(
            media->platform.network_context, begin);
    if (status == PSP_MEDIA_NETWORK_READY) return true;
    if (status == PSP_MEDIA_NETWORK_WAIT) {
        media->open_network_pending = true;
        return false;
    }
    psp_media_job_failed(media, "network", "VIDEO NETWORK UNAVAILABLE");
    psp_media_pipeline_destroy(media);
    psp_media_finish_synchronous_quiesce(media, "network-released");
    return false;
}
