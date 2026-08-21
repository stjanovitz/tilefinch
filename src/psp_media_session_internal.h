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
BrowserYoutubeQuality psp_media_open_quality(PspMediaSession *media);
bool psp_media_offline_route(
    const PspMediaSession *media, const char *url);
bool psp_media_resolved_stream_reusable(const PspMediaSession *media);
void psp_media_raise_error(
    PspMediaSession *media, const char *message, const char *reason);
void psp_media_retire_first_frame(PspMediaSession *media);
PspMediaEvent psp_media_service_completion(
    const PspMediaSession *media, PspMediaEventType type);
bool psp_media_open_phase(PspMediaJobPhase phase);
bool psp_media_seek_phase(PspMediaJobPhase phase);
PspMediaPipelineState psp_media_owned_pipeline(
    const PspMediaSession *media);
void psp_media_session_checkpoint(
    PspMediaSession *media, const char *checkpoint);
void psp_media_finish_synchronous_quiesce(
    PspMediaSession *media, const char *checkpoint);
void psp_media_set_transport_priority(
    PspMediaSession *media, bool active);
void psp_media_release_presentation_preroll(
    PspMediaSession *media, bool clear_floor);
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
void psp_media_remember_retry_state(
    PspMediaSession *media, bool resume_playing);
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
bool psp_media_retry_240p(
    PspMediaSession *media, const char *operation, const char *error,
    bool delivery_candidate_rejected);
bool psp_media_open_pump(
    PspMediaSession *media,
    const TilefinchCancellation *cancellation);
void psp_media_seek_arm_wait_budget(PspMediaSession *media);
void psp_media_open_arm_wait_budget(PspMediaSession *media);
void psp_media_open_clear_wait_budget(PspMediaSession *media);
bool psp_media_seek_decode_pump(
    PspMediaSession *media,
    const TilefinchCancellation *cancellation);
void psp_media_interrupt_decode(PspMediaSession *media);
bool psp_media_machine_wants_playing(const PspMediaSession *media);
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

/* Validation/reporting is deliberately separate from lifecycle authority.
   These functions sample an already-owned session and never mutate playback
   policy, state-machine state, or backend ownership. */
void psp_media_telemetry_report_slow_unit(
    PspMediaSession *media, const char *stage, uint64_t unit_us);
void psp_media_telemetry_report_feed(
    PspMediaSession *media, const char *phase);

#endif
