#ifndef TILEFINCH_PSP_MEDIA_SESSION_H
#define TILEFINCH_PSP_MEDIA_SESSION_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "tilefinch/browser_profile.h"
#include "tilefinch/budget.h"
#include "tilefinch/cancellation.h"
#include "tilefinch/font.h"
#include "tilefinch/media_backend.h"
#include "tilefinch/media_file.h"
#include "tilefinch/media_http.h"
#include "tilefinch/media_mp4.h"
#include "tilefinch/navigation.h"
#include "tilefinch/offline_library.h"
#include "tilefinch/psp_media_present.h"
#include "tilefinch/psp_media_state.h"
#include "tilefinch/psp_ui.h"
#include "tilefinch/session.h"
#include "tilefinch/youtube_resolver.h"

typedef enum {
    PSP_MEDIA_JOB_NONE = 0,
    PSP_MEDIA_JOB_OPEN_RESOLVE,
    PSP_MEDIA_JOB_OPEN_VIDEO_RANGE,
    PSP_MEDIA_JOB_OPEN_VIDEO_DEMUX,
    PSP_MEDIA_JOB_OPEN_VIDEO_PRIME,
    PSP_MEDIA_JOB_OPEN_AUDIO_RANGE,
    PSP_MEDIA_JOB_OPEN_AUDIO_DEMUX,
    PSP_MEDIA_JOB_OPEN_DECODER_PREPARE,
    PSP_MEDIA_JOB_OPEN_PLAYBACK,
    PSP_MEDIA_JOB_SEEK_PREPARE,
    PSP_MEDIA_JOB_SEEK_DECODE,
    PSP_MEDIA_JOB_PREVIEW_RESTORE_PREPARE,
    PSP_MEDIA_JOB_PREVIEW_RESTORE_DECODE
} PspMediaJobPhase;

typedef struct {
    YoutubeStream stream;
    char video_path[OFFLINE_LIBRARY_DIRECTORY_LIMIT + 40u];
    char audio_path[OFFLINE_LIBRARY_DIRECTORY_LIMIT + 40u];
} PspMediaOfflineSource;

typedef struct {
    void *context;
    void *profile_context;
    uint64_t (*now_us)(void *context);
    bool (*cancel_requested)(void *context);
    size_t (*free_memory)(void *context);
    size_t (*maximum_free_block)(void *context);
    bool (*resolve_offline)(
        void *context, const char *url, PspMediaOfflineSource *source);
    void (*profile_changed)(void *context, uint64_t now_us);
    bool (*write_failure_report)(
        void *context, const char *stage, const char *detail,
        const char *url, long http_status, int native_result);
} PspMediaSessionPlatform;

typedef struct {
    Budget *budget;
    BrowserSession *session;
    BrowserProfile *profile;
    const char *profile_path;
    MediaHttpRange *range;
    MediaHttpRange *audio_range;
    MediaFileRange *file_range;
    MediaFileRange *audio_file_range;
    MediaMp4Demux *demux;
    MediaMp4Demux *audio_demux;
    MediaPlayback *playback;
    MediaVideoFrame frame;
    PspUiMediaState ui;
    YoutubeStream stream;
    YoutubeResolveJob *resolver_job;
    PspMediaSessionPlatform platform;
    /*
     * The cooperate scope's token for the open transaction currently being
     * pumped, or NULL outside one. The platform cancel_requested callback
     * reads the process-global scope, which is correct only for as long as
     * the invariant "the active scope is this open job's scope" holds. Keeping
     * the caller's own token here makes open-side cancellation depend on what
     * the caller passed rather than on that invariant.
     */
    const TilefinchCancellation *open_cancellation;
    uint64_t generation;
    uint64_t clock_us;
    /* After a non-preview seek, random-access decode begins at the preceding
       keyframe. Frames before this floor are prerequisites, not presentation
       candidates, and stay internal until the requested position is reached. */
    uint64_t presentation_floor_us;
    /* Adaptive audio stays at the requested AU while video privately walks
       from the preceding random-access point to presentation_floor_us. */
    bool presentation_preroll_audio_held;
    bool controller_audio_hold;
    /* Initial playback has no nonzero seek floor. It waits for the bounded
       decoded-picture queue to fill before audio establishes the clock. */
    bool presentation_preroll_startup;
    bool presentation_preroll_startup_claimed;
    size_t presentation_preroll_displayed_baseline;
    bool have_frame;
    /* Physical open service has been requested but has not entered its first
       pump phase. Lifecycle authority lives in machine.state. */
    bool open_service_pending;
    bool system_suspended;
    bool system_resume_playing;
    bool suspended_for_internal_view;
    bool seek_preview_started;
    bool seek_preview_was_playing;
    bool seek_preview_cancel_pending;
    uint16_t *seek_preview_pixels;
    unsigned no_frame_ms;
    unsigned decode_no_progress_ms;
    /*
     * The first-frame watchdog. `first_frame_started_us` is the last observed
     * progress, not the start of the attempt: `first_frame_opened_us` holds
     * the absolute cap's origin. `first_frame_pump_us` accumulates time spent
     * inside the bounded decode pump since that progress, which separates a
     * starved network from a stalled decoder.
     */
    uint64_t first_frame_started_us;
    uint64_t first_frame_opened_us;
    uint64_t first_frame_pump_us;
    size_t first_frame_bytes;
    /* Validation-log bookkeeping only; shipping builds never read it. */
    bool first_frame_codec_logged;
    size_t decode_last_packets;
    /* Range bytes at the previous advance. A refill that is still on the wire
       is progress, and the stall watchdog must not mistake it for a hung
       decoder now that units return instead of waiting. */
    size_t decode_last_range_bytes;
    /* Wedge detector: the packet count and the moment it last moved, so a
       pipeline that stops submitting while it still has queued samples or
       bytes in flight names the resource it is stuck on exactly once. */
    size_t stall_last_packets;
    uint64_t stall_since_us;
    bool stall_reported;
    bool decode_job_pending;
    /*
     * Refused-access-unit recovery, armed by psp_media_set_refusal_reset.
     *
     * The refusal itself is detected on the codec worker, which publishes it
     * exactly as it always has -- a counter and a log line -- and then carries
     * on. These are the browser thread's side: the refusal total it has
     * already accounted for, so an advance that collected a job can tell a
     * fresh refusal from an old one, and a count of the resets it performed.
     * The worker never reads or waits on either.
     */
    size_t refusal_reset_seen_refusals;
    unsigned refusal_resets;
    /* Physical recovery service and one-shot presentation-boundary facts.
       Lifecycle authority remains exclusively in machine.state. */
    bool recovery_service_active;
    bool pause_boundary_pending;
    /* The source-hold actuator can remain set briefly while a Buffering exit
       event is committed. It is not a second lifecycle state. Presentation
       pauses while the fixed HTTP windows continue filling; no Memory Stick
       cache participates. */
    bool buffering_service_active;
    bool buffering_started_during_startup;
    bool network_buffer_slow_logged;
    bool startup_buffer_applied;
    uint64_t network_starved_since_us;
    uint64_t network_buffer_ready_since_us;
    uint64_t network_buffer_started_us;
    uint64_t network_buffer_total_us;
    size_t network_buffer_source_blocks_seen;
    unsigned network_buffer_events;
    /* One prolonged-buffer snapshot and one terminal snapshot may be useful
       for a route, but neither belongs on the hot path. The lifetime count
       bounds Memory Stick writes even across many failing routes. */
    unsigned failure_report_level;
    unsigned failure_report_writes;
    bool range_pump_audio_first;
    BrowserYoutubeQuality requested_quality;
    bool quality_fallback_attempted;
    uint64_t reopen_resume_us;
    bool reopen_resume_playing;
    bool reopen_resume_pending;
    /* A quality/transport retry that interrupted a tentative scrub restores
       the committed playback position first, then reissues only the latest
       highlighted target. This is continuation data for that invoked
       service, not a parallel lifecycle state. */
    uint64_t reopen_preview_target_us;
    bool reopen_preview_pending;
    uint64_t preview_commit_target_us;
    bool preview_commit_pending;
    bool preview_commit_resume_playing;
    /* A large rewind recreates the firmware backend but may reopen from the
       already-authorized direct URLs while they remain valid. Consumed by
       the next open transaction only. */
    bool reopen_reuse_resolved_stream;
    /* Validation accounting only; this never selects a lifecycle edge or
       service. A backend-recreating backward seek earns one completion only
       after the replacement pipeline has opened at its target. Keeping this
       outside PspMediaMachine prevents telemetry from becoming authority. */
    bool reopen_seek_completion_pending;
    bool resume_profile_dirty;
    unsigned transport_reresolve_attempts;
    uint64_t transport_refresh_rearm_us;
    uint64_t transport_next_expiry_check_us;
    bool offline_source;
    /* One line per opened stream: the clock re-anchors on every later frame
       once a decoder is behind, and a per-frame line would be the log. */
    bool clock_resync_logged;
    unsigned quality_fallbacks;
    uint64_t present_scale_total_us;
    uint64_t present_scale_max_us;
    size_t present_scale_frames;
    /*
     * The graphics-engine presenter's own cost, separated because it has two
     * halves that behave differently: building the display list is a fixed
     * handful of microseconds, while the sync is the CPU blocked until the GE
     * has finished writing the buffer the chrome compositor is about to
     * touch. Only the second scales with the picture.
     */
    uint64_t present_ge_submit_total_us;
    uint64_t present_ge_sync_total_us;
    uint64_t present_ge_sync_max_us;
    /* Of that sync, the part actually blocked after the pump returned. */
    uint64_t present_ge_wait_total_us;
    uint64_t present_ge_wait_max_us;
    size_t present_ge_frames;
    /* Presents the identity skip retired without redrawing anything. */
    size_t present_skipped_frames;
    /*
     * The picture currently staged in the caller's EDRAM texture buffer. The
     * staging itself is not owned here: it lives in the video layout's own
     * VRAM (see include/tilefinch/psp_display.h), because the device measured
     * the engine's texture read at 10,312us out of main memory against 456us
     * to write the frame -- 96% of a video frame was that read, and EDRAM is
     * the memory the engine is fast against.
     */
    uint64_t present_stage_identity;
    size_t present_stage_frames;
    uint64_t present_stage_total_us;
    uint64_t present_stage_max_us;
    /* A stage copy for this picture is running on the DMA worker and a join is
       owed before the graphics engine may sample the texture. When set, the
       present feeds the decoder while the copy runs and collects it, and the
       stage total it records is the residual wait after that feeding -- the
       main-thread cost the overlap did not hide, near zero when it worked. */
    bool present_stage_async;
    /* Where that copy is going and how much of it, kept so the pixel signature
       can be re-taken from the staged bytes once the join has landed. The
       async path is the shipping one, and it is the path on which the copy's
       source is read by a controller running in parallel -- which is exactly
       the case a metadata-only check cannot speak for. */
    uint32_t *present_stage_pixels;
    int present_stage_width;
    int present_stage_rows;
    /* Whether the texture the engine is about to sample is that staged copy or
       the decoder's own surface. The pump that fills the engine's draw feeds
       video only in the first case; in the second the engine is reading the
       memory a newly emitted picture would overwrite. */
    bool present_texture_staged;
    /*
     * The picture whose staging DMA could not be joined, or -1.
     *
     * Its slot is quarantined in the backend and its claim is still standing,
     * so the release that would normally have happened at the join is owed
     * until the presenter observes the abandoned transfer end. Holding the
     * generation as well as the slot is what makes that late release name the
     * same picture the copy read rather than whatever the slot holds by then.
     */
    int dma_quarantine_slot;
    uint32_t dma_quarantine_generation;
    /* The feed taken immediately before the frame blocks for the vertical
       blank, and what it handed the worker to do during it. */
    size_t prevblank_frames;
    size_t prevblank_submitted;
    /*
     * What the playing pump actually got through per browser frame. The
     * decoder needs about 73 access units a second for 240p30 with 44.1 kHz
     * AAC; whether it gets them is a question about units per frame, and
     * whether the frame ran out of slice or ran out of work.
     */
    size_t pump_frames;
    size_t pump_units;
    size_t pump_units_submitted;
    size_t pump_slice_exhausted;
    size_t pump_unit_cap;
    uint64_t pump_total_us;
    uint64_t pump_max_unit_us;
    /* The units run inside a present, while the graphics engine had the
       destination and the CPU had nothing else to do. Counted apart from the
       frame's own pump so the report says where the feed came from. */
    size_t pump_draw_frames;
    size_t pump_draw_units;
    size_t pump_draw_submitted;
    uint64_t pump_draw_us;
    /*
     * The interactive loop's own period, sampled where the loop reaches the
     * media every frame.
     *
     * The present is fully decomposed -- stage, submit, draw-pump, engine
     * wait -- and those four summed to 22.4ms of a 22.5ms measured present,
     * so nothing is hidden inside it. What was hidden is everything outside
     * it: a device cycle delivered 15s of content over a far longer window
     * with a 22.5ms present, and no counter said where the rest of each frame
     * went. This is that number, and the present subtracted from it is the
     * part of the frame the media path does not own.
     */
    unsigned slow_unit_reports;
    bool advance_previous_playing;
    uint64_t advance_previous_us;
    uint64_t advance_period_total_us;
    uint64_t advance_period_max_us;
    size_t advance_periods;
    /* The same period, split by whether the frame it measures was actively
       playing. A slow open or a buffering pause has its own UI and cannot be
       read as a playback hitch; only the playing bucket's max is a hitch a
       viewer would see. */
    uint64_t advance_playing_period_max_us;
    uint64_t advance_playing_period_total_us;
    size_t advance_playing_periods;
    /* The steady-state bucket: the same playing periods with the warmup frames
       (open, first decode, the one-time connection handshake) and any lone
       pathological stall left out, so the sustained cadence a viewer sees is
       not conflated with a one-time startup cost. This total divided by its
       count is the real presented frame-period; its reciprocal is the rate. */
    uint64_t advance_playing_settled_total_us;
    size_t advance_playing_settled_periods;
    size_t accumulated_decoded_video_frames;
    size_t accumulated_dropped_video_frames;
    size_t accumulated_discarded_seek_video_frames;
    size_t accumulated_video_claims;
    size_t accumulated_video_claims_displayed;
    size_t accumulated_video_claims_dropped;
    size_t accumulated_video_claims_quiesced;
    size_t accumulated_audio_packets;
    unsigned accumulated_native_errors;
    size_t peak_external_bytes;
    /* The backend may refuse a stats copy while its firmware worker owns the
       job slot. Keep the last complete snapshot so lifecycle completion and
       teardown never turn a healthy run into a zero-counter report merely
       because they sampled during that short ownership window. */
    MediaBackendStats backend_stats_snapshot;
    uint64_t backend_stats_snapshot_us;
    bool backend_stats_snapshot_valid;
    unsigned seek_completions;
    uint64_t last_resume_saved_us;
    char source[NAVIGATION_URL_LIMIT];
    char offline_video_path[OFFLINE_LIBRARY_DIRECTORY_LIMIT + 40u];
    char offline_audio_path[OFFLINE_LIBRARY_DIRECTORY_LIMIT + 40u];
    PspMediaJobPhase job_phase;
    uint64_t job_target_us;
    uint64_t job_restore_us;
    uint64_t job_started_us;
    uint64_t job_phase_started_us;
    uint64_t job_maximum_unit_us;
    /* Next moment an open transaction says where it is. See
       psp_media_open_report: a stuck open used to be indistinguishable from a
       finished one in a log. */
    uint64_t open_report_next_us;
    size_t job_units;
    bool job_preview;
    bool job_resume_playing;
    /*
     * True while the seek in flight is the one an open performs to reach a
     * saved or recovered position, rather than one a viewer asked for. Its
     * decode leg is retired by design: see psp_media_seek_decode_pump.
     */
    bool job_resume_open;
    /*
     * Always-on lifecycle authority. Validation builds retain a read-only
     * projection of displaced flags and owned resources so a device run can
     * detect drift; decoded pictures, range chunks, and audio blocks never
     * enter this reducer.
     */
    PspMediaMachine machine;
    PspMediaServiceToken service;
    bool dispatch_active;
    bool deferred_event_valid;
    PspMediaEvent deferred_event;
    const char *deferred_checkpoint;
    uint32_t stale_service_completions;
    uint32_t deferred_dispatch_overflows;
#ifdef TILEFINCH_PSP_VALIDATION_LOG
    uint32_t controller_events;
    uint32_t controller_mismatches;
    uint32_t controller_violations;
    uint32_t controller_last_mismatch;
    unsigned controller_trace_head;
    unsigned controller_trace_count;
    PspMediaEventType controller_trace_event[16];
    PspMediaSessionState controller_trace_from[16];
    PspMediaSessionState controller_trace_to[16];
    uint32_t controller_trace_violations[16];
    const char *controller_trace_checkpoint[16];
#endif
} PspMediaSession;

void psp_media_init(
    PspMediaSession *media, Budget *budget, BrowserSession *session,
    BrowserProfile *profile, const char *profile_path,
    const FontFace *title_font, const PspMediaSessionPlatform *platform);
void psp_media_pipeline_destroy(PspMediaSession *media);
bool psp_media_backend_stats_snapshot(
    PspMediaSession *media, MediaBackendStats *stats);
/* Final process teardown. Unlike the user-facing close path this never
   retains a dormant decoder and never persists navigation state. */
void psp_media_shutdown(PspMediaSession *media);
/*
 * Choose how a refused access unit is recovered. Off, the refusal is skipped
 * and decoding continues, which is what shipping does. On, the session resets
 * and reprimes the decoder and resumes from the next keyframe.
 *
 * Process-wide and published once from boot configuration, beside
 * media_psp_backend_set_au_dump and for the same reason: the choice is made
 * before any session exists to carry it, and every session a boot produces
 * gets the recovery that boot asked for.
 */
void psp_media_set_refusal_reset(bool enabled);
uint64_t psp_media_duration_us(const PspMediaSession *media);
bool psp_media_record_resume(PspMediaSession *media, bool persist);
/*
 * Release firmware/audio/network-facing media state across PSP suspend while
 * retaining the route, UI, and timestamp needed to reopen after resume.
 */
void psp_media_suspend(PspMediaSession *media);
void psp_media_resume(PspMediaSession *media);
bool psp_media_system_suspended(const PspMediaSession *media);
bool psp_media_open_work_pending(const PspMediaSession *media);
/*
 * Enforce the open transaction's deadline and cancellation on a frame that
 * does not pump it.
 *
 * The interactive loop suppresses psp_media_advance while a page navigation
 * owns the cooperate scope, and an open that nothing is pumping still has to
 * be bounded and still has to answer CIRCLE. Call this on every frame an open
 * is pending, whether or not the pump also runs; returns true when the
 * transaction was retired.
 */
bool psp_media_open_watchdog(PspMediaSession *media);
bool psp_media_decode_work_pending(const PspMediaSession *media);
void psp_media_prepare_route(
    PspMediaSession *media, const char *url, uint64_t generation);
bool psp_media_request_seek(
    PspMediaSession *media, uint64_t target_us, bool preview);
void psp_media_cancel_decode(PspMediaSession *media);
void psp_media_close(PspMediaSession *media);
/*
 * Release a deliberately retained, hidden playback pipeline before another
 * optional subsystem (notably the voice model) needs its contiguous memory.
 * Visible playback is never disturbed.
 */
bool psp_media_reclaim_hidden_pipeline(PspMediaSession *media);
/*
 * Reclaim a hidden retained decoder before a different page starts allocating.
 * A navigation to the same video keeps the pipeline for instant replay.
 */
bool psp_media_reclaim_hidden_pipeline_for_navigation(
    PspMediaSession *media, const char *target_url);
void psp_media_execute_intent(
    PspMediaSession *media, PspUiMediaIntent intent);
bool psp_media_advance(
    PspMediaSession *media, unsigned elapsed_ms,
    const TilefinchCancellation *cancellation);

/*
 * Feed the decoder while the caller is blocked on something that touches no
 * media memory -- the graphics engine finishing a frame, today.
 *
 * The presenter submits its draw, calls this, and only then waits. It changes
 * no clock, produces no frame and touches no UI: it is the ordinary bounded
 * pump, run in time that would otherwise be spent idling. Returns the units
 * it ran, which is zero whenever a job is in flight or nothing is playing.
 */
size_t psp_media_pump_while_drawing(PspMediaSession *media);

/*
 * The texture the graphics engine should sample for the current picture.
 *
 * Stages a staged copy when the plan admits one and the picture has changed
 * since the last, and otherwise answers the decoder's surface where it lies.
 * Never fails: an allocation that cannot be made simply leaves the linear
 * layout, which is slower and correct.
 *
 * The identity gate is what makes the copy affordable. It runs at the rate
 * pictures arrive -- thirty a second at most -- not at the rate the panel is
 * presented, and never for the second buffer of a double-buffered pair.
 */
void psp_media_present_texture_for(
    PspMediaSession *media, const PspMediaPresentPlan *plan,
    const MediaVideoFrame *frame, void *staging, size_t staging_bytes,
    PspMediaPresentTexture *texture);

/*
 * Collect the picture staged asynchronously by psp_media_present_texture_for.
 *
 * When that posted the copy to the DMA worker it left a join owed; this feeds
 * the decoder while the controller finishes and then joins, so the texture is
 * whole before the caller starts the list. It must be called after
 * texture_for and before the draw is submitted. A no-op when the stage was
 * synchronous or the picture was unchanged.
 */
void psp_media_present_texture_finish(PspMediaSession *media);

/*
 * Give the decoded surface back to the decoder.
 *
 * Only the unstaged present needs this: there the graphics engine samples the
 * surface itself, so the claim psp_media_present_texture_for took has to
 * outlive the draw. Call it once the engine has finished with the rows. The
 * staged present drops its claim with the join and this is then a no-op, as
 * it is for a present that never took one; the next frame's advance releases
 * anything a failed present left standing.
 */
void psp_media_present_texture_release(PspMediaSession *media);

/*
 * Hand the codec worker something to do while this thread is stopped.
 *
 * Call immediately before the frame's wait for the vertical blank. The worker
 * runs one priority below the browser thread, so it decodes only while this
 * thread is blocked, and only what it already holds -- an idle worker stays
 * idle through the whole wait. One bounded submission attempt, no waiting of
 * any kind, and nothing at all unless an ordinary playing session owns the
 * pipeline. Returns what it managed to submit.
 */
size_t psp_media_feed_before_blocking(PspMediaSession *media);

#endif
