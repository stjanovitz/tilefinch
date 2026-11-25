#ifndef TILEFINCH_PSP_MEDIA_SESSION_INTERNAL_H
#define TILEFINCH_PSP_MEDIA_SESSION_INTERNAL_H

#include "tilefinch/psp_media_session.h"

#define PSP_MEDIA_JOB_MAXIMUM_PACKETS 2u

static inline uint64_t psp_media_internal_now_us(
    const PspMediaSession *media)
{
    return media != NULL && media->platform.now_us != NULL
        ? media->platform.now_us(media->platform.context) : 0;
}

uint64_t psp_media_session_decode_clock_us(
    const PspMediaSession *media, bool awaiting_first_frame);

/* Single owner-thread commit point for a controller event. User intents
   have no token. A stamped completion may only consume its own service,
   and an out-of-phase notification cannot cancel the expected result. */
static inline bool psp_media_commit_controller_event(
    PspMediaSession *media, const PspMediaEvent *event)
{
    if (event->service_epoch != 0
        && !psp_media_service_token_matches(&media->service, event)) {
        if (media->stale_service_completions != UINT32_MAX)
            media->stale_service_completions++;
        return false;
    }
    PspMediaDecision decision = psp_media_machine_transition(&media->machine, event);
    media->machine = decision.next;
    if (event->service_epoch != 0 && decision.handled && !decision.deliberate_noop)
        psp_media_service_token_clear(&media->service);
    if (decision.command != PSP_MEDIA_COMMAND_NONE)
        psp_media_service_token_begin(&media->service, decision.command,
                                      psp_media_internal_now_us(media), 0);
    return true;
}
BrowserYoutubeQuality psp_media_open_quality(PspMediaSession *media);
bool psp_media_offline_route(
    const PspMediaSession *media, const char *url);
bool psp_media_resolved_stream_reusable(const PspMediaSession *media);
void psp_media_raise_error(
    PspMediaSession *media, const char *message, const char *reason);
/* As psp_media_raise_error, with a catalog cause on the details line and
   the short English code for bug reports below it; the log keeps the code. */
void psp_media_raise_coded_error(
    PspMediaSession *media, const char *message, const char *cause,
    const char *code);
void psp_media_defer_present_error(PspMediaSession *media, const char *message);
void psp_media_retire_first_frame(PspMediaSession *media);
PspMediaEvent psp_media_service_completion(
    const PspMediaSession *media, PspMediaEventType type);
bool psp_media_open_phase(PspMediaJobPhase phase);
/* Scheduling only. The next phase still gets its own service token,
   cancellation and deadline checks. */
static inline bool psp_media_open_followup_due(
    const PspMediaSession *media, PspMediaJobPhase before, uint64_t elapsed_us)
{
    if (elapsed_us > 4000u || media->machine.state != PSP_MEDIA_SESSION_OPENING
        || media->service.command != PSP_MEDIA_COMMAND_START_OPEN_PHASE
        || !media->service.epoch || media->open_service_pending
        || !media->ui.visible || media->ui.failed || media->page_source
        || media->offline_source || media->use_swdec || media->stream.live_hls)
        return false;
    return (before == PSP_MEDIA_JOB_OPEN_RESOLVE
            && media->job_phase == PSP_MEDIA_JOB_OPEN_DECODER_PREPARE)
        || (before == PSP_MEDIA_JOB_OPEN_DECODER_PREPARE
            && media->job_phase == PSP_MEDIA_JOB_OPEN_VIDEO_RANGE)
        || (before == PSP_MEDIA_JOB_OPEN_VIDEO_PRIME
            && media->job_phase == PSP_MEDIA_JOB_OPEN_PLAYBACK);
}
bool psp_media_seek_phase(PspMediaJobPhase phase);
PspMediaPipelineState psp_media_owned_pipeline(
    const PspMediaSession *media);
void psp_media_session_checkpoint(
    PspMediaSession *media, const char *checkpoint);
void psp_media_finish_synchronous_quiesce(
    PspMediaSession *media, const char *checkpoint);
void psp_media_set_transport_priority(
    PspMediaSession *media, bool active);
void psp_media_publish_track_catalog(PspMediaSession *media);
void psp_media_release_presentation_preroll(PspMediaSession *media);
bool psp_media_begin_startup_preroll(PspMediaSession *media);
bool psp_media_cancel_requested(const PspMediaSession *media);
bool psp_media_cancel_callback(void *opaque);
void psp_media_pump_ranges(PspMediaSession *media);
size_t psp_media_range_bytes(const PspMediaSession *media);
size_t psp_media_free_memory(const PspMediaSession *media);
size_t psp_media_maximum_free_block(const PspMediaSession *media);
size_t psp_media_transport_rate_floor(
    uint64_t content_length, uint64_t duration_ms);
uint64_t psp_media_recovery_position_us(
    const PspMediaSession *media);
void psp_media_capture_resume_audio(PspMediaSession *media);
void psp_media_reset_resume_output(PspMediaSession *media);
void psp_media_remember_retry_state(
    PspMediaSession *media, bool resume_playing);
void psp_media_request_user_retry(PspMediaSession *media);
void psp_media_request_track_reopen(PspMediaSession *media);
void psp_media_clear_resolved_stream(PspMediaSession *media);
bool psp_media_prepare_open_network(PspMediaSession *media, bool begin);
/* Marks the failed-open panel as the unencrypted-audio question or its
   remembered no (SECURITY_MODEL.md, "Unencrypted audio"). */
void psp_media_present_insecure_audio(PspMediaSession *media);
void psp_media_job_failed(
    PspMediaSession *media, const char *operation, const char *error);
void psp_media_report_failure_snapshot(
    PspMediaSession *media, const char *stage, const char *message,
    const char *reason, bool terminal);
bool psp_media_retry_transport_expiry(PspMediaSession *media);
bool psp_media_retry_transport(
    PspMediaSession *media, const char *operation, const char *error,
    bool delivery_candidate_rejected);
bool psp_media_retry_delivery_failure(
    PspMediaSession *media, const char *operation, const char *error,
    bool delivery_candidate_rejected);
/* Opens the page's next video rendition when error says the current one
   cannot work on this device (media_candidate_refused). */
bool psp_media_retry_page_candidate(
    PspMediaSession *media, const char *error);
bool psp_media_retry_240p(
    PspMediaSession *media, const char *operation, const char *error,
    bool delivery_candidate_rejected);
bool psp_media_open_pump(
    PspMediaSession *media,
    const TilefinchCancellation *cancellation);
void psp_media_open_arm_wait_budget(PspMediaSession *media);
void psp_media_open_clear_wait_budget(PspMediaSession *media);
bool psp_media_seek_decode_pump(
    PspMediaSession *media,
    const TilefinchCancellation *cancellation);
bool psp_media_request_seek_with_resume(
    PspMediaSession *media, uint64_t target_us, bool preview,
    bool resume_playing);
void psp_media_interrupt_decode(PspMediaSession *media);
bool psp_media_machine_wants_playing(const PspMediaSession *media);
/* The title an open or recovery shows: `video_title` for video; audio-only
   media keeps the name of what it plays ("Opening audio" before it has
   one). */
const char *psp_media_resolving_title(const PspMediaSession *media,
                                      const char *video_title);
void psp_media_session_dispatch_event(
    PspMediaSession *media, PspMediaEvent event, const char *checkpoint);
void psp_media_buffering_end(PspMediaSession *media, uint64_t now_us);
void psp_media_buffering_begin(
    PspMediaSession *media, bool startup, uint64_t now_us);
void psp_media_buffering_update(
    PspMediaSession *media, const MediaPlaybackJobStats *stats,
    uint64_t now_us);
void psp_media_present_release_claimed_surface(PspMediaSession *media);
void psp_media_present_emit_after_release(PspMediaSession *media);

/* Shared with the host scrub regression: preview admission must synchronize
   the retained decoder as well as the UI's bounded target marker. */
/* What the title bar shows for the stream's title. */
static inline const char *psp_media_shown_title(PspMediaSession *media)
{
    return psp_ui_media_shown_title(&media->ui, media->budget,
                                    media->stream.title);
}

static inline void psp_media_begin_preview(
    PspMediaSession *media, uint64_t target_us, bool resume_playing)
{
    if (psp_media_scrub_begin(&media->scrub, resume_playing))
        media->job_restore_us = media->clock_us;
    /* Submission blocking alone leaves already-queued audio running while
       the UI clock is frozen. Cancel restores scrub.was_playing; committing
       a seek keeps the backend paused until its normal completion. */
    if (media->playback != NULL)
        media_playback_set_playing(media->playback, false);
    if (!media->machine.preview_active) {
        psp_media_session_dispatch_event(media, (PspMediaEvent) {
            .type = PSP_MEDIA_EVENT_PREVIEW_STARTED
        }, "preview-started");
    }
    psp_ui_media_set_seek_preview(&media->ui, target_us);
}

/* Validation/reporting is deliberately separate from lifecycle authority.
   These functions sample an already-owned session and never mutate playback
   policy, state-machine state, or backend ownership. Their only output is
   the validation log, so builds without it compile them to nothing. */
#ifdef TILEFINCH_PSP_VALIDATION_LOG
void psp_media_telemetry_report_slow_unit(
    PspMediaSession *media, const char *stage, uint64_t unit_us);
void psp_media_telemetry_report_feed(
    PspMediaSession *media, const char *phase);
#else
static inline void psp_media_telemetry_report_slow_unit(
    PspMediaSession *media, const char *stage, uint64_t unit_us)
{
    (void) media;
    (void) stage;
    (void) unit_us;
}
static inline void psp_media_telemetry_report_feed(
    PspMediaSession *media, const char *phase)
{
    (void) media;
    (void) phase;
}
#endif

#endif
