#ifndef TILEFINCH_MEDIA_BACKEND_H
#define TILEFINCH_MEDIA_BACKEND_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "tilefinch/budget.h"
#include "tilefinch/cancellation.h"
#include "tilefinch/media_mp4.h"

typedef enum {
    MEDIA_BACKEND_ACCEPTED = 0,
    /* The backend copied and owns this packet, but its bounded native work
       completes asynchronously. The playback scheduler may retire the
       packet and must yield before submitting another one. A later
       advance/submit surfaces completion or error. */
    MEDIA_BACKEND_QUEUED,
    MEDIA_BACKEND_WOULD_BLOCK,
    MEDIA_BACKEND_END,
    MEDIA_BACKEND_ERROR
} MediaBackendResult;

typedef enum {
    MEDIA_PIXEL_RGB565 = 0,
    MEDIA_PIXEL_RGBA8888
} MediaPixelFormat;

typedef struct {
    const void *pixels;
    int width;
    int height;
    int stride_pixels;
    MediaPixelFormat format;
    uint64_t pts_us;
    uint64_t duration_us;
    uint64_t identity;
    /*
     * Which decoded-output slot these pixels live in, and which picture that
     * slot was holding when the claim was made. -1 for a backend with no slots
     * to name, which is every backend but the PSP one.
     *
     * Carried on the frame because a reader that only knows the address cannot
     * tell the picture it claimed from the one that replaced it -- the address
     * is the same and the memory is valid, so nothing else in the system would
     * notice. Every reader passes these back when it claims the surface, and
     * the backend refuses the claim if they no longer describe the slot.
     *
     * The epoch says which seek/reset generation the picture belongs to, so a
     * frame retained across a reset is recognisably not of this stream.
     */
    int slot;
    uint32_t generation;
    uint64_t epoch;
} MediaVideoFrame;

/*
 * During keyframe-preroll seeking, keep decoding until the retained frame is
 * the nearest representable frame at the target. A frame in the first half
 * of its display interval is still the nearest past frame; older frames are
 * discarded without copying their surface.
 */
static inline bool media_video_frame_matches_seek(
    const MediaVideoFrame *frame, uint64_t target_us)
{
    if (frame == NULL) return false;
    if (frame->pts_us >= target_us) return true;
    uint64_t lag_us = target_us - frame->pts_us;
    return frame->duration_us != 0
        && lag_us <= frame->duration_us / 2u;
}

static inline bool media_video_frame_is_due(
    const MediaVideoFrame *frame, uint64_t presentation_clock_us)
{
    if (frame == NULL) return false;
    return frame->pts_us <= presentation_clock_us
        || frame->duration_us == 0
        || frame->pts_us - presentation_clock_us
           <= frame->duration_us / 2u;
}

typedef enum {
    MEDIA_VIDEO_SEEK_WAIT = 0,
    MEDIA_VIDEO_SEEK_MATCH,
    MEDIA_VIDEO_SEEK_FINAL_FALLBACK,
    MEDIA_VIDEO_SEEK_UNAVAILABLE
} MediaVideoSeekDecision;

/*
 * Some valid files end before yielding a frame at or near a requested target
 * (most commonly a seek exactly to duration). Keep the last decoded frame as
 * a deterministic fallback once the bounded decoder reports completion.
 */
static inline MediaVideoSeekDecision media_video_seek_decide(
    const MediaVideoFrame *frame, uint64_t target_us, bool decode_pending)
{
    if (media_video_frame_matches_seek(frame, target_us))
        return MEDIA_VIDEO_SEEK_MATCH;
    if (decode_pending) return MEDIA_VIDEO_SEEK_WAIT;
    return frame != NULL && frame->pixels != NULL
        ? MEDIA_VIDEO_SEEK_FINAL_FALLBACK
        : MEDIA_VIDEO_SEEK_UNAVAILABLE;
}

/*
 * The widest per-slot statistics array any backend reports. The PSP backend
 * pins its own PSP_MEDIA_SURFACE_SLOTS against this with a static assertion,
 * so raising the slot count without widening the reporting cannot compile.
 * Every other backend has no slots and leaves these zero.
 */
#define MEDIA_BACKEND_MAX_SURFACE_SLOTS 2u

/*
 * The three widths the occupancy ladder reports over, pinned here for the same
 * reason the slot count is: a backend may not report a narrower array than it
 * measures. Each mirrors a constant in src/media_backend_psp_policy.h -- the
 * four slot states, the four main-loop phases, and the fourteen job-duration
 * buckets -- and the PSP backend static-asserts every one of them against its
 * own, so widening a definition without widening the reporting cannot compile.
 */
#define MEDIA_BACKEND_SLOT_STATES 4u
#define MEDIA_BACKEND_LOOP_PHASES 4u
#define MEDIA_BACKEND_JOB_HISTOGRAM_BUCKETS 14u

typedef struct {
    size_t submitted_video_packets;
    size_t submitted_audio_packets;
    size_t decoded_video_frames;
    uint64_t decoded_audio_samples;
    size_t dropped_video_frames;
    /* Decoded random-access prerequisites retired below a requested seek
       floor. They were never presentation candidates and must not be counted
       as cadence loss. */
    size_t discarded_seek_video_frames;
    /* Access units firmware looked at and refused, which the pipeline skipped
       rather than failing on. One is a dropped frame; a run of them is a
       stream the decoder will not take. */
    size_t refused_video_packets;
    uint64_t dropped_audio_samples;
    size_t external_bytes;
    uint64_t presented_video_us;
    uint64_t audio_origin_us;
    uint32_t audio_output_blocks;
    unsigned audio_sample_rate;
    /*
     * The bounded wait a pump takes on an in-flight decoder job. Whether that
     * wait is long enough to be worth taking is the difference between one
     * access unit a browser frame and several, and it cannot be inferred from
     * the job-complete elapsed time, which is measured from queue to
     * collection and so reports the wait itself.
     */
    size_t codec_wait_calls;
    size_t codec_wait_collected;
    uint64_t codec_wait_us;
    /*
     * What the decoded surface's reader/writer handshake cost. A borrow that
     * had to wait found a colour conversion already inside firmware; a
     * deferral is a conversion that gave up waiting for a reader and left its
     * picture for a later submit. Both should be rare and neither should ever
     * time out -- these exist so "should" can be read off a device.
     */
    size_t surface_borrow_waits;
    uint64_t surface_borrow_wait_us;
    size_t surface_borrow_timeouts;
    size_t surface_emit_deferrals;
    int last_native_error;
    /*
     * Picture batches firmware returned with more pictures than the pipeline
     * had retained timestamps for. Appended here rather than beside
     * refused_video_packets so no existing positional reader moves.
     *
     * The cause was a refused access unit whose picture firmware kept while the
     * refusal path discarded its timestamp; that is fixed at the source, so
     * this should stay zero. It is published because a nonzero value means the
     * accounting has desynced again, and it is the one counter that says so
     * before anything else goes wrong.
     */
    size_t overrun_video_batches;
    /*
     * Video cadence instrumentation, appended and never reordered.
     *
     * A 24 fps stream presents at 19-20 fps and every configuration tried
     * moved that number by nothing, so the remaining question is not "which
     * knob" but "which segment of one picture's journey is long". These
     * fields time that journey -- submit to decode, decode to colour
     * conversion, conversion to take, take to present -- and count every
     * opportunity the pipeline declined to take along the way, one counter
     * per reason rather than the single aggregate submit-block that could
     * not tell them apart. Published on tilefinch-media-video-cadence.
     *
     * All of them are counters and timestamp differences on paths that
     * already run; none is behind a build flag, because the run that has to
     * answer this is the ordinary soak.
     */
    /* One classification per take_video_frame call. satisfied + empty +
       stale + busy + early sum to calls. */
    size_t video_take_calls;
    size_t video_take_satisfied;
    size_t video_take_empty;   /* nothing has ever been emitted to take */
    size_t video_take_stale;   /* the emitted picture was already taken */
    size_t video_take_busy;    /* the due slot's write lease still stood */
    size_t video_take_early;   /* emitted, but the clock has not reached it */
    uint64_t video_take_age_total_us; /* emit -> take, over satisfied takes */
    uint32_t video_take_age_max_us;
    /* Pictures colour-converted into the single shared surface, by the visit
       that performed the conversion. worker + submit + drain == emits. */
    size_t video_emits;
    size_t video_emits_worker;
    size_t video_emits_submit;
    size_t video_emits_drain;
    /* Decode completion to conversion: what a captured-pending picture waited
       for a visit that was allowed to emit it. */
    uint64_t video_emit_pending_total_us;
    uint32_t video_emit_pending_max_us;
    /* What firmware returned per decode call. A batch above one needs an
       extra submit or drain visit per surplus picture. */
    size_t video_batches;
    size_t video_batch_pictures;
    size_t video_batch_multi;
    uint32_t video_batch_max;
    /* Submit/drain visits turned away by a hold while a captured batch was
       still waiting to be emitted. */
    size_t video_emit_visit_starved;
    /* Stage times, one sample per picture or per picture-bearing job. */
    size_t video_decode_jobs;          /* queue -> firmware returned pictures */
    uint64_t video_decode_total_us;
    uint32_t video_decode_max_us;
    size_t video_presents;             /* take -> borrow release */
    uint64_t video_present_total_us;
    uint32_t video_present_max_us;
    size_t video_reoffers;             /* borrow release -> next accepted AU */
    uint64_t video_reoffer_total_us;
    uint32_t video_reoffer_max_us;
    size_t video_submit_periods;       /* accepted AU -> next accepted AU */
    uint64_t video_submit_period_total_us;
    uint32_t video_submit_period_max_us;
    /* Video-submit and drain opportunities refused, split by hold class. The
       aggregate these came from is MediaPlaybackJobStats::submit_block_calls,
       which is left exactly as it was. */
    size_t video_hold_job_slot;
    size_t video_hold_stage_copy;
    size_t video_hold_refusal;
    size_t video_hold_frame_ready;
    size_t video_hold_batch_pending;   /* spent emitting a pending picture */
    size_t video_hold_timestamps;
    size_t drain_hold_job_slot;
    size_t drain_hold_stage_copy;
    size_t drain_hold_refusal;
    size_t drain_hold_surface;
    /* Pictures converted at a surface release rather than at a pump visit. */
    size_t video_emits_release;
    /*
     * Ownership against presentation, which the counters above conflated.
     *
     * A claim is not a display. presented_video_us updates when the browser
     * thread takes a picture, "take -> present" ends when the source surface
     * is released rather than when anything reached the screen, and nothing
     * counted a claimed picture that was replaced before it was drawn. Every
     * claimed identity now ends in exactly one of displayed or dropped, and
     * the staged count sits between them so a staging failure is separable
     * from a display that never happened.
     */
    size_t video_claims;            /* identities handed to the browser */
    size_t video_claims_staged;     /* ...whose staging copy completed */
    size_t video_claims_displayed;  /* ...that reached the screen */
    size_t video_claims_dropped;    /* ...that did not reach the screen */
    size_t video_claims_quiesced;   /* ...retired by an intentional close */
    /*
     * What a refusal recovery costs in WALL time, which is not the media-time
     * skip and was previously read as if it were. Each interval is summed and
     * maxed over the recoveries in the session; the discontinuity is the media
     * time the stream jumped, reported apart from the outage so neither can be
     * mistaken for the other.
     */
    size_t video_recoveries;
    uint64_t recovery_reset_total_us;    /* refusal seen -> reset begins */
    uint64_t recovery_first_au_total_us; /* reset -> first accepted AU */
    uint64_t recovery_first_csc_total_us;
    uint64_t recovery_first_present_total_us;
    uint32_t recovery_first_present_max_us;
    uint64_t recovery_media_skip_total_us;
    /*
     * The two-slot ownership machine, appended and never reordered.
     *
     * These are the counters that say whether a second decoded-output surface
     * did what it was reserved to do, and -- far more importantly -- whether
     * it introduced the failure it could introduce. A pipeline that displays
     * the right metadata over the wrong pixels raises none of the counters
     * above, so it needs its own.
     *
     * stale_codec_jobs      completions from a stream a seek left behind,
     *                       discarded rather than credited.
     * surface_borrow_stale  reads refused because the slot no longer held the
     *                       picture the reader claimed. Zero in a healthy
     *                       session; nonzero means this check earned itself.
     * stage_mismatches      staging passes whose source tuple was not the
     *                       claimed one.
     * signature_mismatches  staged pixels that did not hash to what the
     *                       conversion left in the slot. Validation builds
     *                       only, and the one direct measurement of the
     *                       substitution hazard rather than a proxy for it.
     * video_slot_conversions/video_slot_claims, per slot, so an interleaving
     *                       that stopped alternating is visible as a number
     *                       rather than inferred from the rate.
     */
    size_t stale_codec_jobs;
    size_t surface_borrow_stale;
    size_t stage_mismatches;
    size_t signature_mismatches;
    size_t video_slot_conversions[MEDIA_BACKEND_MAX_SURFACE_SLOTS];
    size_t video_slot_claims[MEDIA_BACKEND_MAX_SURFACE_SLOTS];
    /*
     * The occupancy ladder, appended and never reordered.
     *
     * Everything above counts events, and the events now balance: conversions
     * (2519) barely exceed claims (2416) on a device that displays 20.1
     * pictures a second against a 24 fps stream, which says the worker
     * converts about once per claim instead of running ahead into the free
     * slot it was given. No counter above can say WHY, because a rate is a
     * question about where the time went and none of them measures time
     * spent -- only times something happened.
     *
     * These do. Each is a cumulative microsecond total or a duration triple
     * (count/total/max) over a live session, sampled at transitions that
     * already exist, and published on tilefinch-media-slots and
     * tilefinch-media-worker beside the cadence line. Every one of them is a
     * counter or a timestamp subtraction on a path that already runs; none is
     * behind a build flag, because the run that has to answer this is the
     * ordinary soak.
     */
    /* Per slot, cumulative dwell in FREE / ME_WRITING / READY / READING.
       Indexed by PspMediaSurfaceSlotState. The in-progress interval is added
       at publication, so the four sum to the session's wall time per slot. */
    uint64_t video_slot_dwell_us[MEDIA_BACKEND_MAX_SURFACE_SLOTS]
                                [MEDIA_BACKEND_SLOT_STATES];
    /* Wall time with no slot a conversion could target, which is the pipeline
       legitimately full and the submit gate legitimately shut. */
    uint64_t slot_no_free_us;
    /*
     * Wall time with a slot FREE and no conversion in flight: the direct
     * measure of the worker failing to run ahead. Every microsecond here is
     * one the Media Engine could have spent decoding into a slot nobody
     * owned, and did not. If the conversion cadence is the limiter, this is
     * the field that says so.
     */
    uint64_t slot_free_idle_us;
    /* READY depth sampled at every transition: unclaimed converted pictures
       waiting. A depth that never exceeds one is a pipeline running at exactly
       one conversion per claim. */
    size_t slot_ready_samples;
    size_t slot_ready_total;
    uint32_t slot_ready_max;
    /* A slot became free with none free before it -> the next accepted video
       access unit. The supply-side half of slot_free_idle_us: a long total
       here with a short free-idle says the pipeline is waiting for bytes, not
       for the worker. */
    size_t slot_free_to_au;
    uint64_t slot_free_to_au_total_us;
    uint32_t slot_free_to_au_max_us;
    /*
     * Whether the fill-all-free-slots conversion actually fires.
     *
     * batch_fill_jobs counts decode/drain jobs that returned at least one
     * picture; batch_fill_converted how many of those pictures the SAME job
     * converted before publishing; batch_fill_complete the jobs that converted
     * every picture they were given. batch_defer_free is the case the
     * condition exists to prevent: pictures left captured while a slot was
     * still FREE, which can only be a reader holding that slot against the
     * writer.
     */
    size_t batch_fill_jobs;
    size_t batch_fill_converted;
    size_t batch_fill_complete;
    size_t batch_defer_free;
    /* Conversion k -> conversion k+1 inside one batch. A gap far above the
       CSC duration is a surplus picture waiting for a slot rather than for
       the Media Engine. */
    size_t batch_gap_samples;
    uint64_t batch_gap_total_us;
    uint32_t batch_gap_max_us;
    /* sceMpegBaseCscAvc itself, which was previously inside the emit-pending
       interval and could not be separated from the wait in front of it. */
    size_t video_csc_calls;
    uint64_t video_csc_total_us;
    uint32_t video_csc_max_us;
    /* Where the browser thread stood when each conversion completed, by
       PspMediaLoopPhase: other / pump / present / vblank. */
    size_t video_csc_phase[MEDIA_BACKEND_LOOP_PHASES];
    /*
     * The worker's dispatch ladder, one sample per job.
     *
     * dispatch is the event flag posted -> the worker observing its job, which
     * is the scheduler latency of a priority-0x19 thread waiting on a
     * priority-0x20 thread's post -- "worker ready but not scheduled" in the
     * only form this target can measure cheaply. prologue is that wake -> the
     * first firmware call entered. firmware is the bounded call pair itself.
     * collect is the worker's release store -> the browser thread observing
     * it, which is a whole browser frame whenever the collection lands on the
     * wrong side of a pump.
     */
    size_t worker_dispatches;
    uint64_t worker_dispatch_total_us;
    uint32_t worker_dispatch_max_us;
    size_t worker_prologues;
    uint64_t worker_prologue_total_us;
    uint32_t worker_prologue_max_us;
    size_t worker_firmware_calls;
    uint64_t worker_firmware_total_us;
    uint32_t worker_firmware_max_us;
    size_t worker_collects;
    uint64_t worker_collect_total_us;
    uint32_t worker_collect_max_us;
    /*
     * One-entry cross-kind lookahead on the PSP codec worker. `prepared`
     * counts browser-thread publications, `chained` the ones the worker began
     * without waiting for a later browser visit, and `wait` is publication to
     * execution. A large prepared-minus-chained gap means the queue exists but
     * is not removing the completion bubble it was built for.
    */
    size_t worker_prepared_jobs;
    size_t worker_prepared_audio;
    size_t worker_chained_jobs;
    uint64_t worker_prepared_wait_total_us;
    uint32_t worker_prepared_wait_max_us;
    size_t worker_prepared_cancelled;
    /*
     * What owned the one shared job slot, by job kind, from queue to
     * collection. Three kinds compete for it and only one of them is video:
     * an AAC job holding it is a video submission refused for a reason
     * nothing about video can fix.
     */
    size_t job_video_count;
    uint64_t job_video_total_us;
    size_t job_drain_count;
    uint64_t job_drain_total_us;
    size_t job_audio_count;
    uint64_t job_audio_total_us;
    size_t job_other_count;
    uint64_t job_other_total_us;
    /* Audio job durations, as bucket counts. The percentiles are derived at
       print time; see psp_media_job_percentile_us. */
    uint32_t job_audio_buckets[MEDIA_BACKEND_JOB_HISTOGRAM_BUCKETS];
    uint32_t job_audio_max_us;
    /* video_hold_job_slot split by what was holding the slot. The aggregate
       it came from is left exactly as it is. */
    size_t hold_job_by_video;
    size_t hold_job_by_audio;
    size_t hold_job_by_drain;
    size_t hold_job_by_other;
    /* How far into the audio job each of those refusals landed, which is what
       turns a refusal count into an occupancy claim. */
    uint64_t hold_job_audio_age_total_us;
    uint32_t hold_job_audio_age_max_us;
    /*
     * The global take-vs-video-job gate. Retired: these are now always zero.
     *
     * They measured a picture whose time had come being refused because ANY
     * video or drain job was running -- including one writing the OTHER slot,
     * which cannot affect the picture being claimed at all. A rule that is
     * correct and necessary for one surface, inherited unchanged by two,
     * where it manufactured the same anti-phase the second slot was reserved
     * to remove. Across soaks it fired 342 to 405 times, about 13% of takes,
     * and in every sample the running job was inside the other slot.
     *
     * The gate is gone. What replaced it is per-slot release/acquire on the
     * slot's own state word, so a take no longer asks anything about the job
     * slot at all and there is no longer a shape for these to count. They are
     * kept rather than deleted because the runs that measured them are the
     * comparison: a soak on this code reads zero here and the same displayed
     * rate or better, and that pair is the evidence. video_take_busy carries
     * what is genuinely left, which is per-slot lease contention.
     */
    size_t take_blocked_other_slot;
    uint64_t take_blocked_other_slot_us;
    /*
     * Decoded-output slots declared unusable because a staging DMA could not
     * be joined, and the transfer had to be assumed still live.
     *
     * Zero on every device run so far. It is published because the path it
     * counts is the one that used to substitute pixels silently, and a number
     * that stays zero is the evidence that it is still never taken.
     */
    size_t surface_quarantines;
    /*
     * Bounded AAC batching, appended.
     *
     * The measurement that produced it: audio owned 27.9s of the one shared
     * job slot against video's 24.5s, over 3,174 jobs of which the firmware
     * call was about 1.1ms in an 8.8ms job -- and it refused video 1,474
     * times to video's own 314 while still delivering 26 blocks a second
     * where 43 are needed. Two units per job halves the round trips without
     * touching the firmware work, so these fields have to separate the two
     * effects rather than report one number that could be either.
     *
     * batch_jobs   jobs that decoded more than one unit.
     * batch_aus    units decoded, over all audio jobs. batch_aus minus the
     *              audio job count is how many round trips were saved.
     * deferrals    units held for a partner. A deferral that did not become a
     *              batch is one the flush had to send alone.
     * flushes      batches sent by the end-of-pump flush rather than by a
     *              partner arriving -- the deferral's cost, and the number to
     *              watch if audio latency moves the wrong way.
     * budget_stops jobs that stopped after one unit on their own time budget.
     *              Expected rare; a large value means the firmware call is
     *              slower than the batching assumed.
     * blocked      extensions declined because video could have used the slot
     *              (video) or the PCM queue had no room (queue).
     */
    size_t audio_batch_jobs;
    size_t audio_batch_aus;
    size_t audio_batch_deferrals;
    size_t audio_batch_flushes;
    size_t audio_batch_budget_stops;
    size_t audio_batch_blocked_video;
    size_t audio_batch_blocked_queue;
    size_t audio_pending_enqueued;
    size_t audio_pending_promoted;
    size_t audio_pending_full;
    size_t audio_pending_peak;
    size_t audio_pending_holds;
    size_t audio_pending_timeouts;
} MediaBackendStats;

/*
 * A compatibility environment may expose the private raw-NAL entry point as
 * a successful no-op. Keep a bounded packet-copy probe distinct from a real
 * firmware decoder error when it never produces a complete authored surface.
 */
#define MEDIA_BACKEND_RAW_NAL_BRIDGE_UNAVAILABLE (-1413873665)

typedef struct {
    void *opaque;
    MediaBackendResult (*submit)(
        void *opaque, const MediaMp4Sample *sample,
        const unsigned char *payload, size_t length,
        char *error, size_t error_size);
    /*
     * Optional end-of-stream drain. It is called at most once per bounded
     * playback advance after every demux source reaches EOF. WOULD_BLOCK
     * retains the playback job so a presented output surface can be released;
     * END commits EOF. ACCEPTED means bounded progress and yields before a
     * later drain call.
     */
    MediaBackendResult (*drain)(
        void *opaque, char *error, size_t error_size);
    bool (*advance)(void *opaque, uint64_t clock_us,
                    char *error, size_t error_size);
    /*
     * Returns a borrowed surface valid until the next submit/drain/reset or
     * backend destruction. The caller must synchronously copy or present it
     * before advancing decode again; ownership is not transferred.
     */
    bool (*take_video_frame)(void *opaque, MediaVideoFrame *frame);
    /* Optional pre-claim seek catch-up. Discard READY pictures strictly
       before `floor_us` without lending their surfaces to the frontend. This
       keeps random-access prerequisite pictures internal instead of briefly
       presenting the keyframe before the user's requested position. */
    size_t (*discard_video_before)(void *opaque, uint64_t floor_us);
    bool (*stats)(const void *opaque, MediaBackendStats *stats);
    /* Optional lock-free cursor for the PCM actually accepted by the output
     * device. Unlike the broad stats snapshot, this remains readable while a
     * codec job is running and can discipline the presentation clock. */
    bool (*audio_cursor_us)(const void *opaque, uint64_t *cursor_us);
    /* Optional lock-free count of decoded pictures waiting to be claimed.
       Startup A/V priming uses this to establish the backend's complete
       bounded lead before releasing audio. */
    unsigned (*ready_video_frames)(const void *opaque);
    /* Optional earliest ready decoded-picture timestamp. Startup may present
       that still before sound begins even when an edit list makes its PTS
       slightly later than zero. */
    bool (*ready_video_start_us)(const void *opaque, uint64_t *start_us);
    /* Optional lock-free count at the final display funnel. Startup audio is
       released only after the first primed picture has reached the LCD, not
       merely after the browser has borrowed or staged it. */
    size_t (*displayed_video_frames)(const void *opaque);
    /* Optional public presentation-clock update. Decode may run against a
       private preroll horizon; take/drop eligibility must use the clock the
       user can actually hear and see. */
    void (*set_presentation_clock_us)(void *opaque, uint64_t clock_us);
    void (*set_playing)(void *opaque, bool playing);
    /* Optional presentation-only pause for network rebuffering. Submission
       and decode remain active; the audio sink stops consuming queued PCM. */
    void (*set_buffering)(void *opaque, bool buffering);
    /* Optional decoder flush used by media_playback_seek(). */
    bool (*reset)(void *opaque, char *error, size_t error_size);
    void (*destroy)(void *opaque);
    /*
     * Optional. Publish a picture the last decode returned but the output
     * surface had no room for, at a moment the caller knows the surface is
     * free -- which for the PSP backend is the instant it releases its read
     * claim. Backends that always emit everything they decode leave it NULL.
     *
     * It exists because a decode can return several pictures and a single
     * surface can hold one, so the surplus waits for a caller visit that is
     * allowed to convert it. Those visits are the same submit and drain calls
     * the surface's own read window holds off, so the picture waited for a
     * window it was itself blocking. Returns true when it converted one.
     */
    bool (*emit_pending_video)(void *opaque, char *error, size_t error_size);
    /*
     * Optional. The caller has finished reading a decoded-output slot for
     * good, not merely for now, and the backend may hand it back to its
     * writer.
     *
     * The distinction is the whole of it, and it is a property of the PATH
     * rather than of the moment. When a picture is staged into a texture the
     * frontend redraws from, the staging copy is the last read the slot ever
     * gets: every later present of that picture samples the staged copy, so
     * the slot is finished with at the join and holding it any longer costs a
     * conversion. When the picture is drawn straight out of the slot -- the
     * unstaged graphics-engine draw and the software scaler -- the frontend
     * reads it again on every redraw, and the slot must stay the claimant's
     * until a later claim supersedes it.
     *
     * So this is called by the staged path and only by it. The generation
     * says which picture the caller read; a release naming one the slot has
     * moved past is a late call and is refused. Returns true when the slot
     * became free.
     */
    bool (*release_video_slot)(
        void *opaque, unsigned slot, uint32_t generation);
    /*
     * Optional. The backend is holding a staged access unit of this track
     * that it would decode together with one more, and the pump may offer a
     * second one inside the same visit.
     *
     * It exists because the bound that made batching possible is not the
     * bound that fills it. A backend that batches two access units per job
     * can only ever see a pair if the pump hands it two before the job goes
     * out, and the pump's per-visit packet budget is shared with video: a
     * visit that spends its first packet on a video unit has nothing left for
     * the partner, so the staged unit is flushed alone. A device soak
     * measured exactly that -- 1.32 blocks per job against a ceiling of 2,
     * with audio delivery pinned at 26 blocks a second either way.
     *
     * So a backend that answers true is granted a small, capped extension of
     * that budget -- and nothing else. It does not change what is selected:
     * the pump still picks the earliest access unit across sources, so a
     * video unit that is due first is still submitted first. It does not
     * fetch: an unbuffered partner ends the visit through the same
     * would-block path any other unbuffered sample takes. And it is asked
     * again rather than remembered, so a backend whose answer depends on
     * whether video now wants the decoder can change its mind between the
     * first unit and the second.
     *
     * A backend that leaves this NULL gets exactly the behaviour it had.
     */
    bool (*wants_paired_submit)(const void *opaque, int track_kind);
} MediaBackend;

/*
 * The hard cap on that extension, per pump visit, independent of what any
 * backend claims to want. A backend that answered true forever would
 * otherwise turn a bounded visit into an unbounded one -- and the whole
 * contract of the bounded pump is that the browser thread comes back.
 */
#define MEDIA_PLAYBACK_MAXIMUM_PAIRED_SUBMITS 3u

typedef struct {
    uint64_t decode_lead_us;
    /*
     * A seek still feeds video from its preceding keyframe, but AAC packets
     * before this presentation time are independent and must not reach the
     * audio output. Zero preserves the ordinary initial-playback schedule.
     */
    uint64_t audio_start_us;
    size_t maximum_packet_bytes;
    /*
     * Lazy fragmented demuxers discover later sample sizes window by window.
     * Device backends can reserve the admitted ceiling up front so a large
     * later keyframe cannot turn heap fragmentation into a mid-play failure.
     */
    bool preallocate_maximum_packet_bytes;
} MediaPlaybackOptions;

typedef struct MediaPlayback MediaPlayback;

typedef enum {
    MEDIA_PLAYBACK_ADVANCE_CANCELLED = -2,
    MEDIA_PLAYBACK_ADVANCE_ERROR = -1,
    MEDIA_PLAYBACK_ADVANCE_COMPLETE = 0,
    MEDIA_PLAYBACK_ADVANCE_PENDING = 1
} MediaPlaybackAdvanceResult;

typedef struct {
    size_t calls;
    size_t yielded_calls;
    size_t packets_submitted;
    size_t would_block_calls;
    size_t drain_calls;
    /*
     * Why a call stopped. A starved pipeline reports thousands of calls
     * against hundreds of packets, and calls/yielded/would-block alone cannot
     * say whether the missing units were held by a busy decoder, refused by
     * the decode horizon, or never asked for because playback was paused.
     * Each counter below is one exit from the bounded loop, so they sum to
     * the calls that entered it.
     */
    size_t idle_calls;          /* paused or ended: nothing to pump */
    size_t submit_block_calls;  /* the decoder still owns its staging */
    size_t drain_block_calls;   /* the decoder still owns its surface */
    size_t horizon_breaks;      /* decoded far enough past the clock */
    size_t packet_limit_breaks; /* hit the caller's per-call packet bound */
    size_t queued_breaks;       /* an asynchronous unit took ownership */
    size_t source_ended_breaks; /* every demux source is drained */
    size_t source_block_calls;  /* the source has not buffered those bytes */
    /*
     * The same two exits, restricted to video, and appended so no positional
     * reader moves.
     *
     * Video and audio share every counter above, and they do not share a
     * problem: audio is a 24 KiB/s trickle that is almost never the source
     * that blocks, while a video window landing late stops the decoder
     * outright. A supply-starved pipeline and a conversion-starved one look
     * identical in source_block_calls and separate here.
     */
    size_t source_block_video;  /* video's bytes were not buffered */
    size_t horizon_break_video; /* video decoded far enough past the clock */
    /* Extensions of the per-visit packet budget granted so a backend holding
       a staged access unit could be offered its partner in the same visit.
       Bounded by MEDIA_PLAYBACK_MAXIMUM_PAIRED_SUBMITS per visit. */
    size_t paired_submits;
    /*
     * Head-of-line blocking, measured and not yet acted on.
     *
     * The scheduler picks the earliest-PTS sample across sources and, when
     * that one cannot go in -- its payload is not buffered, or the backend
     * refuses it -- ends the visit. It never offers the other source. If the
     * usual shape is a video access unit at the head with an audio one five
     * milliseconds behind it, buffered and due, then audio admission is
     * starved by a rule about ordering rather than by supply or by the
     * decoder, and the fix is in the scheduler.
     *
     * That is a hypothesis about a counterfactual, so these count the
     * counterfactual directly: at each such block, what the OTHER source was
     * holding at that instant. They narrow, so they read as a funnel --
     * head_blocks >= alt_pending >= alt_in_horizon >= alt_resident -- and the
     * last is the number that matters: a sample that existed, was due, and
     * whose bytes were already in memory, that the visit went home without
     * offering.
     *
     * It is an upper bound on "would have succeeded", not a proof of it: the
     * backend might have refused the alternate too. Asking it would mean a
     * speculative submit, which is a scheduling change and not a measurement.
     * Split by the blocked track's kind so the hypothesis above is read
     * directly rather than inferred from an aggregate.
     *
     * Adaptive streams only: a progressive MP4 carries both tracks in one
     * source, where there is no other source to have offered.
     */
    size_t head_blocks;          /* blocks with a selected sample in hand */
    size_t head_block_video;     /* ...where the blocked head was video */
    size_t head_block_audio;     /* ...where it was audio */
    size_t head_alt_pending;     /* the other source had a sample peeked */
    size_t head_alt_in_horizon;  /* ...and it was inside the decode horizon */
    size_t head_alt_resident;    /* ...and its payload was already buffered */
    /* Summed (alternate PTS - blocked head PTS) only when the alternate is
       level with or later than the selected head. A split seek can leave the
       alternate temporarily earlier; count that separately rather than
       underflowing this unsigned diagnostic. */
    uint64_t head_alt_lead_total_us;
    size_t head_alt_lead_samples;
    size_t head_alt_behind;
    /*
     * Action taken after the proof funnel above showed that the alternate was
     * safe to offer without fetching. A bypass is one bounded pump-call
     * decision: the blocked source remains pending, and only the other split
     * source may advance for the rest of that call. `submitted` counts units
     * the backend actually accepted from that source; `blocked` means it
     * refused the alternate too.
     */
    size_t head_alt_bypasses;
    size_t head_alt_submitted;
    size_t head_alt_blocked;
} MediaPlaybackJobStats;

/*
 * Portable orchestration shared by the host probes and PSP backend. It owns
 * neither demux nor backend, but owns one bounded packet buffer so range reads
 * never require retaining the MP4 or an unbounded packet queue.
 */
MediaPlayback *media_playback_create(
    Budget *budget, MediaMp4Demux *demux, const MediaBackend *backend,
    const MediaPlaybackOptions *options, char *error, size_t error_size);
/*
 * Two-source form for an adaptive MP4 video stream plus an adaptive MP4
 * audio stream. Samples are merged by normalized decode timestamp while
 * retaining the same one-packet-at-a-time memory bound.
 */
MediaPlayback *media_playback_create_split(
    Budget *budget, MediaMp4Demux *video_demux,
    MediaMp4Demux *audio_demux, const MediaBackend *backend,
    const MediaPlaybackOptions *options, char *error, size_t error_size);
bool media_playback_advance(MediaPlayback *playback, uint64_t clock_us,
                            char *error, size_t error_size);
MediaPlaybackAdvanceResult media_playback_advance_bounded(
    MediaPlayback *playback, uint64_t clock_us, size_t maximum_packets,
    char *error, size_t error_size);
/*
 * A decoder submission already in native code finishes before cancellation
 * is observed. If it accepted the packet, bookkeeping remains consumed and
 * the function returns CANCELLED before another packet or advance callback.
 */
MediaPlaybackAdvanceResult media_playback_advance_bounded_cancelable(
    MediaPlayback *playback, uint64_t clock_us, size_t maximum_packets,
    const TilefinchCancellation *cancellation,
    char *error, size_t error_size);
void media_playback_job_stats(const MediaPlayback *playback,
                              MediaPlaybackJobStats *stats);
bool media_playback_backend_stats(const MediaPlayback *playback,
                                  MediaBackendStats *stats);
bool media_playback_audio_cursor_us(const MediaPlayback *playback,
                                    uint64_t *cursor_us);
unsigned media_playback_ready_video_frames(const MediaPlayback *playback);
bool media_playback_ready_video_start_us(
    const MediaPlayback *playback, uint64_t *start_us);
size_t media_playback_displayed_video_frames(const MediaPlayback *playback);
void media_playback_set_presentation_clock_us(
    MediaPlayback *playback, uint64_t clock_us);
/* Track presence is stable across seek/reset even while the output cursor is
   temporarily unavailable. Clock policy must not mistake that short reset
   interval for a video-only stream. */
bool media_playback_has_audio(const MediaPlayback *playback);
/* Hold the independent adaptive-audio source during video seek preroll. The
   split-source requirement is deliberate: a progressive file cannot retain
   one interleaved audio head while walking video with a one-sample cursor.
   Returns false when a requested hold cannot be represented. */
bool media_playback_set_audio_submission_blocked(
    MediaPlayback *playback, bool blocked);
bool media_playback_take_video_frame(MediaPlayback *playback,
                                     MediaVideoFrame *frame);
size_t media_playback_discard_video_before(
    MediaPlayback *playback, uint64_t floor_us);
/*
 * Hand a decoded-output slot back to its writer, for a reader that is
 * finished with the picture rather than merely finished for this frame.
 * See BackendVTable::release_video_slot for why only the staged path may.
 */
bool media_playback_release_video_slot(
    MediaPlayback *playback, unsigned slot, uint32_t generation);
/*
 * Offer the backend the moment its output surface became free. Backends with
 * no surplus picture to publish, and those with no such concept at all,
 * answer false and cost nothing. Never an error path: a picture that cannot
 * be converted now stays captured for the visit that could already retire it.
 */
bool media_playback_emit_pending_video(MediaPlayback *playback);
bool media_playback_ended(const MediaPlayback *playback);
uint64_t media_playback_buffered_until_us(const MediaPlayback *playback);
/*
 * Establish the video source's connection and cache its first window at
 * `target_us`, blocking (bounded by the range's wait budget) so the handshake
 * is paid here rather than on the first playing frame.
 *
 * A device cycle measured a 497ms TLS handshake landing on the first playing
 * frame after a long think-time closed the connection the open had warmed;
 * audio did not, because its window was already in flight. This warms video
 * the same way. Call after a seek has positioned the demuxer; it advances the
 * video cursor to read the first sample, then re-seeks to `target_us` so the
 * decode that follows starts where it should, with the connection warm and
 * the window cached. Best effort: a would-block or an oversized sample simply
 * leaves the connection to be made on the playing path as before.
 */
bool media_playback_warm_video(MediaPlayback *playback, uint64_t target_us,
                               char *error, size_t error_size);
/*
 * The same for the adaptive audio source, at the position the seek left it.
 *
 * A seek that warmed only video left the audio window at a far offset to be
 * fetched on a connection whose burst allowance was spent: a device soak
 * measured it moving 16 KiB in six seconds, which held video back to keep the
 * interleave and ended the session. Audio is a tenth of video's bitrate and
 * its window is the one playback needs first. False for a progressive MP4,
 * which has no second source and whose audio was warmed with the video.
 */
bool media_playback_warm_audio(MediaPlayback *playback, uint64_t target_us,
                               char *error, size_t error_size);
bool media_playback_seek(MediaPlayback *playback, uint64_t target_us,
                         uint64_t *actual_us, char *error,
                         size_t error_size);
/* Decoder-resetting recovery seek to the first actual video keyframe after
 * target_us. This is intentionally distinct from an ordinary scrub seek,
 * which rounds to the keyframe at or before its target. */
bool media_playback_seek_after(MediaPlayback *playback, uint64_t target_us,
                               uint64_t *actual_us, char *error,
                               size_t error_size);
size_t media_playback_packet_bytes(const MediaPlayback *playback);
void media_playback_set_playing(MediaPlayback *playback, bool playing);
void media_playback_set_buffering(MediaPlayback *playback, bool buffering);
void media_playback_destroy(MediaPlayback *playback);

#ifdef __PSP__
typedef enum {
    MEDIA_PSP_PREPARE_PENDING = 0,
    MEDIA_PSP_PREPARE_READY,
    MEDIA_PSP_PREPARE_ERROR
} MediaPspPrepareResult;

/*
 * PSP-2000/3000 backend using the system AVC and AAC codecs. The returned
 * backend is consumed by media_playback_create and reports every native error
 * through MediaBackendStats for the eventual one-copy hardware validation.
 */
/*
 * Reserve the single block every Media Engine buffer is later carved out of.
 *
 * The PSP-2000/3000 extra 32 MiB RAM bank at 0x0A000000 and above is
 * addressable only by the main CPU: the Media Engine, which executes the AVC
 * and AAC programs and fetches their buffers by DMA, cannot see it. With the
 * extra-memory unlock active the browser's heap grows well past that line, so
 * a media buffer allocated when a video opens can land somewhere firmware is
 * physically unable to read.
 *
 * Call once, early in boot, while the heap cursor is still low. The
 * reservation is never released; media opens and closes reuse it. It is taken
 * only where the partition is large enough to spare it for the whole process,
 * which is also the only case where it is needed: a stock partition lies
 * entirely below the limit. Failing to take it is not a boot failure -- the
 * backend falls back to ordinary heap allocation and reports the resulting
 * addresses either way.
 */
void media_psp_backend_reserve_pool(void);
MediaPspPrepareResult media_psp_backend_prepare_pump(
    char *error, size_t error_size);
bool media_psp_backend_create(
    Budget *budget, const MediaMp4Demux *demux,
    MediaBackend *backend, char *error, size_t error_size);
bool media_psp_backend_create_split(
    Budget *budget, const MediaMp4Demux *video_demux,
    const MediaMp4Demux *audio_demux,
    MediaBackend *backend, char *error, size_t error_size);
/*
 * A backend whose worker or firmware audio channel could not prove that it
 * released borrowed queue memory is intentionally leaked rather than freed.
 * Refuse subsequent decoder allocation for the rest of this process so a
 * repeated retry cannot turn that one safe quarantine into an OOM cascade.
 */
bool media_psp_backend_quarantined(void);
/*
 * True once the wide Main-profile decoder program (create mode 5 / Media
 * Engine boot type 1) has been rejected at its first access unit on this
 * device. The failing video still recovers through the ordinary retry ladder;
 * later opens consult this so they request a stream the proven program can
 * decode instead of repeating the create/prime round trip. Process-lifetime
 * and memory-only: a restart re-tests the wide program.
 */
bool media_psp_backend_wide_program_rejected(void);
/*
 * The decoder-program override, published once from the historically named
 * `experimental_wide_video=` boot key before the first video opens. An absent
 * value selects the hardware-qualified "wide" default. Explicit "off" keeps
 * the 240p compatibility program; "wide-annexb" and "boot4" remain diagnostic
 * alternatives. Unknown persisted values fail the boot-config gate, while a
 * direct unknown call fails closed to off. The returned value is a
 * PspMediaWideProgram from src/media_backend_psp_policy.h.
 */
void media_psp_backend_set_wide_program(const char *name);
int media_psp_backend_wide_program(void);
/*
 * Where in the frame the browser's next advance runs, as a PspMediaAdvanceMode
 * from src/media_backend_psp_policy.h. It decides two things the backend
 * cannot see for itself: whether blocking for the codec worker is free (it is
 * inside a present's dead time, it is frame time anywhere else), and whether
 * the decoded surface is being read by something that a newly emitted picture
 * would overwrite. Bracket a pump with it; leave it at
 * PSP_MEDIA_ADVANCE_FRAME everywhere else.
 */
void media_psp_backend_set_advance_mode(int mode);
/*
 * Where in the frame the browser thread is standing, as a PspMediaLoopPhase
 * from src/media_backend_psp_policy.h. Telemetry only: nothing in the decoder
 * reads it to decide anything, and a session that never calls it loses one
 * field on tilefinch-media-slots and nothing else.
 *
 * It answers a question the conversion counts cannot. A picture converted
 * while the thread is blocked on the vertical blank is collected by the very
 * next take; the same picture converted a moment later, during the pump, waits
 * a whole frame for the take after that. Both are one conversion. Only the
 * phase separates them.
 *
 * The present window is NOT published through this -- it is already announced
 * as an advance mode, and psp_media_loop_phase_resolve derives it from there,
 * so there is no second bracket to keep in step with the first.
 */
void media_psp_backend_set_loop_phase(int phase);
/*
 * Record every video access unit submitted from a decoder reset onward, so a
 * firmware call that never returns leaves the byte sequence that produced it
 * on the card. Validation builds only and a no-op elsewhere; bounded, written
 * once at teardown, and truncated per run.
 */
void media_psp_backend_set_au_dump(int enabled);
/*
 * Choose what a reposition does to the sceMpeg object: 0 flushes and reprimes
 * it where it stands, 1 deletes it and builds a new one out of the same
 * buffers, 2 does not touch it at all. Every wedge four device runs have
 * produced landed four to six seconds after an in-place reset, and PMPlayer
 * Advance -- the hardware-proven raw-NAL player this bridge follows -- ships
 * mode 2: zero sceMpeg calls on a seek. Values outside the range are ignored.
 * Publish once from boot configuration, before a video can open.
 */
void media_psp_backend_set_reset_mode(int mode);
void media_psp_backend_set_refusal_recovery(bool enabled);
/*
 * Report that the browser thread has looked at a refused access unit, releasing
 * the hold the codec worker placed on video submissions when it published the
 * refusal. Call once per advance, at the point the session has decided what to
 * do about it -- including deciding to do nothing, which is why this is not
 * conditional on any recovery being configured. Idempotent.
 *
 * The hold exists because two device runs wedged the Media Engine by submitting
 * one or two more access units in the window between a refusal being published
 * and the browser thread processing it.
 */
void media_psp_backend_release_refusal_hold(void);
/*
 * Declare that a decoded-output slot is held by a reader nobody can join, and
 * later that the reader has been observed to finish.
 *
 * The one reader this describes is a staging DMA whose join timed out: it was
 * abandoned, not stopped, so its borrow lease is gone and a controller may
 * still be sourcing from the slot. While the quarantine stands the slot is
 * never returned to the Media Engine -- a superseding claim leaves it alone,
 * the staged release refuses, and the reset/seek reader quiesce fails rather
 * than freeing it -- so a conversion over a live transfer cannot be built.
 *
 * The release only withdraws that refusal. Handing the slot back is still the
 * ordinary release path's job, and it must be called after this.
 */
void media_psp_backend_quarantine_surface(unsigned slot);
void media_psp_backend_release_surface_quarantine(unsigned slot);
bool media_psp_backend_surface_quarantined(unsigned slot);
/*
 * What is left of the calling pump's window, so a collect wait cannot outlast
 * the dead time that justified it. Zero restores the wait's own bound.
 */
void media_psp_backend_set_wait_limit_us(unsigned wait_limit_us);
/*
 * Claim one decoded-output slot for reading, and release it.
 *
 * The advance mode fences the writes this thread orders; this fences the one
 * it does not. A decode accepted before a read window opens converts its
 * picture into a surface from the codec worker, whenever firmware finishes
 * -- possibly in the middle of the stage copy, or of a draw that samples the
 * surface directly. Take the claim before the read begins and drop it when
 * the reader is done with the rows.
 *
 * The slot names which of the decoded-output surfaces is being read, and the
 * generation which picture the caller believes is in it. A claim whose
 * generation no longer matches is REFUSED and returns false with no lease
 * taken: the picture the caller meant is gone, and reading the address anyway
 * would draw its successor under the claimed identity.
 *
 * So false has two meanings and the caller must not conflate them. A refused
 * claim means the picture is gone: present nothing new. A claim that was
 * granted but timed out waiting for a conversion already inside firmware also
 * returns false, and there the claim stands and the read should proceed --
 * presenting nothing is worse, but the frame may have torn. Callers that
 * cannot tell them apart should treat false as "do not draw", which is safe
 * in both cases and costs at most a frame.
 *
 * Releasing ends the lease and does NOT make the slot writable again: the
 * frontend redraws the picture it last claimed whenever no newer one is due,
 * so the slot stays the claimant's until a later claim supersedes it. Nesting
 * is not supported and is not needed -- one present, one claim.
 */
bool media_psp_backend_borrow_surface(unsigned slot, uint32_t generation);
void media_psp_backend_release_surface(unsigned slot);
/*
 * The same lease, ended without the presentation accounting.
 *
 * release_surface closes a present: it consumes the arm the claim set and
 * records how long the picture took to reach the screen. A reader that is not
 * the presenter -- the seek-preview thumbnail copy -- must not consume it, or
 * it reports a present that never happened and steals the real one's timing.
 * Same fencing, different event.
 */
void media_psp_backend_end_surface_read(unsigned slot);
/*
 * Ownership milestones the browser thread reaches outside the backend, so a
 * picture's record can be closed where it actually happens rather than
 * inferred from the last backend call before it. The whole frame is passed
 * rather than its identity alone: the funnel's invariant is stream equality of
 * {epoch, slot, generation, identity, PTS}, and a milestone carrying only one
 * of those five cannot be checked against it. A milestone that arrives for a
 * picture the decoder has already replaced is recognised as late and dropped
 * rather than credited to its successor.
 *
 * present_path names how it reached the screen: 1 graphics engine from a
 * retained whole-picture staged copy, 2 graphics engine sampling the decoder
 * surface, 3 software scaler, 4 graphics engine from transient wide strips.
 * Process-global for the same reason the surface handshake is -- one session
 * decodes at a time -- and no-ops in every build without the trace.
 */
#define MEDIA_PSP_PRESENT_PATH_GE_STAGED 1
#define MEDIA_PSP_PRESENT_PATH_GE_DIRECT 2
#define MEDIA_PSP_PRESENT_PATH_SOFTWARE 3
#define MEDIA_PSP_PRESENT_PATH_GE_STRIPS 4
void media_psp_backend_note_frame_staged(const MediaVideoFrame *frame);
void media_psp_backend_note_frame_displayed(
    const MediaVideoFrame *frame, int present_path);
void media_psp_backend_note_frame_quiesced(const MediaVideoFrame *frame);
/*
 * The pixels that will actually be drawn, hashed at the fixed sample points
 * the conversion hashed, and compared against what the conversion left in the
 * slot.
 *
 * The one check in the pipeline that is about the picture rather than about
 * the bookkeeping around it. Everything else -- identities, generations,
 * slots, counts -- passes unchanged for a staged copy that read the wrong
 * surface, which is the failure two slots make possible and one did not. A
 * zero signature means the caller could not sample and is ignored.
 *
 * Costs twelve reads and folds away entirely outside validation builds.
 */
void media_psp_backend_note_stage_signature(
    const MediaVideoFrame *frame, uint32_t signature);
/* Print the correlated per-picture ring, newest complete records first. */
void media_psp_backend_dump_picture_trace(const char *phase);
/*
 * A PSP suspend can reset Media Engine state without changing process-local
 * statics. Call after the last backend is destroyed and before power-down so
 * resume never skips the required sceMpegInit/profile selection boundary.
 */
void media_psp_backend_system_suspend(void);
#ifdef TILEFINCH_PSP_VALIDATION_LOG
/*
 * Re-run the firmware colour conversion over the picture the backend most
 * recently converted, with the two undocumented mode words set to a caller's
 * candidate, and report the first `head_count` surface words it wrote.
 *
 * The byte order of that surface is the one free variable in the video present
 * path: the software scaler reads byte 0 as the panel's high 5-bit field and
 * the graphics engine's texture unit reads it as the low one, and no PSP pixel
 * format can reconcile them (measured both ways --
 * tilefinch-media-present-probe: channel-map). Only sceMpegBaseCscAvc decides
 * which component each surface byte holds, and nothing readable here says what
 * a nonzero mode word selects. This exists so a device can answer that instead
 * of the backend guessing inside a call that writes half a megabyte of Media
 * Engine memory.
 *
 * Validation-only, and never reached from a playback session: the surface is
 * pool-owned and is rewritten in place, so a candidate that succeeds leaves a
 * picture the caller must not present. Returns the firmware status; negative
 * means the candidate was refused and nothing was read back.
 */
int media_psp_backend_csc_order_probe(
    void *opaque, int mode0, int mode1,
    uint32_t *head, size_t head_count);
#endif
#endif

#endif
