#include "tilefinch/psp_media_session.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "media_backend_psp_policy.h"
#include "psp_media_session_internal.h"
#include "tilefinch/platform.h"
#include "tilefinch/psp_log.h"

#define printf psp_log_printf
#define KIB 1024u
/*
 * One access unit per browser frame is not playback.
 *
 * The bounded pump stops at the first queued native job, and the codec worker
 * publishes its result a whole main-loop iteration later, so a playing session
 * moved at most one packet per frame. 240p30 with 44.1 kHz AAC needs about 73
 * packets a second -- 30 pictures and 43 1024-sample blocks -- which no
 * per-frame budget reaches. The decoder therefore fell behind the wall clock
 * from the first second, the PCM queue drained faster than it refilled, and
 * playback degraded to a near-still picture with nothing audible behind it.
 *
 * Spend a bounded slice of each frame draining more of the ladder. The slice
 * is wall-clock bounded and cancellation is observed inside every unit, so the
 * interval in which a Circle press goes unanswered is unchanged; the pump
 * stops early the moment a unit makes no progress, which is what happens as
 * soon as the pipeline has caught up with its decode horizon.
 *
 * A unit that submitted nothing is not by itself no progress. The Media
 * Engine holds one job at a time and a picture costs it several milliseconds,
 * so the unit that finds the decoder busy is the ordinary case in a session
 * that is behind: it takes its bounded collect wait, yields to the worker,
 * and the unit after it submits. Ending the pump there is what pinned a
 * playing session to roughly one unit per browser frame -- cycle E3 reports
 * 1211 packets against 4367 calls over 35 seconds, about 35 units a second
 * where the stream needs 73. Keep going while the only thing in the way is
 * an in-flight job, and stop when nothing moved and nothing is pending.
 *
 * The slice is the bound that matters. It is checked before every unit and
 * never inside one, so the pump's worst case is the slice plus a single unit
 * -- one bounded wait and one firmware-free submission -- and the presenter
 * that follows now costs half of what it did, which is where the wall-clock
 * room for a longer slice comes from.
 */
#define PSP_MEDIA_PUMP_SLICE_US 12000u
#define PSP_MEDIA_PUMP_MAXIMUM_UNITS 8u
/*
 * The presentation clock is the browser's own elapsed time, and nothing ever
 * reconciled it with the pictures that actually reached the screen. A decoder
 * running below real time therefore left the clock describing a position the
 * user was never shown: the scrubber, the buffering verdict, and the saved
 * resume position all ran away from the picture, and the decode horizon
 * (clock + lead) ran away from the content the demuxer still had to deliver.
 * Re-anchor the clock to the presented picture when it drifts past this bound,
 * and only in a pump that actually presented one -- a stream with no pictures
 * to anchor to must keep its free-running clock, or the horizon would freeze
 * and stop the pipeline it is meant to protect.
 */
#define PSP_MEDIA_CLOCK_MAXIMUM_LEAD_US 1000000u
/*
 * Media network work runs on the interactive thread inside a cooperate scope,
 * and libcurl reaches our cancellation callback only from its progress
 * callback -- which does not run during connect. Bound a single attempt's
 * connect phase to three seconds so the longest interval in which the main
 * thread cannot observe a Circle press is comparable to one supervisor
 * heartbeat plus one primitive, not to half the request deadline (7.5 s for a
 * range read, 15 s for a resolve).
 */
#define PSP_MEDIA_CONNECT_TIMEOUT_MS 3000
#define PSP_OFFLINE_VIDEO_PREFIX \
    "https://tilefinch.local/offline/video?id="
/* A failure snapshot is an exceptional Memory Stick write, never telemetry.
   Leave two writes for non-media failures under the user's ten-write run
   ceiling even if several distinct videos fail in one process. */
#define PSP_MEDIA_FAILURE_REPORT_MAXIMUM_WRITES 8u

uint64_t psp_media_recovery_position_us(
    const PspMediaSession *media);
static uint64_t psp_media_now_us(const PspMediaSession *media);
static void psp_media_apply_audio_hold(PspMediaSession *media);
void psp_media_release_presentation_preroll(
    PspMediaSession *media, bool clear_floor);

void psp_media_report_failure_snapshot(
    PspMediaSession *media, const char *stage, const char *message,
    const char *reason, bool terminal)
{
    if (media == NULL || media->platform.write_failure_report == NULL)
        return;
    unsigned level = terminal ? 2u : 1u;
    if (media->failure_report_level >= level
        || media->failure_report_writes
             >= PSP_MEDIA_FAILURE_REPORT_MAXIMUM_WRITES) return;

    MediaHttpRangeStats video = {0};
    MediaHttpRangeStats audio = {0};
    MediaBackendStats backend = {0};
    (void) media_http_range_stats(media->range, &video);
    (void) media_http_range_stats(media->audio_range, &audio);
    if (media->playback != NULL)
        (void) media_playback_backend_stats(media->playback, &backend);
    long http_status = video.failures != 0
        ? video.last_http_status
        : audio.failures != 0
            ? audio.last_http_status
            : video.last_http_status != 0
                ? video.last_http_status : audio.last_http_status;
    uint64_t now_us = psp_media_now_us(media);
    uint64_t buffering_us = media->buffering_service_active
        && now_us >= media->network_buffer_started_us
        ? now_us - media->network_buffer_started_us : 0;
    /* Keep structured diagnostics first so even an unexpectedly long free-
       text reason cannot hide the audio half of a split-stream failure. This
       is failure-only stack storage; it never enlarges a frame-loop stack. */
    char detail[1024];
    snprintf(
        detail, sizeof(detail),
        "state=%d phase=%d quality=%u itag=%d attempts=%u/%u "
        "fallback=%u clock-us=%llu "
        "buffering=%d buffer-us=%llu "
        "video-http=%ld video-fail=%zu video-req=%zu video-retry=%zu "
        "video-bytes=%zu video-last=%llu+%zu video-pending=%d "
        "video-inflight=%zu "
        "audio-http=%ld audio-fail=%zu audio-req=%zu audio-retry=%zu "
        "audio-bytes=%zu audio-last=%llu+%zu audio-pending=%d "
        "audio-inflight=%zu message=%.96s reason=%.160s",
        (int) media->machine.state, (int) media->job_phase,
        (unsigned) media->requested_quality, media->stream.itag,
        media->transport_reresolve_attempts,
        PSP_MEDIA_TRANSPORT_REFRESH_MAXIMUM_ATTEMPTS,
        media->quality_fallback_attempted ? 1u : 0u,
        (unsigned long long) media->clock_us,
        media->buffering_service_active ? 1 : 0,
        (unsigned long long) buffering_us,
        video.last_http_status, video.failures, video.requests,
        video.retry_attempts, video.bytes_received,
        (unsigned long long) video.last_read_offset,
        video.last_read_length, video.window_pending ? 1 : 0,
        video.bytes_in_flight,
        audio.last_http_status, audio.failures, audio.requests,
        audio.retry_attempts, audio.bytes_received,
        (unsigned long long) audio.last_read_offset,
        audio.last_read_length, audio.window_pending ? 1 : 0,
        audio.bytes_in_flight,
        message == NULL ? "media failure" : message,
        reason == NULL ? "" : reason);
    if (media->platform.write_failure_report(
            media->platform.context,
            stage == NULL ? "media-playback" : stage,
            detail, media->source, http_status,
            backend.last_native_error)) {
        media->failure_report_level = level;
        media->failure_report_writes++;
    }
}

/*
 * Raise the failed player panel, and commit the validation log to the Memory
 * Stick as the failure becomes visible to the user.
 *
 * A media failure is very often the last thing that happens before the device
 * is powered off or freezes, and the log is only synchronized to the card at
 * checkpoints and at exit -- neither of which the media pipeline reaches. The
 * sync costs tens to low-hundreds of milliseconds on real hardware, so it is
 * spent once, on the transition into the failed state: a caller that reports
 * a second reason for an already-failed panel has already paid for it, and
 * shipping builds compile psp_log_flush out at the call site entirely.
 */
void psp_media_raise_error(
    PspMediaSession *media, const char *message, const char *reason)
{
    if (media == NULL) return;
    bool was_failed = media->ui.failed;
    if (reason != NULL) {
        psp_ui_media_set_error_reason(&media->ui, message, reason);
    } else {
        psp_ui_media_set_error(&media->ui, message);
    }
    if (!was_failed) {
        psp_media_report_failure_snapshot(
            media, "media-playback", message, reason, true);
        (void) psp_log_flush(true);
    }
}

/*
 * Close the first-frame transaction. Its pump keeps decode work pending, so
 * every terminal outcome must retire it or the loop keeps running and later
 * generic diagnoses overwrite the first, specific one.
 */
void psp_media_retire_first_frame(PspMediaSession *media)
{
    if (media == NULL) return;
    media->pause_boundary_pending = false;
    media->first_frame_started_us = 0;
    media->first_frame_opened_us = 0;
    media->first_frame_pump_us = 0;
}

/*
 * Choose the quality a fresh open should ask the resolver for. The promoted
 * wide program admits the profile's 360p preference; explicit compatibility
 * mode and the process-local first-AU rejection latch clamp to the proven
 * 240p program. The latch prevents every later open from repeating a full
 * resolve, range read, demux, create and prime just to fail the same way.
 * Offline routes carry their own saved rendition and are not resolved.
 */
BrowserYoutubeQuality psp_media_open_quality(PspMediaSession *media)
{
    BrowserYoutubeQuality preferred =
        browser_profile_youtube_quality(media->profile);
    int knob = media_psp_backend_wide_program();
    bool enabled = psp_media_wide_program_enabled(knob);
    bool rejected = media_psp_backend_wide_program_rejected();
    unsigned admitted = psp_media_admitted_quality(
        (unsigned) preferred, rejected, enabled);
    printf(
        "tilefinch-media-quality: preferred=%up admitted=%up program=%s "
        "knob=%s reason=%s\n",
        (unsigned) preferred, admitted,
        admitted > 240u ? "wide-mode5-type1" : "proven-mode4-type3",
        psp_media_wide_program_name(knob),
        admitted == (unsigned) preferred ? "profile-preference"
            : rejected ? "wide-program-rejected"
                       : "wide-program-disabled");
    return (BrowserYoutubeQuality) admitted;
}

bool psp_media_offline_route(const PspMediaSession *media,
                             const char *url)
{
    return media != NULL && media->platform.resolve_offline != NULL
        && url != NULL
        && strncmp(url, PSP_OFFLINE_VIDEO_PREFIX,
                   strlen(PSP_OFFLINE_VIDEO_PREFIX)) == 0;
}

static uint64_t psp_media_now_us(const PspMediaSession *media)
{
    return media != NULL && media->platform.now_us != NULL
        ? media->platform.now_us(media->platform.context) : 0;
}

PspMediaEvent psp_media_service_completion(
    const PspMediaSession *media, PspMediaEventType type)
{
    PspMediaEvent event = {.type = type};
    if (media != NULL
        && media->service.command != PSP_MEDIA_COMMAND_NONE) {
        event.service_command = media->service.command;
        event.service_epoch = media->service.epoch;
    }
    return event;
}

bool psp_media_open_phase(PspMediaJobPhase phase)
{
    return phase >= PSP_MEDIA_JOB_OPEN_RESOLVE
        && phase <= PSP_MEDIA_JOB_OPEN_PLAYBACK;
}

bool psp_media_seek_phase(PspMediaJobPhase phase)
{
    return phase == PSP_MEDIA_JOB_SEEK_PREPARE
        || phase == PSP_MEDIA_JOB_SEEK_DECODE
        || phase == PSP_MEDIA_JOB_PREVIEW_RESTORE_PREPARE
        || phase == PSP_MEDIA_JOB_PREVIEW_RESTORE_DECODE;
}

PspMediaPipelineState psp_media_owned_pipeline(
    const PspMediaSession *media)
{
    if (media->playback != NULL) return PSP_MEDIA_PIPELINE_FULL;
    /* A resolver job and an opening phase are invoked work, not a media
       pipeline. The first owned range/demux object makes the pipeline
       partial; this keeps the sampled resource check on the
       same resource definition as the pure model. */
    if (media->range != NULL
        || media->audio_range != NULL || media->file_range != NULL
        || media->audio_file_range != NULL || media->demux != NULL
        || media->audio_demux != NULL)
        return PSP_MEDIA_PIPELINE_PARTIAL;
    return PSP_MEDIA_PIPELINE_NONE;
}

static PspMediaPresentationReadiness psp_media_sample_readiness(
    const PspMediaSession *media)
{
    if (media == NULL || media->buffering_service_active)
        return PSP_MEDIA_PRESENTATION_NEEDS_SOURCE;
    if (!media->have_frame || media->presentation_preroll_audio_held
        || media->pause_boundary_pending)
        return PSP_MEDIA_PRESENTATION_NEEDS_PRIME;
    return PSP_MEDIA_PRESENTATION_READY;
}

static bool psp_media_projection_managed_state(
    PspMediaSessionState state)
{
    return state == PSP_MEDIA_SESSION_PRIMING
        || state == PSP_MEDIA_SESSION_PLAYING
        || state == PSP_MEDIA_SESSION_PAUSED
        || state == PSP_MEDIA_SESSION_BUFFERING
        || state == PSP_MEDIA_SESSION_SEEKING
        || state == PSP_MEDIA_SESSION_RECOVERING
        || state == PSP_MEDIA_SESSION_DORMANT;
}

bool psp_media_machine_wants_playing(const PspMediaSession *media)
{
    if (media == NULL) return false;
    if (media->machine.state == PSP_MEDIA_SESSION_PLAYING) return true;
    return (media->machine.state == PSP_MEDIA_SESSION_OPENING
            || media->machine.state == PSP_MEDIA_SESSION_PRIMING
            || media->machine.state == PSP_MEDIA_SESSION_BUFFERING
            || media->machine.state == PSP_MEDIA_SESSION_SEEKING
            || media->machine.state == PSP_MEDIA_SESSION_RECOVERING)
        && media->machine.resume_target == PSP_MEDIA_RESUME_PLAYING;
}

static void psp_media_apply_active_projection(PspMediaSession *media)
{
    if (media == NULL
        || !psp_media_projection_managed_state(media->machine.state))
        return;
    PspMediaUiProjection projection =
        psp_media_machine_project_ui(&media->machine);
    psp_ui_media_apply_projection(&media->ui, &projection);
    media->controller_audio_hold =
        media->machine.state == PSP_MEDIA_SESSION_PRIMING
        || media->machine.state == PSP_MEDIA_SESSION_SEEKING
        || media->machine.state == PSP_MEDIA_SESSION_RECOVERING
        || media->machine.preview_active;
    psp_media_apply_audio_hold(media);
}

static void psp_media_dispatch(
    PspMediaSession *media, PspMediaEvent event, const char *checkpoint);

#ifdef TILEFINCH_PSP_VALIDATION_LOG
static void psp_media_controller_trace(
    PspMediaSession *media, PspMediaEventType event,
    PspMediaSessionState from, PspMediaSessionState to,
    uint32_t violations, const char *checkpoint)
{
    unsigned at = media->controller_trace_head;
    media->controller_trace_event[at] = event;
    media->controller_trace_from[at] = from;
    media->controller_trace_to[at] = to;
    media->controller_trace_violations[at] = violations;
    media->controller_trace_checkpoint[at] = checkpoint;
    media->controller_trace_head = (at + 1u) % 16u;
    if (media->controller_trace_count < 16u) media->controller_trace_count++;
}

void psp_media_session_checkpoint(
    PspMediaSession *media, const char *checkpoint)
{
    if (media == NULL) return;
    PspMediaPipelineState pipeline =
        psp_media_owned_pipeline(media);
    PspMediaBackendHealth backend = media_psp_backend_quarantined()
        ? PSP_MEDIA_BACKEND_QUARANTINED : PSP_MEDIA_BACKEND_HEALTHY;
    uint32_t violations =
        psp_media_machine_violations(&media->machine);
    uint32_t mismatch = 0;
    if (media->machine.pipeline != pipeline) mismatch |= 1u << 0;
    if (media->machine.backend_health != backend) mismatch |= 1u << 1;
    if (violations != 0) mismatch |= 1u << 2;
    if (mismatch != 0) {
        if (media->controller_mismatches != UINT32_MAX)
            media->controller_mismatches++;
        if (violations != 0 && media->controller_violations != UINT32_MAX)
            media->controller_violations++;
        if (mismatch != media->controller_last_mismatch) {
            psp_media_controller_trace(
                media, PSP_MEDIA_EVENT_NONE, media->machine.state,
                media->machine.state, violations, checkpoint);
        }
    }
    media->controller_last_mismatch = mismatch;
}

static void psp_media_dispatch_one(
    PspMediaSession *media, PspMediaEvent event, const char *checkpoint)
{
    if (media == NULL) return;
    if (event.service_epoch != 0
        && (event.service_epoch != media->service.epoch
            || event.service_command != media->service.command)) {
        if (media->stale_service_completions != UINT32_MAX)
            media->stale_service_completions++;
        return;
    }
    PspMediaSessionState from = media->machine.state;
    PspMediaDecision decision = psp_media_machine_transition(
        &media->machine, &event);
    if (media->controller_events != UINT32_MAX) media->controller_events++;
    media->machine = decision.next;
    if (event.service_epoch != 0)
        psp_media_service_token_clear(&media->service);
    if (decision.command != PSP_MEDIA_COMMAND_NONE) {
        uint64_t now_us = psp_media_now_us(media);
        (void) psp_media_service_token_begin(
            &media->service, decision.command, now_us, 0);
    }
    psp_media_apply_active_projection(media);
    uint32_t violations =
        psp_media_machine_violations(&media->machine);
    psp_media_controller_trace(
        media, event.type, from, media->machine.state,
        violations, checkpoint);
}

static void psp_media_controller_report(PspMediaSession *media)
{
    if (media == NULL || media->controller_events == 0) return;
    char trace[640] = "none";
    if (media->controller_trace_count != 0) {
        size_t used = 0;
        trace[0] = '\0';
        unsigned start = (media->controller_trace_head + 16u
                          - media->controller_trace_count) % 16u;
        for (unsigned i = 0; i < media->controller_trace_count; i++) {
            unsigned at = (start + i) % 16u;
            const char *checkpoint = media->controller_trace_checkpoint[at];
            int written = snprintf(
                trace + used, sizeof(trace) - used,
                "%s%u:%u>%u:v%lx@%.8s",
                i == 0 ? "" : ",",
                (unsigned) media->controller_trace_event[at],
                (unsigned) media->controller_trace_from[at],
                (unsigned) media->controller_trace_to[at],
                (unsigned long) media->controller_trace_violations[at],
                checkpoint == NULL ? "unknown" : checkpoint);
            if (written < 0 || (size_t) written >= sizeof(trace) - used) {
                trace[sizeof(trace) - 1u] = '\0';
                break;
            }
            used += (size_t) written;
        }
    }
    printf(
        "tilefinch-media-state: events=%lu mismatches=%lu violations=%lu "
        "controller=%s pipeline=%d/%d backend=%d/%d "
        "trace-retained=%u trace=%s\n",
        (unsigned long) media->controller_events,
        (unsigned long) media->controller_mismatches,
        (unsigned long) media->controller_violations,
        psp_media_session_state_name(media->machine.state),
        (int) media->machine.pipeline,
        (int) psp_media_owned_pipeline(media),
        (int) media->machine.backend_health,
        media_psp_backend_quarantined()
            ? (int) PSP_MEDIA_BACKEND_QUARANTINED
            : (int) PSP_MEDIA_BACKEND_HEALTHY,
        media->controller_trace_count, trace);
}

void psp_media_finish_synchronous_quiesce(
    PspMediaSession *media, const char *checkpoint)
{
    if (media == NULL
        || media->machine.state != PSP_MEDIA_SESSION_QUIESCING)
        return;
    psp_media_dispatch(media, psp_media_service_completion(
        media, PSP_MEDIA_EVENT_ADMISSION_STOPPED), checkpoint);
    psp_media_dispatch(media, psp_media_service_completion(
        media, PSP_MEDIA_EVENT_TRANSPORT_CANCELLED), checkpoint);
    if (media_psp_backend_quarantined()) {
        psp_media_dispatch(media, psp_media_service_completion(
            media, PSP_MEDIA_EVENT_BACKEND_QUARANTINED), checkpoint);
    } else {
        psp_media_dispatch(media, psp_media_service_completion(
            media, PSP_MEDIA_EVENT_BACKEND_QUIESCED), checkpoint);
    }
    psp_media_controller_report(media);
}

static void psp_media_complete_priming_if_ready(
    PspMediaSession *media, const char *checkpoint)
{
    if (media == NULL
        || media->machine.state != PSP_MEDIA_SESSION_PRIMING
        || !media->have_frame
        || media->pause_boundary_pending)
        return;
    if (media->presentation_preroll_startup) {
        size_t displayed =
            media_playback_displayed_video_frames(media->playback);
        if (!media->presentation_preroll_startup_claimed
            || displayed <= media->presentation_preroll_displayed_baseline)
            return;
    }
    psp_media_dispatch(media, psp_media_service_completion(
        media, PSP_MEDIA_EVENT_PRIME_READY), checkpoint);
    if (media->machine.state != PSP_MEDIA_SESSION_PRIMING)
        psp_media_release_presentation_preroll(media, true);
}
#else
void psp_media_session_checkpoint(
    PspMediaSession *media, const char *checkpoint)
{
    (void) media;
    (void) checkpoint;
}
static void psp_media_dispatch_one(
    PspMediaSession *media, PspMediaEvent event, const char *checkpoint)
{
    (void) checkpoint;
    if (media == NULL) return;
    if (event.service_epoch != 0
        && (event.service_epoch != media->service.epoch
            || event.service_command != media->service.command)) {
        if (media->stale_service_completions != UINT32_MAX)
            media->stale_service_completions++;
        return;
    }
    PspMediaDecision decision = psp_media_machine_transition(
        &media->machine, &event);
    media->machine = decision.next;
    if (event.service_epoch != 0)
        psp_media_service_token_clear(&media->service);
    if (decision.command != PSP_MEDIA_COMMAND_NONE) {
        uint64_t now_us = psp_media_now_us(media);
        (void) psp_media_service_token_begin(
            &media->service, decision.command, now_us, 0);
    }
    psp_media_apply_active_projection(media);
}
void psp_media_finish_synchronous_quiesce(
    PspMediaSession *media, const char *checkpoint)
{
    if (media == NULL
        || media->machine.state != PSP_MEDIA_SESSION_QUIESCING)
        return;
    psp_media_dispatch(media, psp_media_service_completion(
        media, PSP_MEDIA_EVENT_ADMISSION_STOPPED), checkpoint);
    psp_media_dispatch(media, psp_media_service_completion(
        media, PSP_MEDIA_EVENT_TRANSPORT_CANCELLED), checkpoint);
    if (media_psp_backend_quarantined()) {
        psp_media_dispatch(media, psp_media_service_completion(
            media, PSP_MEDIA_EVENT_BACKEND_QUARANTINED), checkpoint);
    } else {
        psp_media_dispatch(media, psp_media_service_completion(
            media, PSP_MEDIA_EVENT_BACKEND_QUIESCED), checkpoint);
    }
}
static void psp_media_complete_priming_if_ready(
    PspMediaSession *media, const char *checkpoint)
{
    if (media == NULL
        || media->machine.state != PSP_MEDIA_SESSION_PRIMING
        || !media->have_frame
        || media->pause_boundary_pending)
        return;
    if (media->presentation_preroll_startup) {
        size_t displayed =
            media_playback_displayed_video_frames(media->playback);
        if (!media->presentation_preroll_startup_claimed
            || displayed <= media->presentation_preroll_displayed_baseline)
            return;
    }
    psp_media_dispatch(media, psp_media_service_completion(
        media, PSP_MEDIA_EVENT_PRIME_READY), checkpoint);
    if (media->machine.state != PSP_MEDIA_SESSION_PRIMING)
        psp_media_release_presentation_preroll(media, true);
}
#define psp_media_controller_report(...) ((void) 0)
#endif

/*
 * Lifecycle service completion may be synchronous (for example, a module
 * preparation refusal) even though the controller itself is single-flight.
 * Commit the state which requested the service first, then drain at most one
 * deferred completion at a time. This prevents dispatch-inside-dispatch and
 * gives a malformed completion chain a hard upper bound rather than a C-stack
 * recursion path on the PSP.
 */
static void psp_media_dispatch(
    PspMediaSession *media, PspMediaEvent event, const char *checkpoint)
{
    if (media == NULL) return;
    if (media->dispatch_active) {
        if (media->deferred_event_valid) {
            if (media->deferred_dispatch_overflows != UINT32_MAX)
                media->deferred_dispatch_overflows++;
            return;
        }
        media->deferred_event = event;
        media->deferred_checkpoint = checkpoint;
        media->deferred_event_valid = true;
        return;
    }

    media->dispatch_active = true;
    for (unsigned depth = 0; depth < 4u; depth++) {
        psp_media_dispatch_one(media, event, checkpoint);
        if (!media->deferred_event_valid) {
            media->dispatch_active = false;
            return;
        }
        event = media->deferred_event;
        checkpoint = media->deferred_checkpoint;
        media->deferred_event_valid = false;
        media->deferred_checkpoint = NULL;
    }

    if (media->deferred_event_valid) {
        media->deferred_event_valid = false;
        media->deferred_checkpoint = NULL;
        if (media->deferred_dispatch_overflows != UINT32_MAX)
            media->deferred_dispatch_overflows++;
    }
    media->dispatch_active = false;
}

void psp_media_session_dispatch_event(
    PspMediaSession *media, PspMediaEvent event, const char *checkpoint)
{
    psp_media_dispatch(media, event, checkpoint);
}

void psp_media_release_presentation_preroll(
    PspMediaSession *media, bool clear_floor)
{
    if (media == NULL) return;
    psp_media_apply_audio_hold(media);
    media->presentation_preroll_startup = false;
    media->presentation_preroll_startup_claimed = false;
    media->presentation_preroll_displayed_baseline = 0;
    if (clear_floor) media->presentation_floor_us = 0;
}

static void psp_media_apply_audio_hold(PspMediaSession *media)
{
    if (media == NULL) return;
    bool blocked = media->controller_audio_hold;
    if (blocked == media->presentation_preroll_audio_held) return;
    if (media->playback != NULL
        && !media_playback_set_audio_submission_blocked(
            media->playback, blocked))
        return;
    media->presentation_preroll_audio_held = blocked;
}

bool psp_media_begin_startup_preroll(PspMediaSession *media)
{
    if (media == NULL || media->playback == NULL
        || media->clock_us != 0)
        return false;
    /* Priming is committed before its service runs, so the projection may
       already have applied this hold. That does not mean the startup
       presentation boundary has been armed yet. */
    media->controller_audio_hold = true;
    psp_media_apply_audio_hold(media);
    if (!media->presentation_preroll_audio_held) return false;
    if (media->presentation_preroll_startup) return true;
    media->presentation_preroll_startup = true;
    media->presentation_preroll_startup_claimed = false;
    media->presentation_preroll_displayed_baseline =
        media_playback_displayed_video_frames(media->playback);
    printf("tilefinch-media-clock: event=startup-prime-begin\n");
    return true;
}

uint64_t psp_media_session_decode_clock_us(
    const PspMediaSession *media, bool awaiting_first_frame)
{
    if (media == NULL) return 0;
    uint64_t base_us = media->clock_us;
    /* Preroll freezes the audible/presentation clock at the requested time,
       but video still has to walk forward from its random-access point. Use
       accepted-source progress to move only the private decode horizon. This
       is the same monotonic horizon used by the exact seek job, without
       allowing wall time or muted audio to leak into presentation. */
    if (media->presentation_preroll_audio_held
        && media->playback != NULL) {
        uint64_t buffered_us =
            media_playback_buffered_until_us(media->playback);
        if (buffered_us > base_us) base_us = buffered_us;
    }
    return psp_media_decode_clock_us(base_us, awaiting_first_frame);
}

bool psp_media_cancel_requested(const PspMediaSession *media)
{
    if (media == NULL) return false;
    /* Two independent sources, both observed. The platform callback is
       the process-global scope the frontend owns; open_cancellation is the
       token the caller handed to this specific open transaction. Requiring
       only one of them means open-side cancellation no longer rests on the
       scope-ownership invariant alone. */
    return tilefinch_cancellation_requested(media->open_cancellation)
        || (media->platform.cancel_requested != NULL
            && media->platform.cancel_requested(media->platform.context));
}

bool psp_media_cancel_callback(void *opaque)
{
    return psp_media_cancel_requested(opaque);
}

/*
 * Bytes the range sources have accepted, including the window still on the
 * wire. Installed windows alone are too coarse to be progress: a 256 KiB
 * refill takes hundreds of milliseconds and shows nothing until it lands, and
 * the watchdogs that decide "stalled" have to be able to see a slow link
 * moving. Counting in-flight bytes is what makes a would-block pump
 * distinguishable from a dead one.
 */
/*
 * Whether either source is in the middle of fetching a window.
 *
 * Byte counts alone cannot answer this. A window whose bytes have all arrived
 * but which has not been installed yet holds its total still, and so does a
 * source with no request outstanding at all -- and a device run held both at
 * once (inflight=0/262144, window-pending=1/1) while the video side waited on
 * a window the link had not started delivering. Nothing moved for two seconds
 * and the pipeline was declared dead, with 134 source-blocked reads saying
 * exactly why it was not.
 */
static bool psp_media_source_refilling(const PspMediaSession *media)
{
    MediaHttpRangeStats video = {0};
    MediaHttpRangeStats audio = {0};
    if (media == NULL) return false;
    (void) media_http_range_stats(media->range, &video);
    (void) media_http_range_stats(media->audio_range, &audio);
    return video.window_pending || audio.window_pending
        || video.bytes_in_flight != 0 || audio.bytes_in_flight != 0;
}

size_t psp_media_range_bytes(const PspMediaSession *media)
{
    MediaHttpRangeStats video = {0};
    MediaHttpRangeStats audio = {0};
    if (media == NULL) return 0;
    (void) media_http_range_stats(media->range, &video);
    (void) media_http_range_stats(media->audio_range, &audio);
    return video.bytes_received + video.bytes_in_flight
        + audio.bytes_received + audio.bytes_in_flight;
}

/*
 * Give both range sources their bounded, non-blocking step. This is the whole
 * reason a media unit no longer waits: the transport makes progress here and
 * inside the reads themselves, never by holding the interactive thread.
 */
void psp_media_pump_ranges(PspMediaSession *media)
{
    if (media == NULL) return;
    /* Both ranges ultimately feed the same one-hop transport worker. If both
       reach a refill boundary on one browser visit, a fixed video-first order
       makes audio lose every tie. Alternate the issue order; each range still
       gets exactly one bounded pump and the worker remains single-flight. */
    if (media->range_pump_audio_first) {
        (void) media_http_range_pump(media->audio_range);
        (void) media_http_range_pump(media->range);
    } else {
        (void) media_http_range_pump(media->range);
        (void) media_http_range_pump(media->audio_range);
    }
    if (media->range != NULL && media->audio_range != NULL)
        media->range_pump_audio_first = !media->range_pump_audio_first;
}

size_t psp_media_free_memory(const PspMediaSession *media)
{
    return media != NULL && media->platform.free_memory != NULL
        ? media->platform.free_memory(media->platform.context) : 0;
}

size_t psp_media_maximum_free_block(const PspMediaSession *media)
{
    return media != NULL && media->platform.maximum_free_block != NULL
        ? media->platform.maximum_free_block(media->platform.context) : 0;
}

static void psp_media_persist_profile_change(PspMediaSession *media)
{
    if (media == NULL) return;
    if (media->platform.profile_changed != NULL) {
        media->platform.profile_changed(
            media->platform.profile_context, psp_media_now_us(media));
    } else if (media->profile_path != NULL) {
        /* Portable callers without the PSP profile-store seam retain the
           original immediate persistence contract. */
        (void) browser_profile_save(media->profile, media->profile_path);
    }
}

static bool psp_media_finish_resume_update(
    PspMediaSession *media, bool changed, bool persist)
{
    if (media == NULL) return false;
    bool dirty = changed || media->resume_profile_dirty;
    if (!persist) {
        /* Updating the bounded in-memory resume table must stay off the Memory
           Stick hot path, but a later close/suspend must still flush it even
           when the clock has not advanced since this update. */
        media->resume_profile_dirty = dirty;
        return dirty;
    }
    if (dirty) psp_media_persist_profile_change(media);
    media->resume_profile_dirty = false;
    return dirty;
}

bool psp_media_backend_stats_snapshot(
    PspMediaSession *media, MediaBackendStats *stats)
{
    if (stats == NULL) return false;
    *stats = (MediaBackendStats) {0};
    if (media == NULL || media->playback == NULL) return false;
    if (media_playback_backend_stats(media->playback, stats)) {
        media->backend_stats_snapshot = *stats;
        media->backend_stats_snapshot_us = psp_media_now_us(media);
        media->backend_stats_snapshot_valid = true;
        return true;
    }
    if (!media->backend_stats_snapshot_valid) return false;
    *stats = media->backend_stats_snapshot;
    return true;
}

void psp_media_pipeline_destroy(PspMediaSession *media)
{
    if (media == NULL) return;
    psp_media_buffering_end(media, psp_media_now_us(media));
    youtube_resolve_job_destroy(media->resolver_job);
    media->resolver_job = NULL;
    media->controller_audio_hold = false;
    psp_media_release_presentation_preroll(media, true);
    if (media->have_frame)
        media_psp_backend_note_frame_quiesced(&media->frame);
    psp_media_telemetry_report_feed(media, "teardown");
    if (media->playback != NULL) {
        MediaPlaybackJobStats stats = {0};
        MediaBackendStats backend_stats = {0};
        media_playback_job_stats(media->playback, &stats);
        bool backend_stats_ready = psp_media_backend_stats_snapshot(
            media, &backend_stats);
        if (backend_stats_ready) {
            media->accumulated_decoded_video_frames +=
                backend_stats.decoded_video_frames;
            media->accumulated_dropped_video_frames +=
                backend_stats.dropped_video_frames;
            media->accumulated_discarded_seek_video_frames +=
                backend_stats.discarded_seek_video_frames;
            media->accumulated_video_claims +=
                backend_stats.video_claims;
            media->accumulated_video_claims_displayed +=
                backend_stats.video_claims_displayed;
            media->accumulated_video_claims_dropped +=
                backend_stats.video_claims_dropped;
            media->accumulated_video_claims_quiesced +=
                backend_stats.video_claims_quiesced;
            media->accumulated_audio_packets +=
                backend_stats.submitted_audio_packets;
            if (backend_stats.last_native_error != 0)
                media->accumulated_native_errors++;
            if (backend_stats.external_bytes > media->peak_external_bytes)
                media->peak_external_bytes = backend_stats.external_bytes;
        }
        /*
         * The presenter's own split, which no ordinary session could report
         * before: scale-* is the whole present, and the two ge-* halves say
         * how much of it was building the display list against waiting for
         * the graphics engine. A device cycle measured 46ms a frame here with
         * no way to tell which half it was, and the answer decides whether
         * the fix is a cheaper draw or a busier wait.
         */
        printf("tilefinch-media-job: calls=%zu yielded=%zu packets=%zu "
               "would-block=%zu stats-ready=%d decoded=%zu "
               "dropped=%zu refused=%zu native=%d "
               "external=%zu scale-frames=%zu scale-total=%lluus "
               "scale-max=%lluus ge-frames=%zu ge-submit=%lluus "
               "ge-sync=%lluus ge-sync-max=%lluus ge-wait=%lluus "
               "ge-wait-max=%lluus present-skipped=%zu "
               "stage-frames=%zu stage-total=%lluus stage-max=%lluus "
               "draw-pump-frames=%zu draw-pump-units=%zu "
               "draw-pump-submitted=%zu draw-pump-total=%lluus\n",
               stats.calls, stats.yielded_calls, stats.packets_submitted,
               stats.would_block_calls,
               backend_stats_ready ? 1 : 0,
               /*
                * The accumulated figures when the backend can no longer be
                * asked. Printing the empty struct's zeros made two device
                * cycles read `decoded=0` for sessions that had decoded 57
                * pictures each -- the counter said the pipeline produced
                * nothing while the presenter beside it reported 63 frames,
                * which is the sort of contradiction that costs a cycle to
                * unpick. Accumulated is what the session knows on its own.
                */
               backend_stats_ready
                   ? backend_stats.decoded_video_frames
                   : media->accumulated_decoded_video_frames,
               backend_stats_ready
                   ? backend_stats.dropped_video_frames
                   : media->accumulated_dropped_video_frames,
               backend_stats.refused_video_packets,
               backend_stats.last_native_error,
               backend_stats.external_bytes,
               media->present_scale_frames,
               (unsigned long long) media->present_scale_total_us,
               (unsigned long long) media->present_scale_max_us,
               media->present_ge_frames,
               (unsigned long long) media->present_ge_submit_total_us,
               (unsigned long long) media->present_ge_sync_total_us,
               (unsigned long long) media->present_ge_sync_max_us,
               (unsigned long long) media->present_ge_wait_total_us,
               (unsigned long long) media->present_ge_wait_max_us,
               media->present_skipped_frames,
               media->present_stage_frames,
               (unsigned long long) media->present_stage_total_us,
               (unsigned long long) media->present_stage_max_us,
               media->pump_draw_frames, media->pump_draw_units,
               media->pump_draw_submitted,
               (unsigned long long) media->pump_draw_us);
    }
    /* The texture staging is the display's EDRAM, not ours; forget only which
       picture was in it, because the next session's first frame must stage
       rather than trust what a previous one left. */
    media->present_stage_identity = 0;
    media_playback_destroy(media->playback);
    media_mp4_close(media->demux);
    media_mp4_close(media->audio_demux);
    media_http_range_destroy(media->range);
    media_http_range_destroy(media->audio_range);
    media_file_range_close(media->file_range);
    media_file_range_close(media->audio_file_range);
    media->playback = NULL;
    media->demux = NULL;
    media->audio_demux = NULL;
    media->range = NULL;
    media->audio_range = NULL;
    media->file_range = NULL;
    media->audio_file_range = NULL;
    media->backend_stats_snapshot = (MediaBackendStats) {0};
    media->backend_stats_snapshot_us = 0;
    media->backend_stats_snapshot_valid = false;
    media->have_frame = false;
    media->presentation_floor_us = 0;
    media->no_frame_ms = 0;
    media->decode_job_pending = false;
    media->recovery_service_active = false;
    media->pause_boundary_pending = false;
    media->buffering_service_active = false;
    media->buffering_started_during_startup = false;
    media->network_buffer_slow_logged = false;
    media->startup_buffer_applied = false;
    media->network_starved_since_us = 0;
    media->network_buffer_ready_since_us = 0;
    media->network_buffer_started_us = 0;
    media->network_buffer_total_us = 0;
    media->network_buffer_source_blocks_seen = 0;
    media->network_buffer_events = 0;
    media->range_pump_audio_first = false;
    media->first_frame_started_us = 0;
    media->first_frame_opened_us = 0;
    media->first_frame_pump_us = 0;
    media->first_frame_bytes = 0;
    media->first_frame_codec_logged = false;
    media->decode_no_progress_ms = 0;
    media->decode_last_packets = 0;
    media->decode_last_range_bytes = 0;
    media->stall_last_packets = 0;
    media->stall_since_us = 0;
    media->stall_reported = false;
    media->job_phase = PSP_MEDIA_JOB_NONE;
    media->job_started_us = 0;
    media->job_phase_started_us = 0;
    media->job_units = 0;
    media->job_maximum_unit_us = 0;
    media->job_preview = false;
    media->job_resume_playing = false;
    media->seek_preview_started = false;
    media->seek_preview_was_playing = false;
    media->seek_preview_cancel_pending = false;
    psp_ui_media_cancel_seek_preview(&media->ui);
    memset(&media->frame, 0, sizeof(media->frame));
}

void psp_media_shutdown(PspMediaSession *media)
{
    if (media == NULL) return;
    psp_media_dispatch(media, (PspMediaEvent) {
        .type = PSP_MEDIA_EVENT_CLOSE,
        .retain_pipeline = false
    }, "shutdown-close");
    psp_media_pipeline_destroy(media);
    psp_media_finish_synchronous_quiesce(
        media, "shutdown-released");
    psp_media_session_checkpoint(media, "shutdown-complete");
}

void psp_media_init(
    PspMediaSession *media, Budget *budget, BrowserSession *session,
    BrowserProfile *profile, const char *profile_path,
    const FontFace *title_font, const PspMediaSessionPlatform *platform)
{
    memset(media, 0, sizeof(*media));
    /* Slot zero is a real slot, so "none" cannot be the zeroed value. */
    media->dma_quarantine_slot = -1;
    media->budget = budget;
    media->session = session;
    media->profile = profile;
    media->profile_path = profile_path;
    if (platform != NULL) media->platform = *platform;
    media->requested_quality =
        browser_profile_youtube_quality(profile);
    psp_ui_media_init(&media->ui);
    psp_ui_media_set_title_font(&media->ui, title_font);
    media->machine = psp_media_machine_initial();
#ifdef TILEFINCH_PSP_VALIDATION_LOG
#endif
    psp_media_session_checkpoint(media, "init");
}

/* Boot's answer to "what does a refused access unit cost", read only by
   psp_media_refusal_reset_recover on the browser thread. Written once, before
   a session exists; the codec worker never touches it. */
static bool psp_media_refusal_reset_enabled;

void psp_media_set_refusal_reset(bool enabled)
{
    psp_media_refusal_reset_enabled = enabled;
    media_psp_backend_set_refusal_recovery(enabled);
}

uint64_t psp_media_duration_us(const PspMediaSession *media)
{
    if (media == NULL) return 0;
    return media->stream.duration_ms > UINT64_MAX / UINT64_C(1000)
        ? UINT64_MAX
        : media->stream.duration_ms * UINT64_C(1000);
}

size_t psp_media_transport_rate_floor(
    uint64_t content_length, uint64_t duration_ms)
{
    if (content_length == 0 || duration_ms == 0)
        return MEDIA_HTTP_TRICKLE_FLOOR_BYTES_PER_SECOND;
    uint64_t average = content_length <= UINT64_MAX / UINT64_C(1000)
        ? content_length * UINT64_C(1000) / duration_ms
        : content_length / duration_ms * UINT64_C(1000);
    /* Ninety percent of the file-wide average is conservative enough for
       bursty PSP Wi-Fi, while any connection below it is guaranteed to spend
       the bounded lookahead faster than it replenishes it. */
    uint64_t floor = average - average / 10u;
    if (floor < MEDIA_HTTP_TRICKLE_FLOOR_BYTES_PER_SECOND)
        floor = MEDIA_HTTP_TRICKLE_FLOOR_BYTES_PER_SECOND;
    return floor > SIZE_MAX ? SIZE_MAX : (size_t) floor;
}

bool psp_media_record_resume(PspMediaSession *media, bool persist)
{
    if (media == NULL || media->profile == NULL
        || media->source[0] == '\0'
        || media->last_resume_saved_us == UINT64_MAX) return false;
    char video_id[YOUTUBE_VIDEO_ID_CAPACITY] = {0};
    uint64_t duration_us = psp_media_duration_us(media);
    if (!youtube_watch_url_video_id(media->source, video_id)) return false;
    if (duration_us <= UINT64_C(5000000)
        || media->clock_us >= duration_us - UINT64_C(5000000)) {
        bool changed = browser_profile_record_resume(
            media->profile, video_id, 0, duration_us);
        bool dirty = psp_media_finish_resume_update(
            media, changed, persist);
        media->last_resume_saved_us = UINT64_MAX;
        return dirty;
    }
    if (media->clock_us < UINT64_C(5000000)) {
        /* Rewinding a previously watched video to its beginning invalidates
           the old resume point just as reaching its end does. */
        bool changed = browser_profile_record_resume(
            media->profile, video_id, 0, duration_us);
        bool dirty = psp_media_finish_resume_update(
            media, changed, persist);
        media->last_resume_saved_us = 0;
        return dirty;
    }
    bool changed = browser_profile_record_resume(
        media->profile, video_id, media->clock_us, duration_us);
    bool dirty = psp_media_finish_resume_update(media, changed, persist);
    media->last_resume_saved_us = media->clock_us;
    return dirty;
}

void psp_media_suspend(PspMediaSession *media)
{
    if (media == NULL || media->system_suspended) return;
    psp_media_session_checkpoint(media, "suspend-begin");
    psp_media_dispatch(media, (PspMediaEvent) {
        .type = PSP_MEDIA_EVENT_SUSPEND
    }, "suspend-request");
    bool preview_transaction = media->seek_preview_started;
    bool was_playing = media->ui.playing || media->job_resume_playing
        || (preview_transaction && media->seek_preview_was_playing);
    uint64_t recovery_us = psp_media_recovery_position_us(media);
    media->system_resume_playing = was_playing;
    media->reopen_resume_us = recovery_us;
    media->reopen_resume_playing = was_playing;
    media->reopen_resume_pending = true;
    media->clock_us = recovery_us;
    psp_media_record_resume(media, true);
    media->open_service_pending = false;
    psp_media_pipeline_destroy(media);
    media_psp_backend_system_suspend();
    media->ui.playing = false;
    media->ui.analog_seek_direction = 0;
    media->seek_preview_started = false;
    media->seek_preview_was_playing = false;
    media->seek_preview_cancel_pending = false;
    media->reopen_preview_target_us = 0;
    media->reopen_preview_pending = false;
    media->preview_commit_target_us = 0;
    media->preview_commit_pending = false;
    media->preview_commit_resume_playing = false;
    media->system_suspended = true;
    psp_media_finish_synchronous_quiesce(media, "suspend-released");
    psp_media_session_checkpoint(media, "suspend-end");
}

void psp_media_resume(PspMediaSession *media)
{
    if (media == NULL || !media->system_suspended) return;
    psp_media_session_checkpoint(media, "resume-begin");
    media->system_suspended = false;
    /* Resume deliberately revalidates the source through the ordinary open
       path. URL reuse is reserved for the single controlled backward-seek
       transaction and must never survive an unrelated lifecycle edge. */
    media->reopen_reuse_resolved_stream = false;
    if (media->source[0] != '\0' && media->ui.visible) {
        media->reopen_resume_playing = media->system_resume_playing;
        media->open_service_pending = true;
        psp_ui_media_set_resolving(&media->ui, "Resuming video");
    } else {
        media->reopen_resume_us = 0;
        media->reopen_resume_playing = false;
        media->reopen_resume_pending = false;
    }
    media->system_resume_playing = false;
    psp_media_dispatch(media, (PspMediaEvent) {
        .type = PSP_MEDIA_EVENT_RESUME
    }, "resume-request");
    psp_media_session_checkpoint(media, "resume-end");
}

bool psp_media_system_suspended(const PspMediaSession *media)
{
    return media != NULL && media->system_suspended;
}

void psp_media_prepare_route(
    PspMediaSession *media, const char *url, uint64_t generation)
{
    if (media == NULL) return;
    if (media->system_suspended) return;
    bool offline_route = psp_media_offline_route(media, url);
    if (!youtube_watch_url_supported(url) && !offline_route) {
        char current_id[YOUTUBE_VIDEO_ID_CAPACITY] = {0};
        char next_id[YOUTUBE_VIDEO_ID_CAPACITY] = {0};
        bool same_internal_video =
            youtube_watch_url_video_id(media->source, current_id)
            && youtube_watch_url_video_id(url, next_id)
            && strcmp(current_id, next_id) == 0;
        if (same_internal_video && media->playback != NULL) {
            psp_media_dispatch(media, (PspMediaEvent) {
                .type = PSP_MEDIA_EVENT_CLOSE,
                .retain_pipeline = true
            }, "internal-view-hide");
            media->suspended_for_internal_view = true;
            media->ui.visible = false;
            media->ui.playing = false;
            media_playback_set_playing(media->playback, false);
            psp_media_session_checkpoint(media, "internal-view-hidden");
            return;
        }
        if (media->source[0] != '\0') {
            psp_media_record_resume(media, true);
            /* A fresh retry can be armed for the watch route before its
               provider navigation finishes. If that navigation lands on an
               internal/non-watch view, leaving the old pending bit alive
               creates an ownerless media-open transaction with an empty
               source: the player never advances, yet its cooperate scope
               keeps repainting until Circle cancels it. Route departure owns
               the pending transaction just as explicit Close does. */
            media->open_service_pending = false;
            psp_media_dispatch(media, (PspMediaEvent) {
                .type = PSP_MEDIA_EVENT_CLOSE,
                .retain_pipeline = false
            }, "leave-video-route");
            psp_media_pipeline_destroy(media);
            psp_media_finish_synchronous_quiesce(
                media, "leave-video-released");
            media->source[0] = '\0';
            media->suspended_for_internal_view = false;
            psp_ui_media_init(&media->ui);
            psp_media_session_checkpoint(media, "leave-video-complete");
        }
        return;
    }
    if (media->suspended_for_internal_view && media->playback != NULL) {
        char current_id[YOUTUBE_VIDEO_ID_CAPACITY] = {0};
        char next_id[YOUTUBE_VIDEO_ID_CAPACITY] = {0};
        if (youtube_watch_url_video_id(media->source, current_id)
            && youtube_watch_url_video_id(url, next_id)
            && strcmp(current_id, next_id) == 0) {
            psp_media_dispatch(media, (PspMediaEvent) {
                .type = PSP_MEDIA_EVENT_OPEN,
                .autoplay = media->machine.resume_target
                    == PSP_MEDIA_RESUME_PLAYING,
                .reuse_pipeline = true,
                .has_separate_audio = media->stream.split_streams
            }, "internal-view-return");
            media->suspended_for_internal_view = false;
            media->generation = generation;
            media->ui.visible = true;
            psp_ui_media_show_controls(&media->ui);
            psp_media_session_checkpoint(media, "internal-view-visible");
            return;
        }
    }
    if (strcmp(media->source, url) == 0) {
        if (media->generation != generation) {
            media->generation = generation;
            if (media->playback != NULL) {
                psp_media_dispatch(media, (PspMediaEvent) {
                    .type = PSP_MEDIA_EVENT_OPEN,
                    .autoplay = media->machine.resume_target
                        == PSP_MEDIA_RESUME_PLAYING,
                    .reuse_pipeline = true,
                    .has_separate_audio = media->stream.split_streams
                }, "same-video-return");
                media->ui.visible = true;
                psp_ui_media_show_controls(&media->ui);
                psp_media_session_checkpoint(media, "same-video-visible");
            } else {
                /* A deliberately closed/failed incomplete transaction keeps
                   its route so the current page does not reopen it by itself.
                   A fresh navigation generation is an explicit retry. */
                media->requested_quality =
                    psp_media_open_quality(media);
                media->quality_fallback_attempted = false;
                media->open_service_pending = true;
                psp_media_dispatch(media, (PspMediaEvent) {
                    .type = PSP_MEDIA_EVENT_OPEN,
                    /* A new navigation generation is an explicit activation
                       of the provider's play surface, not a passive document
                       restore.  Resume/reopen paths preserve their recorded
                       play state separately; a user-selected video starts. */
                    .autoplay = true,
                    .has_separate_audio = false
                }, "same-video-open");
                psp_ui_media_set_resolving(
                    &media->ui,
                    offline_route ? "Saved video" : "YouTube video");
            }
        }
        return;
    }
    psp_media_record_resume(media, true);
    psp_media_dispatch(media, (PspMediaEvent) {
        .type = PSP_MEDIA_EVENT_CLOSE,
        .retain_pipeline = false
    }, "replace-video-route");
    psp_media_pipeline_destroy(media);
    psp_media_finish_synchronous_quiesce(
        media, "replace-video-released");
    media->clock_us = 0;
    media->offline_source = false;
    media->offline_video_path[0] = '\0';
    media->offline_audio_path[0] = '\0';
    media->suspended_for_internal_view = false;
    snprintf(media->source, sizeof(media->source), "%s", url);
    media->generation = generation;
    media->last_resume_saved_us = 0;
    media->requested_quality = psp_media_open_quality(media);
    media->quality_fallback_attempted = false;
    media->reopen_resume_us = 0;
    media->reopen_resume_playing = false;
    media->reopen_resume_pending = false;
    media->reopen_preview_target_us = 0;
    media->reopen_preview_pending = false;
    media->preview_commit_target_us = 0;
    media->preview_commit_pending = false;
    media->preview_commit_resume_playing = false;
    media->reopen_reuse_resolved_stream = false;
    media->reopen_seek_completion_pending = false;
    /* A new route starts a new incident. The cap is not a process/session
       lifetime allowance; it bounds candidate replacements until this route
       proves stable playback. */
    media->transport_reresolve_attempts = 0;
    media->transport_refresh_rearm_us = 0;
    media->transport_next_expiry_check_us = 0;
    media->failure_report_level = 0;
    media->quality_fallbacks = 0;
    media->accumulated_decoded_video_frames = 0;
    media->accumulated_dropped_video_frames = 0;
    media->accumulated_discarded_seek_video_frames = 0;
    media->accumulated_video_claims = 0;
    media->accumulated_video_claims_displayed = 0;
    media->accumulated_video_claims_dropped = 0;
    media->accumulated_video_claims_quiesced = 0;
    media->accumulated_audio_packets = 0;
    media->accumulated_native_errors = 0;
    media->peak_external_bytes = 0;
    media->seek_completions = 0;
    /* Per-stream, because the backend's refusal total is per-stream too. The
       armed flag is boot configuration and outlives every open. */
    media->refusal_reset_seen_refusals = 0;
    media->refusal_resets = 0;
    media->present_scale_total_us = 0;
    media->present_scale_max_us = 0;
    media->present_scale_frames = 0;
    media->pump_frames = 0;
    media->pump_units = 0;
    media->pump_units_submitted = 0;
    media->pump_slice_exhausted = 0;
    media->pump_unit_cap = 0;
    media->pump_total_us = 0;
    media->pump_max_unit_us = 0;
    media->pump_draw_frames = 0;
    media->pump_draw_units = 0;
    media->pump_draw_submitted = 0;
    media->pump_draw_us = 0;
    media->prevblank_frames = 0;
    media->prevblank_submitted = 0;
    media->advance_previous_us = 0;
    media->slow_unit_reports = 0;
    media->advance_period_total_us = 0;
    media->advance_period_max_us = 0;
    media->advance_periods = 0;
    media->advance_playing_period_max_us = 0;
    media->advance_playing_period_total_us = 0;
    media->advance_playing_periods = 0;
    media->advance_playing_settled_total_us = 0;
    media->advance_playing_settled_periods = 0;
    media->advance_previous_playing = false;
    media->present_ge_wait_total_us = 0;
    media->present_ge_wait_max_us = 0;
    media->present_stage_frames = 0;
    media->present_stage_total_us = 0;
    media->present_stage_max_us = 0;
    media->clock_resync_logged = false;
    media->open_service_pending = true;
    psp_media_dispatch(media, (PspMediaEvent) {
        .type = PSP_MEDIA_EVENT_OPEN,
        /* Entering a provider video route is the user's play activation.
           Passing the just-cleared resume flag here opened every fresh video
           paused and let validation's synthetic Play press hide the defect. */
        .autoplay = true,
        .has_separate_audio = false
    }, "route-open");
    psp_ui_media_set_resolving(
        &media->ui, offline_route ? "Saved video" : "YouTube video");
}

void psp_media_close(PspMediaSession *media)
{
    if (media == NULL) return;
    psp_media_session_checkpoint(media, "close-begin");
    uint64_t recovery_us = psp_media_recovery_position_us(media);
    bool recovery_in_flight = media->job_phase == PSP_MEDIA_JOB_SEEK_PREPARE
        || media->job_phase == PSP_MEDIA_JOB_SEEK_DECODE
        || media->job_phase == PSP_MEDIA_JOB_PREVIEW_RESTORE_PREPARE
        || media->job_phase == PSP_MEDIA_JOB_PREVIEW_RESTORE_DECODE;
    bool discard_incomplete_pipeline =
        psp_media_open_work_pending(media)
        || media->job_phase != PSP_MEDIA_JOB_NONE
        || media->pause_boundary_pending
        || media->recovery_service_active
        || media->ui.failed
        || (media->playback != NULL && !media->have_frame);
    bool retain_pipeline = !discard_incomplete_pipeline
        && media->playback != NULL;
    psp_media_dispatch(media, (PspMediaEvent) {
        .type = PSP_MEDIA_EVENT_CLOSE,
        .retain_pipeline = retain_pipeline
    }, "close-request");
    media->open_service_pending = false;
    media->reopen_preview_target_us = 0;
    media->reopen_preview_pending = false;
    media->preview_commit_target_us = 0;
    media->preview_commit_pending = false;
    media->preview_commit_resume_playing = false;
    psp_media_cancel_decode(media);
    if (recovery_in_flight) media->clock_us = recovery_us;
    media->ui.visible = false;
    psp_media_record_resume(media, true);
    if (discard_incomplete_pipeline)
        psp_media_pipeline_destroy(media);
    if (discard_incomplete_pipeline)
        psp_media_finish_synchronous_quiesce(media, "close-released");
    psp_media_session_checkpoint(media, "close-end");
}

bool psp_media_reclaim_hidden_pipeline(PspMediaSession *media)
{
    if (media == NULL || media->ui.visible || media->playback == NULL)
        return false;
    psp_media_dispatch(media, (PspMediaEvent) {
        .type = PSP_MEDIA_EVENT_RECLAIM
    }, "dormant-reclaim");
    psp_media_pipeline_destroy(media);
    psp_media_finish_synchronous_quiesce(media, "dormant-released");
    psp_media_session_checkpoint(media, "dormant-reclaimed");
    printf("tilefinch-media: hidden pipeline reclaimed\n");
    return true;
}

bool psp_media_reclaim_hidden_pipeline_for_navigation(
    PspMediaSession *media, const char *target_url)
{
    if (media == NULL || media->ui.visible || media->playback == NULL)
        return false;
    if (target_url != NULL && strcmp(media->source, target_url) == 0)
        return false;
    char current_id[YOUTUBE_VIDEO_ID_CAPACITY] = {0};
    char target_id[YOUTUBE_VIDEO_ID_CAPACITY] = {0};
    if (youtube_watch_url_video_id(media->source, current_id)
        && youtube_watch_url_video_id(target_url, target_id)
        && strcmp(current_id, target_id) == 0) {
        return false;
    }
    return psp_media_reclaim_hidden_pipeline(media);
}

void psp_media_execute_intent(PspMediaSession *media,
                                     PspUiMediaIntent intent)
{
    if (media == NULL) return;
    switch (intent.action) {
        case PSP_UI_MEDIA_ACTION_PLAY_PAUSE:
            if (media->playback != NULL) {
                bool was_playing = psp_media_machine_wants_playing(media);
                if (media->ui.ended) {
                    psp_ui_media_set_resolving(
                        &media->ui, media->stream.title);
                    psp_ui_media_set_resolving_progress(
                        &media->ui, "RESTARTING VIDEO", 940u);
                    if (psp_media_request_seek(media, 0, false)) {
                        media->job_resume_playing = true;
                    } else {
                        psp_media_raise_error(
                            media, "VIDEO COULD NOT RESTART", NULL);
                    }
                } else {
                    PspMediaPresentationReadiness readiness =
                        !was_playing && media->clock_us == 0
                            ? PSP_MEDIA_PRESENTATION_NEEDS_PRIME
                            : psp_media_sample_readiness(media);
                    psp_media_dispatch(media, (PspMediaEvent) {
                        .type = was_playing
                            ? PSP_MEDIA_EVENT_PAUSE
                            : PSP_MEDIA_EVENT_PLAY,
                        .readiness = readiness
                    }, "play-pause-intent");
                    if (!was_playing) media->pause_boundary_pending = false;
                }
                if (media->job_phase == PSP_MEDIA_JOB_NONE) {
                    bool wants_playing =
                        psp_media_machine_wants_playing(media);
                    if (wants_playing && !was_playing)
                        (void) psp_media_begin_startup_preroll(media);
                    media_playback_set_playing(
                        media->playback, wants_playing);
                    if (!wants_playing) {
                        psp_media_buffering_end(
                            media, psp_media_now_us(media));
                    } else if (!was_playing
                               && !media->startup_buffer_applied) {
                        media->startup_buffer_applied = true;
                        MediaPlaybackJobStats stats = {0};
                        media_playback_job_stats(media->playback, &stats);
                        media->network_buffer_source_blocks_seen =
                            stats.source_block_calls;
                        if (browser_profile_video_startup_buffering(
                                media->profile)) {
                            psp_media_buffering_begin(
                                media, true, psp_media_now_us(media));
                        }
                    }
                }
            }
            break;
        case PSP_UI_MEDIA_ACTION_PREVIEW_SEEK:
            if (psp_media_seek_phase(media->job_phase)) {
                /* The UI target is the bounded one-entry prepared request.
                   Keep the current decoder/reset transaction intact; when it
                   restores the source, advance starts only the latest target. */
                media->ui.seek_preview_time_us = intent.seek_time_us;
            } else {
                (void) psp_media_request_seek(
                    media, intent.seek_time_us, true);
            }
            break;
        case PSP_UI_MEDIA_ACTION_CANCEL_SEEK_PREVIEW:
            if (media->seek_preview_started
                && media->job_phase != PSP_MEDIA_JOB_NONE) {
                media->job_preview = true;
                media->seek_preview_cancel_pending = true;
                media->job_phase =
                    PSP_MEDIA_JOB_PREVIEW_RESTORE_PREPARE;
            } else if (media->seek_preview_started
                       && media->playback != NULL) {
                media->ui.playing = media->seek_preview_was_playing;
                media_playback_set_playing(
                    media->playback, media->ui.playing);
                media->seek_preview_started = false;
                media->seek_preview_cancel_pending = false;
                psp_ui_media_cancel_seek_preview(&media->ui);
                psp_media_dispatch(media, (PspMediaEvent) {
                    .type = PSP_MEDIA_EVENT_PREVIEW_ENDED
                }, "preview-cancelled-idle");
            }
            break;
        case PSP_UI_MEDIA_ACTION_SEEK:
        {
            bool resume = (media->seek_preview_started
                           && media->seek_preview_was_playing)
                || media->reopen_resume_playing
                || psp_media_machine_wants_playing(media);
            if (psp_media_seek_phase(media->job_phase)
                || psp_media_open_phase(media->job_phase)
                || media->playback == NULL) {
                /* Cross is a semantic commit, not permission to overwrite a
                   firmware job. Retain only its latest target and apply it
                   after the current preview/restore or retry-open reaches a
                   safe browser-thread boundary. */
                media->preview_commit_target_us = intent.seek_time_us;
                media->preview_commit_pending = true;
                media->preview_commit_resume_playing = resume;
                media->reopen_preview_pending = false;
                psp_ui_media_set_seek_preview(
                    &media->ui, intent.seek_time_us);
                break;
            }
            media->job_resume_playing = resume;
            media->seek_preview_started = false;
            media->seek_preview_cancel_pending = false;
            (void) psp_media_request_seek(
                media, intent.seek_time_us, false);
            media->job_resume_playing = resume;
            break;
        }
        case PSP_UI_MEDIA_ACTION_RETRY:
            if (media->source[0] != '\0') {
                if (!media->reopen_resume_pending
                    && media->playback != NULL) {
                    psp_media_remember_retry_state(media, false);
                }
                /* An explicit Retry is a new incident initiated by the user,
                   so it receives a fresh bounded candidate allowance. */
                media->transport_reresolve_attempts = 0;
                media->transport_refresh_rearm_us = 0;
                media->transport_next_expiry_check_us = 0;
                media->failure_report_level = 0;
                media->open_service_pending = true;
                psp_ui_media_set_resolving(&media->ui, "YouTube video");
                psp_media_dispatch(media, (PspMediaEvent) {
                    .type = PSP_MEDIA_EVENT_RETRY
                }, "retry-intent");
            }
            break;
        case PSP_UI_MEDIA_ACTION_CLOSE:
            psp_media_close(media);
            break;
        case PSP_UI_MEDIA_ACTION_NONE:
        default:
            break;
    }
    psp_media_session_checkpoint(media, "intent");
}

/*
 * Re-hash the staged copy at the points the conversion hashed, and hand the
 * verdict to the backend.
 *
 * Sampled from the staged bytes rather than from the source, because the
 * staged bytes are what the graphics engine draws: a copy that read the wrong
 * slot differs here and nowhere else. Only when the staged geometry actually
 * contains the sample points -- a strided or truncated stage has nothing to
 * compare and says so with a zero.
 */
static bool psp_media_refusal_reset_recover(
    PspMediaSession *media, bool *decision_ready)
{
    if (decision_ready != NULL) *decision_ready = false;
    if (media == NULL || media->playback == NULL) return false;
    /* With recovery disabled, "continue" is the browser thread's complete
       decision. The worker latch may be released without reading counters. */
    if (!psp_media_refusal_reset_enabled) {
        if (decision_ready != NULL) *decision_ready = true;
        return false;
    }
    MediaBackendStats stats = {0};
    if (!media_playback_backend_stats(media->playback, &stats)) return false;
    size_t seen = media->refusal_reset_seen_refusals;
    size_t fresh = stats.refused_video_packets > seen
        ? stats.refused_video_packets - seen : 0;
    if (fresh == 0) {
        if (decision_ready != NULL) *decision_ready = true;
        return false;
    }
    /* A paused player is not decoding, and a first-frame transaction owes its
       caller a picture at the position it opened on. Neither is a place to move
       the source from. Keep both the refusal count and the worker hold pending;
       the first ordinary playing frame will perform the reset. */
    if (!media->ui.playing || media->pause_boundary_pending)
        return false;
    media->refusal_reset_seen_refusals = stats.refused_video_packets;
    if (decision_ready != NULL) *decision_ready = true;
    psp_media_dispatch(media, (PspMediaEvent) {
        .type = PSP_MEDIA_EVENT_DECODER_REFUSED
    }, "decoder-refused");
    uint64_t started_us = psp_media_now_us(media);
    uint64_t position_us = media->clock_us;
    uint64_t duration_us = psp_media_duration_us(media);
    if (!psp_media_refusal_resume_has_room(position_us, duration_us)) {
        /* There is no safe dependent-frame replay after a refused AU. If no
           later random-access point fits, finish this stream rather than
           submit into firmware state already proven unsafe. */
        printf("tilefinch-media-recovery: event=refusal-reset refusals=%zu "
               "fresh=%zu resets=%u from=%lluus target=0us landing=0us "
               "attempts=0 outcome=no-room elapsed=0us\n",
               stats.refused_video_packets, fresh, media->refusal_resets,
               (unsigned long long) position_us);
        media_playback_set_playing(media->playback, false);
        media->ui.playing = false;
        psp_media_raise_error(media, "VIDEO ENDED AFTER DECODER RECOVERY", NULL);
        psp_media_dispatch(media, psp_media_service_completion(
            media, PSP_MEDIA_EVENT_RECOVERY_FAILED),
            "recovery-no-room");
        psp_media_session_checkpoint(media, "recovery-failed");
        return true;
    }
    /* The borrowed surface belongs to the decoder that is about to be reset. */
    media->have_frame = false;
    memset(&media->frame, 0, sizeof(media->frame));
    char error[256] = {0};
    uint64_t landing_us = 0;
    unsigned attempts = 1;
    /* Ask the demuxer for the actual next random-access sample. Guessing a
       target and letting an ordinary seek round backward performed multiple
       decoder resets and range refills on long GOPs. This performs one reset,
       scans only the necessary retained/sidx metadata, and cannot land back in
       the dependency chain containing the refused access unit. */
    bool seek_failed = !media_playback_seek_after(
        media->playback, position_us, &landing_us, error, sizeof(error));
    bool moved_forward = !seek_failed && landing_us > position_us;
    media->refusal_resets++;
    printf("tilefinch-media-recovery: event=refusal-reset refusals=%zu "
           "fresh=%zu resets=%u from=%lluus target=%lluus landing=%lluus "
           "attempts=%u outcome=%s elapsed=%lluus\n",
           stats.refused_video_packets, fresh, media->refusal_resets,
           (unsigned long long) position_us,
           (unsigned long long) position_us,
           (unsigned long long) landing_us, attempts,
           seek_failed ? "seek-failed"
               : (moved_forward ? "resumed" : "no-forward-keyframe"),
           (unsigned long long) (psp_media_now_us(media) - started_us));
    if (seek_failed || !moved_forward) {
        psp_media_remember_retry_state(media, true);
        media_playback_set_playing(media->playback, false);
        media->ui.playing = false;
        media->decode_job_pending = false;
        psp_media_retire_first_frame(media);
        psp_media_raise_error(
            media, "VIDEO DECODER RESET FAILED",
            error[0] == '\0' ? NULL : error);
        psp_media_dispatch(media, psp_media_service_completion(
            media, PSP_MEDIA_EVENT_RECOVERY_FAILED),
            "recovery-reset-failed");
        psp_media_session_checkpoint(media, "recovery-failed");
        return true;
    }
    /* A refusal recovery advances only to a real random-access point. This is
       the conservative fallback; the corrected Detail2 contract should keep
       it dormant rather than making time jumps part of normal playback. */
    media->clock_us = landing_us;
    media_playback_set_playing(media->playback, media->ui.playing);
    /* The hitch this costs is a recovery, not a stall. Re-arm the watchdogs
       that measure "nothing has arrived recently" so they time the pipeline
       after the reset rather than through it. */
    media->decode_job_pending = false;
    media->decode_no_progress_ms = 0;
    media->no_frame_ms = 0;
    media->stall_since_us = 0;
    media->stall_reported = false;
    psp_media_dispatch(media, psp_media_service_completion(
        media, PSP_MEDIA_EVENT_RECOVERY_COMPLETE),
        "recovery-complete");
    psp_media_session_checkpoint(media, "recovery-complete");
    return true;
}

/*
 * The presenter's dead time, spent on the one thing that is starving.
 *
 * Only while an ordinary playing session owns the pipeline: an open, a seek or
 * a preview restore is a transaction with its own unit accounting, and a
 * first-frame wait belongs to the watchdog that is timing it. Nothing here
 * advances the clock or takes a frame -- so the source picture the parallel
 * stage copy is reading is never overwritten -- and a frame taken now would
 * have nowhere to go while the destination is spoken for.
 *
 * The caller's `still_busy` is the whole warrant for feeding here: the graphics
 * engine's draw, or the DMA worker's stage copy. When it stops, the frame owes
 * this time back to the present -- the wait, or the join -- and the ordinary
 * per-frame pump is where further feeding belongs.
 */
static bool psp_media_pump_dma_quarantine(PspMediaSession *media)
{
    /*
     * Keyed on the presenter, which is the authority: it posted the transfer
     * and its join is what gave up. The session's remembered slot says which
     * picture is owed a release and may legitimately be absent -- a quarantine
     * with no slot to hand back still has to be polled to its end, or staging
     * would be refused for the rest of the session with nothing left to clear
     * it.
     */
    if (media == NULL || !psp_media_present_ge_stage_dma_quarantined())
        return false;
    int slot = media->dma_quarantine_slot;
    if (psp_media_present_ge_stage_dma_quarantine_poll()) {
        printf("tilefinch-media-present: event=dma-quarantine-cleared "
               "slot=%d generation=%u\n",
               slot, (unsigned) media->dma_quarantine_generation);
        media->dma_quarantine_slot = -1;
        if (slot >= 0) {
            /* The refusal is withdrawn first, because the release below is
               one of the paths it refuses. */
            media_psp_backend_release_surface_quarantine((unsigned) slot);
            (void) media_playback_release_video_slot(
                media->playback, (unsigned) slot,
                media->dma_quarantine_generation);
            psp_media_present_emit_after_release(media);
        }
        return false;
    }
    if (!psp_media_present_ge_stage_dma_quarantine_expired()) return false;
    printf("tilefinch-media-present: event=dma-quarantine-expired slot=%d "
           "generation=%u\n", slot,
           (unsigned) media->dma_quarantine_generation);
    if (media->playback != NULL) {
        media_playback_set_playing(media->playback, false);
    }
    media->ui.playing = false;
    media->decode_job_pending = false;
    psp_media_retire_first_frame(media);
    psp_media_raise_error(
        media, "VIDEO STAGING TRANSFER DID NOT COMPLETE", NULL);
    return true;
}

/*
 * Cross commits a highlighted time even when a preview decoder transaction
 * or a quality retry is still in flight. The input side retains only that
 * latest semantic request; this is the single safe point that turns it into
 * firmware work. It deliberately runs before another tentative preview is
 * reissued, so a confirmed target can never be demoted back to a thumbnail.
 */
static bool psp_media_start_pending_preview_commit(
    PspMediaSession *media)
{
    if (media == NULL || !media->preview_commit_pending
        || media->playback == NULL
        || media->job_phase != PSP_MEDIA_JOB_NONE) return false;
    uint64_t target_us = media->preview_commit_target_us;
    bool resume_playing = media->preview_commit_resume_playing;
    /* A large backward seek reads this before rebuilding the pipeline. */
    media->job_resume_playing = resume_playing;
    if (!psp_media_request_seek(media, target_us, false)) return false;
    media->preview_commit_target_us = 0;
    media->preview_commit_pending = false;
    media->preview_commit_resume_playing = false;
    media->reopen_preview_target_us = 0;
    media->reopen_preview_pending = false;
    media->seek_preview_started = false;
    media->seek_preview_cancel_pending = false;
    media->job_resume_playing = resume_playing;
    psp_ui_media_cancel_seek_preview(&media->ui);
    return true;
}

bool psp_media_advance(
    PspMediaSession *media, unsigned elapsed_ms,
    const TilefinchCancellation *cancellation)
{
    if (media == NULL) return false;
    psp_media_session_checkpoint(media, "advance-begin");
    /* Before anything else this frame: a decoded slot may be held by a
       transfer nobody is tracking, and every path below assumes the ownership
       machine describes reality. */
    if (psp_media_pump_dma_quarantine(media)) return true;
    /*
     * No claim on the decoded surface outlives the present that made it. A
     * present that failed after the engine had the rows -- the list refused,
     * the sync refused -- leaves its claim standing on purpose: the software
     * scaler the caller falls back to reads the same surface. This is where
     * that ends, one frame later, so a lost release can cost a frame of
     * decoder latency and never the session.
     */
    psp_media_present_release_claimed_surface(media);
    psp_media_present_emit_after_release(media);
    bool changed = false;
    /* A closed player may retain a complete paused pipeline for an immediate
       replay, and an internal provider view may temporarily hide one. Neither
       state is permission to refresh expiring URLs, poll firmware, or resume
       a first-frame/seek continuation in the background. */
    if (!media->ui.visible) return changed;
    bool open_work = psp_media_open_work_pending(media);
    if (open_work) {
        (void) psp_media_open_pump(media, cancellation);
        /* Each open-phase event reapplies the machine projection. That
           correctly disables ordinary controls, but a quality/transport
           retry must keep the one highlighted scrub target visible and
           committable throughout the replacement open, not only after its
           final PLAYBACK_CREATE phase. */
        if (media->reopen_preview_pending
            || media->preview_commit_pending) {
            uint64_t target_us = media->preview_commit_pending
                ? media->preview_commit_target_us
                : media->reopen_preview_target_us;
            media->ui.duration_us = psp_media_duration_us(media);
            psp_ui_media_set_seek_preview(&media->ui, target_us);
        }
        return true;
    }
    if (psp_media_start_pending_preview_commit(media)) return true;
    if (media->reopen_preview_pending
        && media->playback != NULL
        && media->job_phase == PSP_MEDIA_JOB_NONE) {
        uint64_t target_us = media->reopen_preview_target_us;
        media->reopen_preview_target_us = 0;
        media->reopen_preview_pending = false;
        psp_ui_media_set_seek_preview(&media->ui, target_us);
        if (psp_media_request_seek(media, target_us, true)) return true;
    }
    if (media->job_phase == PSP_MEDIA_JOB_SEEK_PREPARE
        || media->job_phase == PSP_MEDIA_JOB_SEEK_DECODE
        || media->job_phase
             == PSP_MEDIA_JOB_PREVIEW_RESTORE_PREPARE
        || media->job_phase
             == PSP_MEDIA_JOB_PREVIEW_RESTORE_DECODE) {
        (void) psp_media_seek_decode_pump(media, cancellation);
        if (!psp_media_seek_phase(media->job_phase)
            && !media->ui.failed
            && psp_media_start_pending_preview_commit(media)) return true;
        if (!psp_media_seek_phase(media->job_phase)
            && !media->ui.failed
            && media->seek_preview_started
            && media->ui.seek_preview_active
            && media->ui.seek_preview_time_us != media->job_target_us) {
            uint64_t latest_target_us = media->ui.seek_preview_time_us;
            /* PREVIEW_STARTED remains active and request_seek therefore keeps
               the original playback/clock restore point. Repeated direction
               presses collapse to this single latest target. */
            (void) psp_media_request_seek(
                media, latest_target_us, true);
            return true;
        }
        if (!psp_media_seek_phase(media->job_phase)
            && !media->ui.failed) {
            psp_media_dispatch(
                media,
                psp_media_service_completion(
                    media, PSP_MEDIA_EVENT_SEEK_COMPLETE),
                "seek-complete");
            if (psp_media_sample_readiness(media)
                == PSP_MEDIA_PRESENTATION_READY) {
                psp_media_dispatch(media, psp_media_service_completion(
                    media, PSP_MEDIA_EVENT_PRIME_READY),
                    "seek-prime-ready");
            }
        }
#ifdef TILEFINCH_PSP_VALIDATION_LOG
        psp_media_session_checkpoint(media, "seek-pump");
#endif
        return true;
    }
    if (media->playback == NULL) return changed;
    /* Sampled here because this is the one point the interactive loop reaches
       on every frame of a playing session, whatever else it did. */
    uint64_t ranges_started_us = psp_media_now_us(media);
    uint64_t advance_now_us = psp_media_now_us(media);
    if (media->advance_previous_us != 0
        && advance_now_us > media->advance_previous_us) {
        uint64_t period_us = advance_now_us - media->advance_previous_us;
        media->advance_period_total_us += period_us;
        media->advance_periods++;
        if (period_us > media->advance_period_max_us)
            media->advance_period_max_us = period_us;
        /* Classified by the frame's own state: a period whose frame was
           actively presenting video is the only one a stall shows up in as a
           visible hitch. The previous frame set advance_previous_playing. */
        if (media->advance_previous_playing) {
            media->advance_playing_periods++;
            media->advance_playing_period_total_us += period_us;
            if (period_us > media->advance_playing_period_max_us)
                media->advance_playing_period_max_us = period_us;
            /* Steady state: past the warmup periods and under the one-time
               stall ceiling. What survives both is the sustained cadence the
               mandate is read against, kept apart from the open, the first
               decode and the one handshake that would otherwise inflate it. */
            if (media->advance_playing_periods
                    > PSP_MEDIA_PLAYING_STEADY_WARMUP_PERIODS
                && period_us <= PSP_MEDIA_PLAYING_STEADY_CEILING_US) {
                media->advance_playing_settled_periods++;
                media->advance_playing_settled_total_us += period_us;
            }
        }
    }
    media->advance_previous_us = advance_now_us;
    media->advance_previous_playing =
        media->machine.state == PSP_MEDIA_SESSION_PLAYING
        && media->have_frame;
    psp_media_pump_ranges(media);
    /* The frame's own transport step, outside any pump unit -- if the stall
       lives here rather than inside a unit, this is what names it. */
    psp_media_telemetry_report_slow_unit(
        media, "pump-ranges", psp_media_now_us(media) - ranges_started_us);
    if (psp_media_retry_transport_expiry(media)) {
        return true;
    }
    if (media->machine.state == PSP_MEDIA_SESSION_PLAYING
        && !media->machine.preview_active
        && !media->presentation_preroll_audio_held) {
        uint64_t elapsed_us = (uint64_t) elapsed_ms * 1000u;
        media->clock_us = elapsed_us > UINT64_MAX - media->clock_us
            ? UINT64_MAX : media->clock_us + elapsed_us;
        /* The DAC is the stable clock for an A/V stream. Its cursor is based
           on blocks actually accepted by the output worker, not on decode
           head or queued PCM. Following it prevents a startup video delay
           from permanently pulling the UI/video clock behind audible sound.
           Video-only streams retain the wall clock above. */
        uint64_t audio_cursor_us = 0;
        if (media_playback_audio_cursor_us(
                media->playback, &audio_cursor_us))
            media->clock_us = audio_cursor_us;
    }
    char error[256] = {0};
    bool awaiting_first_frame =
        media->pause_boundary_pending && !media->have_frame;
    uint64_t decode_clock_us = psp_media_session_decode_clock_us(
        media, awaiting_first_frame);
    bool catch_up = media->ui.playing && !awaiting_first_frame;
    uint64_t pump_started_us = awaiting_first_frame || catch_up
        ? psp_media_now_us(media) : 0;
    size_t pump_packets_before = 0;
    if (catch_up) {
        MediaPlaybackJobStats entering = {0};
        media_playback_job_stats(media->playback, &entering);
        pump_packets_before = entering.packets_submitted;
    }
    /* Telemetry only: which part of the frame a conversion completing on the
       codec worker landed in. See media_psp_backend_set_loop_phase. */
    media_psp_backend_set_loop_phase(PSP_MEDIA_LOOP_PHASE_PUMP);
    MediaPlaybackAdvanceResult advance =
        media_playback_advance_bounded_cancelable(
        media->playback, decode_clock_us,
        PSP_MEDIA_JOB_MAXIMUM_PACKETS, cancellation,
        error, sizeof(error));
    /*
     * Keep draining the ladder while the slice lasts. The budget is checked
     * before every unit and never inside one, so at most one costly operation
     * -- a range read, a bounded collect wait, a firmware call -- still runs
     * per unit: the interval in which a Circle press goes unanswered is what
     * it was.
     *
     * Stopping needs the reason, not just the absence of a packet. A unit that
     * found the decoder holding its staging made progress that this frame can
     * still finish: the collect wait inside it yielded to the codec worker, so
     * the unit after it is the one that submits. A unit that submitted nothing
     * and left nothing pending is the caught-up case, and that is where the
     * browser owes the rest of the frame to the page.
     */
    /* A video that owns the whole screen has no page behind it to owe the
       rest of the frame to, and the device measured this pump unit-capped on
       374 of 405 frames while spending 1.2ms of its 12ms slice. Give it the
       units; the slice is still the bound that matters. */
    unsigned unit_ceiling = media->ui.visible && media->have_frame
        ? PSP_MEDIA_PUMP_FULLSCREEN_MAXIMUM_UNITS
        : PSP_MEDIA_PUMP_MAXIMUM_UNITS;
    unsigned units = 1;
    uint64_t unit_started_us = pump_started_us;
    while (catch_up && units < unit_ceiling
           && advance == MEDIA_PLAYBACK_ADVANCE_PENDING) {
        uint64_t now_us = psp_media_now_us(media);
        if (now_us - unit_started_us > media->pump_max_unit_us)
            media->pump_max_unit_us = now_us - unit_started_us;
        psp_media_telemetry_report_slow_unit(
            media, "playing-pump", now_us - unit_started_us);
        if (now_us - pump_started_us >= PSP_MEDIA_PUMP_SLICE_US) {
            media->pump_slice_exhausted++;
            break;
        }
        unit_started_us = now_us;
        MediaPlaybackJobStats before = {0};
        MediaPlaybackJobStats after = {0};
        media_playback_job_stats(media->playback, &before);
        advance = media_playback_advance_bounded_cancelable(
            media->playback, decode_clock_us,
            PSP_MEDIA_JOB_MAXIMUM_PACKETS, cancellation,
            error, sizeof(error));
        media_playback_job_stats(media->playback, &after);
        units++;
        if (after.packets_submitted != before.packets_submitted) continue;
        /*
         * A unit that submitted nothing has nothing more to give this frame.
         *
         * A busy decoder used to be worth spending further units on, because
         * the collect wait inside the next one yielded to the codec worker and
         * the unit after it was the one that submitted. That wait has moved to
         * the pumps that run inside the present, where the thread is blocked
         * on hardware regardless -- so re-entering here now re-polls, at full
         * frame cost, a job that nothing has given the CPU to finish. A window
         * still on the wire was never going to land inside this frame either.
         * Both cases owe the rest of the frame to the page, and to the
         * presenter that is about to feed properly.
         */
        break;
    }
    media_psp_backend_set_loop_phase(PSP_MEDIA_LOOP_PHASE_OTHER);
    if (catch_up) {
        MediaPlaybackJobStats pumped = {0};
        media_playback_job_stats(media->playback, &pumped);
        uint64_t pump_ended_us = psp_media_now_us(media);
        if (pump_ended_us - unit_started_us > media->pump_max_unit_us)
            media->pump_max_unit_us = pump_ended_us - unit_started_us;
        media->pump_frames++;
        media->pump_units += units;
        media->pump_units_submitted +=
            pumped.packets_submitted - pump_packets_before;
        if (units >= unit_ceiling) media->pump_unit_cap++;
        media->pump_total_us += pump_ended_us - pump_started_us;
        /* Detailed reports are deliberately deferred to teardown. Writing
           their multi-line snapshot through host0 during playback can stop
           the browser thread for hundreds of milliseconds and create a
           false cadence outlier. All source counters remain cumulative, so
           the teardown report loses no information. */
    }
    if (awaiting_first_frame) {
        /* Time the browser spent blocked inside the bounded pump. The codec
           worker is asynchronous, so seconds here mean an HTTP range read. */
        uint64_t pump_ended_us = psp_media_now_us(media);
        media->first_frame_pump_us += pump_ended_us > pump_started_us
            ? pump_ended_us - pump_started_us : 0;
    }
    if (advance == MEDIA_PLAYBACK_ADVANCE_CANCELLED) {
        bool interrupted_first_frame = awaiting_first_frame;
        psp_media_interrupt_decode(media);
        if (interrupted_first_frame && !media->have_frame) {
            /* cancel_decode retires pause_boundary_pending and the
               first-frame deadline together, so nothing is left to re-arm
               the recovery ladder or to replace the "DECODING FIRST FRAME"
               surface. Without this the player sat on that surface forever
               -- most visibly when a shared cancellation token belonging to
               a page-load or render scope stopped a first-frame decode that
               no media scope was going to close. Land on the failed surface
               instead: it is closeable with CIRCLE and retryable with
               CROSS. */
            psp_media_raise_error(
                media, "VIDEO START CANCELLED", NULL);
            printf("tilefinch-media: first-frame decode cancelled; "
                   "surfaced closeable failure\n");
        }
        return true;
    }
    if (advance == MEDIA_PLAYBACK_ADVANCE_ERROR) {
        if (psp_media_retry_delivery_failure(
                media, "decode", error, false)) return true;
        if (psp_media_retry_240p(
                media, "decode", error, false)) return true;
        MediaHttpRangeStats video_http = {0};
        MediaHttpRangeStats audio_http = {0};
        (void) media_http_range_stats(media->range, &video_http);
        (void) media_http_range_stats(media->audio_range, &audio_http);
        if (video_http.failures != 0 || audio_http.failures != 0) {
            long status = video_http.last_http_status != 0
                ? video_http.last_http_status
                : audio_http.last_http_status;
            printf(
                "tilefinch-youtube: stage=transport-mid status=%ld "
                "action=failed video-failures=%zu audio-failures=%zu\n",
                status, video_http.failures, audio_http.failures);
        }
        bool retry_playing = media->ui.playing;
        psp_media_remember_retry_state(media, retry_playing);
        psp_media_release_presentation_preroll(media, true);
        media_playback_set_playing(media->playback, false);
        media->ui.playing = false;
        media->decode_job_pending = false;
        /* Retire the first-frame transaction with the failure. Leaving it
           armed kept decode_work_pending true, so the pump ran on: the
           backend's specific reason (an AVC status code, or the raw-NAL
           bridge being unavailable) was replaced two seconds later by
           "VIDEO DECODER MADE NO PROGRESS" and then by the first-frame
           timeout -- the generic label users end up reporting. */
        psp_media_retire_first_frame(media);
        psp_media_raise_error(media, error, NULL);
        MediaPlaybackJobStats stats = {0};
        media_playback_job_stats(media->playback, &stats);
        printf("tilefinch-media: advance failed: %s calls=%zu yielded=%zu "
               "packets=%zu would-block=%zu\n",
               error, stats.calls, stats.yielded_calls,
               stats.packets_submitted, stats.would_block_calls);
        return true;
    }
    /* A refusal the advance above collected, recovered by resetting rather than
       by skipping. It moves the source and drops the surface, so the frame ends
       where the recovery does. */
    bool refusal_decision_ready = false;
    bool refusal_recovered = psp_media_refusal_reset_recover(
        media, &refusal_decision_ready);
    /* Lift the worker's hold only after a real decision. A prepared audio job
       can leave the general stats snapshot temporarily unavailable after the
       refused video completion was collected; treating that as "no refusal"
       reopened the exact refusal-then-next-AU race this latch exists to close. */
    if (refusal_decision_ready)
        media_psp_backend_release_refusal_hold();
    if (refusal_recovered) return true;
    bool was_pending = media->decode_job_pending;
    media->decode_job_pending =
        advance == MEDIA_PLAYBACK_ADVANCE_PENDING;
    MediaVideoFrame frame;
    bool received_frame = false;
    bool frame_progress = false;
    bool priming_before_take = !media->have_frame
        || media->pause_boundary_pending
        || media->presentation_preroll_audio_held;
    /* `decode_clock_us` may be a private seek/startup horizon ahead of the
       frozen public clock. Decoder admission uses that lead; early/late frame
       decisions below must not. */
    uint64_t take_clock_us = media->clock_us;
    if (media->presentation_preroll_startup
        && !media->presentation_preroll_startup_claimed) {
        uint64_t first_ready_us = 0;
        if (media_playback_ready_video_start_us(
                media->playback, &first_ready_us))
            take_clock_us = first_ready_us;
    }
    if (media->presentation_floor_us != 0) {
        size_t discarded = media_playback_discard_video_before(
            media->playback, media->presentation_floor_us);
        if (discarded != 0) frame_progress = true;
        /* A requested seek time normally falls between authored frames. The
           presentation clock is intentionally frozen at that request while
           audio is held, so the ordinary half-frame due test can reject the
           first legal frame forever (for example, a 40 ms frame beginning
           29 ms after the target). Once prerequisite pictures are discarded,
           lend the due gate the actual earliest retained PTS. This admits one
           frame at or after the floor; the receipt path below then releases
           preroll and resumes the observed clock at the requested time. */
        uint64_t first_ready_us = 0;
        bool have_ready_frame = media_playback_ready_video_start_us(
            media->playback, &first_ready_us);
        take_clock_us = psp_media_seek_take_clock_us(
            take_clock_us, media->presentation_floor_us,
            have_ready_frame, first_ready_us);
    }
    media_playback_set_presentation_clock_us(
        media->playback, take_clock_us);
    bool startup_claim_allowed = false;
    if (media->presentation_preroll_startup) {
        unsigned ready =
            media_playback_ready_video_frames(media->playback);
        /* Two surfaces are the PSP backend's complete bounded queue. Do not
           claim the head while filling it: that would free one slot and turn
           priming back into an ever-moving one-frame target. Once full, lend
           exactly the first picture while sound remains held. Audio starts at
           time zero only after the final display funnel confirms that picture
           reached the LCD, so player setup cannot make sound start ahead. */
        size_t displayed =
            media_playback_displayed_video_frames(media->playback);
        if (media->presentation_preroll_startup_claimed
            && displayed
                 > media->presentation_preroll_displayed_baseline) {
            printf("tilefinch-media-clock: event=startup-prime-ready "
                   "video-ready=%u displayed=%zu\n", ready, displayed);
            psp_media_complete_priming_if_ready(
                media, "startup-prime-ready");
        } else if (!media->presentation_preroll_startup_claimed
                   && psp_media_startup_preroll_ready(ready)) {
            startup_claim_allowed = true;
        }
    }
    if (!media->presentation_preroll_startup || startup_claim_allowed)
        received_frame =
            media_playback_take_video_frame(media->playback, &frame);
    frame_progress = frame_progress || received_frame;
    if (received_frame) {
        if (media->presentation_preroll_startup)
            media->presentation_preroll_startup_claimed = true;
        if (media->presentation_floor_us != 0
            && frame.pts_us >= media->presentation_floor_us) {
            uint64_t floor_us = media->presentation_floor_us;
            printf("tilefinch-media-clock: event=seek-floor-reached "
                   "floor=%lluus video=%lluus\n",
                   (unsigned long long) floor_us,
                   (unsigned long long) frame.pts_us);
            /* Release sound at the same requested instant as the first
               eligible picture. The clock stayed frozen here while video
               privately walked the preceding random-access interval. */
            media->clock_us = floor_us;
        }
        media->frame = frame;
        media->have_frame = true;
        media->first_frame_started_us = 0;
        media->no_frame_ms = 0;
        if (media->pause_boundary_pending) {
            media->pause_boundary_pending = false;
            psp_media_dispatch(media, psp_media_service_completion(
                media, PSP_MEDIA_EVENT_FRAME_DISPLAYED),
                "first-frame-displayed");
            psp_media_release_presentation_preroll(media, true);
            media_playback_set_playing(media->playback, false);
            media->ui.playing = false;
            printf("tilefinch-media-first-frame: stage=take-frame "
                   "elapsed=%lluus pump=%lluus\n",
                   (unsigned long long) (
                       psp_media_now_us(media)
                       - media->first_frame_opened_us),
                   (unsigned long long) media->first_frame_pump_us);
            printf("tilefinch-media: first frame ready through bounded job\n");
        }
        if (priming_before_take)
            psp_media_complete_priming_if_ready(
                media, "frame-prime-ready");
        /*
         * Re-anchor a runaway clock to the picture that just reached the
         * screen. Only here: this is the one place with a fresh presented
         * timestamp to anchor to, so a stream that stops producing pictures
         * keeps the free-running clock which lets the horizon -- and the
         * pipeline -- advance out of the stall.
         */
        /* Cursor validity is transient across an A/V seek: the reset audio
           epoch has a track but no presented block yet. Use stable track
           presence here, otherwise that brief interval is mistaken for a
           video-only stream and the decoder-behind pullback can rewind the
           target clock by a full keyframe interval. */
        bool audio_track_present =
            media_playback_has_audio(media->playback);
        if (media->ui.playing && !audio_track_present
            && media->clock_us
               > frame.pts_us + PSP_MEDIA_CLOCK_MAXIMUM_LEAD_US) {
            uint64_t lead_us = media->clock_us - frame.pts_us;
            media->clock_us = frame.pts_us + PSP_MEDIA_CLOCK_MAXIMUM_LEAD_US;
            if (!media->clock_resync_logged) {
                media->clock_resync_logged = true;
                printf("tilefinch-media-clock: event=resync "
                       "reason=decoder-behind clock=%lluus video=%lluus "
                       "lead=%lluus\n",
                       (unsigned long long) media->clock_us,
                       (unsigned long long) frame.pts_us,
                       (unsigned long long) lead_us);
            }
        }
        changed = true;
        if (psp_media_transport_recovery_stable(
                media->transport_reresolve_attempts,
                media->transport_refresh_rearm_us,
                media->clock_us, true)) {
            /* Reset only after a real frame and five seconds of playback.
               The two-attempt cap therefore applies per transport incident,
               not per media session; expiry hours later gets a fresh budget. */
            media->transport_reresolve_attempts = 0;
            media->transport_refresh_rearm_us = 0;
            printf("tilefinch-youtube: stage=transport-mid "
                   "action=refresh-rearmed clock=%lluus\n",
                   (unsigned long long) media->clock_us);
        }
    } else if (media->ui.playing) {
        media->no_frame_ms =
            elapsed_ms > UINT_MAX - media->no_frame_ms
                ? UINT_MAX : media->no_frame_ms + elapsed_ms;
    } else {
        media->no_frame_ms = 0;
    }
    MediaPlaybackJobStats job_stats = {0};
    media_playback_job_stats(media->playback, &job_stats);
    psp_media_buffering_update(
        media, &job_stats, psp_media_now_us(media));
    bool packets_advanced =
        job_stats.packets_submitted != media->decode_last_packets;
    /* A pipeline waiting on its next window is making progress as long as the
       transport is. Without this the 2-second stall watchdog fires on a slow
       link the moment units stop waiting for it. */
    size_t advance_range_bytes = psp_media_range_bytes(media);
    bool range_advanced =
        advance_range_bytes != media->decode_last_range_bytes;
    media->decode_last_range_bytes = advance_range_bytes;
    bool decode_progress = frame_progress || packets_advanced
        || range_advanced || !media->decode_job_pending;
    /*
     * Name a wedge while it is happening.
     *
     * A pipeline that submits nothing for seconds while it still holds queued
     * samples, or has window bytes sitting in the transport, is stuck on one
     * of three things, and the feed line alone cannot say which: eligibility
     * (the horizon refuses everything), the source (a window that never
     * installs), or the decoder (staging never released). Every wedge this
     * project has hit surfaced instead as a timeout panel minutes later, which
     * names the watchdog rather than the cause. One line, once per wedge.
     */
    if (catch_up) {
        uint64_t stall_now_us = psp_media_now_us(media);
        if (packets_advanced || frame_progress) {
            media->stall_since_us = stall_now_us;
            media->stall_reported = false;
        } else if (media->stall_since_us == 0) {
            media->stall_since_us = stall_now_us;
        }
        media->stall_last_packets = job_stats.packets_submitted;
        bool holding = job_stats.packets_submitted != 0
            && (media->decode_job_pending || advance_range_bytes != 0);
        if (!media->stall_reported && holding
            && media->stall_since_us != 0
            && stall_now_us - media->stall_since_us
                 >= PSP_MEDIA_STALL_REPORT_US) {
            MediaHttpRangeStats stall_video = {0};
            MediaHttpRangeStats stall_audio = {0};
            (void) media_http_range_stats(media->range, &stall_video);
            (void) media_http_range_stats(media->audio_range, &stall_audio);
            MediaPlaybackJobStats before = job_stats;
            uint64_t stall_buffered_us =
                media_playback_buffered_until_us(media->playback);
            media->stall_reported = true;
            printf(
                "tilefinch-media-stall: event=no-submission for=%lluus "
                "packets=%zu horizon=%zu submit-block=%zu source-block=%zu "
                "clock=%lluus buffered=%lluus window-pending=%d/%d "
                "inflight=%zu/%zu suspect=%s\n",
                (unsigned long long) (
                    stall_now_us - media->stall_since_us),
                before.packets_submitted, before.horizon_breaks,
                before.submit_block_calls, before.source_block_calls,
                (unsigned long long) media->clock_us,
                (unsigned long long) stall_buffered_us,
                stall_video.window_pending ? 1 : 0,
                stall_audio.window_pending ? 1 : 0,
                stall_video.bytes_in_flight, stall_audio.bytes_in_flight,
                psp_media_stall_suspect_name(
                    psp_media_stall_suspect(
                        media->clock_us, stall_buffered_us,
                        before.horizon_breaks, before.submit_block_calls,
                        stall_video.window_pending
                            || stall_audio.window_pending,
                        media->decode_job_pending)));
        }
    } else {
        media->stall_since_us = 0;
        media->stall_reported = false;
    }
    if (packets_advanced && media->decode_last_packets == 0
        && media->first_frame_opened_us != 0) {
        printf("tilefinch-media-first-frame: stage=first-packet "
               "elapsed=%lluus packets=%zu\n",
               (unsigned long long) (
                   psp_media_now_us(media) - media->first_frame_opened_us),
               job_stats.packets_submitted);
    }
    media->decode_last_packets = job_stats.packets_submitted;
#ifdef TILEFINCH_PSP_VALIDATION_LOG
    /*
     * Name the stage the next hardware failure died in. The backend counters
     * only advance once the asynchronous codec worker has published a
     * completed job, so their first nonzero value is the moment firmware
     * accepted an access unit -- and their split says whether the audio or
     * the video path got there. Reading them costs a backend call, so it is
     * validation-only; shipping builds keep the field and skip the block.
     */
    if (media->first_frame_opened_us != 0
        && !media->first_frame_codec_logged) {
        MediaBackendStats codec_stats = {0};
        if (media_playback_backend_stats(media->playback, &codec_stats)
            && codec_stats.submitted_video_packets
                 + codec_stats.submitted_audio_packets != 0) {
            media->first_frame_codec_logged = true;
            printf("tilefinch-media-first-frame: stage=codec-job-complete "
                   "elapsed=%lluus video=%zu audio=%zu native=%d\n",
                   (unsigned long long) (
                       psp_media_now_us(media)
                       - media->first_frame_opened_us),
                   codec_stats.submitted_video_packets,
                   codec_stats.submitted_audio_packets,
                   codec_stats.last_native_error);
        }
    }
#endif
    if (decode_progress) {
        media->decode_no_progress_ms = 0;
    } else {
        media->decode_no_progress_ms =
            elapsed_ms > UINT_MAX - media->decode_no_progress_ms
                ? UINT_MAX : media->decode_no_progress_ms + elapsed_ms;
    }
    /* A refill still on the wire is the pipeline working, not failing: it buys
       the cold-fetch budget and nothing else does. Sampled here rather than
       folded into decode_progress so a window that never lands still fails,
       just later. */
    bool source_refilling = psp_media_source_refilling(media);
    if (media->buffering_service_active && source_refilling)
        media->decode_no_progress_ms = 0;
    if (!media->buffering_service_active && media->decode_no_progress_ms
        >= psp_media_decode_no_progress_budget_ms(source_refilling)) {
        if (psp_media_retry_240p(
                media, "no-progress",
                "VIDEO DECODER MADE NO PROGRESS", false)) return true;
        psp_media_remember_retry_state(media, media->ui.playing);
        media_playback_set_playing(media->playback, false);
        media->ui.playing = false;
        media->decode_job_pending = false;
        MediaBackendStats stalled_stats = {0};
        char stalled_reason[64];
        (void) media_playback_backend_stats(
            media->playback, &stalled_stats);
        if (stalled_stats.last_native_error != 0) {
            snprintf(stalled_reason, sizeof(stalled_reason),
                     "DECODER STALLED %08X",
                     (unsigned) stalled_stats.last_native_error);
        } else {
            snprintf(stalled_reason, sizeof(stalled_reason),
                     "NO PACKET ACCEPTED IN %ums",
                     media->decode_no_progress_ms);
        }
        psp_media_retire_first_frame(media);
        psp_media_raise_error(
            media, "VIDEO DECODER MADE NO PROGRESS", stalled_reason);
        printf("tilefinch-media: bounded decoder stalled for %ums "
               "calls=%zu yielded=%zu packets=%zu would-block=%zu "
               "refilling=%d\n",
               media->decode_no_progress_ms, job_stats.calls,
               job_stats.yielded_calls, job_stats.packets_submitted,
               job_stats.would_block_calls, source_refilling ? 1 : 0);
        return true;
    }
    awaiting_first_frame =
        media->pause_boundary_pending && !media->have_frame;
    uint64_t now_us = psp_media_now_us(media);
    if (!awaiting_first_frame) {
        media->first_frame_opened_us = 0;
        media->first_frame_pump_us = 0;
    } else if (media->first_frame_opened_us != 0) {
        size_t range_bytes = psp_media_range_bytes(media);
        bool progressed = received_frame || packets_advanced
            || range_bytes != media->first_frame_bytes;
        PspMediaFirstFrameVerdict verdict = psp_media_first_frame_verdict(
            media->first_frame_opened_us, media->first_frame_started_us,
            media->first_frame_pump_us, now_us, progressed);
        uint64_t pumped_us = media->first_frame_pump_us;
        uint64_t waited_us = now_us - media->first_frame_opened_us;
        if (progressed) {
            media->first_frame_bytes = range_bytes;
            media->first_frame_started_us = now_us;
            media->first_frame_pump_us = 0;
        }
        if (verdict != PSP_MEDIA_FIRST_FRAME_WAITING) {
            bool network =
                verdict == PSP_MEDIA_FIRST_FRAME_NETWORK_STALLED;
            char reason[64];
            MediaBackendStats backend_stats = {0};
            (void) media_playback_backend_stats(
                media->playback, &backend_stats);
            if (network) {
                snprintf(reason, sizeof(reason),
                         "NETWORK STALLED %zuKB IN", range_bytes / KIB);
            } else if (backend_stats.last_native_error != 0) {
                snprintf(reason, sizeof(reason), "DECODER STALLED %08X",
                         (unsigned) backend_stats.last_native_error);
            } else {
                snprintf(reason, sizeof(reason),
                         "DECODER STALLED AT %zu PACKETS",
                         job_stats.packets_submitted);
            }
            printf("tilefinch-media-first-frame: stage=timeout "
                   "verdict=%s elapsed=%lluus pump=%lluus packets=%zu "
                   "bytes=%zu calls=%zu yielded=%zu would-block=%zu\n",
                   network ? "network" : "decoder",
                   (unsigned long long) waited_us,
                   (unsigned long long) pumped_us,
                   job_stats.packets_submitted,
                   range_bytes, job_stats.calls,
                   job_stats.yielded_calls, job_stats.would_block_calls);
            /* A starved network is not a decoder-capability problem. Spending
               the one-shot 240p fallback on it only re-resolves and
               re-downloads a second stream over the same weak link -- nothing
               survives pipeline_destroy, so the retry starts from zero bytes.
               Reserve that attempt for a decoder which genuinely stalled. */
            if (!network
                && psp_media_retry_240p(
                       media, "first-frame-timeout", reason, false))
                return true;
            psp_media_remember_retry_state(media, false);
            media_playback_set_playing(media->playback, false);
            media->ui.playing = false;
            media->decode_job_pending = false;
            psp_media_retire_first_frame(media);
            psp_media_raise_error(
                media, "VIDEO FIRST FRAME TIMED OUT", reason);
            return true;
        }
    }
    if (was_pending != media->decode_job_pending) changed = true;
    bool ended = media_playback_ended(media->playback);
    if (ended && !media->machine.ended) {
        psp_media_dispatch(media, (PspMediaEvent) {
            .type = PSP_MEDIA_EVENT_PLAYBACK_ENDED
        }, "playback-ended");
    }
    if (awaiting_first_frame) {
        size_t packet_steps = job_stats.packets_submitted > 7u
            ? 7u : job_stats.packets_submitted;
        /* Network-versus-decoder attribution remains in telemetry. The
           visible surface stays stable while this percentage advances. */
        psp_ui_media_set_resolving_progress(
            &media->ui, "Loading...",
            920u + (unsigned) packet_steps * 10u);
    } else {
        psp_ui_media_set(
            &media->ui, media->ui.visible,
            media->ui.playing && !ended,
            ended, media->clock_us,
            psp_media_duration_us(media),
            media->stream.title);
    }
    uint64_t buffered =
        media_playback_buffered_until_us(media->playback);
    bool buffering = media->pause_boundary_pending
        || media->buffering_service_active
        || (media->ui.playing && media->no_frame_ms >= 500u
            && buffered <= media->clock_us + UINT64_C(50000));
    psp_ui_media_set_buffering(&media->ui, buffering, buffered);
    psp_ui_media_tick(&media->ui, elapsed_ms);
    if (ended && media->last_resume_saved_us != UINT64_MAX) {
        psp_media_record_resume(media, true);
    } else if (media->ui.playing
        && media->last_resume_saved_us != UINT64_MAX
        && media->clock_us
             >= media->last_resume_saved_us + UINT64_C(10000000)) {
        /* Keep the hot playback loop off the slow Memory Stick. The bounded
           in-memory record is flushed on close, navigation, or clean exit. */
        psp_media_record_resume(media, false);
    }
    psp_media_session_checkpoint(media, "advance-end");
    return changed;
}
