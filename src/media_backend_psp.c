#ifdef __PSP__

#include "tilefinch/media_backend.h"
#include "tilefinch/platform.h"
#include "tilefinch/psp_log.h"
#include "tilefinch/psp_threads.h"

#include <malloc.h>
#include <pspaudio.h>
#include <pspaudiocodec.h>
#include <pspkernel.h>
#include <pspmpeg.h>
#include <pspmpegbase.h>
#include <pspmodulemgr.h>
#include <psputility_modules.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "media_backend_psp_policy.h"
#include "media_backend_psp_pool.h"
#include "media_h264_psp_compat.h"
#include "psp_thread_contract.h"
#include "psp_media_ownership.h"
#include "psp_utility_module_contract.h"
#include "psp_systemctrl_stubs.h"

#define PSP_MEDIA_AUDIO_EVENT_READY 0x1u
#define PSP_MEDIA_AUDIO_EVENT_STOP 0x2u
#define PSP_MEDIA_CODEC_EVENT_READY 0x1u
#define PSP_MEDIA_CODEC_EVENT_STOP 0x2u
#define PSP_MEDIA_AUDIO_WORKER_WAIT 1
#define PSP_MEDIA_AUDIO_WORKER_OUTPUT 2
#define PSP_MEDIA_AUDIO_WORKER_THREAD 3
/* PSP_MEDIA_AUDIO_CODEC_WORDS/_BYTES, PSP_MEDIA_AUDIO_PCM_BYTES,
   PSP_MEDIA_AUDIO_QUEUE_BYTES and PSP_MEDIA_VIDEO_AU_BYTES live in
   media_backend_psp_pool.h: the boot-time Media Engine pool is sized from
   them, and that sizing has to be host-testable. */
#define PSP_MEDIA_AUDIO_STOP_WAIT_US 250000u
#define PSP_MEDIA_AUDIO_RELEASE_WAIT_US 100000u
/* No firmware output call returns this, so it cannot be confused with a real
   verdict: sceAudioOutputBlocking answers with a sample count or a negative
   error code. */
#define PSP_MEDIA_AUDIO_OUTPUT_PENDING INT32_MIN
#define PSP_MEDIA_CODEC_QUIESCE_WAIT_US 250000u
/*
 * A completed job used to be observed only when the browser next pumped -- one
 * whole main-loop iteration later. Every device codec-job-complete line
 * reported the same ~15.4 ms whatever the job was (AAC decode, AVC decode,
 * colour conversion), which was that pump period and not a firmware cost.
 * Spend a bounded slice sleeping instead, so one browser frame can carry more
 * than one access unit.
 *
 * The worker now outranks this thread (see psp_threads.h), so
 * this is no longer the only CPU the worker gets and no longer a yield it
 * depends on. What it still buys is the look: a job that finishes during the
 * sleep is collected inside this pump unit rather than at the next one, and
 * the unit after it is the one that submits.
 *
 * And no longer than that look needs to be. Four milliseconds was one wait,
 * sized to outlast a whole job. Inside a dead-time window it became the
 * window: the device measured a 4.45ms draw pump completing 0.6 submissions a
 * frame, because a single unit's wait consumed everything the engine's draw
 * had to give. What the pump wants is not a longer sleep but more of them --
 * sleep, look, submit, sleep -- and one sleep of the kernel's own granularity
 * is the smallest useful one there is. The caller's remaining slice bounds it
 * further.
 */
#define PSP_MEDIA_CODEC_COLLECT_WAIT_US 1500u
#define PSP_MEDIA_AUDIO_CHANNEL_DRAIN_WAIT_US 160000u
#define PSP_MEDIA_AUDIO_CHANNEL_POLL_US 2000u
#define PSP_MEDIA_AUDIO_SRC_QUEUE_BUSY UINT32_C(0x80268002)
/* How much of each firmware-owned control structure a post-failure dump
   records. Enough to see whether firmware wrote anything at all, short
   enough that the line stays inside one log record. */
#define PSP_MEDIA_AU_DUMP_BYTES ((size_t) 24u)
#define PSP_MEDIA_AU_HEAD_DUMP_BYTES ((size_t) 32u)
#define PSP_MEDIA_CODEC_DUMP_WORDS ((size_t) 12u)

typedef struct {
    void *sps_buffer;
    int sps_size;
    void *pps_buffer;
    int pps_size;
    int nal_prefix_size;
    void *nal_buffer;
    int nal_size;
    int mode;
} PspAvcNal;

typedef struct {
    SceInt32 unknown0;
    SceInt32 unknown1;
    SceInt32 width;
    SceInt32 height;
    SceInt32 unknown4;
    SceInt32 unknown5;
    SceInt32 unknown6;
    SceInt32 unknown7;
    SceInt32 unknown8;
    SceInt32 unknown9;
} PspAvcInfo;

typedef struct {
    void *buffer[8];
    SceInt32 unknown[3];
} PspAvcYuv;

typedef struct {
    SceInt32 unknown0[4];
    PspAvcInfo *info_buffer;
    SceInt32 unknown1[6];
    PspAvcYuv *yuv_buffer;
    SceInt32 unknown2[12];
} PspAvcDetail2;

typedef struct {
    SceInt32 height;
    SceInt32 width;
    SceInt32 mode0;
    SceInt32 mode1;
    void *buffer[8];
} PspAvcCsc;

typedef enum {
    PSP_MEDIA_CODEC_JOB_IDLE = 0,
    PSP_MEDIA_CODEC_JOB_RUNNING = 1,
    PSP_MEDIA_CODEC_JOB_DONE = 2
} PspMediaCodecJobState;

typedef enum {
    PSP_MEDIA_CODEC_KIND_NONE = 0,
    PSP_MEDIA_CODEC_KIND_VIDEO = 1,
    PSP_MEDIA_CODEC_KIND_AUDIO = 2,
    PSP_MEDIA_CODEC_KIND_DRAIN = 3,
    PSP_MEDIA_CODEC_KIND_TEARDOWN = 4,
    /* sceMpegDelete and sceMpegCreate are the same class of unbounded Media
       Engine call the teardown tail runs here for, so a reset that rebuilds the
       decoder object rides the worker for the same reason. */
    PSP_MEDIA_CODEC_KIND_RECREATE = 5
} PspMediaCodecJobKind;

/*
 * One job may be prepared while a different codec kind is running.
 *
 * The browser thread fills the audio-specific staging buffer and descriptor
 * before publishing READY. The worker either claims READY or atomically closes
 * an EMPTY slot before it publishes the active completion. That handshake
 * closes the otherwise tiny race where the browser observes RUNNING and the
 * worker goes to sleep immediately before seeing the prepared job.
 */
typedef enum {
    PSP_MEDIA_CODEC_COMPLETION_EMPTY = 0,
    PSP_MEDIA_CODEC_COMPLETION_READY = 1,
    PSP_MEDIA_CODEC_COMPLETION_READING = 2
} PspMediaCodecCompletionState;

typedef struct {
    PspMediaCodecJobKind kind;
    uint64_t epoch;
    uint32_t prepared_us;
    uint64_t audio_pts_us;
} PspMediaCodecPreparedJob;

/*
 * The worker copies a successfully completed active job here before starting
 * the prepared one. This is the one result slot the browser has not collected
 * yet; a second chain is impossible until it has been consumed, so completion
 * storage stays bounded without ever overwriting a verdict.
 */
typedef struct {
    PspMediaCodecJobKind kind;
    MediaBackendResult result;
    uint64_t epoch;
    uint32_t started_us;
    uint32_t done_us;
    int native_stage;
    char error[192];
} PspMediaCodecCompletion;

typedef enum {
    PSP_MEDIA_CODEC_STAGE_NONE = 0,
    PSP_MEDIA_CODEC_STAGE_AVC_BRIDGE = 1,
    PSP_MEDIA_CODEC_STAGE_AVC_DECODE = 2,
    PSP_MEDIA_CODEC_STAGE_AVC_DETAIL = 3,
    PSP_MEDIA_CODEC_STAGE_AVC_CSC = 4,
    PSP_MEDIA_CODEC_STAGE_AAC_DECODE = 5,
    PSP_MEDIA_CODEC_STAGE_AUDIO_SIGNAL = 6,
    PSP_MEDIA_CODEC_STAGE_WORKER_WAIT = 7,
    PSP_MEDIA_CODEC_STAGE_AVC_STOP = 8,
    PSP_MEDIA_CODEC_STAGE_AUDIO_EDRAM = 9,
    PSP_MEDIA_CODEC_STAGE_MPEG_DELETE = 10,
    PSP_MEDIA_CODEC_STAGE_MPEG_FINISH = 11,
    /* Appended, never renumbered: a stage number is what a hang log names. */
    PSP_MEDIA_CODEC_STAGE_MPEG_CREATE = 12,
    PSP_MEDIA_CODEC_STAGE_MPEG_INIT_AU = 13
} PspMediaCodecNativeStage;

/* These are private firmware ABIs copied from the mature raw-MP4 player
   contract, not public PSPSDK types. A host build cannot validate their
   32-bit pointer layout; make the Allegrex build fail before a field drift
   turns into a firmware write through the wrong pointer. */
_Static_assert(sizeof(void *) == 4, "PSP media ABI requires 32-bit pointers");
_Static_assert(sizeof(unsigned long) == 4,
               "PSP audiocodec ABI requires 32-bit control words");
_Static_assert(sizeof(PspAvcNal) == 32, "PspAvcNal ABI drift");
_Static_assert(sizeof(PspAvcInfo) == 40, "PspAvcInfo ABI drift");
_Static_assert(sizeof(PspAvcYuv) == 44, "PspAvcYuv ABI drift");
_Static_assert(sizeof(PspAvcDetail2) == 96, "PspAvcDetail2 ABI drift");
/* A dump may only read inside the structure it is dumping. */
_Static_assert(PSP_MEDIA_AU_DUMP_BYTES <= PSP_MEDIA_VIDEO_AU_BYTES,
               "AU dump reads past the access-unit descriptor");
_Static_assert(PSP_MEDIA_CODEC_DUMP_WORDS <= PSP_MEDIA_AUDIO_CODEC_WORDS,
               "codec dump reads past the audiocodec control block");
_Static_assert(offsetof(PspAvcDetail2, info_buffer) == 16,
               "PspAvcDetail2 info pointer ABI drift");
/* The reported per-slot arrays have to be able to hold every slot. Raising the
   slot count without widening MediaBackendStats would otherwise publish a
   pipeline half-described. */
_Static_assert(
    PSP_MEDIA_SURFACE_SLOTS <= MEDIA_BACKEND_MAX_SURFACE_SLOTS,
    "PSP surface slots must fit the reported per-slot statistics");
/* Two, and the failure-log line above names both by index. */
_Static_assert(
    PSP_MEDIA_SURFACE_SLOTS == 2u,
    "PSP decoded-output slots are a pair; the diagnostics name both");
/* The same rule for the three widths the occupancy ladder reports over. A
   fifth slot state, a fifth loop phase or a fifteenth duration bucket would
   otherwise write past the arrays MediaBackendStats publishes them in. */
_Static_assert(
    PSP_MEDIA_SLOT_STATES == MEDIA_BACKEND_SLOT_STATES,
    "slot-state dwell is reported per state; the widths must agree");
_Static_assert(
    PSP_MEDIA_LOOP_PHASES == MEDIA_BACKEND_LOOP_PHASES,
    "conversions are tagged per loop phase; the widths must agree");
_Static_assert(
    PSP_MEDIA_JOB_HISTOGRAM_BUCKETS
        == MEDIA_BACKEND_JOB_HISTOGRAM_BUCKETS,
    "job durations are reported per bucket; the widths must agree");
_Static_assert(offsetof(PspAvcDetail2, yuv_buffer) == 44,
               "PspAvcDetail2 YUV pointer ABI drift");
_Static_assert(sizeof(PspAvcCsc) == 48, "PspAvcCsc ABI drift");

/*
 * Raw MP4 AVC entry points exported by mpeg_vsh.prx but not declared by the
 * public PSPSDK headers. Their stable firmware NIDs are isolated in
 * media_backend_psp_imports.S.
 */
extern int sceMpegGetAvcNalAu(SceMpeg *mpeg, PspAvcNal *nal, SceMpegAu *au);
extern int sceMpegAvcDecodeFlush(SceMpeg *mpeg);
extern int sceMpegAvcDecodeDetail2(
    SceMpeg *mpeg, PspAvcDetail2 **detail);
extern int sceMpegBaseCscAvc(
    void *destination, void *unknown, int stride, PspAvcCsc *csc);

/*
 * Cadence counters the browser thread owns.
 *
 * Kept out of MediaBackendStats until they are published. A take and a
 * video-submit hold are both recorded while the codec worker is inside a job
 * and still incrementing that accumulator, so writing them there would be two
 * threads storing into one structure with nothing between them. psp_media_stats
 * runs on the browser thread and refuses while a job is running, which makes it
 * the one place both halves can be assembled for free.
 */
typedef struct {
    size_t take_calls;
    size_t take_satisfied;
    size_t take_empty;
    size_t take_stale;
    size_t take_busy;
    size_t take_early;
    uint64_t take_age_total_us;
    uint32_t take_age_max_us;
    size_t emits_submit;
    size_t emits_drain;
    size_t emits_release;
    size_t emit_visit_starved;
    size_t reoffers;
    uint64_t reoffer_total_us;
    uint32_t reoffer_max_us;
    size_t submit_periods;
    uint64_t submit_period_total_us;
    uint32_t submit_period_max_us;
    size_t hold_job_slot;
    size_t hold_stage_copy;
    size_t hold_refusal;
    size_t hold_frame_ready;
    size_t hold_batch_pending;
    size_t hold_timestamps;
    size_t drain_hold_job_slot;
    size_t drain_hold_stage_copy;
    size_t drain_hold_refusal;
    size_t drain_hold_surface;
} PspMediaVideoCadence;

typedef struct {
    Budget *budget;
    BudgetReservation external;
    MediaBackendStats stats;
    PspMediaVideoCadence cadence;
    MediaMp4TrackInfo video;
    MediaMp4TrackInfo audio;
    bool have_video;
    bool have_audio;
    uint16_t decoded_width;
    uint16_t decoded_height;
    unsigned frame_stride;
    unsigned surface_rows;
    unsigned csc_rows;
    int mpeg_mode;
    int me_boot_type;
    size_t surface_bytes;
    uint8_t nal_length_size;
    MediaH264PspCompat h264_compat;
    bool mpeg_created;
    /* An AAC work buffer has been attached to the codec control block and
       must be detached at teardown. */
    bool audio_edram;
    /* ...and it came from the firmware's own grant, so the detach is a real
       sceAudiocodecReleaseEDRAM. False for PMPlayer's pool-backed substitute,
       whose storage the pool owns and firmware must never be asked to free. */
    bool audio_edram_real;
    atomic_bool playing;
    atomic_bool buffering;
    atomic_bool audio_stop;
    atomic_bool audio_resetting;
    atomic_bool codec_stop;
    atomic_int codec_job_state;
    atomic_int codec_prepared_state;
    atomic_int codec_completion_state;
    atomic_int codec_native_stage;
    uint32_t codec_worker_next_health_us;
    _Atomic uint32_t audio_output_in_flight_slot;
    /*
     * Which seek/reset generation this backend is in. Browser-thread-owned and
     * plain rather than atomic: it is copied into a job before the release
     * store that publishes the job, and read back from that copy, so no thread
     * ever loads these eight bytes while another writes them.
     */
    uint64_t session_epoch;
    /* The epoch the in-flight job was stamped with, and the worker's read-only
       copy of it. A completion carrying anything else belongs to a stream this
       backend has already left behind. */
    uint64_t codec_job_epoch;
    /*
     * No further access unit may enter the decoder. Set for the whole of a
     * reset, so the ordered teardown below it cannot race a submission that
     * the same thread would otherwise still be allowed to make from a nested
     * pump, and cleared on every exit -- a reset that failed is one the
     * session retries, not one that ends the stream.
     */
    bool admissions_closed;
    atomic_bool audio_origin_initialized;
    int audio_channel;
    SceUID audio_event;
    SceUID audio_thread;
    SceUID codec_event;
    SceUID codec_thread;
    _Atomic uint32_t audio_queue_read;
    _Atomic uint32_t audio_queue_write;
    _Atomic uint32_t audio_queue_generation;
    /* Firmware accepts several blocks ahead at startup. The first successful
       submission starts an elapsed wall-time DAC cursor; accepted blocks only
       cap that cursor and are never mistaken for heard audio. */
    _Atomic uint32_t audio_first_output_us;
    uint32_t audio_cursor_last_us;
    uint64_t audio_cursor_elapsed_us;
    uint32_t audio_output_origin_blocks;
    /*
     * Output-side counters. Everything the decoder publishes about audio today
     * describes the DECODE half: event=codec-job-complete kind=2 says firmware
     * accepted an access unit and the worker was signalled, and says nothing
     * about whether a single sample ever reached the DAC. Three device
     * sessions were read as "audio output failed" from decode-side lines
     * alone. The worker owns the writes -- it may never log -- and the browser
     * thread publishes their cumulative result during teardown.
     */
    _Atomic uint32_t audio_output_blocks;
    _Atomic uint32_t audio_output_starves;
    atomic_int audio_output_first_status;
    atomic_bool audio_output_reported;
    atomic_int audio_worker_error;
    atomic_int audio_worker_stage;
    uint32_t audio_worker_next_health_us;
    SceInt32 video_status;
    PspAvcDetail2 *video_detail;
    unsigned video_picture_count;
    unsigned video_picture_index;
    unsigned maximum_video_picture_batch;
    /* The identity counter every converted picture draws its name from. The
       picture's own copy lives in the slot that holds it; this is only the
       source of the next one. */
    uint64_t frame_identity;
    /* Decode order, incremented with the identity, and what breaks a tie two
       equal or unset presentation times cannot. */
    uint64_t frame_sequence;
    bool video_startup_catchup;
    uint64_t presentation_clock_us;
    uint64_t presented_frame_pts_us;
    uint64_t audio_origin_us;
    PspMediaVideoTimestampQueue video_timestamps;
    unsigned video_drain_calls;
    unsigned video_drain_surface_waits;
    /*
     * Cadence stamps. Written by whichever side owns the picture at the time
     * and read by the other across the codec_job_state release/acquire handoff,
     * exactly as a slot's pts_us is: the worker converts, publishes the job,
     * and only then can the browser thread take.
     */
    uint32_t video_decode_done_us;  /* when firmware returned the batch */
    uint32_t video_submit_last_us;  /* the previous accepted video AU */
    /*
     * The occupancy ladder's working state. Every cumulative total it feeds
     * lives in backend->stats, where the session already reads it; these are
     * only the stamps and edge flags the totals are charged from, and none of
     * them is published.
     *
     * Ownership follows the slots themselves. A slot transition and a
     * conversion can happen on the codec worker or on the browser thread, but
     * never on both at once: a video or drain job owns every slot for its
     * whole duration, take_frame refuses to claim while one is running, and
     * an audio job -- the one kind that may run alongside a claim -- touches
     * no slot at all. So these are plain fields, exactly as the slots they
     * describe are.
     */
    PspMediaSlotDwell slot_dwell[PSP_MEDIA_SURFACE_SLOTS];
    uint32_t slot_sample_us;
    bool slot_no_free;
    bool slot_free_idle;
    /* Armed when a slot becomes free with none free before it, disarmed by
       the access unit that takes the opportunity. */
    uint32_t slot_free_since_us;
    /* The worker's own three stamps: when it observed its job, and -- for the
       browser thread to read across the completion handoff -- when it
       published the result. */
    uint32_t worker_wake_us;
    uint32_t codec_job_done_us;
    /* The previous conversion of the batch currently being drained, so the
       gap between picture k and picture k+1 is measurable. Zeroed when a
       batch arrives, so the first picture of one charges nothing. */
    uint32_t batch_convert_us;

    SceMpeg mpeg;
    SceMpegRingbuffer ringbuffer;
    SceMpegAu *video_au;
    void *video_parameter_sets;
    size_t video_parameter_sets_bytes;
    int video_sps_bytes;
    int video_pps_bytes;
    unsigned char *packet_staging;
    size_t packet_staging_bytes;
    void *mpeg_memory;
    int mpeg_memory_bytes;
    void *ddr_memory;
    void *video_es;
    uint32_t *surfaces[PSP_MEDIA_SURFACE_SLOTS];
    /*
     * Who owns each of those surfaces. This is the ownership machine: a
     * conversion targets a FREE slot, a claim takes the earliest READY one,
     * and the claim it replaces is the only thing that frees a slot again.
     * See PspMediaSurfaceSlotState for why a reader's release is not enough.
     */
    PspMediaSurfaceSlot slots[PSP_MEDIA_SURFACE_SLOTS];
    /* The slot the browser thread is holding, or -1. Exactly one slot can be
       READING, and naming it here means the claim that supersedes it does not
       have to search for it. */
    int reading_slot;
    bool decoder_primed;
    bool raw_nal_probe_pending;
    unsigned raw_nal_probe_packets;
    size_t last_packet_bytes;
    uint64_t last_packet_offset;
    uint64_t last_packet_dts_us;
    uint64_t last_packet_pts_us;
    int last_nal_mode;
    atomic_int codec_job_kind;
    MediaBackendResult codec_job_result;
    PspMediaVideoTimestampQueue codec_job_timestamps_before;
    /*
     * Access units firmware looked at and refused. Kept apart from
     * last_native_error, which every recovery path reads as "the decoder is
     * broken": a refused access unit is a statement about one AU's content,
     * not about the decoder, and poisoning that field would send the retry
     * ladder after the wrong thing.
     */
    /* Picture batches longer than the timestamps retained for them. Counted
       apart from refusals because they are the downstream symptom rather than
       the event, and because the session watches the refusal total to decide
       whether to reposition -- an over-long batch is recovered in place and
       must not spend a reposition of its own. */
    unsigned video_batch_overruns_consecutive;
    unsigned video_batch_overruns_total;
    unsigned video_batch_overrun_window_count;
    uint64_t video_batch_overrun_window_started_us;
    unsigned video_refusals_consecutive;
    unsigned video_refusals_total;
    unsigned video_refusal_window_count;
    uint64_t video_refusal_window_started_us;
    int last_refused_error;
    /* Firmware may report a content refusal and hang on the very next AU.
       Once dirty, no further video submission is safe until reset recreates
       the decoder and the demuxer has returned to a random-access point. */
    _Atomic bool video_refusal_dirty;
    uint64_t codec_job_audio_pts_us;
    _Atomic uint32_t codec_job_started_us;
    /* A watchdog verdict needs two browser visits against the same running
       job. A prepared job can replace the active job without changing the
       RUNNING state, so a one-sample {state, started} read can splice the old
       state to the new timestamp (or vice versa) and condemn a healthy job. */
    uint32_t codec_watchdog_candidate_started_us;
    bool codec_watchdog_candidate;
    char codec_job_error[192];
    PspMediaCodecPreparedJob codec_prepared;
    PspMediaCodecCompletion codec_completion;
    /* Media Engine teardown crosses the worker boundary in both directions:
       destroy decides on the main thread whether the process-wide MPEG runtime
       must be finished, and reads back what the firmware returned. The
       release/acquire handoff on codec_job_state publishes both. */
    bool codec_teardown_finish_runtime;
    bool codec_teardown_ran;
    int codec_teardown_edram_release;
    /* The same handoff for a reset that rebuilds the decoder object: the worker
       reports whether it ran and what firmware returned, and the main thread
       reads both only after the job has published. */
    bool codec_recreate_ran;
    int codec_recreate_status;
    /* Access units still to come before a no-touch reposition stops treating an
       over-long picture batch as the Media Engine releasing what it held.
       Zero everywhere except inside that window. */
    unsigned video_reposition_drain_units;

    unsigned long *audio_codec;
    int16_t *audio_pcm;
    int16_t *audio_queue;
    /*
     * Compressed AAC access units staged for the worker, and how far through
     * them it has got.
     *
     * Its own buffer rather than the shared packet staging, and that is the
     * whole reason this could not be done in place: the shared buffer is
     * overwritten by the next video submission, and a staged audio unit now
     * outlives the submit that staged it -- deliberately, because waiting one
     * more sample for a partner is what halves the round trips.
     *
     * `count` is what the browser thread staged, `index` how many the worker
     * has consumed. The browser thread reads and writes both only while the
     * job slot is idle, and the worker only while it owns the job, so the
     * release/acquire pair on codec_job_state orders them exactly as it
     * orders every other field that crosses that handoff. A job that stops
     * early on its own budget leaves index < count, which the flush path
     * treats as a job still owed.
     */
    unsigned char *audio_staging;
    size_t audio_staged_bytes[PSP_MEDIA_AUDIO_BATCH_MAXIMUM];
    uint64_t audio_staged_pts_us[PSP_MEDIA_AUDIO_BATCH_MAXIMUM];
    unsigned audio_staged_count;
    unsigned audio_staged_index;
    unsigned char *audio_pending;
    size_t audio_pending_bytes[PSP_MEDIA_AUDIO_PENDING_SLOTS];
    uint64_t audio_pending_pts_us[PSP_MEDIA_AUDIO_PENDING_SLOTS];
    unsigned audio_pending_read;
    unsigned audio_pending_write;
    uint32_t audio_pending_since_us;
#ifdef TILEFINCH_PSP_VALIDATION_LOG
    /*
     * The last conversion request, kept so the byte-order probe can repeat it.
     * A picture batch is released as soon as its last picture is emitted, so
     * video_detail is NULL by the time anything outside the decoder can ask a
     * question about the picture that was just converted. This descriptor is
     * self-contained -- macroblock counts and the eight plane pointers -- and
     * those planes stay firmware's until the next decode, which the probe
     * never issues.
     */
    PspAvcCsc csc_probe_request;
    bool csc_probe_request_valid;
#endif
} PspMediaBackend;

/*
 * The one reservation every Media Engine buffer is carved out of, taken at
 * boot by media_psp_backend_reserve_pool while the heap cursor is still far
 * below PSP_MEDIA_ME_VISIBLE_LIMIT. See media_backend_psp_pool.h for why the
 * address matters at all. The allocator that draws from it is defined below
 * the logging block, because an exhausted pool has to say so.
 */
static PspMediaPool psp_media_pool;

/*
 * Real firmware historically requires a 64-byte-aligned address AND length
 * for any dcache op that invalidates -- the pure invalidate and the
 * writeback-invalidate alike -- and rejects anything else. The status is not
 * returned, so an exact struct size silently voids the protection instead of
 * failing loudly. Every buffer invalidated with this starts on a 64-byte
 * boundary, and rounding the length up to that boundary names exactly the
 * cache lines the unrounded length already covered: it changes only whether
 * firmware accepts the request. PPSSPP's coherent memory hides both halves.
 *
 * Which form to use is a question about who wrote the range, not about who
 * reads it. Firmware calls that run on the main CPU -- sceMpegInitAu,
 * sceMpegGetAvcNalAu, sceAudiocodecCheckNeedMem, sceAudiocodecGetEDRAM,
 * sceAudiocodecInit and the rest of the control-block calls -- store through
 * this cache, so a pure invalidate over their output destroys it before it
 * reaches memory. Only ranges the Media Engine writes exclusively, with no
 * dirty CPU line left over them, may be invalidated outright; every other
 * range takes the writeback-invalidate form, which is correct in both
 * directions. PMPlayer settles the same question by never invalidating a
 * range at all and writing the whole cache back instead.
 */
static unsigned psp_media_cache_extent(size_t bytes)
{
    return (unsigned) ((bytes + 63u) & ~(size_t) 63u);
}

static int16_t *psp_media_audio_queue_slot(
    PspMediaBackend *backend, unsigned slot)
{
    return backend->audio_queue
         + (size_t) slot * PSP_MEDIA_AUDIO_SAMPLES * 2u;
}

static bool psp_media_audio_release_retryable(int status)
{
    uint32_t code = (uint32_t) status;
    return code == (uint32_t) SCE_AUDIO_ERROR_OUTPUT_BUSY
        || code == PSP_MEDIA_AUDIO_SRC_QUEUE_BUSY;
}

/* A blocking output call can return after queueing its final DMA buffer. On
   real firmware the immediate channel release may then report busy even
   though the worker has already stopped. Never free that buffer while the
   audio service may still own it: give the single admitted 1024-sample block
   a bounded drain window, and let the caller quarantine state if ownership
   still cannot be released. */
static bool psp_media_release_audio_channel(
    PspMediaBackend *backend, uint32_t wait_limit_us,
    int *native_status, unsigned *attempts, uint32_t *waited_us)
{
    if (native_status != NULL) *native_status = 0;
    if (attempts != NULL) *attempts = 0;
    if (waited_us != NULL) *waited_us = 0;
    if (backend == NULL) return false;
    if (backend->audio_channel == -1) return true;
    if (backend->audio_channel < -2) {
        if (native_status != NULL)
            *native_status = (int) SCE_AUDIO_ERROR_INVALID_CH;
        return false;
    }
    uint32_t started = sceKernelGetSystemTimeLow();
    for (;;) {
        if (attempts != NULL) (*attempts)++;
        int status = backend->audio_channel == -2
            ? sceAudioSRCChRelease()
            : sceAudioChRelease(backend->audio_channel);
        if (native_status != NULL) *native_status = status;
        if (status >= 0
            || (uint32_t) status == (uint32_t) SCE_AUDIO_ERROR_NOT_RESERVED) {
            backend->audio_channel = -1;
            return true;
        }
        uint32_t elapsed = sceKernelGetSystemTimeLow() - started;
        if (waited_us != NULL) *waited_us = elapsed;
        if (!psp_media_audio_release_retryable(status)
            || elapsed >= wait_limit_us) return false;
        uint32_t remaining = wait_limit_us - elapsed;
        SceUInt delay = remaining < PSP_MEDIA_AUDIO_CHANNEL_POLL_US
            ? remaining : PSP_MEDIA_AUDIO_CHANNEL_POLL_US;
        if (delay == 0 || sceKernelDelayThread(delay) < 0) return false;
    }
}

static bool psp_media_ensure_packet_staging(
    PspMediaBackend *backend, size_t bytes)
{
    if (backend == NULL || bytes == 0
        || bytes > PSP_MEDIA_PACKET_STAGING_BYTES) return false;
    return backend->packet_staging != NULL
        && bytes <= backend->packet_staging_bytes;
}

/* Firmware modules and the process-wide MPEG runtime deliberately live for
   the browser process. Per-stream backends now reset in place for seeking;
   loading modules per seek both rejects the second create on some firmware
   and fragments several megabytes of scarce user memory. */
static bool psp_avcodec_module_loaded;
static bool psp_mpeg_vsh_module_loaded;
static bool psp_mpeg_runtime_initialized;
static bool psp_media_modules_ready;
static SceUID psp_mpeg_vsh_module = -1;
static SceUID psp_media_module_thread = -1;
static int psp_media_module_failure;
static const char *psp_media_module_failure_stage = "none";
static bool psp_media_backend_is_quarantined;
/* Set by the codec worker when the wide decoder program is rejected at its
   first access unit, read by the browser thread when it picks a quality for a
   new open. The worker publishes it through the same release store on
   codec_job_state that the failing job's error already crosses, and the reader
   only ever runs after that job has been collected. Memory-only on purpose:
   a firmware or mode fix must get to re-test the program after a restart. */
static bool psp_media_wide_program_rejected;
/* The experimental wide-video knob, published once from boot configuration
   before any video opens and read-only afterwards. Absent or unrecognized
   spellings leave the proven program as the only selectable one. */
static int psp_media_wide_program_mode = PSP_MEDIA_WIDE_PROGRAM_OFF;
/*
 * Where in the frame the advance the browser is making right now runs, which
 * decides what it may afford (PspMediaAdvanceMode, media_backend_psp_policy.h).
 *
 * Process-wide rather than per-backend, and browser-thread-only: one session
 * decodes at a time, and this says which of that thread's call sites is
 * executing, not anything about a particular decoder. The pumps bracket their
 * own calls; every other path leaves it where it belongs, on the frame.
 */
static int psp_media_advance_mode = PSP_MEDIA_ADVANCE_FRAME;
/*
 * Video submissions held because firmware refused an access unit and the
 * browser thread has not yet looked at it.
 *
 * Two device runs died in this window. The codec worker publishes a survivable
 * refusal and returns; the browser thread does not act on it until it has
 * collected the job and reached its recovery decision, which is the end of that
 * frame. In between, the pump's remaining units happily submitted the next one
 * or two access units into a decoder whose refusal nobody had processed, and
 * the Media Engine wedged. Two other runs won the same race twenty-three times
 * out of twenty-three -- which is what a race is.
 *
 * So the worker latches, and the browser thread's decision point clears. The
 * hold is video only: audio jobs were never the trigger, and starving audio
 * across the window buys nothing but skew. The release is unconditional and
 * independent of whether any recovery is configured -- recovery is optional,
 * the hold is not -- so this cannot stick with the recovery knob off.
 */
static atomic_bool psp_media_video_refusal_hold;
static unsigned psp_media_video_refusal_blocked;
#if defined(TILEFINCH_PSP_VALIDATION_LOG)
/*
 * The access units that led firmware somewhere it did not come back from.
 *
 * Two soaks wedged inside sceMpegGetAvcNalAu on the same unit, and the only
 * thing recovered from either was sixteen bytes of head in a log line. This
 * records the whole sequence from the seek's decoder reset onward, so the next
 * occurrence collects its own evidence and an offline replay needs no further
 * instrumented cycle.
 *
 * Buffered in memory and written once to host0:, on teardown. The diagnostic
 * deliberately has no Memory Stick fallback: a card write costs milliseconds,
 * violates the playback no-card-I/O rule, and would distort the cadence being
 * reported. Without PSPLink the optional dump simply cannot be created.
 * The cap is generous against what it has to hold -- the wedge arrives about
 * forty access units after the reset, some 50 to 100 KiB -- and recording
 * simply stops there rather than wrapping, because the sequence that matters
 * is the one that starts at the reset. One file per run, truncated on open.
 */
#define PSP_MEDIA_AU_DUMP_CAP_BYTES ((size_t) (256u * 1024u))
#define PSP_MEDIA_AU_DUMP_PATH "host0:/tilefinch-au-dump.bin"
#define PSP_MEDIA_AU_DUMP_MAGIC UINT32_C(0x55414654) /* "TFAU", LE */
#define PSP_MEDIA_AU_DUMP_VERSION UINT32_C(1)
static int psp_media_au_dump_enabled;
static bool psp_media_au_dump_armed;
static unsigned char *psp_media_au_dump_buffer;
static size_t psp_media_au_dump_used;
static uint32_t psp_media_au_dump_sequence;
static bool psp_media_au_dump_capped;
#endif
/* What is left of the calling pump's own window, so a collect wait can never
   outlast the dead time that justified it. Zero means "the caller did not
   say", which leaves the wait on its own bound. */
static uint32_t psp_media_advance_wait_limit_us;
/*
 * The decoded surface has one writer and several readers, and they are on
 * different threads.
 *
 * The writer is the colour conversion inside psp_media_emit_captured_picture,
 * which the codec worker performs at the end of a decode it was handed before
 * any of this was decided. The readers are the presenter's stage copy -- a DMA
 * controller running in parallel -- and, when a picture cannot be staged, the
 * graphics engine sampling the surface for a whole draw. Holding video at the
 * submit boundary cannot separate them: the submission that produced the write
 * happened earlier, on a different frame.
 *
 * Two flags, claimed in opposite orders, on threads that interleave on one
 * CPU. The reader stores its claim and then asks whether a conversion is
 * under way; the writer stores its announcement and then asks whether the
 * surface is claimed. Sequentially consistent stores make at least one of the
 * two see the other, so a conversion and a read can never both believe they
 * have the surface -- and if both see the other, the reader waits out a
 * conversion that is already inside firmware while the writer waits out the
 * claim, which is correct, just briefly redundant.
 *
 * Per slot, because with two of them the question is no longer "is the surface
 * busy" but "is THIS one". A single pair would have made a claim on either
 * slot fence a conversion into the other, which is the whole of what two slots
 * were reserved to stop.
 *
 * These are the fine-grained lease, not the ownership state. The state says
 * who a slot belongs to across frames; the lease says whether a transfer into
 * or out of it is in flight right now, which is the only thing a DMA
 * controller and the Media Engine can disagree about.
 */
static atomic_int psp_media_surface_borrowed[PSP_MEDIA_SURFACE_SLOTS];
static atomic_int psp_media_surface_writing[PSP_MEDIA_SURFACE_SLOTS];
/*
 * A slot a reader is still inside that nothing is tracking any more.
 *
 * The lease above is dropped by the reader that took it, which works for
 * every reader that can be joined. One cannot: a staging DMA whose join
 * timed out was abandoned rather than stopped, and the thread that gave up on
 * it dropped the lease on its way past. From then on the lease says the slot
 * is unread and a controller is reading it -- so the quiesce that a seek and
 * a reset perform passes, the slot is freed, and the Media Engine is handed
 * memory a live transfer is sourcing from.
 *
 * This is that reader, named where the lease cannot name it: a standing
 * refusal that outlives whoever set it, cleared only when the presenter has
 * OBSERVED the transfer end (see PspMediaPresentDmaJoin). It is deliberately
 * NOT cleared by psp_media_surface_handshake_reset -- a new session does not
 * stop somebody else's DMA controller -- which is what makes a reset that
 * cannot proceed fail its quiesce rather than free the slot anyway.
 */
static atomic_int psp_media_surface_quarantined[PSP_MEDIA_SURFACE_SLOTS];
static atomic_uint psp_media_surface_quarantines;
/* Pump visits that would have fetched a partner for a staged AAC unit and
   were declined because video could use the decoder. Counted beside the
   handshake rather than in backend->stats because the question is asked
   through a const backend -- the pump is asking, not the decoder. */
static atomic_uint psp_media_audio_batch_video_declines;
/*
 * What a reader is allowed to believe about a slot, published where a reader
 * can see it.
 *
 * The ownership state lives in the backend, and the presenter holds no
 * backend: media_psp_backend_borrow_surface is called from the graphics path,
 * which knows a frame and nothing else. So the two facts a reader has to check
 * -- that this slot is still the claimed one, and that it still holds the same
 * picture -- are published here as the slot moves. One session decodes at a
 * time, which is the same assumption the handshake above already rests on.
 */
static atomic_int psp_media_surface_lease_readable[PSP_MEDIA_SURFACE_SLOTS];
static atomic_uint psp_media_surface_lease_generation[
    PSP_MEDIA_SURFACE_SLOTS];
static atomic_uint psp_media_surface_borrow_stale;
static atomic_uint psp_media_surface_borrow_waits;
static atomic_uint psp_media_surface_borrow_wait_us;
static atomic_uint psp_media_surface_borrow_timeouts;
static atomic_uint psp_media_surface_emit_deferrals;
/*
 * The presented half of one picture's journey, measured where the backend has
 * no decoder to hang it on: the borrow release runs on the browser thread from
 * the presenter, which holds no backend pointer, and it can run while a decode
 * job is in flight. Browser-thread-only writes, so the read-modify-writes below
 * need no more than relaxed atomics to be exact; they are atomic at all so a
 * stats read can never see a torn word.
 *
 * Both timestamps are armed-or-zero. A frame may be presented more than once --
 * the loop redraws whatever it last took -- and only the first release after a
 * take is that picture's present, so the arm is consumed rather than compared.
 */
static atomic_uint psp_media_frame_taken_us;
static atomic_uint psp_media_present_done_us;
static atomic_uint psp_media_presents;
static atomic_uint psp_media_present_total_us;
static atomic_uint psp_media_present_max_us;
/*
 * Claimed, staged, displayed -- three different things the counters used to
 * call one. A claimed identity that is replaced in the surface before it is
 * drawn was previously indistinguishable from one that reached the screen, so
 * every rate read off take-ok was an upper bound nobody had checked. The
 * milestones arrive from the presenter, which holds no decoder, so they land
 * here beside the surface handshake and reset with it.
 *
 * What identifies a picture, in full.
 *
 * The duplicate filter used to hold an atomic_uint and compare a 64-bit
 * identity truncated into it. That was survivable while one surface meant one
 * picture in flight; with two slots the tuple is the invariant -- every
 * displayed {epoch, slot, generation, identity, PTS} appeared as exactly one
 * claim -- and a filter that cannot see the slot or the generation cannot
 * check it. Compared whole instead.
 *
 * Plain, not atomic, and that is a narrowing rather than a relaxation: every
 * writer and reader of these two is the browser thread -- the claim in
 * psp_media_take_frame, the staging and display milestones from the presenter,
 * the trace dump. The counters beside them stay atomic because they are read
 * for statistics; a key is never read except by the thread that wrote it.
 */
typedef struct {
    uint64_t epoch;
    uint64_t identity;
    uint64_t pts_us;
    uint32_t generation;
    int slot;
} PspMediaFrameKey;

static bool psp_media_frame_key_equal(
    const PspMediaFrameKey *a, const PspMediaFrameKey *b)
{
    return a->slot == b->slot && a->generation == b->generation
        && a->identity == b->identity && a->epoch == b->epoch
        && a->pts_us == b->pts_us;
}

static bool psp_media_frame_key_armed(const PspMediaFrameKey *key)
{
    return key->identity != 0 && key->slot >= 0;
}

static PspMediaFrameKey psp_media_claim_key;
static PspMediaFrameKey psp_media_display_key;
static atomic_uint psp_media_claims_staged;
static atomic_uint psp_media_claims_displayed;
static atomic_uint psp_media_claims_dropped;
static atomic_uint psp_media_claims_quiesced;
/*
 * The two ways the funnel can be wrong rather than merely short.
 *
 * A staging pass whose source tuple is not the claimed one, and -- in
 * validation builds -- a staged copy whose pixels do not hash to what the
 * conversion left in the slot. Both are the same failure seen from different
 * ends: right metadata, wrong pixels. Every other counter in this file would
 * record that as a success, which is why these are the ones worth having.
 */
static atomic_uint psp_media_stage_mismatches;
static atomic_uint psp_media_signature_mismatches;
/*
 * What a refusal recovery costs in wall time. Armed by the worker the moment
 * firmware refuses an access unit and closed by the first picture that reaches
 * the screen afterwards; each milestone in between is a stage this can stop
 * at, so a recovery that never completes is visible as a stage that never
 * advanced rather than as a missing sample.
 */
#define PSP_MEDIA_RECOVERY_IDLE 0
#define PSP_MEDIA_RECOVERY_REFUSED 1
#define PSP_MEDIA_RECOVERY_RESET 2
#define PSP_MEDIA_RECOVERY_FED 3
#define PSP_MEDIA_RECOVERY_CONVERTED 4
static atomic_int psp_media_recovery_stage;
static atomic_uint psp_media_recovery_refusal_us;
static atomic_uint psp_media_recovery_reset_us;
static atomic_uint psp_media_recovery_au_us;
static atomic_uint psp_media_recovery_csc_us;
static atomic_uint psp_media_recoveries;
static atomic_uint psp_media_recovery_reset_total_us;
static atomic_uint psp_media_recovery_au_total_us;
static atomic_uint psp_media_recovery_csc_total_us;
static atomic_uint psp_media_recovery_present_total_us;
static atomic_uint psp_media_recovery_present_max_us;
/* Media time the stream jumped across the recovery, which is not the outage
   above and was being read as though it were. */
static atomic_uint psp_media_recovery_skip_total_ms;
static uint64_t psp_media_recovery_last_pts_us;

/* Zero is the disarmed value for every stamp above, so it is never a reading.
   The microsecond it costs once every seventy-one minutes of uptime is a
   cheaper price than a branch-free arm that can silently disarm itself. */
static uint32_t psp_media_stamp_us(void)
{
    uint32_t now_us = sceKernelGetSystemTimeLow();
    return now_us == 0u ? 1u : now_us;
}

/*
 * Where the browser thread is standing, for the conversions that complete
 * while it is there. Written by the session on the browser thread, read by
 * the codec worker at each colour conversion, and read by nothing that
 * decides anything -- see media_psp_backend_set_loop_phase.
 */
static atomic_int psp_media_loop_phase = PSP_MEDIA_LOOP_PHASE_OTHER;

/*
 * Close the interval this slot has been in its current state, and re-arm.
 *
 * Called with the state being LEFT, immediately before each transition is
 * written -- never instead of writing it. The four ownership transitions are
 * load-bearing enough that a reviewer, a contract test and the compiler can
 * all still read them exactly where they were; this only puts a stopwatch
 * beside them.
 */
static void psp_media_slot_charge(PspMediaBackend *backend, unsigned slot)
{
    if (backend == NULL || slot >= PSP_MEDIA_SURFACE_SLOTS) return;
    /* The caller owns this slot -- charging is the last thing it does before
       publishing the transition away from the state being charged -- so the
       peek reads a value no other thread can be changing. */
    psp_media_slot_dwell_charge(
        &backend->slot_dwell[slot], psp_media_slot_peek(&backend->slots[slot]),
        psp_media_stamp_us());
}

/*
 * Re-evaluate the two pipeline-wide conditions, charging the interval since
 * the last evaluation to whichever of them held over it.
 *
 * Called after each transition, so the conditions are always sampled at the
 * instants they can change -- there is no periodic tick to alias against and
 * no interval is attributed to a state that had already ended.
 *
 * "No slot free" is the pipeline legitimately full: one picture claimed, one
 * converted and waiting, nowhere to put a third. "A slot free and nothing
 * converting" is its opposite and the reason this exists: a decoded-output
 * surface nobody owns, a Media Engine with nothing in flight, and a stream
 * arriving at 24 pictures a second that is not being decoded into it.
 */
static void psp_media_slot_sample(PspMediaBackend *backend)
{
    if (backend == NULL) return;
    uint32_t now_us = psp_media_stamp_us();
    uint32_t elapsed_us = now_us - backend->slot_sample_us;
    if (backend->slot_sample_us != 0u) {
        if (backend->slot_no_free)
            backend->stats.slot_no_free_us += (uint64_t) elapsed_us;
        if (backend->slot_free_idle)
            backend->stats.slot_free_idle_us += (uint64_t) elapsed_us;
    }
    backend->slot_sample_us = now_us;
    unsigned free_slots = psp_media_slot_free_count(
        backend->slots, PSP_MEDIA_SURFACE_SLOTS);
    unsigned ready = psp_media_slot_ready_count(
        backend->slots, PSP_MEDIA_SURFACE_SLOTS);
    backend->stats.slot_ready_samples++;
    backend->stats.slot_ready_total += ready;
    if (ready > backend->stats.slot_ready_max)
        backend->stats.slot_ready_max = ready;
    /*
     * A conversion is in flight exactly while the worker owns the surfaces,
     * which is the two job kinds that convert. An audio job holds the shared
     * job slot without holding a surface, so counting it here would report
     * the pipeline as busy at the very moments video is idle -- which is the
     * confusion this whole line exists to end.
     */
    int job_state = atomic_load_explicit(
        &backend->codec_job_state, memory_order_relaxed);
    PspMediaCodecJobKind job_kind =
        (PspMediaCodecJobKind) atomic_load_explicit(
            &backend->codec_job_kind, memory_order_relaxed);
    bool converting = job_state == PSP_MEDIA_CODEC_JOB_RUNNING
        && (job_kind == PSP_MEDIA_CODEC_KIND_VIDEO
            || job_kind == PSP_MEDIA_CODEC_KIND_DRAIN);
    /* The first slot to become free after a full pipeline starts the clock
       the next accepted access unit stops. Later frees are inside that
       interval and must not restart it. */
    if (free_slots != 0 && backend->slot_free_since_us == 0u
        && backend->slot_no_free)
        backend->slot_free_since_us = now_us;
    backend->slot_no_free = free_slots == 0;
    backend->slot_free_idle = free_slots != 0 && !converting;
}

/* One firmware call pair, timed from the stamp its caller took immediately
   before entering it. Both codecs report through this, so the video and audio
   halves of the shared job slot are measured the same way. */
static void psp_media_note_worker_firmware(
    PspMediaBackend *backend, uint32_t entered_us)
{
    if (backend == NULL || entered_us == 0u) return;
    uint32_t firmware_us = psp_media_stamp_us() - entered_us;
    backend->stats.worker_firmware_calls++;
    backend->stats.worker_firmware_total_us += (uint64_t) firmware_us;
    if (firmware_us > backend->stats.worker_firmware_max_us)
        backend->stats.worker_firmware_max_us = firmware_us;
    if (backend->worker_wake_us != 0u) {
        /* The worker woke, did its cache maintenance and its argument
           marshalling, and only now is inside firmware. Charged once per job,
           at the first firmware call, which is why the arm is cleared. */
        uint32_t prologue_us = entered_us - backend->worker_wake_us;
        backend->worker_wake_us = 0u;
        backend->stats.worker_prologues++;
        backend->stats.worker_prologue_total_us += (uint64_t) prologue_us;
        if (prologue_us > backend->stats.worker_prologue_max_us)
            backend->stats.worker_prologue_max_us = prologue_us;
    }
}

/*
 * One record per picture, correlated end to end.
 *
 * Five segment means computed over five different populations -- per decode
 * job, per picture, per take, per release, per access unit -- sum to something
 * that looks like a cycle and is not one. Under concurrent lifecycles they
 * summed to 92ms against a 52ms period, which is the arithmetic saying out
 * loud that the pictures being averaged were never the same picture. Averages
 * cannot be made to answer this; a record that follows ONE picture from its
 * access unit to the screen can.
 *
 * Keyed by frame identity, which increments once per conversion and is the
 * only value every stage of the pipeline already agrees on. A milestone that
 * arrives for an identity the ring has since reused is late by more than the
 * whole ring and is discarded rather than credited to the picture now living
 * in that slot.
 *
 * Validation builds only: 6 KiB of static storage and a dump that costs real
 * Memory Stick traffic. The counters this sits beside stay unconditional.
 */
#if defined(TILEFINCH_PSP_VALIDATION_LOG)
#define PSP_MEDIA_TRACE_SLOTS 128u
typedef struct {
    uint32_t identity;
    uint32_t au_id;
    uint32_t batch_id;
    uint32_t pts_ms;
    uint32_t submit_us;      /* AU accepted into the decoder */
    uint32_t decode_us;      /* firmware returned the batch holding it */
    uint32_t csc_us;         /* conversion into the surface completed */
    uint32_t take_us;        /* claimed by the browser thread */
    uint32_t stage_us;       /* staging copy completed */
    uint32_t present_us;     /* reached the screen */
    uint32_t next_submit_us; /* the AU accepted after this claim */
    uint32_t generation;     /* the slot's generation when it was converted */
    uint32_t signature;      /* pixel signature taken straight after the CSC */
    uint32_t stage_signature;/* and re-taken from the staged copy */
    uint8_t take_result;
    uint8_t present_path;
    uint8_t batch_index;     /* which picture of its batch */
    uint8_t batch_count;
    /* Which decoded-output slot held this picture. The one field that makes a
       two-slot pipeline readable: an interleaving that stops alternating, or a
       picture whose slot changes between its claim and its display, is visible
       on the line rather than inferred from the rate. */
    uint8_t slot;
} PspMediaPictureTrace;

static PspMediaPictureTrace psp_media_trace[PSP_MEDIA_TRACE_SLOTS];
static uint32_t psp_media_trace_next_au;
static uint32_t psp_media_trace_next_batch;
static uint32_t psp_media_trace_dumped_identity;
/* The access unit and decode this picture came from, carried forward from the
   submit that started it to the conversion that finally names it. */
static uint32_t psp_media_trace_open_au;
static uint32_t psp_media_trace_open_submit_us;
/* The access unit the batch now being emitted came from. Latched when
   firmware returns the batch, because a surplus picture is converted at some
   later visit by which time a further access unit has been accepted -- and it
   belongs to the decode that produced it, not to whatever is in flight. */
static uint32_t psp_media_trace_batch_au;
static uint32_t psp_media_trace_batch_submit_us;

static PspMediaPictureTrace *psp_media_trace_slot(uint64_t identity)
{
    if (identity == 0) return NULL;
    PspMediaPictureTrace *slot =
        &psp_media_trace[(uint32_t) (identity % PSP_MEDIA_TRACE_SLOTS)];
    /* The slot belongs to whoever last opened it. A milestone for anything
       else is older than the ring is deep. */
    return slot->identity == (uint32_t) identity ? slot : NULL;
}
#endif

/* The take classification, shared by the counters and the ring so the two can
   never disagree about what a given attempt was. */
#define PSP_MEDIA_TAKE_OK 1
#define PSP_MEDIA_TAKE_EMPTY 2
#define PSP_MEDIA_TAKE_STALE 3
#define PSP_MEDIA_TAKE_BUSY 4
#define PSP_MEDIA_TAKE_EARLY 5

/* First attempt against a picture wins the record: later ones are the loop
   coming back to a picture whose verdict is already written. */
static void psp_media_trace_take(uint64_t identity, int result)
{
#if defined(TILEFINCH_PSP_VALIDATION_LOG)
    PspMediaPictureTrace *slot = psp_media_trace_slot(identity);
    if (slot == NULL || slot->take_us != 0) return;
    slot->take_us = psp_media_stamp_us();
    slot->take_result = (uint8_t) result;
#else
    (void) identity;
    (void) result;
#endif
}

/*
 * Which Media Engine program a real boot call has actually put on the engine,
 * or -1 for "nobody has said".
 *
 * PMPlayer's me_boot_start initializes the same cache to 3 and skips the
 * bridge for it -- but PMPlayer's first real-world stream was always Baseline
 * (type 4) or wide (type 1), so it always made a real call before decoding
 * anything and the type-3 short-circuit only ever ran after one had
 * succeeded. Tilefinch's default program IS type 3, so the same initializer
 * meant the engine may never have been handed the codec program at all in
 * this process. Start unknown, so the first open of the process always calls
 * for whatever type it selected; every later same-type open still skips, and
 * every path that invalidates the cache already writes -1.
 */
static int psp_media_me_boot_type = -1;
static uint32_t psp_media_me_boot_resolved_nid;

static void psp_media_error(char *error, size_t error_size,
                            const char *format, ...);
static bool psp_media_validate_output_surface(
    PspMediaBackend *backend, unsigned write_slot,
    char *error, size_t error_size);
static bool psp_media_batch_pending(const PspMediaBackend *backend);
static bool psp_media_surface_proven(const PspMediaBackend *backend);
static bool psp_media_wait_codec_job(
    PspMediaBackend *backend, uint32_t wait_limit_us, uint32_t *waited_us);
/* Reset dispatches its decoder rebuild to the worker and so has to ask the same
   question destroy does, from further up the file. */
static bool psp_media_codec_worker_dispatchable(
    const PspMediaBackend *backend);

#if defined(TILEFINCH_PSP_VALIDATION_LOG)
static void psp_media_log(const char *format, ...)
{
    char line[384];
    va_list arguments;
    va_start(arguments, format);
    vsnprintf(line, sizeof(line), format, arguments);
    va_end(arguments);
    tilefinch_platform_log_message(line);
}

/*
 * Commit the validation log to the Memory Stick.
 *
 * psp_log_printf writes into a fixed sequential buffer; the card is flushed
 * and synchronized only at checkpoints, classified media failures, and exit.
 * The media pipeline reaches neither ordinary checkpoint nor exit after a
 * hard firmware wedge, so the failure path commits the section that says why.
 * psp_log_flush takes the same semaphore psp_log_printf does, so the codec
 * worker may call this while the browser thread is logging.
 *
 * A sync is tens to low-hundreds of milliseconds on real hardware. Every
 * caller is therefore on a failure path or inside the first-frame window, and
 * shipping builds compile psp_log_flush -- and all of this -- out entirely.
 */
static void psp_media_commit_log(void)
{
    (void) psp_log_flush(true);
}

/*
 * The failing stage and the moment it was last committed. Failures can repeat
 * per packet and per retry, and paying a card sync for each would turn a
 * diagnosable failure into a hang. Sync when the stage changes, and otherwise
 * at most once every few seconds. Both are atomics only so that a race
 * between the codec worker and the browser thread costs at most one extra
 * sync rather than a torn read; neither needs ordering.
 */
#define PSP_MEDIA_FAILURE_SYNC_INTERVAL_US UINT32_C(3000000)
static const char *_Atomic psp_media_failure_sync_stage;
static _Atomic uint32_t psp_media_failure_sync_us;

static void psp_media_commit_failure_log(const char *stage)
{
    /* Every stage argument in this file is a string literal or a pointer
       into a static name table, so retaining it is safe and comparing the
       pointer is enough to notice a changed stage. */
    uint32_t now_us = (uint32_t) sceKernelGetSystemTimeWide();
    const char *previous = atomic_load_explicit(
        &psp_media_failure_sync_stage, memory_order_relaxed);
    uint32_t previous_us = atomic_load_explicit(
        &psp_media_failure_sync_us, memory_order_relaxed);
    if (previous == stage && previous_us != 0
        && now_us - previous_us < PSP_MEDIA_FAILURE_SYNC_INTERVAL_US) return;
    atomic_store_explicit(
        &psp_media_failure_sync_stage, stage, memory_order_relaxed);
    atomic_store_explicit(
        &psp_media_failure_sync_us, now_us == 0 ? 1u : now_us,
        memory_order_relaxed);
    psp_media_commit_log();
}

/*
 * Hex for the memory dumps that accompany a failure line.
 *
 * Both firmware codecs report every rejection as one opaque status, so the
 * bytes firmware did or did not write into the buffers it was handed are the
 * only evidence of whether the Media Engine ran at all. The formatting lives
 * with the log lines inside this guard: a shipping build must not pay for it
 * even though psp_media_log is already a no-op there.
 */
static const char psp_media_hex_digits[] = "0123456789abcdef";

/* Writes count*2 characters and a terminator. */
static void psp_media_hex_bytes(
    const void *source, size_t count, char *text)
{
    const unsigned char *bytes = (const unsigned char *) source;
    for (size_t at = 0; at < count; at++) {
        text[at * 2u] = psp_media_hex_digits[(bytes[at] >> 4) & 0x0fu];
        text[at * 2u + 1u] = psp_media_hex_digits[bytes[at] & 0x0fu];
    }
    text[count * 2u] = '\0';
}

/* Writes count comma-separated eight-digit words and a terminator, which is
   count*9 characters including it. One log token, so the line stays parseable
   by the same key=value harvesters every other line is read with. */
static void psp_media_hex_words(
    const unsigned long *words, size_t count, char *text)
{
    for (size_t at = 0; at < count; at++) {
        unsigned long value = words[at];
        for (unsigned nibble = 0; nibble < 8u; nibble++)
            text[at * 9u + nibble] = psp_media_hex_digits[
                (unsigned) (value >> ((7u - nibble) * 4u)) & 0x0fu];
        text[at * 9u + 8u] = ',';
    }
    text[count == 0 ? 0 : count * 9u - 1u] = '\0';
}

static void psp_media_log_failure(
    const PspMediaBackend *backend, const char *stage, int status)
{
    if (backend == NULL) return;
    PspMediaCodecJobKind kind = (PspMediaCodecJobKind) atomic_load_explicit(
        &backend->codec_job_kind, memory_order_acquire);
    if ((kind == PSP_MEDIA_CODEC_KIND_VIDEO
            || kind == PSP_MEDIA_CODEC_KIND_DRAIN)
        && backend->packet_staging != NULL
        && backend->last_packet_bytes != 0u) {
        size_t head_bytes = backend->last_packet_bytes;
        if (head_bytes > PSP_MEDIA_AU_HEAD_DUMP_BYTES)
            head_bytes = PSP_MEDIA_AU_HEAD_DUMP_BYTES;
        char head[PSP_MEDIA_AU_HEAD_DUMP_BYTES * 2u + 1u];
        psp_media_hex_bytes(backend->packet_staging, head_bytes, head);
        psp_media_log(
            "tilefinch-media-decoder: event=failure-packet stage=%s "
            "kind=%d bytes=%s",
            stage == NULL ? "unknown" : stage, (int) kind, head);
    }
    psp_media_log(
        "tilefinch-media-decoder: event=failure stage=%s "
        "status=0x%08X source=%ux%u mode=%d stride=%u rows=%u/%u "
        "packet=%zuB nal-mode=%d submitted=%zu decoded=%zu "
        "video-status=%d timestamps=%u pictures=%u/%u "
        "probe=%d/%u canary=%d extent=%d drain=%u/%u "
        "slots=%d/%d gen=%u/%u free=%u reading=%d epoch=%llu",
        stage == NULL ? "unknown" : stage, (unsigned) status,
        backend->decoded_width, backend->decoded_height,
        backend->mpeg_mode, backend->frame_stride,
        backend->csc_rows, backend->surface_rows,
        backend->last_packet_bytes, backend->last_nal_mode,
        backend->stats.submitted_video_packets,
        backend->stats.decoded_video_frames,
        (int) backend->video_status,
        backend->video_timestamps.count,
        backend->video_picture_index, backend->video_picture_count,
        backend->raw_nal_probe_pending ? 1 : 0,
        backend->raw_nal_probe_packets,
        backend->slots[0].canary_armed || backend->slots[1].canary_armed
            ? 1 : 0,
        psp_media_surface_proven(backend) ? 1 : 0,
        backend->video_drain_calls,
        backend->video_drain_surface_waits,
        psp_media_slot_peek(&backend->slots[0]),
        psp_media_slot_peek(&backend->slots[1]),
        backend->slots[0].generation, backend->slots[1].generation,
        psp_media_slot_free_count(backend->slots, PSP_MEDIA_SURFACE_SLOTS),
        backend->reading_slot,
        (unsigned long long) backend->session_epoch);
    psp_media_commit_failure_log(stage);
}
#else
#define psp_media_log(...) ((void) 0)
#define psp_media_log_failure(...) ((void) 0)
#define psp_media_commit_log() ((void) 0)
#endif

/*
 * Allocate a buffer firmware will DMA from. Pool first, ordinary heap second.
 *
 * The fallback is deliberate rather than fatal: a buffer that may be above
 * the Media Engine's reach is still a better outcome than refusing to attempt
 * the video at all, and the event=me-memory line printed before the first
 * decode reports every address either way, so an exhausted pool cannot hide.
 */
static void *psp_media_alloc_shared(size_t bytes, size_t alignment)
{
    if (bytes == 0 || bytes > SIZE_MAX - 63u) return NULL;
    size_t needed = 0;
    size_t remaining = 0;
    bool had_pool = psp_media_pool_available(&psp_media_pool);
    void *block = psp_media_pool_alloc(
        &psp_media_pool, bytes, alignment, &needed, &remaining);
    if (block != NULL) return block;
    /* A reservation that was never taken already said so once at boot; only
       a pool that really ran out is worth a line per buffer. */
    if (had_pool) {
        psp_media_log(
            "tilefinch-media-decoder: event=me-pool-exhausted need=%u "
            "remaining=%u",
            (unsigned) needed, (unsigned) remaining);
    }
    /*
     * Real firmware historically requires a 64-byte-aligned address AND
     * length for a pure dcache invalidate; the pool guarantees both and the
     * heap fallback has to match it.
     */
    size_t rounded = (bytes + 63u) & ~(size_t) 63u;
    return memalign(alignment > 64u ? alignment : 64u, rounded);
}

static void *psp_media_alloc64(size_t bytes)
{
    return psp_media_alloc_shared(bytes, 64u);
}

/* Media buffers are released together at backend destroy, and pool storage is
   never handed back individually: the pool rewinds as a whole once teardown
   has proved firmware no longer owns any of it. */
static void psp_media_release(void *pointer)
{
    if (pointer == NULL) return;
    if (psp_media_pool_owns(&psp_media_pool, pointer)) return;
    free(pointer);
}

/*
 * PMPlayer's substitute for sceAudiocodecGetEDRAM, transcribed from
 * cooleyesAudiocodecGetEDRAM (ppa/mod/audiodecoder.c).
 *
 * The hardware-proven player never calls the firmware grant, for any of its
 * three codecs. It allocates control word 4 -- the size sceAudiocodecCheckNeedMem
 * has just written -- as ordinary 64-byte-aligned, 64-byte-rounded user
 * storage and puts the pointer in control word 3, which is the only effect of
 * the grant anything downstream can observe. That bypass is deliberate in a
 * player proven on this hardware, which makes the real grant the suspect and
 * not the substitute.
 *
 * Tilefinch draws the storage from the Media Engine pool rather than the heap
 * so the codec that reads it by DMA can also address it; the rest is cooleyes'
 * code, rounding included (psp_media_alloc_shared rounds to 64 exactly as
 * malloc_64 does).
 *
 * There is deliberately no matching release. The pool owns this storage: a
 * teardown that completed rewinds it wholesale, and a quarantined one leaks it
 * on purpose along with every other buffer firmware may still be reading.
 */
static int psp_media_pool_audiocodec_edram(
    PspMediaBackend *backend, unsigned *bytes)
{
    unsigned long needed = backend->audio_codec[4];
    if (bytes != NULL) *bytes = (unsigned) needed;
    /* CheckNeedMem's whole purpose is to write this word. A zero here is a
       firmware answer nothing can honour, and silently attaching no buffer
       would surface much later as an ME-side decode failure. */
    if (needed == 0) return -1;
    void *block = psp_media_alloc64((size_t) needed);
    if (block == NULL) return -1;
    backend->audio_codec[3] = (unsigned long) (uintptr_t) block;
    return 0;
}

/*
 * Latch the process-wide refusal, and take the Media Engine pool down with
 * it. A quarantined teardown leaks every firmware-visible buffer on purpose
 * -- firmware may still be inside a call that reads them -- so the pool's
 * storage is exactly as unusable as the buffers themselves. Poison rather
 * than release: the reservation stays owned so nothing else can be handed
 * memory firmware is still reading.
 */
static void psp_media_quarantine(void)
{
    psp_media_backend_is_quarantined = true;
    if (psp_media_pool.poisoned) return;
    psp_media_pool_poison(&psp_media_pool);
    psp_media_log(
        "tilefinch-media-decoder: event=me-pool-poisoned reason=quarantine "
        "used=%u bytes=%u",
        (unsigned) psp_media_pool.cursor, (unsigned) psp_media_pool.bytes);
}

/*
 * Sample how much memory the heap can still hand out, which is the quantity
 * the reservation below is actually spending. See
 * PSP_MEDIA_POOL_PROBE_BLOCKS in media_backend_psp_pool.h for why this is
 * measured rather than asked for, and why measuring it here is safe.
 *
 * Cold and noinline: this is called once, from boot, and its block array must
 * not sit in the caller's frame.
 */
__attribute__((noinline, cold))
static size_t psp_media_probe_heap_capacity(void)
{
    void *blocks[PSP_MEDIA_POOL_PROBE_BLOCKS];
    size_t taken = 0;
    while (taken < PSP_MEDIA_POOL_PROBE_BLOCKS) {
        blocks[taken] = malloc(PSP_MEDIA_POOL_PROBE_BLOCK_BYTES);
        if (blocks[taken] == NULL) break;
        taken++;
    }
    /* Ascending, so the blocks coalesce back into one run rather than leaving
       the reservation to squeeze past a freed hole. */
    for (size_t index = 0; index < taken; index++) free(blocks[index]);
    return taken * PSP_MEDIA_POOL_PROBE_BLOCK_BYTES;
}

/*
 * Reserve the Media Engine pool. Called once, early in boot, before the
 * browser has allocated anything large enough to push the heap cursor toward
 * the extra-RAM bank.
 *
 * Admission: the reservation is taken only where it is both needed and
 * affordable, and the measurement that decides it has to be of the heap,
 * because the heap is where memalign takes it from. The first version of this
 * gate asked sceKernelTotalFreeMemSize instead and shipped inert: newlib has
 * already claimed the partition by the time main() runs, so the device
 * answered 2,326,528 bytes, the reservation was refused on every boot, and the
 * hardware log recorded `me-pool base=0x00000000` -- the pool the extra-memory
 * unlock made necessary, never once taken on a machine with the unlock.
 *
 * Budget accounting: the reservation is intentionally NOT charged to the
 * engine Budget here. These are the same bytes the media backend has always
 * taken when a video opens, and they are still charged there, through the
 * existing external reservation in media_psp_backend_create_split. Moving the
 * charge to boot would permanently remove several megabytes from the 24 MiB
 * content ceiling for every page whether or not a video is ever played, which
 * is a budget change, not a memory change -- and this commit is only moving
 * where in physical memory the buffers live.
 */
void media_psp_backend_reserve_pool(void)
{
    if (psp_media_pool.base != NULL) return;
    /* The heap is what the memalign below draws from, so the heap is what
       decides whether the reservation is affordable. The partition figure is
       still reported -- it is the number every other boot line quotes -- but
       after PSPSDK has given newlib the partition it is a constant ~2.2 MB
       and cannot admit anything. */
    size_t capacity_bytes = psp_media_probe_heap_capacity();
    int free_bytes = sceKernelTotalFreeMemSize();
    bool admitted = psp_media_pool_reservation_admitted(capacity_bytes);
    void *base = admitted
        ? memalign(PSP_MEDIA_DDR_ALIGNMENT, PSP_MEDIA_POOL_BYTES) : NULL;
    psp_media_pool_init(&psp_media_pool, base, PSP_MEDIA_POOL_BYTES);
    bool high = psp_media_me_invisible((uintptr_t) base);
    psp_media_log(
        "tilefinch-media-decoder: event=me-pool base=0x%08X bytes=%u high=%d "
        "heap=%u free=%u",
        (unsigned) (uintptr_t) base,
        (unsigned) (base == NULL ? 0u : PSP_MEDIA_POOL_BYTES),
        high ? 1 : 0, (unsigned) capacity_bytes, (unsigned) free_bytes);
    if (base == NULL || high) {
        /* Not a boot failure: the ordinary heap path still runs, and the
           me-memory line will report exactly where each buffer landed. A heap
           too small to admit the reservation is also one that fits entirely
           inside the stock partition, whose every address the Media Engine
           can already reach. */
        psp_media_log(
            "tilefinch-media-decoder: event=me-pool-unusable reason=%s "
            "base=0x%08X limit=0x%08X heap=%u free=%u largest=%u",
            !admitted ? "stock-heap"
                      : base == NULL ? "allocation-failed" : "above-limit",
            (unsigned) (uintptr_t) base,
            (unsigned) PSP_MEDIA_ME_VISIBLE_LIMIT,
            (unsigned) capacity_bytes,
            (unsigned) free_bytes,
            (unsigned) sceKernelMaxFreeMemSize());
    }
    /* Reported, not decided on: a shipping build compiles both lines above
       away and would otherwise carry an unused partition reading. */
    (void) free_bytes;
    psp_media_commit_log();
}

void media_psp_backend_system_suspend(void)
{
    /* A quarantined codec worker may still be inside libmpeg firmware. Do
       not tear down process-global MPEG state underneath that call. */
    if (psp_media_backend_is_quarantined) {
        psp_media_log(
            "tilefinch-media-decoder: event=system-suspend "
            "mpeg-finish=0 reason=codec-quarantine");
        return;
    }
    bool finished_runtime = psp_mpeg_runtime_initialized;
    if (finished_runtime) sceMpegFinish();
    /* Firmware may reset the ME program while suspended, but that boundary
       is not specified well enough to assume either the process-start type 3
       or the pre-suspend stream's type survived. Process statics do survive.
       Mark the program unknown so the first resumed stream explicitly selects
       its required type instead of accidentally skipping a transition. */
    psp_mpeg_runtime_initialized = false;
    psp_media_me_boot_type = -1;
    psp_media_me_boot_resolved_nid = 0;
    psp_media_log(
        "tilefinch-media-decoder: event=system-suspend "
        "mpeg-finish=%d",
        finished_runtime ? 1 : 0);
    (void) finished_runtime;
}

static const char *psp_media_module_failure_code(const char *stage)
{
    if (stage == NULL) return "??";
    if (strcmp(stage, "load-avcodec") == 0) return "AC";
    if (strcmp(stage, "load-mpeg-vsh") == 0) return "ML";
    if (strcmp(stage, "start-mpeg-vsh") == 0) return "MS";
    if (strcmp(stage, "create-module-worker") == 0) return "WC";
    if (strcmp(stage, "start-module-worker") == 0) return "WS";
    if (strcmp(stage, "poll-module-worker") == 0) return "WP";
    if (strcmp(stage, "delete-module-worker") == 0) return "WD";
    return "??";
}

static void psp_media_module_error(
    char *error, size_t error_size, const char *stage, int status)
{
    /* The media overlay draws only its first twenty characters.  Put a
       stable stage and complete status word first, then retain the readable
       stage for validation logs and future wider surfaces. */
    psp_media_error(
        error, error_size, "AV %s %08X (%s)",
        psp_media_module_failure_code(stage),
        (unsigned) status, stage == NULL ? "unknown" : stage);
}

static uint32_t psp_media_me_boot_nid(uint32_t devkit_version)
{
    if (devkit_version < UINT32_C(0x03070000)) return UINT32_C(0x47DB48C2);
    if (devkit_version < UINT32_C(0x03080000)) return UINT32_C(0xC287AD90);
    if (devkit_version < UINT32_C(0x03090500)) return UINT32_C(0xD857CF93);
    if (devkit_version < UINT32_C(0x05000000)) return UINT32_C(0x8988AD49);
    if (devkit_version < UINT32_C(0x06020000)) return UINT32_C(0x051C1601);
    if (devkit_version < UINT32_C(0x06030500)) return UINT32_C(0x3A2E60BB);
    if (devkit_version < UINT32_C(0x06060000)) return UINT32_C(0x99E4DBFA);
    return UINT32_C(0x5DFF5C50);
}

/*
 * Select the Media Engine's codec program, PMPlayer's me_boot_start through a
 * user-mode bridge: resolve sceMeBootStart for the running firmware and call
 * it with kubridge.
 *
 * The short-circuit is PMPlayer's and stays PMPlayer's -- but it only ever
 * skips a type a real call already put on the engine, because
 * psp_media_me_boot_type starts unknown rather than assuming type 3 is
 * running. Every path that can invalidate the engine's program (suspend, a
 * forced MPEG runtime restart, a teardown that finished the runtime) already
 * writes -1, so each of those correctly earns another real call.
 */
static int psp_media_boot_media_engine(
    int boot_type, uint32_t *resolved_nid)
{
    if (boot_type == psp_media_me_boot_type) {
        if (resolved_nid != NULL)
            *resolved_nid = psp_media_me_boot_resolved_nid;
        return 0;
    }
    static const uint32_t compatibility_nids[] = {
        UINT32_C(0x5DFF5C50), UINT32_C(0x99E4DBFA),
        UINT32_C(0x3A2E60BB), UINT32_C(0x051C1601),
        UINT32_C(0x8988AD49), UINT32_C(0xD857CF93),
        UINT32_C(0xC287AD90), UINT32_C(0x47DB48C2)
    };
    uint32_t preferred = psp_media_me_boot_nid(sceKernelDevkitVersion());
    uint32_t address = sctrlHENFindFunction(
        "sceMeCodecWrapper", "sceMeCore_driver", preferred);
    if (!psp_kernel_callable_address(address)) address = 0;
    uint32_t nid = preferred;
    for (size_t at = 0;
         address == 0
         && at < sizeof(compatibility_nids) / sizeof(compatibility_nids[0]);
         at++) {
        if (compatibility_nids[at] == preferred) continue;
        address = sctrlHENFindFunction(
            "sceMeCodecWrapper", "sceMeCore_driver",
            compatibility_nids[at]);
        if (psp_kernel_callable_address(address)) {
            nid = compatibility_nids[at];
        } else {
            address = 0;
        }
    }
    if (address == 0) return -1;
    KernelCallArg arguments;
    memset(&arguments, 0, sizeof(arguments));
    arguments.arg1 = (uint32_t) boot_type;
    int status = kuKernelCall(
        (void *) (uintptr_t) address, &arguments);
    if (status >= 0) status = (int) arguments.ret1;
    if (status >= 0) {
        psp_media_me_boot_type = boot_type;
        psp_media_me_boot_resolved_nid = nid;
        if (resolved_nid != NULL) *resolved_nid = nid;
    }
    return status;
}

static int psp_media_module_loader(
    SceSize argument_size, void *arguments)
{
    (void) argument_size;
    (void) arguments;
    int status = 0;
    if (!psp_avcodec_module_loaded) {
        psp_media_module_failure_stage = "load-avcodec";
        PspUtilityModuleLoadDisposition disposition =
            psp_utility_load_av_module(
                PSP_AV_MODULE_AVCODEC, &status);
        psp_media_log(
            "tilefinch-media-modules: stage=load-avcodec status=0x%08X "
            "free=%u largest=%u",
            (unsigned) status, sceKernelTotalFreeMemSize(),
            sceKernelMaxFreeMemSize());
        if (disposition != PSP_UTILITY_MODULE_LOAD_FAILED) {
            psp_avcodec_module_loaded = true;
            status = 0;
        }
    }
    if (status >= 0 && !psp_mpeg_vsh_module_loaded) {
        psp_media_module_failure_stage = "load-mpeg-vsh";
        /*
         * The public MPEGBASE utility module does not implement the raw MP4
         * NAL bridge. Proven device players load the VSH MPEG module through
         * KUBridge, and do so off the main/GU thread.
         */
        if (psp_mpeg_vsh_module < 0) {
            psp_mpeg_vsh_module = kuKernelLoadModule(
                "flash0:/kd/mpeg_vsh.prx", 0, NULL);
            psp_media_log(
                "tilefinch-media-modules: stage=load-mpeg-vsh "
                "status=0x%08X devkit=0x%08X free=%u largest=%u",
                (unsigned) psp_mpeg_vsh_module,
                (unsigned) sceKernelDevkitVersion(),
                sceKernelTotalFreeMemSize(), sceKernelMaxFreeMemSize());
        }
        if (psp_kernel_module_already_resident(
                psp_mpeg_vsh_module)) {
            /* Exclusive-load means this process already has the singleton
               firmware module.  Its exports are therefore resident and a
               second start has no module id to target. */
            psp_mpeg_vsh_module_loaded = true;
            psp_mpeg_vsh_module = -1;
            status = 0;
        } else if (psp_mpeg_vsh_module >= 0) {
            int module_status = 0;
            psp_media_module_failure_stage = "start-mpeg-vsh";
            status = sceKernelStartModule(
                psp_mpeg_vsh_module, 0, NULL, &module_status, NULL);
            psp_media_log(
                "tilefinch-media-modules: stage=start-mpeg-vsh "
                "status=0x%08X module-status=0x%08X uid=%d "
                "free=%u largest=%u",
                (unsigned) status, (unsigned) module_status,
                (int) psp_mpeg_vsh_module,
                sceKernelTotalFreeMemSize(), sceKernelMaxFreeMemSize());
            if (psp_kernel_module_start_succeeded(
                    status, module_status)) {
                psp_mpeg_vsh_module_loaded = true;
                status = 0;
            } else if (status >= 0) {
                /* Preserve the module entry point's failure, not the
                   successful module-manager syscall result.  Retaining the
                   UID lets an explicit retry start the same loaded module
                   rather than turning it into a false exclusive-load hit. */
                status = module_status;
            }
        } else {
            status = psp_mpeg_vsh_module;
        }
    }
    /* AAC is decoded through sceAudiocodec, exported by AVCodec above.
       PSP_AV_MODULE_AAC is the separate high-level sceAac library and is not
       a dependency of this backend.  Real 6.6x firmware can reject that
       redundant utility load with SCE_KERNEL_ERROR_ILLEGAL_ARGUMENT even
       though emulators accept it.  Proven sceAudiocodec players likewise
       load AVCodec only. */
    if (status >= 0) psp_media_module_failure_stage = "none";
    psp_media_log(
        "tilefinch-media-modules: stage=complete status=0x%08X "
        "avcodec=%d mpeg-vsh=%d aac-path=audiocodec free=%u largest=%u",
        (unsigned) status, psp_avcodec_module_loaded ? 1 : 0,
        psp_mpeg_vsh_module_loaded ? 1 : 0,
        sceKernelTotalFreeMemSize(), sceKernelMaxFreeMemSize());
    return status;
}

MediaPspPrepareResult media_psp_backend_prepare_pump(
    char *error, size_t error_size)
{
    if (error != NULL && error_size != 0) error[0] = '\0';
    if (psp_media_modules_ready) return MEDIA_PSP_PREPARE_READY;
    if (psp_media_module_failure < 0) {
        psp_media_module_error(
            error, error_size, psp_media_module_failure_stage,
            psp_media_module_failure);
        return MEDIA_PSP_PREPARE_ERROR;
    }
    if (psp_media_module_thread < 0) {
        psp_media_module_failure_stage = "create-module-worker";
        psp_media_log(
            "tilefinch-media-modules: stage=create-module-worker "
            "free=%u largest=%u",
            sceKernelTotalFreeMemSize(), sceKernelMaxFreeMemSize());
        psp_media_module_thread = sceKernelCreateThread(
            "tilefinch_media_modules", psp_media_module_loader,
            TILEFINCH_PSP_THREAD_PRIORITY_MODULE_LOADER, 16u * 1024u,
            PSP_THREAD_ATTR_USER, NULL);
        if (psp_media_module_thread >= 0) {
            psp_media_module_failure_stage = "start-module-worker";
            int status = sceKernelStartThread(
                psp_media_module_thread, 0, NULL);
            if (status < 0) {
                sceKernelDeleteThread(psp_media_module_thread);
                psp_media_module_thread = status;
            }
        }
        if (psp_media_module_thread < 0) {
            /* Thread allocation/start failures are pressure-dependent.  Do
               not poison the process-wide module state: the 360p fallback
               and an explicit retry first destroy the demux/range pipeline,
               after which the same preparation may succeed. */
            psp_media_module_error(
                error, error_size, psp_media_module_failure_stage,
                psp_media_module_thread);
            return MEDIA_PSP_PREPARE_ERROR;
        }
        return MEDIA_PSP_PREPARE_PENDING;
    }
    /* A zero timeout pointer value is not a portable polling contract:
       real 6.6x firmware rejects sceKernelWaitThreadEnd(thid, &zero) with
       SCE_KERNEL_ERROR_ILLEGAL_ARGUMENT (0x800200D2). ReferThreadStatus is
       the actual nonblocking query and, unlike the old error path, never
       deletes a worker which may still be loading firmware modules. */
    PspThreadObservation observation;
    int referred = psp_thread_observe(
        psp_media_module_thread, &observation);
    PspModuleWorkerPollDisposition poll =
        psp_module_worker_poll_disposition(
            referred, observation.status);
    if (poll == PSP_MODULE_WORKER_POLL_PENDING) {
        return MEDIA_PSP_PREPARE_PENDING;
    }
    int status;
    if (referred < 0) {
        psp_media_module_failure_stage = "poll-module-worker";
        status = referred;
    } else {
        if (psp_module_worker_was_killed(observation.status)
            && observation.exit_status >= 0) {
            psp_media_module_failure_stage = "poll-module-worker";
            status = (int) PSP_MODULE_ERROR_THREAD_TERMINATED;
        } else {
            status = observation.exit_status;
        }
        int deleted = sceKernelDeleteThread(psp_media_module_thread);
        if (deleted < 0) {
            psp_media_module_failure_stage = "delete-module-worker";
            status = deleted;
        } else {
            psp_media_module_thread = -1;
        }
    }
    if (status < 0) {
        /* Module-manager busy and allocation failures can clear after the
           failed open releases its range/demux state.  Unsupported or
           missing firmware remains process-wide and is latched so repeated
           UI retries do not hammer flash/module management. */
        if (!psp_media_module_failure_retryable(status))
            psp_media_module_failure = status;
        psp_media_module_error(
            error, error_size, psp_media_module_failure_stage, status);
        psp_media_log(
            "tilefinch-media-modules: stage=%s status=0x%08X "
            "retryable=%d free=%u largest=%u",
            psp_media_module_failure_stage, (unsigned) status,
            psp_media_module_failure_retryable(status) ? 1 : 0,
            sceKernelTotalFreeMemSize(), sceKernelMaxFreeMemSize());
        return MEDIA_PSP_PREPARE_ERROR;
    }
    psp_media_modules_ready = true;
    psp_media_log("tilefinch-media-modules: stage=ready");
    return MEDIA_PSP_PREPARE_READY;
}

static void psp_media_error(char *error, size_t error_size,
                            const char *format, ...)
{
    if (error == NULL || error_size == 0) return;
    va_list arguments;
    va_start(arguments, format);
    vsnprintf(error, error_size, format, arguments);
    va_end(arguments);
}

static uint64_t psp_media_timestamp_us(uint64_t value, uint32_t timescale)
{
    if (timescale == 0) return 0;
    uint32_t scale_remainder = UINT32_C(1000000) % timescale;
    if (value <= UINT32_MAX
        && (uint64_t) (timescale - 1u) * scale_remainder
               <= UINT32_MAX) {
        uint32_t small = (uint32_t) value;
        uint32_t whole = small / timescale;
        uint32_t remainder = small % timescale;
        uint32_t scale_whole = UINT32_C(1000000) / timescale;
        return (uint64_t) whole * UINT32_C(1000000)
             + (uint64_t) remainder * scale_whole
             + (remainder * scale_remainder) / timescale;
    }
    uint64_t whole = value / timescale;
    uint64_t remainder = value % timescale;
    if (whole > UINT64_MAX / UINT64_C(1000000))
        return UINT64_MAX;
    uint64_t base = whole * UINT64_C(1000000);
    uint64_t fraction =
        remainder * UINT64_C(1000000) / timescale;
    return fraction > UINT64_MAX - base
        ? UINT64_MAX : base + fraction;
}

static uint64_t psp_sample_pts_us(const MediaMp4Sample *sample)
{
    uint64_t pts = sample->pts < 0 ? 0 : (uint64_t) sample->pts;
    return psp_media_timestamp_us(pts, sample->timescale);
}

static uint64_t psp_sample_duration_us(const MediaMp4Sample *sample)
{
    return psp_media_timestamp_us(sample->duration, sample->timescale);
}

static bool psp_avcc_sets(const MediaMp4TrackInfo *track,
                          PspAvcNal *nal)
{
    const unsigned char *data = track->codec_config;
    size_t length = track->codec_config_length;
    if (data == NULL || length < 7u || data[0] != 1) return false;
    nal->nal_prefix_size = (data[4] & 3) + 1;
    unsigned sps_count = data[5] & 31u;
    size_t cursor = 6;
    /* The private raw-NAL bridge accepts one SPS and one PPS pointer. Do not
       admit an avcC whose later samples may select a second parameter-set id
       which was never supplied to firmware. Ordinary YouTube AVC renditions
       carry exactly one of each. */
    if (sps_count != 1u || cursor + 2u > length) return false;
    size_t sps_length = ((size_t) data[cursor] << 8) | data[cursor + 1];
    cursor += 2;
    if (sps_length == 0 || sps_length > length - cursor) return false;
    nal->sps_buffer = (void *) (data + cursor);
    nal->sps_size = (int) sps_length;
    cursor += sps_length;
    if (cursor >= length) return false;
    unsigned pps_count = data[cursor++];
    if (pps_count != 1u || cursor + 2u > length) return false;
    size_t pps_length = ((size_t) data[cursor] << 8) | data[cursor + 1];
    cursor += 2;
    if (pps_length == 0 || pps_length > length - cursor) return false;
    nal->pps_buffer = (void *) (data + cursor);
    nal->pps_size = (int) pps_length;
    return true;
}

static bool psp_media_copy_parameter_sets(PspMediaBackend *backend)
{
    if (backend == NULL) return false;
    PspAvcNal source;
    memset(&source, 0, sizeof(source));
    if (!psp_avcc_sets(&backend->video, &source)
        || source.sps_size <= 0 || source.pps_size <= 0
        || (size_t) source.sps_size > SIZE_MAX - (size_t) source.pps_size)
        return false;
    size_t bytes = (size_t) source.sps_size + (size_t) source.pps_size;
    /*
     * The Media Engine consumes SPS/PPS through DMA. The mature raw-MP4
     * implementation copies both sets into one cache-line-aligned allocation;
     * pointing into avcC leaves the first SPS at an unaligned +8 offset and is
     * observably less strict in emulators than on hardware.
     */
    void *sets = psp_media_alloc64(bytes);
    if (sets == NULL) return false;
    memcpy(sets, source.sps_buffer, (size_t) source.sps_size);
    memcpy((unsigned char *) sets + source.sps_size,
           source.pps_buffer, (size_t) source.pps_size);
    backend->video_parameter_sets = sets;
    backend->video_parameter_sets_bytes = bytes;
    backend->video_sps_bytes = source.sps_size;
    backend->video_pps_bytes = source.pps_size;
    return true;
}

/*
 * Complete the firmware decode transaction even when it produced no picture.
 *
 * PMPlayer Advance's hardware-proven raw-NAL path calls DecodeDetail2 after
 * every successful sceMpegAvcDecode and only gates CSC on the returned picture
 * count.  Tilefinch used to skip this call for a zero-picture decode.  That is
 * not an inert optimization: it leaves the firmware's per-AU detail step
 * unobserved before the next AU is submitted, which matches the recurring
 * refusal/wedge pattern seen only on real hardware.
 */
static int psp_media_query_avc_detail(
    PspMediaBackend *backend, int picture_count, PspAvcDetail2 **result,
    char *error, size_t error_size)
{
    PspAvcDetail2 *detail = NULL;
    int status = sceMpegAvcDecodeDetail2(&backend->mpeg, &detail);
    if (status < 0) {
        psp_media_log_failure(
            backend, "sceMpegAvcDecodeDetail2", status);
        psp_media_error(
            error, error_size,
            "PSP AVC detail failed: 0x%08X", (unsigned) status);
        return status;
    }
    /* A zero-picture result has no arrays to consume. The call itself is the
       contract; firmware is allowed to publish no detail pointer here. */
    if (picture_count == 0) {
        if (result != NULL) *result = detail;
        return 0;
    }
    if (detail != NULL) {
        sceKernelDcacheWritebackInvalidateRange(
            detail, psp_media_cache_extent(sizeof(*detail)));
    }
    if (detail == NULL || detail->info_buffer == NULL
        || detail->yuv_buffer == NULL) {
        psp_media_log_failure(
            backend, "sceMpegAvcDecodeDetail2-output", -1);
        psp_media_error(error, error_size,
                        "PSP AVC returned no YUV picture");
        return -1;
    }
    sceKernelDcacheWritebackInvalidateRange(
        detail->info_buffer,
        psp_media_cache_extent(
            (size_t) picture_count * sizeof(*detail->info_buffer)));
    sceKernelDcacheWritebackInvalidateRange(
        detail->yuv_buffer,
        psp_media_cache_extent(
            (size_t) picture_count * sizeof(*detail->yuv_buffer)));
    if (result != NULL) *result = detail;
    return 0;
}

static int psp_media_capture_avc_pictures(
    PspMediaBackend *backend, int picture_count, PspAvcDetail2 *detail,
    char *error, size_t error_size)
{
    /* Pictures the engine was still holding when a no-touch reposition moved
       the stream under it. Zero in every other mode and outside the window. */
    unsigned stale = psp_media_reposition_stale_pictures(
        backend->video_reposition_drain_units, picture_count,
        backend->video_timestamps.count);
    /*
     * And outside that window, an over-long batch is no longer the end of the
     * session.
     *
     * The cause is understood -- a refused access unit whose picture firmware
     * kept while we discarded its timestamp -- and the branch above now keeps
     * that timestamp, so this should not fire. It stays as defence in depth
     * because the failure it replaces was severe out of all proportion to the
     * damage: one unpairable picture ended a run the Media Engine had not
     * failed. Skipping it costs a frame. So it is bounded the way a refusal is
     * bounded, by the same consecutive-and-window rule, and only a stream that
     * keeps doing it is still fatal.
     */
    if (stale == 0) {
        unsigned unpaired = psp_media_unpaired_pictures(
            picture_count, backend->video_timestamps.count);
        if (unpaired != 0) {
            uint64_t now_us = (uint64_t) sceKernelGetSystemTimeWide();
            if (backend->video_batch_overrun_window_started_us == 0
                || now_us - backend->video_batch_overrun_window_started_us
                       >= PSP_MEDIA_AVC_REFUSAL_WINDOW_US) {
                backend->video_batch_overrun_window_started_us = now_us;
                backend->video_batch_overrun_window_count = 0;
            }
            backend->video_batch_overruns_consecutive++;
            backend->video_batch_overruns_total++;
            backend->video_batch_overrun_window_count++;
            bool survivable = psp_media_avc_refusal_survivable(
                backend->video_batch_overruns_consecutive,
                backend->video_batch_overrun_window_count);
            psp_media_log(
                "tilefinch-media-decoder: event=batch-overrun batch=%d "
                "timestamps=%u unpaired=%u consecutive=%u total=%u window=%u "
                "survivable=%d",
                picture_count, backend->video_timestamps.count, unpaired,
                backend->video_batch_overruns_consecutive,
                backend->video_batch_overruns_total,
                backend->video_batch_overrun_window_count,
                survivable ? 1 : 0);
            if (survivable) stale = unpaired;
        }
    }
    if (!psp_media_output_batch_admitted(
            picture_count - (int) stale, backend->video_timestamps.count)) {
        psp_media_log_failure(
            backend, "decode-picture-count", picture_count);
        psp_media_error(
            error, error_size,
            "PSP AVC returned invalid picture batch %d/%u",
            picture_count, backend->video_timestamps.count);
        return -1;
    }
    if (stale != 0) {
        psp_media_log(
            "tilefinch-media-decoder: event=reposition-stale-pictures "
            "skipped=%u batch=%d timestamps=%u drain-units=%u",
            stale, picture_count, backend->video_timestamps.count,
            backend->video_reposition_drain_units);
    } else {
        /* A batch that pairs without help means the engine has released
           everything it was holding; the window has done its job, and any run
           of unpairable batches is over too. */
        backend->video_reposition_drain_units = 0u;
        backend->video_batch_overruns_consecutive = 0;
    }
    /*
     * Written by firmware inside sceMpegAvcDecodeDetail2, which runs on the
     * main CPU: the descriptor and the picture arrays it points at are filled
     * through the CPU's own cache for anything the query assembles, and by
     * the Media Engine for anything the decode left in RAM. A pure invalidate
     * would discard the CPU-side half before it ever reached memory, exactly
     * the way it discarded the audio codec block's needed-size word, and the
     * refetch would then read whatever the workspace held before the call.
     * Write the dirty lines back first and drop them, so both writers are
     * observed. Nothing here can be dirty from our own stores -- the bridge
     * below writeback-invalidates the whole cache before entering firmware --
     * so the writeback half cannot overwrite Media Engine output.
     */
    backend->video_detail = detail;
    backend->video_picture_count = (unsigned) picture_count;
    /* Start past the held pictures rather than at the front: skipping them is
       what keeps a post-reposition timestamp from being spent on content from
       before it. Zero unless a no-touch reposition is draining. */
    backend->video_picture_index = stale;
    if ((unsigned) picture_count
        > backend->maximum_video_picture_batch) {
        backend->maximum_video_picture_batch =
            (unsigned) picture_count;
        psp_media_log(
            "tilefinch-media-decoder: output-batch=%u "
            "queued-timestamps=%u\n",
            backend->maximum_video_picture_batch,
            backend->video_timestamps.count);
    }
    return 0;
}

static int psp_media_convert_avc_picture(
    PspMediaBackend *backend, unsigned picture_index, unsigned write_slot,
    char *error, size_t error_size)
{
    if (write_slot >= PSP_MEDIA_SURFACE_SLOTS) {
        psp_media_error(error, error_size,
                        "PSP AVC conversion has no output slot");
        return -1;
    }
    if (backend->video_detail == NULL
        || picture_index >= backend->video_picture_count) {
        psp_media_log_failure(
            backend, "picture-batch-state", -1);
        psp_media_error(error, error_size,
                        "PSP AVC picture batch unavailable");
        return -1;
    }
    const PspAvcInfo *info =
        backend->video_detail->info_buffer + picture_index;
    const PspAvcYuv *yuv =
        backend->video_detail->yuv_buffer + picture_index;
    /*
     * Firmware reports the CODED picture size; decoded_width/decoded_height
     * are the cropped DISPLAY size the SPS authored. A 426x240 stream is coded
     * as 432x240, so demanding equality here rejected every picture of it as a
     * mid-stream geometry change. Expect the macroblock-padded size instead --
     * the same rounding the CSC block counts below already perform -- which
     * still refuses a real geometry change, because that moves the coded size
     * too.
     */
    if (!psp_media_decoded_geometry_admitted(
            info->width, info->height,
            backend->decoded_width, backend->decoded_height)) {
        psp_media_log_failure(
            backend, "decoded-geometry", -1);
        psp_media_error(
            error, error_size,
            "PSP AVC output geometry changed to %dx%d",
            (int) info->width, (int) info->height);
        return -1;
    }
    /*
     * The byte order this call writes is the one unresolved fact in the video
     * present path.
     *
     * The software scaler and graphics engine now both follow PSP-native
     * channel order (byte 0 -> the low red field), measured by
     * tilefinch-media-present-probe: channel-map. This call is still where the
     * firmware decides the decoded surface's byte order.
     *
     * mode0/mode1 are transcribed as zero from the mature raw-MP4 player this
     * backend follows, and no reading of firmware available here says what a
     * nonzero value means. A guess in these two words is a guess in a call that
     * writes half a megabyte of Media Engine memory, so they stay as the
     * working player leaves them until the device has answered what they select.
     */
    PspAvcCsc csc = {
        .height = (info->height + 15) >> 4,
        .width = (info->width + 15) >> 4,
        .mode0 = 0,
        .mode1 = 0
    };
    for (unsigned i = 0; i < 8u; i++) {
        if (yuv->buffer[i] == NULL) {
            psp_media_log_failure(
                backend, "decoded-yuv-planes", -1);
            psp_media_error(error, error_size,
                            "PSP AVC returned incomplete YUV planes");
            return -1;
        }
        csc.buffer[i] = yuv->buffer[i];
    }
#ifdef TILEFINCH_PSP_VALIDATION_LOG
    backend->csc_probe_request = csc;
    backend->csc_probe_request_valid = true;
#endif
    /*
     * CSC is a Media Engine write, and the surface is the one range in this
     * file the engine writes exclusively: the only CPU store into it is the
     * extent canary, which is published with a writeback of its own on the
     * submit path, and the writeback-invalidate below then leaves no dirty
     * line anywhere in the range. That is what makes the pure invalidate
     * after the call legitimate here -- it discards nothing, because there is
     * nothing of the CPU's left to discard, and it is the only way to see
     * what the engine put in RAM. The firmware API validates a user
     * destination pointer; mature PMPlayer/OpenTube implementations pass the
     * ordinary cached address here rather than an uncached alias.
     *
     * The range is this slot's and only this slot's. Two surfaces sit in the
     * same pool a few hundred kilobytes apart, so a length taken from the
     * wrong one would reach into its neighbour -- which is a claimed picture
     * whenever the pipeline is doing its job.
     */
    uint32_t *surface = backend->surfaces[write_slot];
    sceKernelDcacheWritebackInvalidateRange(
        surface, backend->surface_bytes);
    sceKernelDcacheWritebackRange(&csc, sizeof(csc));
    /*
     * The conversion itself, timed apart from everything in front of it.
     *
     * Until now the only measurement covering this call was the
     * decode-to-conversion interval, which is dominated by the WAIT for a
     * writable slot -- so a conversion that is slow and a conversion that
     * waited a long time to start produced the same number. They have nothing
     * in common: one is a Media Engine cost and the other is an ownership
     * cost, and the whole question of whether the conversion cadence limits
     * the rate turns on which of the two this is.
     */
    uint32_t csc_started_us = psp_media_stamp_us();
    int status = sceMpegBaseCscAvc(
        surface, NULL, (int) backend->frame_stride, &csc);
    uint32_t csc_us = psp_media_stamp_us() - csc_started_us;
    backend->stats.video_csc_calls++;
    backend->stats.video_csc_total_us += (uint64_t) csc_us;
    if (csc_us > backend->stats.video_csc_max_us)
        backend->stats.video_csc_max_us = csc_us;
    /* And where the browser thread was standing while it ran, which decides
       whether the take that wants this picture is the next one or the one
       after it. */
    backend->stats.video_csc_phase[
        psp_media_loop_phase_resolve(
            atomic_load_explicit(
                &psp_media_loop_phase, memory_order_relaxed),
            psp_media_advance_mode)]++;
    if (status >= 0) {
        sceKernelDcacheInvalidateRange(
            surface, backend->surface_bytes);
    }
    if (status < 0) {
        psp_media_log_failure(
            backend, "sceMpegBaseCscAvc", status);
        psp_media_error(
            error, error_size,
            "PSP AVC color conversion failed: 0x%08X",
            (unsigned) status);
    }
    return status;
}

/*
 * Firmware has just returned a picture batch. Close segment one and record how
 * many pictures this decode owes the surface.
 *
 * The count is the reason a batch matters: the surface holds one picture, so a
 * decode that returns three needs two further submit or drain visits before the
 * next access unit can even be offered. Runs on the codec worker, which owns
 * every field it touches until the job is published.
 */
static void psp_media_note_picture_batch(
    PspMediaBackend *backend, unsigned pictures)
{
    backend->video_decode_done_us = psp_media_stamp_us();
#if defined(TILEFINCH_PSP_VALIDATION_LOG)
    psp_media_trace_next_batch++;
    psp_media_trace_batch_au = psp_media_trace_open_au;
    psp_media_trace_batch_submit_us = psp_media_trace_open_submit_us;
#endif
    uint32_t decode_us = backend->video_decode_done_us
        - atomic_load_explicit(
            &backend->codec_job_started_us, memory_order_acquire);
    backend->stats.video_decode_jobs++;
    backend->stats.video_decode_total_us += decode_us;
    if (decode_us > backend->stats.video_decode_max_us)
        backend->stats.video_decode_max_us = decode_us;
    backend->stats.video_batches++;
    backend->stats.video_batch_pictures += pictures;
    if (pictures > 1u) backend->stats.video_batch_multi++;
    if (pictures > backend->stats.video_batch_max)
        backend->stats.video_batch_max = pictures;
    /* A new batch. The conversion gap is measured WITHIN one -- across two,
       the interval is a whole decode and says nothing about the fill. */
    backend->batch_convert_us = 0u;
    backend->stats.batch_fill_jobs++;
}

/*
 * Convert one captured picture into a slot nobody owns.
 *
 * `epoch` is the caller's own -- the worker passes the copy stamped into its
 * job, the browser thread passes the session's -- so a conversion performed
 * for a stream that has since been sought away from stamps the slot with the
 * epoch it belonged to and is refused by every later reader, rather than
 * appearing as a picture of the current stream.
 */
static int psp_media_emit_captured_picture(
    PspMediaBackend *backend, unsigned write_slot, uint64_t epoch,
    char *error, size_t error_size)
{
    if (backend == NULL
        || backend->video_picture_index
           >= backend->video_picture_count) {
        psp_media_error(error, error_size,
                        "PSP AVC picture batch exhausted");
        return -1;
    }
    if (write_slot >= PSP_MEDIA_SURFACE_SLOTS
        || psp_media_slot_observe(&backend->slots[write_slot])
           != PSP_MEDIA_SLOT_FREE) {
        /* The one invariant this function cannot be allowed to break. A
           conversion into anything but a FREE slot is a write over a picture
           somebody still owns, which is a silent substitution rather than a
           fault -- so it is refused here as well as at every call site. */
        psp_media_error(error, error_size,
                        "PSP AVC conversion target was not free");
        return -1;
    }
    /* Where this picture sits in its batch, read before the emit retires the
       batch counters below. */
#if defined(TILEFINCH_PSP_VALIDATION_LOG)
    unsigned batch_index = backend->video_picture_index;
    unsigned batch_count = backend->video_picture_count;
#endif
    PspMediaVideoTimestamp timestamp = {0};
    if (!psp_media_timestamp_pop(
            &backend->video_timestamps, &timestamp)) {
        psp_media_error(error, error_size,
                        "PSP AVC picture timestamp queue empty");
        return -1;
    }
    PspMediaSurfaceSlot *state = &backend->slots[write_slot];
    /* The Media Engine owns it from here. Announced before the firmware call
       so a claim arriving mid-conversion is refused rather than granted, and
       the generation moves with the announcement so a reader holding the
       previous picture's pair can tell.

       The generation and the epoch are written first and the state published
       after them, which is the rule every transition in this file follows: a
       thread that observes the new state must already be able to see the
       fields that state describes. */
    state->generation++;
    state->epoch = epoch;
    /* Publish enough ordering metadata with ME_WRITING for a concurrent take
       to tell whether this conversion precedes an already-READY picture. The
       values are repeated after success for clarity; a failed conversion
       returns the slot to FREE and they are never consumed. */
    state->sequence = backend->frame_sequence + UINT64_C(1);
    state->pts_us = timestamp.pts_us;
    state->duration_us = timestamp.duration_us;
    psp_media_slot_charge(backend, write_slot);
    psp_media_slot_publish(state, PSP_MEDIA_SLOT_ME_WRITING);
    psp_media_slot_sample(backend);
    int status = psp_media_convert_avc_picture(
        backend, backend->video_picture_index, write_slot,
        error, error_size);
    if (status < 0 || !psp_media_validate_output_surface(
            backend, write_slot, error, error_size)) {
        /* Nothing usable landed, so the slot goes back rather than staying
           half-owned. The generation is not rewound: it already moved, and a
           reader that captured it must still be told the picture is gone.

           Erased before it is published, not after: FREE is what invites the
           next conversion in, and a clear that landed behind that invitation
           would wipe a field of the picture replacing this one. */
        state->identity = 0;
        psp_media_slot_charge(backend, write_slot);
        psp_media_slot_publish(state, PSP_MEDIA_SLOT_FREE);
        psp_media_slot_sample(backend);
        return status < 0 ? status : -1;
    }
    backend->video_picture_index++;
    if (backend->video_picture_index
        == backend->video_picture_count) {
        backend->video_detail = NULL;
        backend->video_picture_count = 0;
        backend->video_picture_index = 0;
    }
    backend->frame_identity++;
    backend->frame_sequence++;
    state->identity = backend->frame_identity;
    state->sequence = backend->frame_sequence;
    state->pts_us = timestamp.pts_us;
    state->duration_us = timestamp.duration_us;
    state->signature = psp_media_surface_signature(
        backend->surfaces[write_slot], backend->decoded_width,
        backend->decoded_height, backend->frame_stride);
    backend->stats.decoded_video_frames++;
    backend->stats.video_slot_conversions[write_slot]++;
    /* Segment two closes here: firmware returned this batch at
       video_decode_done_us and this picture is only now in the surface. For
       the picture the worker converts inside its own decode job that interval
       is the conversion itself; for every surplus picture of a batch it is the
       wait for a submit or drain visit that was allowed to emit. */
    state->emitted_us = psp_media_stamp_us();
    /*
     * And it is a picture anybody may claim from here. Published last, after
     * every field that describes it, so a claim can never observe READY over
     * metadata still being written.
     *
     * This release store is the ONE that orders this whole function against
     * the browser thread. It used to be backed up by the codec job handoff --
     * the browser refused every take for the length of the job, then acquired
     * DONE before reading any slot -- and that gate is gone, so the pairing is
     * this store against the acquire load in psp_media_slot_holds_picture and
     * nothing else. Everything above it must stay above it.
     */
    psp_media_slot_charge(backend, write_slot);
    psp_media_slot_publish(state, PSP_MEDIA_SLOT_READY);
    psp_media_slot_sample(backend);
    backend->stats.video_emits++;
    /*
     * And how long after the previous picture of THIS batch it landed.
     *
     * The fill-all-free-slots loop is supposed to convert a multi-picture
     * batch back to back, bounded only by the Media Engine. A gap far longer
     * than the conversion is a second picture that waited for a slot instead
     * -- which is the surplus-picture phase this pipeline has removed once
     * already, and the shape it would take if it came back.
     */
    if (backend->batch_convert_us != 0u) {
        uint32_t gap_us = state->emitted_us - backend->batch_convert_us;
        backend->stats.batch_gap_samples++;
        backend->stats.batch_gap_total_us += (uint64_t) gap_us;
        if (gap_us > backend->stats.batch_gap_max_us)
            backend->stats.batch_gap_max_us = gap_us;
    }
    backend->batch_convert_us = state->emitted_us;
    /* The first conversion after a refusal says the decoder is producing
       again -- not yet that anything was seen, which the display closes. */
    if (atomic_load_explicit(
            &psp_media_recovery_stage, memory_order_relaxed)
        == PSP_MEDIA_RECOVERY_FED) {
        atomic_fetch_add_explicit(
            &psp_media_recovery_csc_total_us,
            state->emitted_us - atomic_load_explicit(
                &psp_media_recovery_au_us, memory_order_relaxed),
            memory_order_relaxed);
        atomic_store_explicit(
            &psp_media_recovery_csc_us, state->emitted_us,
            memory_order_relaxed);
        atomic_store_explicit(
            &psp_media_recovery_stage, PSP_MEDIA_RECOVERY_CONVERTED,
            memory_order_relaxed);
        /* Media time the stream jumped, which is a property of where the
           recovery landed and not of how long it took. */
        if (psp_media_recovery_last_pts_us != 0
            && timestamp.pts_us > psp_media_recovery_last_pts_us) {
            atomic_fetch_add_explicit(
                &psp_media_recovery_skip_total_ms,
                (uint32_t) ((timestamp.pts_us
                             - psp_media_recovery_last_pts_us) / 1000u),
                memory_order_relaxed);
        }
    }
    psp_media_recovery_last_pts_us = timestamp.pts_us;
#if defined(TILEFINCH_PSP_VALIDATION_LOG)
    {
        PspMediaPictureTrace *record = &psp_media_trace[
            (uint32_t) (backend->frame_identity % PSP_MEDIA_TRACE_SLOTS)];
        *record = (PspMediaPictureTrace) {
            .identity = (uint32_t) backend->frame_identity,
            .au_id = psp_media_trace_batch_au,
            .batch_id = psp_media_trace_next_batch,
            .pts_ms = (uint32_t) (timestamp.pts_us / 1000u),
            .submit_us = psp_media_trace_batch_submit_us,
            .decode_us = backend->video_decode_done_us,
            .csc_us = state->emitted_us,
            .generation = state->generation,
            .signature = state->signature,
            .batch_index = (uint8_t) batch_index,
            .batch_count = (uint8_t) batch_count,
            .slot = (uint8_t) write_slot
        };
    }
#endif
    if (backend->video_decode_done_us != 0u) {
        uint32_t pending_us =
            state->emitted_us - backend->video_decode_done_us;
        backend->stats.video_emit_pending_total_us += pending_us;
        if (pending_us > backend->stats.video_emit_pending_max_us)
            backend->stats.video_emit_pending_max_us = pending_us;
    }
    return 0;
}

static bool psp_media_validate_output_surface(
    PspMediaBackend *backend, unsigned write_slot,
    char *error, size_t error_size)
{
    if (backend == NULL) return true;
    if (write_slot >= PSP_MEDIA_SURFACE_SLOTS) return false;
    PspMediaSurfaceSlot *state = &backend->slots[write_slot];
    if (state->extent_validated) {
        /*
         * A picture is still an answer to the probe, even when the extent
         * proof it also answers was taken long ago. The disarm used to sit
         * below this return, so a probe armed after that proof -- which every
         * seek did -- could never be retired by success. Only by failure: the
         * first reordered access unit that produced no picture.
         */
        backend->raw_nal_probe_pending = false;
        backend->raw_nal_probe_packets = 0;
        return true;
    }
    uint32_t *surface = backend->surfaces[write_slot];
    sceKernelDcacheWritebackInvalidateRange(
        surface, backend->surface_bytes);
    uint32_t *extent_canary = surface
        + (size_t) (backend->csc_rows - 1u)
            * backend->frame_stride;
    /*
     * The armed flag and the canary are both this slot's. Reading either from
     * a different slot would let a conversion that landed in the wrong surface
     * prove itself against a canary nothing had overwritten -- or, worse, be
     * proven by the canary its neighbour's conversion had already cleared.
     * That is the mis-slotted write this proof is now able to catch, and it
     * only catches it because both halves are indexed the same way.
     */
    if (!state->canary_armed
        || !psp_media_surface_canary_was_overwritten(
            extent_canary, backend->decoded_width)) {
        if (backend->raw_nal_probe_pending) {
            backend->stats.last_native_error =
                MEDIA_BACKEND_RAW_NAL_BRIDGE_UNAVAILABLE;
            psp_media_error(
                error, error_size,
                "PSP raw-NAL decoder unavailable in this environment");
        } else {
            backend->stats.last_native_error = -1;
            psp_media_error(
                error, error_size,
                "PSP AVC output did not cover the authored surface");
        }
        psp_media_log_failure(
            backend, "decoded-surface-proof",
            backend->stats.last_native_error);
        return false;
    }
    state->extent_validated = true;
    state->canary_armed = false;
    backend->raw_nal_probe_pending = false;
    backend->raw_nal_probe_packets = 0;
    return true;
}

/* Whether any slot has proven its extent. The raw-NAL probe asks a question
   about the firmware bridge, not about a surface, and one picture anywhere
   answers it; the per-slot proof above is the separate question of whether a
   conversion covered the slot it claimed to. */
static bool psp_media_surface_proven(const PspMediaBackend *backend)
{
    for (unsigned slot = 0; slot < PSP_MEDIA_SURFACE_SLOTS; slot++)
        if (backend->slots[slot].extent_validated) return true;
    return false;
}

static MediaBackendResult psp_media_raw_nal_probe_failed(
    PspMediaBackend *backend, char *error, size_t error_size)
{
    backend->stats.last_native_error =
        MEDIA_BACKEND_RAW_NAL_BRIDGE_UNAVAILABLE;
    psp_media_log_failure(
        backend, "raw-nal-no-picture",
        MEDIA_BACKEND_RAW_NAL_BRIDGE_UNAVAILABLE);
    psp_media_error(
        error, error_size,
        "PSP raw-NAL decoder unavailable in this environment");
    return MEDIA_BACKEND_ERROR;
}

static int psp_media_audio_thread(SceSize argument_size, void *arguments)
{
    PspMediaBackend *backend = NULL;
    if (arguments != NULL && argument_size == sizeof(backend))
        memcpy(&backend, arguments, sizeof(backend));
    if (backend == NULL) return -1;
    while (!backend->audio_stop) {
        uint32_t bits = 0;
        int waited = sceKernelWaitEventFlag(
            backend->audio_event,
            PSP_MEDIA_AUDIO_EVENT_READY | PSP_MEDIA_AUDIO_EVENT_STOP,
            PSP_EVENT_WAITOR | PSP_EVENT_WAITCLEAR, &bits, NULL);
        if (waited < 0) {
            atomic_store(
                &backend->audio_worker_stage,
                PSP_MEDIA_AUDIO_WORKER_WAIT);
            backend->audio_worker_error = waited;
            break;
        }
        if ((bits & PSP_MEDIA_AUDIO_EVENT_STOP) != 0
            || backend->audio_stop) break;
        if (!atomic_load(&backend->playing)
            || atomic_load(&backend->buffering)) continue;
        /*
         * A wake-up with nothing to output is the audible failure mode of a
         * decoder that cannot keep 43 AAC blocks a second in front of the
         * hardware: the queue empties, every later block plays alone, and the
         * user hears silence with clicks in it. Count it here -- the browser
         * thread cannot see an empty queue it never sampled -- and never log
         * from this thread.
         */
        if (atomic_load(&backend->audio_queue_read)
            == atomic_load(&backend->audio_queue_write)) {
            atomic_fetch_add(&backend->audio_output_starves, 1u);
        }
        while (backend->audio_queue_read != backend->audio_queue_write
               && !backend->audio_stop
               && atomic_load(&backend->playing)
               && !atomic_load(&backend->buffering)) {
            uint32_t generation =
                atomic_load(&backend->audio_queue_generation);
            unsigned slot = backend->audio_queue_read
                % PSP_MEDIA_AUDIO_QUEUE_SLOTS;
            if (atomic_load_explicit(
                    &backend->audio_resetting, memory_order_acquire)) break;
            atomic_store_explicit(
                &backend->audio_output_in_flight_slot,
                slot + 1u, memory_order_release);
            /* Close the race where reset starts after the loop condition but
               before ownership of this DMA buffer is published. */
            if (atomic_load_explicit(
                    &backend->audio_resetting, memory_order_acquire)
                || !atomic_load(&backend->playing)
                || atomic_load(&backend->buffering)) {
                atomic_store_explicit(
                    &backend->audio_output_in_flight_slot,
                    0, memory_order_release);
                break;
            }
            int status = backend->audio_channel == -2
                ? sceAudioSRCOutputBlocking(
                    PSP_AUDIO_VOLUME_MAX,
                    psp_media_audio_queue_slot(backend, slot))
                : sceAudioOutputBlocking(
                    backend->audio_channel, PSP_AUDIO_VOLUME_MAX,
                    psp_media_audio_queue_slot(backend, slot));
            atomic_store_explicit(
                &backend->audio_output_in_flight_slot,
                0, memory_order_release);
            /* Publish the first verdict either way: a refusal is the one
               result the decode-side counters can never express. */
            int expected_first = PSP_MEDIA_AUDIO_OUTPUT_PENDING;
            (void) atomic_compare_exchange_strong(
                &backend->audio_output_first_status, &expected_first, status);
            if (status < 0) {
                atomic_store(
                    &backend->audio_worker_stage,
                    PSP_MEDIA_AUDIO_WORKER_OUTPUT);
                backend->audio_worker_error = status;
                backend->audio_stop = true;
                break;
            }
            atomic_fetch_add(&backend->audio_output_blocks, 1u);
            uint32_t first_output = 0u;
            uint32_t output_us = psp_media_stamp_us();
            if (output_us == 0u) output_us = 1u;
            (void) atomic_compare_exchange_strong_explicit(
                &backend->audio_first_output_us, &first_output,
                output_us, memory_order_release, memory_order_relaxed);
            if (generation
                == atomic_load(&backend->audio_queue_generation))
                backend->audio_queue_read++;
        }
    }
    return 0;
}

#ifdef TILEFINCH_PSP_VALIDATION_LOG
static const char *psp_media_audio_worker_stage_name(int stage)
{
    if (stage == PSP_MEDIA_AUDIO_WORKER_WAIT)
        return "sceKernelWaitEventFlag-audio";
    if (stage == PSP_MEDIA_AUDIO_WORKER_OUTPUT)
        return "sceAudioOutputBlocking";
    if (stage == PSP_MEDIA_AUDIO_WORKER_THREAD)
        return "audio-worker-thread";
    return "audio-worker";
}
#endif

static int psp_media_audio_worker_health(PspMediaBackend *backend)
{
    if (backend == NULL) return -1;
    int recorded = atomic_load_explicit(
        &backend->audio_worker_error, memory_order_acquire);
    if (recorded < 0 || backend->audio_thread < 0
        || atomic_load_explicit(
               &backend->audio_stop, memory_order_acquire)) {
        return recorded;
    }
    /* ReferThreadStatus is a kernel crossing. Audio submit can run once per
       AAC frame and presentation once per browser pump, so observing on both
       hot paths without a cadence would spend more CPU proving the worker is
       healthy than reacting to its rare failure. A terminal worker is still
       surfaced within 100 ms; explicit worker errors remain immediate. */
    uint32_t now_us = sceKernelGetSystemTimeLow();
    if (backend->audio_worker_next_health_us != 0
        && (int32_t) (
               now_us - backend->audio_worker_next_health_us) < 0)
        return 0;
    backend->audio_worker_next_health_us = now_us + UINT32_C(100000);
    PspThreadObservation observation;
    int referred = psp_thread_observe(
        backend->audio_thread, &observation);
    PspModuleWorkerPollDisposition poll =
        psp_module_worker_poll_disposition(
            referred, observation.status);
    if (poll == PSP_MODULE_WORKER_POLL_PENDING) return 0;
    int failure = poll == PSP_MODULE_WORKER_POLL_ERROR
        ? referred
        : psp_unexpected_worker_exit_status(
              observation.status, observation.exit_status);
    int expected = 0;
    bool installed = atomic_compare_exchange_strong_explicit(
        &backend->audio_worker_error, &expected, failure,
        memory_order_release, memory_order_relaxed);
    if (installed) {
        atomic_store_explicit(
            &backend->audio_worker_stage, PSP_MEDIA_AUDIO_WORKER_THREAD,
            memory_order_release);
    }
    return atomic_load_explicit(
        &backend->audio_worker_error, memory_order_acquire);
}

bool media_psp_backend_quarantined(void)
{
    return psp_media_backend_is_quarantined;
}

bool media_psp_backend_wide_program_rejected(void)
{
    return psp_media_wide_program_rejected;
}

void media_psp_backend_set_wide_program(const char *name)
{
    psp_media_wide_program_mode =
        (int) psp_media_wide_program_configured(name);
    /* `no-boot` restores the historical process-start assumption, which is
       exactly the cache this seeds. Published once at boot before any video
       can open, so seeding here and nowhere else keeps every later reset --
       suspend, a forced runtime restart, a failed teardown -- meaning
       "unknown" for both spellings, which is what they both meant before. */
    if (!psp_media_cold_boot_call_enabled(psp_media_wide_program_mode))
        psp_media_me_boot_type = PSP_MEDIA_DEFAULT_ME_BOOT_TYPE;
    psp_media_log(
        "tilefinch-media-decoder: event=wide-program-knob value=%s",
        psp_media_wide_program_name(psp_media_wide_program_mode));
}

int media_psp_backend_wide_program(void)
{
    return psp_media_wide_program_mode;
}

void media_psp_backend_set_advance_mode(int mode)
{
    psp_media_advance_mode = mode;
}

/* Telemetry only, and deliberately not validated against the enum here: an
   out-of-range value is folded to OTHER where it is read, so a caller that
   publishes nonsense costs one mis-tagged conversion rather than an
   out-of-bounds increment. */
void media_psp_backend_set_loop_phase(int phase)
{
    atomic_store_explicit(
        &psp_media_loop_phase, phase, memory_order_relaxed);
}

#if defined(TILEFINCH_PSP_VALIDATION_LOG)
static void psp_media_au_dump_store32(unsigned char *at, uint32_t value)
{
    at[0] = (unsigned char) (value & 0xFFu);
    at[1] = (unsigned char) ((value >> 8) & 0xFFu);
    at[2] = (unsigned char) ((value >> 16) & 0xFFu);
    at[3] = (unsigned char) ((value >> 24) & 0xFFu);
}

/* Record one submitted access unit: 16-byte header then its staged bytes.
   Header is offset (u64), size (u32), sequence (u32), all little-endian, and
   the file opens with magic and version so a truncated run is still
   readable. */
static void psp_media_au_dump_record(
    uint64_t offset, const unsigned char *bytes, size_t length)
{
    if (!psp_media_au_dump_enabled || !psp_media_au_dump_armed
        || psp_media_au_dump_capped || bytes == NULL || length == 0) return;
    if (psp_media_au_dump_buffer == NULL) {
        psp_media_au_dump_buffer = malloc(PSP_MEDIA_AU_DUMP_CAP_BYTES);
        if (psp_media_au_dump_buffer == NULL) {
            psp_media_au_dump_capped = true;
            return;
        }
        psp_media_au_dump_store32(
            psp_media_au_dump_buffer, PSP_MEDIA_AU_DUMP_MAGIC);
        psp_media_au_dump_store32(
            psp_media_au_dump_buffer + 4, PSP_MEDIA_AU_DUMP_VERSION);
        psp_media_au_dump_used = 8u;
        psp_media_au_dump_sequence = 0;
    }
    size_t needed = 16u + length;
    if (psp_media_au_dump_used + needed > PSP_MEDIA_AU_DUMP_CAP_BYTES) {
        psp_media_au_dump_capped = true;
        return;
    }
    unsigned char *at = psp_media_au_dump_buffer + psp_media_au_dump_used;
    psp_media_au_dump_store32(at, (uint32_t) (offset & 0xFFFFFFFFu));
    psp_media_au_dump_store32(at + 4, (uint32_t) (offset >> 32));
    psp_media_au_dump_store32(at + 8, (uint32_t) length);
    psp_media_au_dump_store32(at + 12, psp_media_au_dump_sequence++);
    memcpy(at + 16, bytes, length);
    psp_media_au_dump_used += needed;
}

/* Once, at teardown: the whole run's sequence in one host write, truncating
   any previous run's file. There is intentionally no ms0: fallback. */
static void psp_media_au_dump_flush(void)
{
    if (psp_media_au_dump_buffer == NULL) return;
    if (psp_media_au_dump_used > 8u) {
        int file = sceIoOpen(
            PSP_MEDIA_AU_DUMP_PATH,
            PSP_O_WRONLY | PSP_O_CREAT | PSP_O_TRUNC, 0777);
        if (file >= 0) {
            (void) sceIoWrite(
                file, psp_media_au_dump_buffer, (SceSize)
                psp_media_au_dump_used);
            (void) sceIoClose(file);
            psp_media_log(
                "tilefinch-media-decoder: event=au-dump path=%s bytes=%zu "
                "units=%u capped=%d",
                PSP_MEDIA_AU_DUMP_PATH, psp_media_au_dump_used,
                (unsigned) psp_media_au_dump_sequence,
                psp_media_au_dump_capped ? 1 : 0);
        }
    }
    free(psp_media_au_dump_buffer);
    psp_media_au_dump_buffer = NULL;
    psp_media_au_dump_used = 0;
    psp_media_au_dump_capped = false;
    psp_media_au_dump_armed = false;
}
#else
#define psp_media_au_dump_record(offset, bytes, length) ((void) 0)
#define psp_media_au_dump_flush() ((void) 0)
#endif

void media_psp_backend_set_au_dump(int enabled)
{
#if defined(TILEFINCH_PSP_VALIDATION_LOG)
    psp_media_au_dump_enabled = enabled;
#else
    (void) enabled;
#endif
}

/* What a reposition does to the sceMpeg object: in place, rebuilt, or nothing
   at all. Published once from boot configuration, read only by psp_media_reset
   on the browser thread. Not confined to validation builds: the shipping
   default is the in-place flush, and the point of the knob is that one device
   run can change it. */
static int psp_media_reset_mode = PSP_MEDIA_RESET_MODE_IN_PLACE;
static bool psp_media_refusal_recovery_enabled = true;

void media_psp_backend_set_reset_mode(int mode)
{
    if (mode < PSP_MEDIA_RESET_MODE_IN_PLACE
        || mode > PSP_MEDIA_RESET_MODE_NO_TOUCH) return;
    psp_media_reset_mode = mode;
}

void media_psp_backend_set_refusal_recovery(bool enabled)
{
    psp_media_refusal_recovery_enabled = enabled;
}

/*
 * The browser thread has looked at the refusal; video may flow again.
 *
 * Called once per advance at the point the session has decided what to do about
 * a refusal -- recovered, or declined because no recovery is configured. It is
 * deliberately not conditional on that decision: the hold exists to keep access
 * units out of a decoder whose refusal nobody has processed, and "processed and
 * did nothing" is still processed. Idempotent, so every pump loop that reaches
 * the Media Engine can call it without coordinating.
 */
void media_psp_backend_release_refusal_hold(void)
{
    if (!atomic_exchange_explicit(
            &psp_media_video_refusal_hold, false, memory_order_acq_rel))
        return;
    psp_media_log(
        "tilefinch-media-decoder: event=refusal-hold-released blocked=%u",
        psp_media_video_refusal_blocked);
    psp_media_video_refusal_blocked = 0;
}

void media_psp_backend_set_wait_limit_us(unsigned wait_limit_us)
{
    psp_media_advance_wait_limit_us = (uint32_t) wait_limit_us;
}

/* Publish, or withdraw, a reader's right to one slot. Called by whichever step
   moves the slot into or out of READING, so the two can never drift. */
static void psp_media_surface_lease_publish(
    unsigned slot, uint32_t generation, bool readable)
{
    if (slot >= PSP_MEDIA_SURFACE_SLOTS) return;
    atomic_store(&psp_media_surface_lease_generation[slot], generation);
    atomic_store(&psp_media_surface_lease_readable[slot], readable ? 1 : 0);
}

static bool psp_media_surface_lease_matches(
    unsigned slot, uint32_t generation)
{
    return slot < PSP_MEDIA_SURFACE_SLOTS
        && atomic_load(&psp_media_surface_lease_readable[slot]) != 0
        && atomic_load(&psp_media_surface_lease_generation[slot])
           == generation;
}

/*
 * Claim a lease on one slot for a read.
 *
 * The generation is the caller's statement of WHICH picture it means. A reader
 * that captured {slot, generation} at claim time and finds the slot has moved
 * on is holding a stale pointer -- the address is still valid memory, which is
 * precisely why nothing else would catch it -- and is told so rather than
 * allowed to read a successor under its predecessor's name. In the shipping
 * flow this cannot fire, because a claimed slot stays READING until the claim
 * is superseded; it fires when a reset moved the pipeline underneath a reader,
 * and then it is the only thing standing between that reader and a silent
 * substitution.
 */
bool media_psp_backend_borrow_surface(unsigned slot, uint32_t generation)
{
    if (slot >= PSP_MEDIA_SURFACE_SLOTS) return false;
    atomic_store(&psp_media_surface_borrowed[slot], 1);
    if (!psp_media_surface_lease_matches(slot, generation)) {
        atomic_store(&psp_media_surface_borrowed[slot], 0);
        atomic_fetch_add_explicit(
            &psp_media_surface_borrow_stale, 1u, memory_order_relaxed);
        return false;
    }
    /* The claim fences every conversion that has not started. This waits out
       the one case it cannot fence: a conversion already inside firmware,
       whose Media Engine writes are in flight before the claim was made. */
    if (atomic_load(&psp_media_surface_writing[slot]) == 0) return true;
    uint32_t started_us = sceKernelGetSystemTimeLow();
    atomic_fetch_add_explicit(
        &psp_media_surface_borrow_waits, 1u, memory_order_relaxed);
    for (;;) {
        uint32_t elapsed_us = sceKernelGetSystemTimeLow() - started_us;
        bool clear = atomic_load(&psp_media_surface_writing[slot]) == 0;
        if (clear || elapsed_us >= PSP_MEDIA_SURFACE_BORROW_WAIT_US) {
            atomic_fetch_add_explicit(
                &psp_media_surface_borrow_wait_us, elapsed_us,
                memory_order_relaxed);
            if (clear) return true;
            /* Longer than any conversion this device performs. Read anyway --
               refusing would only mean presenting nothing -- and let the count
               say a frame may have torn. */
            atomic_fetch_add_explicit(
                &psp_media_surface_borrow_timeouts, 1u, memory_order_relaxed);
            return false;
        }
        if (sceKernelDelayThread(500) < 0) return false;
    }
}

static PspMediaFrameKey psp_media_frame_key_of(const MediaVideoFrame *frame)
{
    if (frame == NULL) return (PspMediaFrameKey) {.slot = -1};
    return (PspMediaFrameKey) {
        .epoch = frame->epoch,
        .identity = frame->identity,
        .pts_us = frame->pts_us,
        .generation = frame->generation,
        .slot = frame->slot
    };
}

void media_psp_backend_note_frame_staged(const MediaVideoFrame *frame)
{
    PspMediaFrameKey key = psp_media_frame_key_of(frame);
    if (!psp_media_frame_key_armed(&key)) return;
    /*
     * The staged copy has to be the picture the claim named, and this is the
     * one place that can still say so: after the join, before the draw. A
     * staging pass that read a slot the pipeline had moved on from would
     * otherwise reach the screen looking exactly like a correct one.
     */
    if (!psp_media_frame_key_equal(&key, &psp_media_claim_key)) {
        atomic_fetch_add_explicit(
            &psp_media_stage_mismatches, 1u, memory_order_relaxed);
        return;
    }
    atomic_fetch_add_explicit(
        &psp_media_claims_staged, 1u, memory_order_relaxed);
#if defined(TILEFINCH_PSP_VALIDATION_LOG)
    PspMediaPictureTrace *slot = psp_media_trace_slot(key.identity);
    if (slot != NULL) slot->stage_us = psp_media_stamp_us();
#endif
}

void media_psp_backend_note_frame_displayed(
    const MediaVideoFrame *frame, int present_path)
{
    PspMediaFrameKey key = psp_media_frame_key_of(frame);
    if (!psp_media_frame_key_armed(&key)) return;
    uint32_t now_us = psp_media_stamp_us();
    /* A picture may be drawn into both back buffers; the second is the same
       picture reaching the screen again, not another one arriving. Compared as
       a whole tuple: two pictures can share a truncated identity, and with two
       slots they can share an address as well. */
    PspMediaFrameKey previous = psp_media_display_key;
    psp_media_display_key = key;
    if (!psp_media_frame_key_equal(&previous, &key)) {
        atomic_fetch_add_explicit(
            &psp_media_claims_displayed, 1u, memory_order_relaxed);
        /* The first picture on screen after a refusal closes the outage. Only
           here: everything earlier says the pipeline restarted, not that the
           user saw anything. */
        if (atomic_load_explicit(
                &psp_media_recovery_stage, memory_order_relaxed)
            == PSP_MEDIA_RECOVERY_CONVERTED) {
            uint32_t refusal_us = atomic_load_explicit(
                &psp_media_recovery_refusal_us, memory_order_relaxed);
            uint32_t outage_us = now_us - refusal_us;
            atomic_fetch_add_explicit(
                &psp_media_recovery_present_total_us, outage_us,
                memory_order_relaxed);
            if (outage_us > atomic_load_explicit(
                    &psp_media_recovery_present_max_us,
                    memory_order_relaxed)) {
                atomic_store_explicit(
                    &psp_media_recovery_present_max_us, outage_us,
                    memory_order_relaxed);
            }
            atomic_fetch_add_explicit(
                &psp_media_recoveries, 1u, memory_order_relaxed);
            atomic_store_explicit(
                &psp_media_recovery_stage, PSP_MEDIA_RECOVERY_IDLE,
                memory_order_relaxed);
        }
    }
#if defined(TILEFINCH_PSP_VALIDATION_LOG)
    PspMediaPictureTrace *record = psp_media_trace_slot(key.identity);
    if (record != NULL && record->present_us == 0) {
        record->present_us = now_us;
        record->present_path = (uint8_t) present_path;
    }
#else
    (void) present_path;
#endif
}

void media_psp_backend_note_frame_quiesced(const MediaVideoFrame *frame)
{
    PspMediaFrameKey key = psp_media_frame_key_of(frame);
    if (!psp_media_frame_key_armed(&key)
        || !psp_media_frame_key_equal(&key, &psp_media_claim_key))
        return;
    if (!psp_media_frame_key_equal(&key, &psp_media_display_key)) {
        /* A close between take and scanout is an intentional terminal verdict
           on this claim, not a missing picture whose fate has to be inferred
           from claims != displayed + dropped. Keep it in the ordinary dropped
           branch of the funnel and publish the reason as a strict subset. */
        atomic_fetch_add_explicit(
            &psp_media_claims_dropped, 1u, memory_order_relaxed);
        atomic_fetch_add_explicit(
            &psp_media_claims_quiesced, 1u, memory_order_relaxed);
    }
    psp_media_claim_key = (PspMediaFrameKey) {.slot = -1};
}

void media_psp_backend_note_stage_signature(
    const MediaVideoFrame *frame, uint32_t signature)
{
    PspMediaFrameKey key = psp_media_frame_key_of(frame);
    if (signature == 0 || !psp_media_frame_key_armed(&key)) return;
    if (!psp_media_frame_key_equal(&key, &psp_media_claim_key)) return;
#if defined(TILEFINCH_PSP_VALIDATION_LOG)
    PspMediaPictureTrace *record = psp_media_trace_slot(key.identity);
    if (record == NULL) return;
    record->stage_signature = signature;
    if (record->signature == 0 || record->signature == signature) return;
    /*
     * Right metadata, wrong pixels. The staged copy carries this picture's
     * identity, slot and generation, and its bytes are somebody else's -- the
     * one outcome every counter in this file would otherwise report as a
     * clean frame. Logged rather than acted on: by the time this is known the
     * copy is done, and a session that keeps playing produces the evidence
     * that says which slot it read.
     */
    atomic_fetch_add_explicit(
        &psp_media_signature_mismatches, 1u, memory_order_relaxed);
    psp_media_log(
        "tilefinch-media-decoder: event=stage-signature-mismatch id=%u "
        "slot=%d gen=%u csc=0x%08x staged=0x%08x",
        (unsigned) key.identity, key.slot, key.generation,
        (unsigned) record->signature, (unsigned) signature);
#else
    (void) signature;
#endif
}

void media_psp_backend_dump_picture_trace(const char *phase)
{
#if defined(TILEFINCH_PSP_VALIDATION_LOG) \
    && defined(TILEFINCH_PSP_MEDIA_PICTURE_TRACE)
    /*
     * Newest records, oldest first, and only those not already printed. One
     * line per picture: the whole point is that a reader can follow a single
     * identity from its access unit to the screen without averaging anything.
     * All timestamps are relative to that picture's own submit, so a line is
     * readable on its own and the ring needs no wall-clock anchor.
     */
    uint32_t newest = (uint32_t) psp_media_claim_key.identity;
    uint32_t oldest = newest > PSP_MEDIA_TRACE_SLOTS
        ? newest - PSP_MEDIA_TRACE_SLOTS : 1u;
    if (psp_media_trace_dumped_identity > oldest)
        oldest = psp_media_trace_dumped_identity + 1u;
    for (uint32_t identity = oldest; identity <= newest; identity++) {
        const PspMediaPictureTrace *slot = psp_media_trace_slot(identity);
        if (slot == NULL || slot->csc_us == 0) continue;
        uint32_t base = slot->submit_us;
        psp_media_log(
            "tilefinch-media-picture: phase=%s id=%u au=%u batch=%u/%u/%u "
            "pts=%ums decode=%u csc=%u take=%u stage=%u present=%u "
            "next-au=%u take-result=%u path=%u slot=%u gen=%u "
            "sig=0x%08x stage-sig=0x%08x",
            phase == NULL ? "unknown" : phase,
            slot->identity, slot->au_id, slot->batch_id,
            (unsigned) slot->batch_index, (unsigned) slot->batch_count,
            slot->pts_ms,
            slot->decode_us == 0 ? 0u : slot->decode_us - base,
            slot->csc_us == 0 ? 0u : slot->csc_us - base,
            slot->take_us == 0 ? 0u : slot->take_us - base,
            slot->stage_us == 0 ? 0u : slot->stage_us - base,
            slot->present_us == 0 ? 0u : slot->present_us - base,
            slot->next_submit_us == 0 ? 0u : slot->next_submit_us - base,
            (unsigned) slot->take_result, (unsigned) slot->present_path,
            (unsigned) slot->slot, slot->generation,
            (unsigned) slot->signature, (unsigned) slot->stage_signature);
    }
    psp_media_trace_dumped_identity = newest;
#else
    (void) phase;
#endif
}

/*
 * End a lease. The slot does NOT become writable here.
 *
 * A reader letting go says only that this reader is finished, and the browser
 * loop redraws the picture it last claimed whenever no newer one is due -- so
 * the address is still live for the frontend after every reader has released
 * it. What frees the slot is the next claim, which is the moment nothing can
 * refer to this picture any more. See PspMediaSurfaceSlotState.
 */
void media_psp_backend_end_surface_read(unsigned slot)
{
    if (slot >= PSP_MEDIA_SURFACE_SLOTS) return;
    atomic_store(&psp_media_surface_borrowed[slot], 0);
}

/*
 * Declare a slot to be held by a reader that can no longer be joined, and
 * later that the reader has been seen to finish.
 *
 * Called by the presenter, which is the only component that can know either
 * fact: it posted the transfer, it is the one whose join timed out, and its
 * worker's completion event is the only sound evidence the transfer ended.
 * The backend cannot ask -- it holds no presenter -- so the presenter tells
 * it, exactly as the reader's ordinary lease is published from the same side.
 *
 * Idempotent in both directions. The release is not a promise that the slot
 * is now free; it only withdraws the refusal, and the ordinary release path
 * is what actually returns the slot to its writer.
 */
void media_psp_backend_quarantine_surface(unsigned slot)
{
    if (slot >= PSP_MEDIA_SURFACE_SLOTS) return;
    if (atomic_exchange(&psp_media_surface_quarantined[slot], 1) == 0) {
        atomic_fetch_add_explicit(
            &psp_media_surface_quarantines, 1u, memory_order_relaxed);
    }
}

void media_psp_backend_release_surface_quarantine(unsigned slot)
{
    if (slot >= PSP_MEDIA_SURFACE_SLOTS) return;
    atomic_store(&psp_media_surface_quarantined[slot], 0);
}

bool media_psp_backend_surface_quarantined(unsigned slot)
{
    if (slot >= PSP_MEDIA_SURFACE_SLOTS) return false;
    return atomic_load(&psp_media_surface_quarantined[slot]) != 0;
}

void media_psp_backend_release_surface(unsigned slot)
{
    if (slot >= PSP_MEDIA_SURFACE_SLOTS) return;
    media_psp_backend_end_surface_read(slot);
    /*
     * Segment four closes here, and only for the first release that follows a
     * take. The loop redraws whatever it last took, so later releases of the
     * same picture are re-presents rather than that picture's present, and
     * counting them would both inflate the segment count and break the
     * arithmetic the four segments exist to satisfy.
     *
     * The same release arms segment five, which is the interval this whole
     * line was added to expose: how long after the presenter let go the pump
     * took to get the next access unit into the decoder.
     */
    uint32_t taken_us = atomic_exchange_explicit(
        &psp_media_frame_taken_us, 0u, memory_order_relaxed);
    if (taken_us == 0u) return;
    uint32_t done_us = psp_media_stamp_us();
    uint32_t present_us = done_us - taken_us;
    atomic_fetch_add_explicit(
        &psp_media_presents, 1u, memory_order_relaxed);
    atomic_fetch_add_explicit(
        &psp_media_present_total_us, present_us, memory_order_relaxed);
    if (present_us > atomic_load_explicit(
            &psp_media_present_max_us, memory_order_relaxed)) {
        atomic_store_explicit(
            &psp_media_present_max_us, present_us, memory_order_relaxed);
    }
    atomic_store_explicit(
        &psp_media_present_done_us, done_us, memory_order_relaxed);
}

/*
 * The writer's half. True when the conversion may proceed, with the
 * announcement left standing for psp_media_surface_end_write to retire.
 *
 * A claimed surface is waited out rather than worked around, because the
 * alternative -- leaving the picture pending for a later submit -- moves the
 * conversion onto the browser thread, which is the cost c95cd6a existed to
 * remove. This thread has nothing else to do and every claim is one present
 * long. The budget is what makes it a wait rather than a hang; past it the
 * picture stays captured-pending and the next submit or drain emits it, on
 * the path the advance mode already fences.
 */
static bool psp_media_surface_begin_write(unsigned slot)
{
    if (slot >= PSP_MEDIA_SURFACE_SLOTS) return false;
    uint32_t started_us = sceKernelGetSystemTimeLow();
    for (;;) {
        atomic_store(&psp_media_surface_writing[slot], 1);
        if (atomic_load(&psp_media_surface_borrowed[slot]) == 0) return true;
        atomic_store(&psp_media_surface_writing[slot], 0);
        if (sceKernelGetSystemTimeLow() - started_us
            >= PSP_MEDIA_SURFACE_LEASE_WAIT_US) {
            atomic_fetch_add_explicit(
                &psp_media_surface_emit_deferrals, 1u, memory_order_relaxed);
            return false;
        }
        if (sceKernelDelayThread(500) < 0) return false;
    }
}

static void psp_media_surface_end_write(unsigned slot)
{
    if (slot >= PSP_MEDIA_SURFACE_SLOTS) return;
    atomic_store(&psp_media_surface_writing[slot], 0);
}

/*
 * Wait out every reader before a reset is allowed to say a slot is free.
 *
 * Bounded, and expected to return immediately: the only borrower is the
 * browser thread, and a reset runs on it, so any lease still standing here is
 * one this same thread failed to release. It is the step that turns "the
 * presenter has joined its copy" from an ordering the code happens to have
 * into one the reset asks for -- and a DMA controller that outlived its join
 * is exactly the case that must not be answered by handing its source slot to
 * the Media Engine.
 */
static bool psp_media_surface_quiesce_readers(uint32_t *waited_us)
{
    uint32_t started_us = sceKernelGetSystemTimeLow();
    for (;;) {
        bool clear = true;
        for (unsigned slot = 0; slot < PSP_MEDIA_SURFACE_SLOTS; slot++)
            if (atomic_load(&psp_media_surface_borrowed[slot]) != 0
                /* And the reader the lease cannot describe. A quarantined
                   slot is one an abandoned DMA may still be sourcing from, so
                   it is exactly the case this wait exists for -- and the one
                   the lease answers "unread" for, because the thread that
                   gave up on the transfer dropped the lease on its way past.
                   It will not clear inside this bound, and it should not: the
                   caller's failure path is the right answer to a reader that
                   cannot be waited out. */
                || atomic_load(&psp_media_surface_quarantined[slot]) != 0)
                clear = false;
        uint32_t elapsed_us = sceKernelGetSystemTimeLow() - started_us;
        if (waited_us != NULL) *waited_us = elapsed_us;
        if (clear) return true;
        if (elapsed_us >= PSP_MEDIA_SURFACE_BORROW_WAIT_US) return false;
        if (sceKernelDelayThread(500) < 0) return false;
    }
}

/* One session decodes at a time, and the handshake describes this one. */
static void psp_media_surface_handshake_reset(void)
{
    for (unsigned slot = 0; slot < PSP_MEDIA_SURFACE_SLOTS; slot++) {
        atomic_store(&psp_media_surface_borrowed[slot], 0);
        atomic_store(&psp_media_surface_writing[slot], 0);
        psp_media_surface_lease_publish(slot, 0, false);
    }
    atomic_store(&psp_media_surface_borrow_stale, 0);
    atomic_store(&psp_media_surface_borrow_waits, 0);
    atomic_store(&psp_media_surface_borrow_wait_us, 0);
    atomic_store(&psp_media_surface_borrow_timeouts, 0);
    atomic_store(&psp_media_surface_emit_deferrals, 0);
    /* The cadence segments measured outside any decoder belong to the same
       session scope as the handshake above, and a stale arm from the previous
       stream would be charged to the first picture of this one. */
    atomic_store(&psp_media_frame_taken_us, 0);
    atomic_store(&psp_media_present_done_us, 0);
    atomic_store(&psp_media_presents, 0);
    atomic_store(&psp_media_present_total_us, 0);
    atomic_store(&psp_media_present_max_us, 0);
    psp_media_claim_key = (PspMediaFrameKey) {.slot = -1};
    psp_media_display_key = (PspMediaFrameKey) {.slot = -1};
    atomic_store(&psp_media_claims_staged, 0);
    atomic_store(&psp_media_claims_displayed, 0);
    atomic_store(&psp_media_claims_dropped, 0);
    atomic_store(&psp_media_claims_quiesced, 0);
    atomic_store(&psp_media_recovery_stage, PSP_MEDIA_RECOVERY_IDLE);
    atomic_store(&psp_media_recoveries, 0);
    atomic_store(&psp_media_recovery_reset_total_us, 0);
    atomic_store(&psp_media_recovery_au_total_us, 0);
    atomic_store(&psp_media_recovery_csc_total_us, 0);
    atomic_store(&psp_media_recovery_present_total_us, 0);
    atomic_store(&psp_media_recovery_present_max_us, 0);
    atomic_store(&psp_media_recovery_skip_total_ms, 0);
    psp_media_recovery_last_pts_us = 0;
#if defined(TILEFINCH_PSP_VALIDATION_LOG)
    memset(psp_media_trace, 0, sizeof(psp_media_trace));
    psp_media_trace_next_au = 0;
    psp_media_trace_next_batch = 0;
    psp_media_trace_dumped_identity = 0;
    psp_media_trace_open_au = 0;
    psp_media_trace_open_submit_us = 0;
    psp_media_trace_batch_au = 0;
    psp_media_trace_batch_submit_us = 0;
#endif
    psp_media_advance_mode = PSP_MEDIA_ADVANCE_FRAME;
    /* A refusal belongs to the stream that produced it; a new one must never
       open holding a hold nobody is going to release. */
    atomic_store(&psp_media_video_refusal_hold, false);
    psp_media_video_refusal_blocked = 0;
}

/*
 * Convert every picture of a returned batch there is a slot free to hold.
 *
 * A decode can return two, three or four pictures. Converting only one leaves
 * the rest waiting for a later submit or drain visit that is allowed to emit.
 * That surplus wait is the phase this whole change exists to
 * remove -- a device run measured 5281 visits turned away with a picture
 * pending -- so converting one and deferring the rest while a slot sat FREE
 * would have reintroduced it on the very commit that removed it.
 *
 * The loop stops at the first slot it cannot get, which is the honest answer
 * to a genuinely full pipeline: the picture stays captured, the batch arrays
 * stay retained, and the next visit picks it up exactly as before. Nothing
 * here may run after a further access unit has entered the decoder -- the next
 * sceMpegAvcDecodeDetail2 invalidates the info and YUV arrays this reads --
 * and nothing does: psp_media_submit refuses to accept a unit while a batch is
 * pending, which is the guard that makes the retention safe.
 *
 * Both writers use this: the codec worker inside its decode or drain job, and
 * the browser thread at a release. Which thread is running decides only which
 * epoch it stamps.
 */
static int psp_media_emit_batch_into_free_slots(
    PspMediaBackend *backend, uint64_t epoch, unsigned *converted,
    char *error, size_t error_size)
{
    unsigned emitted = 0;
    int status = 0;
    while (psp_media_batch_pending(backend)) {
        int slot = psp_media_slot_free_index(
            backend->slots, PSP_MEDIA_SURFACE_SLOTS);
        if (slot < 0) break;
        /* The announcement, then the conversion, then the retraction -- in
           that order, on every writer, because it is what orders this against
           a reader that claims the same slot from another thread. */
        if (!psp_media_surface_begin_write((unsigned) slot)) break;
        status = psp_media_emit_captured_picture(
            backend, (unsigned) slot, epoch, error, error_size);
        psp_media_surface_end_write((unsigned) slot);
        if (status < 0) break;
        emitted++;
    }
    /*
     * A picture left captured while a slot was still FREE.
     *
     * The loop stops for three reasons and only one of them is legitimate:
     * the pipeline is full. The other two are a reader holding the slot
     * against psp_media_surface_begin_write, and a conversion that failed --
     * and both leave a surplus picture waiting for a later visit while the
     * Media Engine has somewhere to put it right now. That is the exact phase
     * the fill-all-free-slots condition was added to remove, so it is
     * measured rather than assumed absent.
     */
    if (status >= 0 && psp_media_batch_pending(backend)
        && psp_media_slot_free_count(
               backend->slots, PSP_MEDIA_SURFACE_SLOTS) != 0) {
        backend->stats.batch_defer_free +=
            backend->video_picture_count - backend->video_picture_index;
    }
    if (converted != NULL) *converted = emitted;
    return status;
}

static const char *psp_media_codec_stage_name(int stage)
{
    switch (stage) {
    case PSP_MEDIA_CODEC_STAGE_AVC_BRIDGE:
        return "sceMpegGetAvcNalAu";
    case PSP_MEDIA_CODEC_STAGE_AVC_DECODE:
        return "sceMpegAvcDecode";
    case PSP_MEDIA_CODEC_STAGE_AVC_DETAIL:
        return "sceMpegAvcDecodeDetail2";
    case PSP_MEDIA_CODEC_STAGE_AVC_CSC:
        return "sceMpegBaseCscAvc";
    case PSP_MEDIA_CODEC_STAGE_AAC_DECODE:
        return "sceAudiocodecDecode";
    case PSP_MEDIA_CODEC_STAGE_AUDIO_SIGNAL:
        return "sceKernelSetEventFlag-audio";
    case PSP_MEDIA_CODEC_STAGE_WORKER_WAIT:
        return "sceKernelWaitEventFlag-codec";
    case PSP_MEDIA_CODEC_STAGE_AVC_STOP:
        return "sceMpegAvcDecodeStop";
    case PSP_MEDIA_CODEC_STAGE_AUDIO_EDRAM:
        return "sceAudiocodecReleaseEDRAM";
    case PSP_MEDIA_CODEC_STAGE_MPEG_DELETE:
        return "sceMpegDelete";
    case PSP_MEDIA_CODEC_STAGE_MPEG_FINISH:
        return "sceMpegFinish";
    case PSP_MEDIA_CODEC_STAGE_MPEG_CREATE:
        return "sceMpegCreate-recreate";
    case PSP_MEDIA_CODEC_STAGE_MPEG_INIT_AU:
        return "sceMpegInitAu-recreate";
    default:
        return "codec-worker";
    }
}

static void psp_media_codec_stage(
    PspMediaBackend *backend, PspMediaCodecNativeStage stage)
{
    if (backend == NULL) return;
    atomic_store_explicit(
        &backend->codec_native_stage, stage, memory_order_release);
}

static MediaBackendResult psp_media_decode_staged_video(
    PspMediaBackend *backend, char *error, size_t error_size)
{
    if (backend == NULL || backend->video_parameter_sets == NULL
        || backend->video_sps_bytes <= 0
        || backend->video_pps_bytes <= 0
        || backend->last_packet_bytes == 0
        || backend->last_packet_bytes > INT_MAX) {
        psp_media_error(error, error_size, "PSP AVC config invalid");
        return MEDIA_BACKEND_ERROR;
    }
    backend->video_status = 0;
    /* sceMpegGetAvcNalAu sets decoder_primed on success, so the admission
       verdict has to be sampled before the bridge is entered. */
    bool primed_before = backend->decoder_primed;
    PspAvcNal nal;
    memset(&nal, 0, sizeof(nal));
    nal.sps_buffer = backend->video_parameter_sets;
    nal.sps_size = backend->video_sps_bytes;
    nal.pps_buffer = (unsigned char *) backend->video_parameter_sets
                   + backend->video_sps_bytes;
    nal.pps_size = backend->video_pps_bytes;
    nal.nal_prefix_size = backend->nal_length_size;
    nal.nal_buffer = backend->packet_staging;
    nal.nal_size = (int) backend->last_packet_bytes;
    nal.mode = backend->decoder_primed ? 0 : 3;
    backend->last_nal_mode = nal.mode;

    /*
     * The Annex-B experiment. This runs on the codec worker, which is where
     * the bridge itself is entered, and touches only this backend's own
     * staging copy of the access unit -- no firmware call, no allocation, and
     * nothing the main thread can observe mid-rewrite. It is deliberately
     * scoped to the wide program: the proven program is the one that decodes
     * AVCC today and must not be perturbed by an experiment aimed at the one
     * that does not.
     */
    unsigned annexb_nals = 0;
    bool annexb = psp_media_wide_program_annexb(psp_media_wide_program_mode)
        && backend->mpeg_mode == 5;
    if (annexb
        && !psp_media_annexb_rewrite(
               backend->packet_staging, backend->last_packet_bytes,
               backend->nal_length_size, &annexb_nals)) {
        backend->video_timestamps = backend->codec_job_timestamps_before;
        psp_media_log_failure(backend, "annexb-rewrite", -1);
        psp_media_log(
            "tilefinch-media-decoder: event=annexb-rewrite-rejected "
            "nal-prefix=%d nal-size=%u",
            (int) backend->nal_length_size,
            (unsigned) backend->last_packet_bytes);
        psp_media_error(
            error, error_size,
            "PSP AVC Annex-B rewrite rejected a %u-byte access unit",
            (unsigned) backend->last_packet_bytes);
        return MEDIA_BACKEND_ERROR;
    }
    (void) annexb_nals;

#if defined(TILEFINCH_PSP_VALIDATION_LOG)
    /* Record the priming transaction, not every steady-state AU. The latter
       used to produce thousands of formatted lines and synchronous stream
       writes in a two-minute validation run, perturbing the cadence the run
       was meant to measure. Refusals still dump their exact AU head/state, and
       the optional bounded AU capture remains available for packet-level
       diagnosis. Shipping builds compile the entire block out. */
    if (!primed_before || annexb) {
        char head[33];
        size_t head_bytes = backend->last_packet_bytes < 16u
            ? backend->last_packet_bytes : 16u;
        psp_media_hex_bytes(backend->packet_staging, head_bytes, head);
        psp_media_log(
            "tilefinch-media-decoder: event=avc-bridge-submit "
            "nal-mode=%d nal-prefix=%d nal-size=%d sps=%d pps=%d "
            "primed=%d head=%s",
            nal.mode, nal.nal_prefix_size, nal.nal_size,
            nal.sps_size, nal.pps_size, primed_before ? 1 : 0, head);
        /* The head above is already the converted framing, so one extra line
           records how many units it covers when the explicit experiment is
           active. */
        if (annexb) {
            psp_media_log(
                "tilefinch-media-decoder: event=annexb-submit "
                "nals=%u nal-size=%u head=%s",
                annexb_nals, (unsigned) backend->last_packet_bytes, head);
        }
        /*
         * Commit the submitted bytes to the card before entering the bridge.
         * sceMpegGetAvcNalAu is where every device session so far has either
         * been rejected or hard-frozen the machine, and a frozen machine
         * never runs another sceIoSync -- so an unsynced submit line is
         * exactly the one that would be lost.
         *
         * Priming only. decoder_primed is set by the first successful bridge
         * call, so this fires for the first-frame window and then stops.
         */
        if (!primed_before) psp_media_commit_log();
    }
#endif
    /* All four ranges are CPU-written and firmware-read: the staged access
       unit and the parameter sets are this backend's own copies, the NAL
       descriptor is a stack struct, and the AU descriptor carries whatever
       sceMpegInitAu and the previous decode left in it. Publish, never
       discard -- the AU gets the writeback-invalidate form so the bridge also
       refetches anything the engine changed last time round. */
    sceKernelDcacheWritebackRange(
        backend->packet_staging, (unsigned) backend->last_packet_bytes);
    sceKernelDcacheWritebackRange(
        backend->video_parameter_sets,
        (unsigned) backend->video_parameter_sets_bytes);
    sceKernelDcacheWritebackRange(&nal, sizeof(nal));
    sceKernelDcacheWritebackInvalidateRange(
        backend->video_au, PSP_MEDIA_VIDEO_AU_BYTES);
    /* PMPlayer's hardware-proven raw-NAL path publishes the complete MPEG
       workspace before crossing into the Media Engine. Range-only cache
       maintenance cannot name firmware-owned state in the MPEG and DDR
       arenas, and PPSSPP's coherent memory cannot expose this omission. */
    sceKernelDcacheWritebackInvalidateAll();
    psp_media_codec_stage(backend, PSP_MEDIA_CODEC_STAGE_AVC_BRIDGE);
    /*
     * What the descriptor holds on the way in, so a refusal can be attributed.
     *
     * The dump below reads it back after the failure and has twice now shown
     * esSize=0 with a valid esBuffer -- which cannot distinguish "the bridge
     * handed firmware an empty access unit" from "firmware consumed the
     * elementary stream and zeroed the field on its way out". Those are
     * opposite defects. Twenty-four bytes copied per submitted packet, at
     * around forty packets a second, settles it.
     */
    unsigned char au_before[PSP_MEDIA_AU_DUMP_BYTES];
    memset(au_before, 0, sizeof(au_before));
    /* Everything before this line is ours -- the rewrite, the marshalling and
       the cache maintenance -- and everything after it until the decode
       returns is firmware's. The existing decode timing spans both plus the
       dispatch that preceded them, so it cannot say which of the three a slow
       job was slow in. */
    uint32_t firmware_entered_us = psp_media_stamp_us();
    SceInt32 status = sceMpegGetAvcNalAu(
        &backend->mpeg, &nal, backend->video_au);
    if (status >= 0) {
        backend->decoder_primed = true;
        memcpy(au_before, backend->video_au, sizeof(au_before));
        psp_media_codec_stage(backend, PSP_MEDIA_CODEC_STAGE_AVC_DECODE);
        /* sceMpegGetAvcNalAu just filled this descriptor from the main CPU.
           Publish those lines for the decode; never drop them without a
           writeback first. */
        sceKernelDcacheWritebackRange(
            backend->video_au, PSP_MEDIA_VIDEO_AU_BYTES);
        status = sceMpegAvcDecode(
            &backend->mpeg, backend->video_au,
            (SceInt32) PSP_MEDIA_MPEG_DECODE_WIDTH, NULL,
            &backend->video_status);
    }
    psp_media_note_worker_firmware(backend, firmware_entered_us);
    if (status < 0) {
        /*
         * Restoring the snapshot un-pushes the refused unit's timestamp, so a
         * dropped access unit takes its own metadata with it and A/V sync
         * does not walk by a frame. That is right for a unit firmware never
         * took, and wrong for one it did -- see the content-refusal branch
         * below, which puts the timestamp back. Keep the submitted queue so
         * that branch has something to put back.
         */
        PspMediaVideoTimestampQueue timestamps_submitted =
            backend->video_timestamps;
        backend->video_timestamps =
            backend->codec_job_timestamps_before;
        int failed_stage = atomic_load_explicit(
            &backend->codec_native_stage, memory_order_acquire);
        const char *stage_name = psp_media_codec_stage_name(failed_stage);
        /*
         * Firmware refusing one access unit's content is not the decoder
         * failing.
         *
         * The evidence for the distinction is structural rather than a status
         * allowlist. sceMpegGetAvcNalAu had already filled the descriptor --
         * au_before carries a non-zero esSize, so the bridge handed firmware a
         * real access unit -- the failure came from the decode stage, and this
         * decoder has already produced pictures, so nothing about the program,
         * the modules or the bridge is in question. What is left is one unit
         * of content firmware would not take. Drop it, keep the pipeline, and
         * let a run of them be the thing that fails.
         */
        uint32_t refused_es_size = (uint32_t) au_before[20]
            | ((uint32_t) au_before[21] << 8)
            | ((uint32_t) au_before[22] << 16)
            | ((uint32_t) au_before[23] << 24);
        bool content_refusal =
            failed_stage == PSP_MEDIA_CODEC_STAGE_AVC_DECODE
            && refused_es_size != 0
            && backend->stats.decoded_video_frames != 0;
        if (content_refusal) {
            uint64_t now_us = (uint64_t) sceKernelGetSystemTimeWide();
            if (backend->video_refusal_window_started_us == 0
                || now_us - backend->video_refusal_window_started_us
                       >= PSP_MEDIA_AVC_REFUSAL_WINDOW_US) {
                backend->video_refusal_window_started_us = now_us;
                backend->video_refusal_window_count = 0;
            }
            backend->video_refusals_consecutive++;
            backend->video_refusals_total++;
            backend->video_refusal_window_count++;
            backend->last_refused_error = status;
            if (psp_media_avc_refusal_survivable(
                    backend->video_refusals_consecutive,
                    backend->video_refusal_window_count)) {
                psp_media_log(
                    "tilefinch-media-decoder: event=au-refused status=0x%08X "
                    "packet=%zuB offset=%llu dts=%lluus pts=%lluus es=%uB "
                    "consecutive=%u total=%u window=%u",
                    (unsigned) status, backend->last_packet_bytes,
                    (unsigned long long) backend->last_packet_offset,
                    (unsigned long long) backend->last_packet_dts_us,
                    (unsigned long long) backend->last_packet_pts_us,
                    (unsigned) refused_es_size,
                    backend->video_refusals_consecutive,
                    backend->video_refusals_total,
                    backend->video_refusal_window_count);
#if defined(TILEFINCH_PSP_VALIDATION_LOG)
                /*
                 * The head of the unit firmware would not take. Inside a
                 * length-prefixed access unit these bytes are the first NAL's
                 * length and type, which is what says whether the offending
                 * shape is an SEI, a parameter set or a slice. Recorded so a
                 * later refinement can be aimed; nothing pre-filters on it
                 * today, because skipping is the correct general answer
                 * whatever the shape turns out to be.
                 */
                {
                    size_t head = backend->last_packet_bytes;
                    if (head > PSP_MEDIA_AU_HEAD_DUMP_BYTES)
                        head = PSP_MEDIA_AU_HEAD_DUMP_BYTES;
                    char head_bytes[PSP_MEDIA_AU_HEAD_DUMP_BYTES * 2u + 1u];
                    psp_media_hex_bytes(
                        backend->packet_staging, head, head_bytes);
                    psp_media_log(
                        "tilefinch-media-decoder: event=au-refused-head "
                        "bytes=%s", head_bytes);
                }
                /*
                 * And the state the refusal leaves behind, which is what a
                 * reset recovery is aimed at and what a skip inherits.
                 *
                 * `au` is the descriptor read back after the failure. Its bytes
                 * are firmware's from sceMpegInitAu onward, so a descriptor
                 * still reading as the 0xFF fill says the engine never touched
                 * it, while a mutated one says it ran and refused the content.
                 * `video-status` is the picture count the refused decode
                 * reported. `batch` is index/count of the picture batch a
                 * previous decode left un-emitted -- a refusal arriving with
                 * pictures still pending is a different shape of damage from
                 * one arriving with the batch drained. `queue` is the timestamp
                 * depth after the restore above un-pushed the refused unit's
                 * own entry, so it describes the queue the next unit will meet.
                 *
                 * The descriptor is written by firmware on the main CPU,
                 * through the data cache. Write those lines back before
                 * dropping them: a pure invalidate would destroy exactly the
                 * evidence this line exists to read, and the address and length
                 * are the whole 64-byte-aligned descriptor either way.
                 */
                {
                    char state_au[PSP_MEDIA_AU_DUMP_BYTES * 2u + 1u];
                    sceKernelDcacheWritebackInvalidateRange(
                        backend->video_au, PSP_MEDIA_VIDEO_AU_BYTES);
                    psp_media_hex_bytes(
                        backend->video_au, PSP_MEDIA_AU_DUMP_BYTES, state_au);
                    psp_media_log(
                        "tilefinch-media-decoder: event=au-refused-state "
                        "au=%s video-status=%d queue=%u batch=%u/%u",
                        state_au, (int) backend->video_status,
                        backend->video_timestamps.count,
                        backend->video_picture_index,
                        backend->video_picture_count);
                }
#endif
                /*
                 * Put the refused unit's timestamp back, because firmware kept
                 * its picture.
                 *
                 * This is the phantom five device runs were chasing. A content
                 * refusal is by definition a unit whose elementary stream
                 * firmware had already ingested -- refused_es_size is nonzero
                 * on the way in, and the descriptor read back above says
                 * esSize=0 on the way out, so the engine consumed the bytes and
                 * then reported an error. It keeps a picture for them. The
                 * blanket rollback above then threw away the only timestamp
                 * that picture could ever be paired with, and the engine's
                 * picture count and our timestamp count disagreed by exactly
                 * one from that moment on.
                 *
                 * Every symptom followed from that. In the no-touch mode the
                 * mismatch surfaced honestly two units later as a four-picture
                 * batch against three timestamps -- "invalid picture batch
                 * 4/3", our own invariant ending a run the Media Engine had not
                 * failed. In the flushing and rebuilding modes the reset landed
                 * on top of the same desynced accounting and the engine wedged
                 * four to six seconds later instead. One mechanism, five runs.
                 *
                 * Keeping the timestamp costs nothing when the guess is wrong:
                 * the queue is sorted by presentation time and popped smallest
                 * first, so a spare entry is spent by the next picture in
                 * order rather than mispairing anything.
                 */
                backend->video_timestamps = timestamps_submitted;
                atomic_store_explicit(
                    &backend->video_refusal_dirty,
                    psp_media_refusal_recovery_enabled,
                    memory_order_release);
                /*
                 * Hold video here, on the worker, before this job is published
                 * as complete. The browser thread collects the completion and
                 * only reaches its recovery decision at the end of that frame;
                 * everything the pump would have submitted in between is what
                 * wedged two device runs. Set before the release store on
                 * codec_job_state, so a thread that can see the completion can
                 * already see the hold.
                 */
                atomic_store_explicit(
                    &psp_media_video_refusal_hold, true,
                    memory_order_release);
                /* Arms the wall-time outage. Only if one is not already in
                   flight: a run of refusals is one recovery, not several. */
                if (atomic_load_explicit(
                        &psp_media_recovery_stage, memory_order_relaxed)
                    == PSP_MEDIA_RECOVERY_IDLE) {
                    atomic_store_explicit(
                        &psp_media_recovery_refusal_us, psp_media_stamp_us(),
                        memory_order_relaxed);
                    atomic_store_explicit(
                        &psp_media_recovery_stage, PSP_MEDIA_RECOVERY_REFUSED,
                        memory_order_relaxed);
                }
                /*
                 * Accepted, in the sense the caller needs: this unit is
                 * retired and the next one may be submitted. decoder_primed
                 * stays true because sceMpegGetAvcNalAu succeeded, and
                 * nothing else was torn down -- no reset, no re-prime, no
                 * quarantine. last_native_error is deliberately untouched:
                 * every recovery path reads it as "the decoder is broken".
                 */
                backend->stats.submitted_video_packets++;
                return MEDIA_BACKEND_ACCEPTED;
            }
            psp_media_log(
                "tilefinch-media-decoder: event=au-refused-escalated "
                "status=0x%08X consecutive=%u total=%u window=%u",
                (unsigned) status, backend->video_refusals_consecutive,
                backend->video_refusals_total,
                backend->video_refusal_window_count);
        }
        backend->stats.last_native_error = status;
        if (!psp_media_wide_program_rejected
            && psp_media_wide_program_rejected_by(
                   backend->mpeg_mode, backend->me_boot_type,
                   failed_stage == PSP_MEDIA_CODEC_STAGE_AVC_BRIDGE
                       || failed_stage == PSP_MEDIA_CODEC_STAGE_AVC_DECODE,
                   primed_before)) {
            /* The wide program never decoded anything on this device. Let the
               ordinary retry ladder recover this video, but stop paying the
               same failing create/prime round trip on every later one. */
            psp_media_wide_program_rejected = true;
            psp_media_log(
                "tilefinch-media-decoder: event=wide-program-rejected "
                "mode=%d me-type=%d stage=%s status=0x%08X source=%ux%u",
                backend->mpeg_mode, backend->me_boot_type, stage_name,
                (unsigned) status, backend->decoded_width,
                backend->decoded_height);
        }
#if defined(TILEFINCH_PSP_VALIDATION_LOG)
        /*
         * What the access-unit descriptor holds after the refusal, which is
         * the one thing that separates the two explanations the status cannot.
         * The descriptor is memset to 0xFF before sceMpegInitAu and is
         * firmware's to write from then on: if it still reads back as the
         * pattern InitAu left, the Media Engine never touched it and the
         * bridge rejected an argument; if it changed, firmware ran and
         * rejected the content it was given.
         *
         * Written by firmware on both sides: sceMpegGetAvcNalAu and
         * sceMpegAvcDecode both run on the main CPU and update this descriptor
         * through the CPU's own cache before any of it reaches the engine. A
         * pure invalidate would throw those partial writes away -- and they
         * are the evidence this dump exists to read. Write back first, then
         * drop, so a CPU-side refusal survives and an engine-side write is
         * still refetched from RAM. The address is 64-byte aligned and the
         * length is the whole 64-byte descriptor, which is what real firmware
         * requires of any invalidating cache op.
         */
        {
            char au_bytes[PSP_MEDIA_AU_DUMP_BYTES * 2u + 1u];
            char au_before_bytes[PSP_MEDIA_AU_DUMP_BYTES * 2u + 1u];
            sceKernelDcacheWritebackInvalidateRange(
                backend->video_au, PSP_MEDIA_VIDEO_AU_BYTES);
            psp_media_hex_bytes(
                backend->video_au, PSP_MEDIA_AU_DUMP_BYTES, au_bytes);
            /* SceMpegAu is {pts[8], dts[8], esBuffer[4], esSize[4]}, little
               endian. Before against after is what says which side produced
               the zero. */
            psp_media_hex_bytes(
                au_before, PSP_MEDIA_AU_DUMP_BYTES, au_before_bytes);
            psp_media_log(
                "tilefinch-media-decoder: event=au-before stage=%s bytes=%s",
                stage_name, au_before_bytes);
            psp_media_log(
                "tilefinch-media-decoder: event=au-dump stage=%s bytes=%s",
                stage_name, au_bytes);
        }
#endif
        /* The failure line syncs the card, so it goes last: the dump above
           rides that sync rather than paying for one of its own. */
        psp_media_log_failure(backend, stage_name, status);
        psp_media_error(
            error, error_size, "PSP AVC %s failed: 0x%08X",
            stage_name, (unsigned) status);
        return MEDIA_BACKEND_ERROR;
    }
    if (!psp_media_output_count_sane(backend->video_status)) {
        backend->stats.last_native_error = -1;
        psp_media_log_failure(
            backend, "decode-picture-count", backend->video_status);
        psp_media_error(
            error, error_size,
            "PSP AVC returned invalid picture count %d",
            (int) backend->video_status);
        return MEDIA_BACKEND_ERROR;
    }
    PspAvcDetail2 *detail = NULL;
    psp_media_codec_stage(backend, PSP_MEDIA_CODEC_STAGE_AVC_DETAIL);
    status = psp_media_query_avc_detail(
        backend, backend->video_status, &detail, error, error_size);
    if (status < 0) {
        backend->stats.last_native_error = status;
        return MEDIA_BACKEND_ERROR;
    }
    if (backend->raw_nal_probe_pending)
        backend->raw_nal_probe_packets++;
    if (backend->raw_nal_probe_pending
        && psp_media_raw_nal_probe_exhausted(
            backend->raw_nal_probe_packets,
            backend->video_status > 0)) {
        return psp_media_raw_nal_probe_failed(
            backend, error, error_size);
    }
    backend->stats.submitted_video_packets++;
    /* One unit closer to the end of a no-touch reposition's drain window. The
       access unit is the right clock for it: the engine releases what it held
       as new units arrive, not as time passes. */
    if (backend->video_reposition_drain_units != 0u)
        backend->video_reposition_drain_units--;
    /* A decoded unit ends any run of refusals; the total and the window keep
       counting, because a stream that refuses one unit a second is still a
       stream to give up on. */
    backend->video_refusals_consecutive = 0;
    if (backend->video_status > 0) {
        psp_media_note_picture_batch(
            backend, (unsigned) backend->video_status);
        status = psp_media_capture_avc_pictures(
            backend, backend->video_status, detail, error, error_size);
        /* Capture reads the firmware's picture arrays; only the conversion
           below writes a surface, and only it has to wait for one. A batch
           this could not finish stays captured-pending, which is a state the
           submit path already knows how to retire. */
        if (status >= 0) {
            unsigned converted = 0;
            psp_media_codec_stage(backend, PSP_MEDIA_CODEC_STAGE_AVC_CSC);
            status = psp_media_emit_batch_into_free_slots(
                backend, backend->codec_job_epoch, &converted,
                error, error_size);
            backend->stats.video_emits_worker += converted;
            /* Whether the fill-all-free-slots condition actually fires: how
               many of this batch's pictures the SAME job converted, and
               whether it got through all of them before publishing. */
            backend->stats.batch_fill_converted += converted;
            if (status >= 0 && !psp_media_batch_pending(backend))
                backend->stats.batch_fill_complete++;
        }
        if (status < 0) {
            backend->stats.last_native_error = status;
            return MEDIA_BACKEND_ERROR;
        }
    }
    return MEDIA_BACKEND_ACCEPTED;
}

/*
 * One AAC access unit, from the staging slot the browser thread put it in.
 *
 * Split out of the job so the job can run it more than once. Everything here
 * is what the single-unit path already did -- the same control-block words,
 * the same cache discipline in both directions, the same PCM queue push --
 * with the source and the output slot named by index rather than assumed.
 *
 * The cache maintenance is per unit and stays per unit. Each decode writes
 * one PSP_MEDIA_AUDIO_PCM_BYTES block and each block is published with its
 * own writeback, so a batched job publishes exactly the extent it wrote, one
 * block at a time; widening a single op to cover both would name a range the
 * second decode has not filled yet.
 */
static MediaBackendResult psp_media_decode_one_audio_au(
    PspMediaBackend *backend, unsigned index,
    char *error, size_t error_size)
{
    unsigned char *staged =
        backend->audio_staging + (size_t) index * PSP_MEDIA_AUDIO_AU_BYTES;
    size_t staged_bytes = backend->audio_staged_bytes[index];
    backend->audio_codec[6] =
        (unsigned long) staged;
    backend->audio_codec[7] =
        (unsigned long) staged_bytes;
    backend->audio_codec[8] = (unsigned long) backend->audio_pcm;
    backend->audio_codec[9] = PSP_MEDIA_AUDIO_PCM_BYTES;
    sceKernelDcacheWritebackRange(
        staged, (unsigned) staged_bytes);
    sceKernelDcacheWritebackRange(
        backend->audio_codec,
        psp_media_cache_extent(PSP_MEDIA_AUDIO_CODEC_BYTES));
    sceKernelDcacheWritebackInvalidateRange(
        backend->audio_pcm, PSP_MEDIA_AUDIO_PCM_BYTES);
    psp_media_codec_stage(backend, PSP_MEDIA_CODEC_STAGE_AAC_DECODE);
    /* AAC crosses into the same Media Engine through the same one job slot,
       so it is measured on the same clock as AVC. A video submission refused
       while this call is in flight is refused for the length of this call. */
    uint32_t firmware_entered_us = psp_media_stamp_us();
    int status = sceAudiocodecDecode(
        backend->audio_codec, PSP_CODEC_AAC);
    psp_media_note_worker_firmware(backend, firmware_entered_us);
    /* The control block is written from both sides: sceAudiocodecDecode's
       main-CPU wrapper updates its own words through the data cache, and the
       engine reports through the same block. Write back before dropping, or
       the CPU-side half is destroyed before it reaches RAM. */
    sceKernelDcacheWritebackInvalidateRange(
        backend->audio_codec,
        psp_media_cache_extent(PSP_MEDIA_AUDIO_CODEC_BYTES));
    /* PCM is Media Engine bulk output, and the CPU holds no dirty line of it
       here: the writeback-invalidate above ran before the call and nothing on
       this thread stores into it in between. A pure invalidate discards
       nothing and is the cheapest way to see what the engine wrote. */
    sceKernelDcacheInvalidateRange(
        backend->audio_pcm, PSP_MEDIA_AUDIO_PCM_BYTES);
    if (status < 0) {
        backend->stats.last_native_error = status;
#if defined(TILEFINCH_PSP_VALIDATION_LOG)
        /*
         * The head of the codec control block after the refusal. Words 0-11
         * cover the firmware-owned header, the substitute work buffer in word
         * 3, its size in word 4, and the input/output descriptors this decode
         * was handed -- so an unchanged block says the AAC program never ran
         * and a mutated one says it ran and refused the frame.
         *
         * The writeback-invalidate that follows every sceAudiocodecDecode
         * above already put firmware's copy in front of the CPU without
         * throwing away the words firmware wrote CPU-side; a second cache op
         * here would only cost cycles.
         */
        {
            char codec_words[PSP_MEDIA_CODEC_DUMP_WORDS * 9u];
            psp_media_hex_words(
                backend->audio_codec, PSP_MEDIA_CODEC_DUMP_WORDS,
                codec_words);
            psp_media_log(
                "tilefinch-media-decoder: event=codec-dump words=%s",
                codec_words);
        }
#endif
        /* Last, because the failure line is what syncs the card. */
        psp_media_log_failure(
            backend, "sceAudiocodecDecode", status);
        psp_media_error(
            error, error_size, "AAC decode: 0x%08X",
            (unsigned) status);
        return MEDIA_BACKEND_ERROR;
    }
    backend->stats.submitted_audio_packets++;
    backend->stats.decoded_audio_samples += PSP_MEDIA_AUDIO_SAMPLES;
    if (!atomic_load_explicit(
            &backend->audio_origin_initialized, memory_order_acquire)) {
        /* This unit's own presentation time, not the job's first: with a
           batch the two differ by one AAC frame, and the origin the whole
           audio clock is measured from must name the block that actually
           reached the queue first. */
        backend->audio_origin_us = backend->audio_staged_pts_us[index];
        atomic_store_explicit(
            &backend->audio_origin_initialized, true, memory_order_release);
    }
    unsigned slot =
        atomic_load(&backend->audio_queue_write)
        % PSP_MEDIA_AUDIO_QUEUE_SLOTS;
    int16_t *queued_pcm = psp_media_audio_queue_slot(backend, slot);
    memcpy(queued_pcm, backend->audio_pcm, PSP_MEDIA_AUDIO_PCM_BYTES);
    sceKernelDcacheWritebackRange(
        queued_pcm, PSP_MEDIA_AUDIO_PCM_BYTES);
    atomic_fetch_add(&backend->audio_queue_write, 1u);
    psp_media_codec_stage(backend, PSP_MEDIA_CODEC_STAGE_AUDIO_SIGNAL);
    status = sceKernelSetEventFlag(
        backend->audio_event, PSP_MEDIA_AUDIO_EVENT_READY);
    if (status < 0) {
        backend->stats.last_native_error = status;
        psp_media_log_failure(
            backend, "sceKernelSetEventFlag-audio", status);
        psp_media_error(
            error, error_size, "PCM output: 0x%08X",
            (unsigned) status);
        return MEDIA_BACKEND_ERROR;
    }
    return MEDIA_BACKEND_ACCEPTED;
}

/*
 * The job: every access unit the browser thread staged, back to back, on one
 * dispatch and one collection.
 *
 * The measured cost of an audio job was 8.8ms of which about 1.1ms was the
 * firmware call -- the rest being the dispatch to this thread and the wait
 * for the browser thread to collect. Decoding the second unit here rather
 * than in a job of its own pays the firmware cost twice and the round trip
 * once, which is the entire idea. It cannot make audio cheaper; it makes
 * audio interrupt video half as often.
 *
 * The loop stops early on its own time budget and leaves the remainder
 * staged. Every other guard was checked by the browser thread before the job
 * was queued, on facts that were true then; this is the one that depends on
 * what the first decode actually cost, which only this side knows.
 */
static MediaBackendResult psp_media_decode_staged_audio(
    PspMediaBackend *backend, char *error, size_t error_size)
{
    uint32_t started_us = psp_media_stamp_us();
    unsigned decoded = 0;
    while (backend->audio_staged_index < backend->audio_staged_count) {
        MediaBackendResult result = psp_media_decode_one_audio_au(
            backend, backend->audio_staged_index, error, error_size);
        if (result != MEDIA_BACKEND_ACCEPTED) return result;
        backend->audio_staged_index++;
        decoded++;
        if (backend->audio_staged_index >= backend->audio_staged_count) break;
        if (psp_media_stamp_us() - started_us
            >= PSP_MEDIA_AUDIO_BATCH_BUDGET_US) {
            /* The remainder stays staged and the flush path queues it. A
               partial batch is a job the browser thread still owes, not a
               dropped access unit. */
            backend->stats.audio_batch_budget_stops++;
            break;
        }
    }
    if (decoded > 1u) backend->stats.audio_batch_jobs++;
    backend->stats.audio_batch_aus += decoded;
    /* Retire the batch only when every unit of it has been decoded, so a
       budget stop leaves index < count for the flush to pick up. */
    if (backend->audio_staged_index >= backend->audio_staged_count) {
        backend->audio_staged_index = 0;
        backend->audio_staged_count = 0;
    }
    return MEDIA_BACKEND_ACCEPTED;
}

static MediaBackendResult psp_media_drain_staged_video(
    PspMediaBackend *backend, char *error, size_t error_size)
{
    SceInt32 output_status = 0;
    backend->video_drain_calls++;
    psp_media_codec_stage(backend, PSP_MEDIA_CODEC_STAGE_AVC_STOP);
    int status = sceMpegAvcDecodeStop(
        &backend->mpeg, (SceInt32) PSP_MEDIA_MPEG_DECODE_WIDTH,
        NULL, &output_status);
    if (status < 0) {
        backend->stats.last_native_error = status;
        psp_media_log_failure(
            backend, "sceMpegAvcDecodeStop", status);
        psp_media_error(
            error, error_size,
            "PSP AVC drain failed: 0x%08X", (unsigned) status);
        return MEDIA_BACKEND_ERROR;
    }
    if (!psp_media_output_count_sane(output_status)) {
        backend->stats.last_native_error = -1;
        psp_media_log_failure(
            backend, "drain-picture-count", output_status);
        psp_media_error(
            error, error_size,
            "PSP AVC drain returned invalid picture count %d",
            (int) output_status);
        return MEDIA_BACKEND_ERROR;
    }
    if (output_status > 0) {
        psp_media_note_picture_batch(backend, (unsigned) output_status);
        PspAvcDetail2 *detail = NULL;
        psp_media_codec_stage(backend, PSP_MEDIA_CODEC_STAGE_AVC_DETAIL);
        status = psp_media_query_avc_detail(
            backend, output_status, &detail, error, error_size);
        if (status >= 0)
            status = psp_media_capture_avc_pictures(
                backend, output_status, detail, error, error_size);
        /* The drain converts into the same slots, on the same thread, and
           waits for the same claims. */
        if (status >= 0) {
            unsigned converted = 0;
            psp_media_codec_stage(backend, PSP_MEDIA_CODEC_STAGE_AVC_CSC);
            status = psp_media_emit_batch_into_free_slots(
                backend, backend->codec_job_epoch, &converted,
                error, error_size);
            backend->stats.video_emits_worker += converted;
            /* Whether the fill-all-free-slots condition actually fires: how
               many of this batch's pictures the SAME job converted, and
               whether it got through all of them before publishing. */
            backend->stats.batch_fill_converted += converted;
            if (status >= 0 && !psp_media_batch_pending(backend))
                backend->stats.batch_fill_complete++;
        }
        if (status < 0) {
            backend->stats.last_native_error = status;
            return MEDIA_BACKEND_ERROR;
        }
        return MEDIA_BACKEND_ACCEPTED;
    }
    if (backend->raw_nal_probe_pending)
        return psp_media_raw_nal_probe_failed(
            backend, error, error_size);
    backend->stats.dropped_video_frames +=
        backend->video_timestamps.count;
    backend->video_timestamps.count = 0;
    return MEDIA_BACKEND_END;
}

/*
 * sceMpegDelete and sceMpegFinish are unbounded firmware calls into the Media
 * Engine, and destroy used to make them from the main thread after every other
 * step had already been bounded. Hardware proved why that is not enough: a
 * stream the ME rejected left the engine wedged, teardown walked its bounded
 * steps correctly, and then hung forever in the tail. The codec worker already
 * owns every other firmware codec call and every bounded-wait/quarantine
 * mechanism built around it, so the tail runs there too. sceAudiocodecReleaseEDRAM
 * joins it for uniformity: it is the same class of call and must still happen
 * after the audio channel has been released.
 *
 * Runs on the codec worker for a dispatched teardown job, and directly on the
 * main thread only when no worker exists to dispatch to (an early create
 * failure), which is exactly the pre-worker status quo.
 */
static MediaBackendResult psp_media_run_teardown_job(
    PspMediaBackend *backend, char *error, size_t error_size)
{
    (void) error;
    (void) error_size;
    if (backend->audio_edram) {
        if (backend->audio_edram_real) {
            /* Only a real call gets the stage: a reported stage names the
               firmware call a hung teardown is inside, and the substitute's
               release is a store that cannot hang. */
            psp_media_codec_stage(
                backend, PSP_MEDIA_CODEC_STAGE_AUDIO_EDRAM);
            backend->codec_teardown_edram_release =
                sceAudiocodecReleaseEDRAM(backend->audio_codec);
        } else if (backend->audio_codec != NULL) {
            /* PMPlayer's cooleyesAudiocodecReleaseEDRAM frees control word 3
               and zeroes it. Only the zeroing is ours to do: the storage is
               pool-owned, so it is reclaimed by the rewind at the end of a
               completed destroy and leaked on purpose by a quarantined one,
               exactly like every other buffer firmware may still be reading.
               No sceAudiocodecReleaseEDRAM call is made at all -- firmware
               must never be asked to free memory it never granted. */
            backend->audio_codec[3] = 0;
        }
        backend->audio_edram = false;
        backend->audio_edram_real = false;
    }
    if (backend->mpeg_created) {
        psp_media_codec_stage(
            backend, PSP_MEDIA_CODEC_STAGE_MPEG_DELETE);
        sceMpegDelete(&backend->mpeg);
        backend->mpeg_created = false;
    }
    /* A failed native submission/reset may leave libmpeg's process-global
       state unusable even after its per-stream object is deleted. Successful
       streams retain the cheap same-profile runtime; failures force only the
       documented Finish/Init boundary before a retry. The caller decided this
       on the main thread and applies the matching process statics once the
       job is proven complete. */
    if (backend->codec_teardown_finish_runtime) {
        psp_media_codec_stage(
            backend, PSP_MEDIA_CODEC_STAGE_MPEG_FINISH);
        sceMpegFinish();
    }
    backend->codec_teardown_ran = true;
    return MEDIA_BACKEND_ACCEPTED;
}

/*
 * Rebuild the decoder object instead of flushing it, which is what
 * validation_media_reset_mode=1 selects.
 *
 * Why this shape. PMPlayer Advance is the hardware-proven raw-NAL player this
 * bridge was modelled on, and its entire sceMpeg vocabulary after an open is
 * three calls: sceMpegGetAvcNalAu, sceMpegAvcDecode, sceMpegAvcDecodeDetail2.
 * It never flushes, never re-inits the access unit, and never touches the
 * decoder on a seek at all -- mp4_play_do_seek repositions the reader and
 * nothing else. sceMpegAvcDecodeFlush does not appear anywhere in that tree.
 * So the two calls our in-place reset makes are precisely the two the reference
 * never makes, and four device runs now put every wedge four to six seconds
 * after one of them while the first prime of a session has never produced one.
 *
 * The scope is the per-stream object and nothing above it. sceMpegDelete and
 * sceMpegCreate destroy and rebuild everything the hypothesis implicates -- the
 * elementary-stream and ringbuffer read/write state, stream registration, the
 * detail and conversion buffers. sceMpegInit and sceMpegFinish manage the
 * library runtime, which the create ladder already treats as process-lifetime
 * (it finishes only when the Media Engine boot type changes), and re-running
 * that pair would also mean re-booting the engine through kuKernelCall from
 * this thread. Left alone deliberately: if a rebuilt object still wedges, the
 * runtime tier is the next thing to try, and it is a separate experiment.
 *
 * Every buffer is reused where it lies. The Media Engine pool is a bump
 * allocator with no per-block free, so re-running the create ladder's
 * allocations would take a second 4 MiB set, exhaust the pool, fall back to the
 * newlib heap above the engine's addressable limit, and fail both codecs on
 * valid input -- the exact defect the pool exists to prevent. mpeg_memory, the
 * DDR arena, the AU object and the parameter sets keep their addresses; the
 * size query below is a check that they still fit, never a reason to allocate.
 *
 * Runs on the codec worker, for the same reason the teardown tail does.
 */
static MediaBackendResult psp_media_run_recreate_job(
    PspMediaBackend *backend, char *error, size_t error_size)
{
    backend->codec_recreate_ran = false;
    backend->codec_recreate_status = 0;
    const char *stage = "sceMpegDelete-recreate";
    if (backend->mpeg_created) {
        psp_media_codec_stage(
            backend, PSP_MEDIA_CODEC_STAGE_MPEG_DELETE);
        sceMpegDelete(&backend->mpeg);
        backend->mpeg_created = false;
    }
    psp_media_codec_stage(backend, PSP_MEDIA_CODEC_STAGE_MPEG_CREATE);
    /* The pins must not move. A mode change is the only thing that could make
       firmware want more than the block already reserved for it, and a reset
       never changes the mode -- so a larger answer here means an assumption
       broke, and the honest response is to fail the reset rather than to
       allocate a second time out of a pool that cannot give one back. */
    int required = (int) sceMpegQueryMemSize(backend->mpeg_mode);
    int status = required;
    if (required >= 0 && required > backend->mpeg_memory_bytes) {
        stage = "sceMpegQueryMemSize-recreate";
        status = -1;
    } else if (required >= 0) {
        memset(&backend->ringbuffer, 0, sizeof(backend->ringbuffer));
        stage = "sceMpegCreate-recreate";
        status = sceMpegCreate(
            &backend->mpeg, backend->mpeg_memory,
            backend->mpeg_memory_bytes, &backend->ringbuffer,
            (SceInt32) PSP_MEDIA_MPEG_DECODE_WIDTH, backend->mpeg_mode,
            (SceInt32) (uintptr_t) backend->ddr_memory);
        if (status >= 0) {
            backend->mpeg_created = true;
            backend->video_es =
                (unsigned char *) backend->ddr_memory + 0x10000;
            memset(backend->video_au, 0xFF, PSP_MEDIA_VIDEO_AU_BYTES);
            sceKernelDcacheWritebackInvalidateRange(
                backend->video_au, PSP_MEDIA_VIDEO_AU_BYTES);
            psp_media_codec_stage(
                backend, PSP_MEDIA_CODEC_STAGE_MPEG_INIT_AU);
            stage = "sceMpegInitAu-recreate";
            status = sceMpegInitAu(
                &backend->mpeg, backend->video_es, backend->video_au);
            /* sceMpegInitAu fills the descriptor from this CPU, so its stores
               are dirty in the data cache. Write them back before the lines are
               dropped -- a pure invalidate here is what once left RAM holding
               the 0xFF fill and handed the bridge a garbage descriptor. */
            if (status >= 0) {
                sceKernelDcacheWritebackInvalidateRange(
                    backend->video_au, PSP_MEDIA_VIDEO_AU_BYTES);
            }
        }
    }
    psp_media_log(
        "tilefinch-media-decoder: event=mpeg-recreate status=0x%08X "
        "stage=%s required=%d held=%d mode=%d",
        (unsigned) status, stage, required,
        backend->mpeg_memory_bytes, backend->mpeg_mode);
    backend->codec_recreate_status = status;
    backend->codec_recreate_ran = true;
    if (status < 0) {
        backend->stats.last_native_error = status;
        psp_media_log_failure(backend, stage, status);
        psp_media_error(
            error, error_size, "PSP decoder %s failed: 0x%08X",
            stage, (unsigned) status);
        return MEDIA_BACKEND_ERROR;
    }
    return MEDIA_BACKEND_ACCEPTED;
}

static void psp_media_capture_active_completion(
    PspMediaBackend *backend, PspMediaCodecCompletion *completion,
    uint32_t done_us)
{
    *completion = (PspMediaCodecCompletion) {
        .kind = (PspMediaCodecJobKind) atomic_load_explicit(
            &backend->codec_job_kind, memory_order_acquire),
        .result = backend->codec_job_result,
        .epoch = backend->codec_job_epoch,
        .started_us = atomic_load_explicit(
            &backend->codec_job_started_us, memory_order_acquire),
        .done_us = done_us,
        .native_stage = atomic_load_explicit(
            &backend->codec_native_stage, memory_order_acquire)
    };
    memcpy(completion->error, backend->codec_job_error,
           sizeof(completion->error));
    completion->error[sizeof(completion->error) - 1u] = '\0';
}

/*
 * Publish the finished active result and immediately promote one prepared job.
 *
 * The prepared-state CAS is the ownership transfer. EMPTY is closed atomically
 * before DONE is published, so a preparation can never be stranded behind a
 * worker that went back to sleep without seeing it.
 */
static bool psp_media_worker_chain_prepared(
    PspMediaBackend *backend, uint32_t done_us)
{
    if (backend->codec_job_result == MEDIA_BACKEND_ERROR
        || atomic_load_explicit(
               &backend->codec_stop, memory_order_acquire)
        || atomic_load_explicit(
               &backend->codec_job_state, memory_order_acquire)
               != PSP_MEDIA_CODEC_JOB_RUNNING) return false;
    {
        bool completion_empty = atomic_load_explicit(
            &backend->codec_completion_state, memory_order_acquire)
            == PSP_MEDIA_CODEC_COMPLETION_EMPTY;
        PspMediaPreparedWorkerResult prepared_result =
            psp_media_prepared_worker_take_or_close(
                &backend->codec_prepared_state, completion_empty);
        if (prepared_result != PSP_MEDIA_PREPARED_WORKER_CLAIMED)
            return false;

        psp_media_capture_active_completion(
            backend, &backend->codec_completion, done_us);
        atomic_store_explicit(
            &backend->codec_completion_state,
            PSP_MEDIA_CODEC_COMPLETION_READY, memory_order_release);

        PspMediaCodecPreparedJob prepared = backend->codec_prepared;
        uint32_t started_us = psp_media_stamp_us();
        uint32_t prepared_wait_us = started_us - prepared.prepared_us;
        backend->stats.worker_chained_jobs++;
        backend->stats.worker_prepared_wait_total_us +=
            (uint64_t) prepared_wait_us;
        if (prepared_wait_us > backend->stats.worker_prepared_wait_max_us)
            backend->stats.worker_prepared_wait_max_us = prepared_wait_us;
        atomic_store_explicit(
            &backend->codec_job_kind, prepared.kind, memory_order_release);
        backend->codec_job_epoch = prepared.epoch;
        atomic_store_explicit(
            &backend->codec_job_started_us, started_us,
            memory_order_release);
        backend->codec_job_result = MEDIA_BACKEND_ERROR;
        backend->codec_job_error[0] = '\0';
        backend->codec_job_audio_pts_us = prepared.audio_pts_us;
        psp_media_codec_stage(backend, PSP_MEDIA_CODEC_STAGE_NONE);
        /* CLAIMED has one owner and cancellation cannot take it. The helper's
           result exists for the host stress test; there is no recovery path
           here that may overwrite the already-published completion. */
        (void) psp_media_prepared_release_claimed(
            &backend->codec_prepared_state);
        return true;
    }
}

static int psp_media_codec_thread(SceSize argument_size, void *arguments)
{
    PspMediaBackend *backend = NULL;
    if (arguments != NULL && argument_size == sizeof(backend))
        memcpy(&backend, arguments, sizeof(backend));
    if (backend == NULL) return -1;
    while (!atomic_load_explicit(
               &backend->codec_stop, memory_order_acquire)) {
        uint32_t bits = 0;
        int waited = sceKernelWaitEventFlag(
            backend->codec_event,
            PSP_MEDIA_CODEC_EVENT_READY | PSP_MEDIA_CODEC_EVENT_STOP,
            PSP_EVENT_WAITOR | PSP_EVENT_WAITCLEAR, &bits, NULL);
        if (waited < 0) {
            psp_media_codec_stage(
                backend, PSP_MEDIA_CODEC_STAGE_WORKER_WAIT);
            psp_media_error(
                backend->codec_job_error,
                sizeof(backend->codec_job_error),
                "Codec worker wait: 0x%08X", (unsigned) waited);
            backend->codec_job_result = MEDIA_BACKEND_ERROR;
            atomic_store_explicit(
                &backend->codec_job_state, PSP_MEDIA_CODEC_JOB_DONE,
                memory_order_release);
            break;
        }
        if ((bits & PSP_MEDIA_CODEC_EVENT_STOP) != 0
            || atomic_load_explicit(
                &backend->codec_stop, memory_order_acquire)) break;
        if (atomic_load_explicit(
                &backend->codec_job_state, memory_order_acquire)
            != PSP_MEDIA_CODEC_JOB_RUNNING) continue;
        /*
         * The worker has its job. Everything between the browser thread's
         * event-flag post and this instant is scheduler latency -- the only
         * form of "ready but not running" this target can measure without a
         * profiler, and the first thing to rule out before blaming the
         * conversion cadence for a rate the Media Engine could have met.
         */
        for (;;) {
            backend->worker_wake_us = psp_media_stamp_us();
            {
                uint32_t dispatch_us =
                    backend->worker_wake_us
                    - atomic_load_explicit(
                        &backend->codec_job_started_us,
                        memory_order_acquire);
                backend->stats.worker_dispatches++;
                backend->stats.worker_dispatch_total_us +=
                    (uint64_t) dispatch_us;
                if (dispatch_us > backend->stats.worker_dispatch_max_us)
                    backend->stats.worker_dispatch_max_us = dispatch_us;
            }
            backend->codec_job_error[0] = '\0';
            PspMediaCodecJobKind job_kind =
                (PspMediaCodecJobKind) atomic_load_explicit(
                    &backend->codec_job_kind, memory_order_acquire);
            if (job_kind == PSP_MEDIA_CODEC_KIND_VIDEO) {
                backend->codec_job_result = psp_media_decode_staged_video(
                    backend, backend->codec_job_error,
                    sizeof(backend->codec_job_error));
            } else if (job_kind == PSP_MEDIA_CODEC_KIND_AUDIO) {
                backend->codec_job_result = psp_media_decode_staged_audio(
                    backend, backend->codec_job_error,
                    sizeof(backend->codec_job_error));
            } else if (job_kind == PSP_MEDIA_CODEC_KIND_DRAIN) {
                backend->codec_job_result = psp_media_drain_staged_video(
                    backend, backend->codec_job_error,
                    sizeof(backend->codec_job_error));
            } else if (job_kind == PSP_MEDIA_CODEC_KIND_TEARDOWN) {
                backend->codec_job_result = psp_media_run_teardown_job(
                    backend, backend->codec_job_error,
                    sizeof(backend->codec_job_error));
            } else if (job_kind == PSP_MEDIA_CODEC_KIND_RECREATE) {
                backend->codec_job_result = psp_media_run_recreate_job(
                    backend, backend->codec_job_error,
                    sizeof(backend->codec_job_error));
            } else {
                psp_media_codec_stage(backend, PSP_MEDIA_CODEC_STAGE_NONE);
                psp_media_error(
                    backend->codec_job_error,
                    sizeof(backend->codec_job_error),
                    "PSP codec worker received invalid job");
                backend->codec_job_result = MEDIA_BACKEND_ERROR;
            }
            uint32_t done_us = psp_media_stamp_us();
            if (psp_media_worker_chain_prepared(backend, done_us))
                continue;
            /* Written before the release store that publishes the job, so the
               browser thread reads a whole value through the same acquire that
               makes the result visible. */
            backend->codec_job_done_us = done_us;
            atomic_store_explicit(
                &backend->codec_job_state, PSP_MEDIA_CODEC_JOB_DONE,
                memory_order_release);
            break;
        }
    }
    return 0;
}

static int psp_media_account_codec_completion(
    PspMediaBackend *backend, const PspMediaCodecCompletion *completion,
    char *error, size_t error_size)
{
    uint32_t now_us = psp_media_stamp_us();
    uint32_t elapsed = now_us - completion->started_us;
    /* Per-job logging once emitted several thousand lines per soak and made
       validation I/O part of the measured codec cadence. The counters below
       retain count, total, maximum, buckets and collection latency; exceptional
       completions are still classified by the stale/failure paths below. */
    switch (completion->kind) {
    case PSP_MEDIA_CODEC_KIND_VIDEO:
        backend->stats.job_video_count++;
        backend->stats.job_video_total_us += (uint64_t) elapsed;
        break;
    case PSP_MEDIA_CODEC_KIND_DRAIN:
        backend->stats.job_drain_count++;
        backend->stats.job_drain_total_us += (uint64_t) elapsed;
        break;
    case PSP_MEDIA_CODEC_KIND_AUDIO:
        backend->stats.job_audio_count++;
        backend->stats.job_audio_total_us += (uint64_t) elapsed;
        backend->stats.job_audio_buckets[psp_media_job_bucket(elapsed)]++;
        if (elapsed > backend->stats.job_audio_max_us)
            backend->stats.job_audio_max_us = elapsed;
        break;
    default:
        backend->stats.job_other_count++;
        backend->stats.job_other_total_us += (uint64_t) elapsed;
        break;
    }
    if (completion->done_us != 0u) {
        uint32_t collect_us = now_us - completion->done_us;
        backend->stats.worker_collects++;
        backend->stats.worker_collect_total_us += (uint64_t) collect_us;
        if (collect_us > backend->stats.worker_collect_max_us)
            backend->stats.worker_collect_max_us = collect_us;
    }
    bool stale = !psp_media_epoch_current(
        completion->epoch, backend->session_epoch);
    if (stale) {
        psp_media_log(
            "tilefinch-media-decoder: event=codec-job-stale kind=%d "
            "result=%d job-epoch=%llu session-epoch=%llu surfaced=%d",
            (int) completion->kind, (int) completion->result,
            (unsigned long long) completion->epoch,
            (unsigned long long) backend->session_epoch,
            completion->result == MEDIA_BACKEND_ERROR ? 1 : 0);
        backend->stats.stale_codec_jobs++;
        if (completion->result != MEDIA_BACKEND_ERROR) return 0;
    }
    if (completion->result != MEDIA_BACKEND_ERROR) return 0;
    psp_media_error(
        error, error_size, "%s",
        completion->error[0] != '\0'
            ? completion->error : "PSP codec worker failed");
    return -1;
}

/* Main-thread collector.  The release/acquire job-state handoff makes all
   firmware output and queue metadata visible before the browser observes a
   completed job. Returns 1 while native work is pending, 0 on success/idle,
   and -1 after surfacing a completed error. */
static int psp_media_collect_codec_job(
    PspMediaBackend *backend, char *error, size_t error_size)
{
    int completion_expected = PSP_MEDIA_CODEC_COMPLETION_READY;
    if (atomic_compare_exchange_strong_explicit(
            &backend->codec_completion_state, &completion_expected,
            PSP_MEDIA_CODEC_COMPLETION_READING,
            memory_order_acq_rel, memory_order_acquire)) {
        PspMediaCodecCompletion completion = backend->codec_completion;
        int completion_result = psp_media_account_codec_completion(
            backend, &completion, error, error_size);
        atomic_store_explicit(
            &backend->codec_completion_state,
            PSP_MEDIA_CODEC_COMPLETION_EMPTY, memory_order_release);
        if (completion_result < 0) return -1;
    }
    int state = atomic_load_explicit(
        &backend->codec_job_state, memory_order_acquire);
    if (state == PSP_MEDIA_CODEC_JOB_RUNNING) {
        /* A worker which exits outside its normal publication path would
           otherwise look identical to a firmware call still in progress and
           consume the full frontend no-progress timeout.  ReferThreadStatus
           is nonblocking on real 6.6x firmware; cadence it because submit and
           advance are hot paths. */
        uint32_t now_us = sceKernelGetSystemTimeLow();
        if (backend->codec_worker_next_health_us == 0
            || (int32_t) (
                   now_us - backend->codec_worker_next_health_us) >= 0) {
            backend->codec_worker_next_health_us =
                now_us + UINT32_C(100000);
            PspThreadObservation observation;
            int referred = psp_thread_observe(
                backend->codec_thread, &observation);
            PspModuleWorkerPollDisposition poll =
                psp_module_worker_poll_disposition(
                    referred, observation.status);
            if (poll != PSP_MODULE_WORKER_POLL_PENDING) {
                /* Completion publication and thread exit are adjacent. The
                   first state load can race just before the worker's release
                   store while ReferThreadStatus observes it just after exit.
                   Re-read before classifying that terminal state as an
                   unexpected death, or a successful frame can be replaced by
                   a synthetic worker error. */
                state = atomic_load_explicit(
                    &backend->codec_job_state, memory_order_acquire);
                if (state == PSP_MEDIA_CODEC_JOB_RUNNING) {
                    int failure = poll == PSP_MODULE_WORKER_POLL_ERROR
                        ? referred
                        : psp_unexpected_worker_exit_status(
                              observation.status,
                              observation.exit_status);
                    backend->stats.last_native_error = failure;
                    psp_media_log_failure(
                        backend, "codec-worker-thread", failure);
                    psp_media_error(
                        backend->codec_job_error,
                        sizeof(backend->codec_job_error),
                        "Codec worker exited: 0x%08X",
                        (unsigned) failure);
                    backend->codec_job_result = MEDIA_BACKEND_ERROR;
                    atomic_store_explicit(
                        &backend->codec_job_state,
                        PSP_MEDIA_CODEC_JOB_DONE,
                        memory_order_release);
                    state = PSP_MEDIA_CODEC_JOB_DONE;
                }
            }
        }
        /*
         * A worker that is alive and inside firmware looks identical to one
         * doing useful work, and the health check above deliberately reports
         * it as healthy -- it is. What it is not is finite. Two soaks entered
         * sceMpegGetAvcNalAu with the same access unit and never came out, and
         * because nothing could be submitted after that the whole pipeline
         * went quiet: the frontend eventually blamed the absence of packets,
         * and the transport, which had simply not been asked for anything,
         * looked idle. Name the call that stopped returning, here, where the
         * stage is known.
         */
        uint32_t started_us = atomic_load_explicit(
            &backend->codec_job_started_us, memory_order_acquire);
        uint32_t running_us = now_us - started_us;
        if (state == PSP_MEDIA_CODEC_JOB_RUNNING
            && running_us >= PSP_MEDIA_CODEC_JOB_WATCHDOG_US) {
            /* RUNNING deliberately spans a prepared-job promotion. Re-sample
               both members, then require one later browser visit to observe
               the same overdue job. This prevents the watchdog from joining
               RUNNING from the completing job to started_us/native_stage from
               its successor. Device logs exposed the old race as a synthetic
               1.5 s AAC timeout followed by a 7 ms successful completion. */
            int confirmed_state = atomic_load_explicit(
                &backend->codec_job_state, memory_order_acquire);
            uint32_t confirmed_started_us = atomic_load_explicit(
                &backend->codec_job_started_us, memory_order_acquire);
            if (confirmed_state != PSP_MEDIA_CODEC_JOB_RUNNING
                || confirmed_started_us != started_us) {
                backend->codec_watchdog_candidate = false;
                state = confirmed_state;
            } else if (!backend->codec_watchdog_candidate
                       || backend->codec_watchdog_candidate_started_us
                              != started_us) {
                backend->codec_watchdog_candidate = true;
                backend->codec_watchdog_candidate_started_us = started_us;
                return 1;
            } else {
                int wedged_stage = atomic_load_explicit(
                    &backend->codec_native_stage, memory_order_acquire);
                const char *wedged_name =
                    psp_media_codec_stage_name(wedged_stage);
                backend->stats.last_native_error =
                    (int) PSP_MEDIA_ERROR_BUSY;
                psp_media_log_failure(
                    backend, wedged_name, (int) PSP_MEDIA_ERROR_BUSY);
                psp_media_error(
                    error, error_size,
                    "PSP %s did not return after %ums",
                    wedged_name, (unsigned) (running_us / 1000u));
                /* Do not forge a worker completion. The worker still owns
                   the job fields and may return just after this verdict. The
                   caller stops the session; destroy then either observes its
                   genuine completion or quarantines the still-running
                   firmware owner. */
                return -1;
            }
        }
        if (state == PSP_MEDIA_CODEC_JOB_RUNNING) return 1;
    }
    backend->codec_watchdog_candidate = false;
    if (state == PSP_MEDIA_CODEC_JOB_IDLE) return 0;
    PspMediaCodecCompletion completion;
    psp_media_capture_active_completion(
        backend, &completion, backend->codec_job_done_us);
    backend->codec_job_done_us = 0u;
    atomic_store_explicit(
        &backend->codec_job_kind, PSP_MEDIA_CODEC_KIND_NONE,
        memory_order_release);
    atomic_store_explicit(
        &backend->codec_job_state, PSP_MEDIA_CODEC_JOB_IDLE,
        memory_order_release);
    (void) psp_media_prepared_reopen_closed(
        &backend->codec_prepared_state);
    return psp_media_account_codec_completion(
        backend, &completion, error, error_size);
}

static bool psp_media_prepared_pair_allowed(
    PspMediaCodecJobKind active, PspMediaCodecJobKind prepared)
{
    /* First device experiment: remove the measured video->audio collection
       bubble without making video admission outlive its surface/refusal
       checks. The reverse direction can be added only if this measured step
       leaves enough throughput on the table to justify the larger proof. */
    return active == PSP_MEDIA_CODEC_KIND_VIDEO
        && prepared == PSP_MEDIA_CODEC_KIND_AUDIO;
}

static MediaBackendResult psp_media_queue_codec_job(
    PspMediaBackend *backend, PspMediaCodecJobKind kind,
    char *error, size_t error_size);

static MediaBackendResult psp_media_prepare_staged_audio(
    PspMediaBackend *backend, char *error, size_t error_size)
{
    if (backend == NULL || backend->admissions_closed)
        return MEDIA_BACKEND_WOULD_BLOCK;
    int state = atomic_load_explicit(
        &backend->codec_job_state, memory_order_acquire);
    PspMediaCodecJobKind active =
        (PspMediaCodecJobKind) atomic_load_explicit(
            &backend->codec_job_kind, memory_order_acquire);
    if (state != PSP_MEDIA_CODEC_JOB_RUNNING
        || !psp_media_prepared_pair_allowed(
            active, PSP_MEDIA_CODEC_KIND_AUDIO))
        return MEDIA_BACKEND_WOULD_BLOCK;

    /* audio_staging is disjoint from the packet buffer and decoded surfaces
       used by the active video job. Fill the descriptor before READY is
       published; if the worker closes EMPTY first, the CAS simply refuses the
       preparation and the ordinary collection path queues it next visit. */
    backend->codec_prepared = (PspMediaCodecPreparedJob) {
        .kind = PSP_MEDIA_CODEC_KIND_AUDIO,
        .epoch = backend->session_epoch,
        .prepared_us = psp_media_stamp_us(),
        .audio_pts_us =
            backend->audio_staged_pts_us[backend->audio_staged_index]
    };
    if (!psp_media_prepared_try_publish(&backend->codec_prepared_state)) {
        int expected = atomic_load_explicit(
            &backend->codec_prepared_state, memory_order_acquire);
        if (expected == PSP_MEDIA_CODEC_PREPARED_CLOSED) {
            int collected = psp_media_collect_codec_job(
                backend, error, error_size);
            if (collected < 0) return MEDIA_BACKEND_ERROR;
            if (collected == 0)
                return psp_media_queue_codec_job(
                    backend, PSP_MEDIA_CODEC_KIND_AUDIO,
                    error, error_size);
        }
        /* The caller already copied this access unit into the bounded audio
           staging buffer. Keep that ownership and let the end-of-visit flush
           queue it, rather than returning WOULD_BLOCK and causing the demuxer
           to offer (and stage) the same unit a second time. */
        return MEDIA_BACKEND_ACCEPTED;
    }
    backend->stats.worker_prepared_jobs++;
    backend->stats.worker_prepared_audio++;
    return MEDIA_BACKEND_QUEUED;
}

static void psp_media_cancel_prepared_job(PspMediaBackend *backend)
{
    if (backend == NULL) return;
    if (psp_media_prepared_try_cancel(&backend->codec_prepared_state)) {
        if (backend->codec_prepared.kind == PSP_MEDIA_CODEC_KIND_AUDIO) {
            backend->audio_staged_count = 0u;
            backend->audio_staged_index = 0u;
        }
        backend->stats.worker_prepared_cancelled++;
        (void) psp_media_prepared_release_claimed(
            &backend->codec_prepared_state);
        return;
    }
    (void) psp_media_prepared_reopen_closed(
        &backend->codec_prepared_state);
}

static MediaBackendResult psp_media_queue_codec_job(
    PspMediaBackend *backend, PspMediaCodecJobKind kind,
    char *error, size_t error_size)
{
    atomic_store_explicit(
        &backend->codec_job_kind, kind, memory_order_release);
    backend->codec_job_result = MEDIA_BACKEND_ERROR;
    backend->codec_job_error[0] = '\0';
    /*
     * The epoch this job belongs to, copied rather than shared.
     *
     * Written before the release store below and never touched again until the
     * job completes, so the worker reads a whole value without an atomic and a
     * seek that increments the session epoch mid-job cannot change what this
     * job thinks it is. Everything the worker stamps -- every slot it converts
     * into -- carries this, and a completion carrying anything but the current
     * session epoch is discarded rather than credited.
     */
    backend->codec_job_epoch = backend->session_epoch;
    psp_media_codec_stage(backend, PSP_MEDIA_CODEC_STAGE_NONE);
    atomic_store_explicit(
        &backend->codec_job_started_us, sceKernelGetSystemTimeLow(),
        memory_order_release);
    atomic_store_explicit(
        &backend->codec_job_state, PSP_MEDIA_CODEC_JOB_RUNNING,
        memory_order_release);
    int status = sceKernelSetEventFlag(
        backend->codec_event, PSP_MEDIA_CODEC_EVENT_READY);
    if (status >= 0) return MEDIA_BACKEND_QUEUED;
    if (kind == PSP_MEDIA_CODEC_KIND_VIDEO)
        backend->video_timestamps =
            backend->codec_job_timestamps_before;
    atomic_store_explicit(
        &backend->codec_job_state, PSP_MEDIA_CODEC_JOB_IDLE,
        memory_order_release);
    atomic_store_explicit(
        &backend->codec_job_kind, PSP_MEDIA_CODEC_KIND_NONE,
        memory_order_release);
    backend->stats.last_native_error = status;
    psp_media_log_failure(
        backend, "sceKernelSetEventFlag-codec", status);
    psp_media_error(
        error, error_size, "Codec queue: 0x%08X",
        (unsigned) status);
    return MEDIA_BACKEND_ERROR;
}

/* A decode returned more pictures than the surface can hold, and the surplus is
   still waiting for a visit allowed to convert it. */
static bool psp_media_batch_pending(const PspMediaBackend *backend)
{
    return backend->video_picture_index < backend->video_picture_count;
}

/*
 * Name the hold that spent this video-submit or drain opportunity.
 *
 * MediaPlaybackJobStats::submit_block_calls counts the same events and is left
 * exactly as it is; it simply cannot say which of four independent conditions
 * produced a WOULD_BLOCK, and the four have entirely different fixes. A visit
 * turned away while a captured batch was pending is recorded twice on purpose:
 * once as its hold, once as a conversion that had a picture and no visit.
 */
static void psp_media_note_video_hold(
    PspMediaBackend *backend, size_t *counter)
{
    (*counter)++;
    if (psp_media_batch_pending(backend))
        backend->cadence.emit_visit_starved++;
}

/*
 * Which job kind spent this video-submit opportunity.
 *
 * cadence.hold_job_slot already counts them, and its whole value was that a
 * WOULD_BLOCK could finally be attributed to one of four holds instead of an
 * aggregate. This is the same argument one level down: the job-slot hold is
 * itself three different events. A video job holding the slot is the pipeline
 * working -- the next picture is being decoded. An audio job holding it is
 * video stopped for a reason no amount of video tuning can reach, and the
 * only fix for it is a second job slot. Counting them together makes those
 * two look like one number that is simply large.
 *
 * The audio case also records how far into the audio job the refusal landed,
 * because a count alone cannot become an occupancy claim: ten refusals spread
 * across one long job and ten refusals against ten short ones are the same
 * count and different pipelines.
 *
 * Called from the video submit path only, so the four sum exactly to
 * cadence.hold_job_slot and a reader can check that they do.
 */
static void psp_media_note_job_holder(PspMediaBackend *backend)
{
    switch ((PspMediaCodecJobKind) atomic_load_explicit(
                &backend->codec_job_kind, memory_order_acquire)) {
    case PSP_MEDIA_CODEC_KIND_VIDEO:
        backend->stats.hold_job_by_video++;
        break;
    case PSP_MEDIA_CODEC_KIND_DRAIN:
        backend->stats.hold_job_by_drain++;
        break;
    case PSP_MEDIA_CODEC_KIND_AUDIO: {
        backend->stats.hold_job_by_audio++;
        uint32_t age_us =
            psp_media_stamp_us() - atomic_load_explicit(
                &backend->codec_job_started_us, memory_order_acquire);
        backend->stats.hold_job_audio_age_total_us += (uint64_t) age_us;
        if (age_us > backend->stats.hold_job_audio_age_max_us)
            backend->stats.hold_job_audio_age_max_us = age_us;
        break;
    }
    default:
        backend->stats.hold_job_by_other++;
        break;
    }
}

/*
 * Would a video access unit be admitted right now?
 *
 * Every gate psp_media_submit's video path applies, asked without a sample in
 * hand, so the audio path can decline to extend a job that video is waiting
 * on. It is deliberately the same list and in the same order: a copy that
 * drifted would either starve audio of batches it could safely take, or --
 * worse -- claim video was not waiting when it was.
 *
 * It cannot know whether the pump HAS a video unit to offer; that is the
 * scheduler's knowledge. It answers the question the backend can answer,
 * which is whether one would get in, and that is the half that changes: in
 * the measured soak both slots sat READY for 76 of 90 seconds, so no video
 * unit was admissible and every one of those windows was free for audio.
 */
static bool psp_media_video_admissible(const PspMediaBackend *backend)
{
    if (backend == NULL || !backend->have_video
        || backend->admissions_closed) return false;
    if (!psp_media_advance_may_submit_video(psp_media_advance_mode))
        return false;
    if (atomic_load_explicit(
            &psp_media_video_refusal_hold, memory_order_acquire)) return false;
    if (atomic_load_explicit(
            &backend->codec_job_state, memory_order_acquire)
        == PSP_MEDIA_CODEC_JOB_RUNNING) return false;
    if (psp_media_slot_free_count(
            backend->slots, PSP_MEDIA_SURFACE_SLOTS) == 0) return false;
    if (psp_media_batch_pending(backend)) return false;
    return backend->video_timestamps.count < PSP_MEDIA_VIDEO_TIMESTAMP_SLOTS;
}

static unsigned psp_media_audio_pending_count(
    const PspMediaBackend *backend)
{
    return backend == NULL ? 0u
        : backend->audio_pending_write - backend->audio_pending_read;
}

static MediaBackendResult psp_media_enqueue_pending_audio(
    PspMediaBackend *backend, const MediaMp4Sample *sample,
    const unsigned char *payload, size_t length)
{
    unsigned count = psp_media_audio_pending_count(backend);
    if (backend == NULL || sample == NULL || payload == NULL
        || backend->audio_pending == NULL
        || length > PSP_MEDIA_AUDIO_AU_BYTES
        || count >= PSP_MEDIA_AUDIO_PENDING_SLOTS) {
        if (backend != NULL) backend->stats.audio_pending_full++;
        return MEDIA_BACKEND_WOULD_BLOCK;
    }
    unsigned slot = backend->audio_pending_write
        % PSP_MEDIA_AUDIO_PENDING_SLOTS;
    memcpy(backend->audio_pending
               + (size_t) slot * PSP_MEDIA_AUDIO_AU_BYTES,
           payload, length);
    backend->audio_pending_bytes[slot] = length;
    backend->audio_pending_pts_us[slot] = psp_sample_pts_us(sample);
    backend->audio_pending_write++;
    if (count == 0u) backend->audio_pending_since_us = psp_media_stamp_us();
    count++;
    backend->stats.audio_pending_enqueued++;
    if (count > backend->stats.audio_pending_peak)
        backend->stats.audio_pending_peak = count;
    return MEDIA_BACKEND_ACCEPTED;
}

static void psp_media_promote_pending_audio(PspMediaBackend *backend)
{
    if (backend == NULL || backend->audio_staged_count != 0u
        || backend->audio_pending == NULL) return;
    unsigned count = psp_media_audio_pending_count(backend);
    unsigned promote = count < PSP_MEDIA_AUDIO_BATCH_MAXIMUM
        ? count : PSP_MEDIA_AUDIO_BATCH_MAXIMUM;
    for (unsigned at = 0; at < promote; at++) {
        unsigned slot = backend->audio_pending_read
            % PSP_MEDIA_AUDIO_PENDING_SLOTS;
        size_t length = backend->audio_pending_bytes[slot];
        memcpy(backend->audio_staging
                   + (size_t) at * PSP_MEDIA_AUDIO_AU_BYTES,
               backend->audio_pending
                   + (size_t) slot * PSP_MEDIA_AUDIO_AU_BYTES,
               length);
        backend->audio_staged_bytes[at] = length;
        backend->audio_staged_pts_us[at] =
            backend->audio_pending_pts_us[slot];
        backend->audio_pending_read++;
        backend->stats.audio_pending_promoted++;
    }
    backend->audio_staged_index = 0u;
    backend->audio_staged_count = promote;
    if (psp_media_audio_pending_count(backend) == 0u)
        backend->audio_pending_since_us = 0u;
}

/*
 * Yes, offer the partner now -- inside this same pump visit.
 *
 * The batching mechanism was complete and starved: a device soak measured
 * 1.32 blocks per job against a ceiling of 2, with audio delivery pinned at
 * 26 blocks a second. The reason was upstream of everything the worker does.
 * A pump visit spends a bounded number of packets and shares them with video,
 * so a visit whose first packet went to a video unit had nothing left for the
 * partner, and the staged unit was flushed alone. Pairs only formed when two
 * visits happened to precede a job.
 *
 * This is the backend saying it is holding one and would take another. Three
 * things make it safe to grant, and all three are re-decided here rather than
 * inherited from the decision that staged the first unit:
 *
 *   - It is audio. Video is never deferred and never batched.
 *   - The batch has room, and is not empty. An empty batch has no partner to
 *     pair with, and a full one is already on its way to the worker.
 *   - Video cannot use the decoder right now. This is the same question
 *     psp_media_submit asked before deferring the first unit, asked again
 *     because the answer moves: a slot may have been claimed since, and if it
 *     has, the visit belongs to video and the staged unit goes out alone.
 *
 * What it does NOT do is choose. The pump still selects the earliest access
 * unit across both sources, so a video unit due before the partner is still
 * submitted first; this only ensures the visit has a packet left to do it
 * with. And it never reaches for bytes: a partner the demuxer does not
 * already hold ends the visit on the ordinary would-block path.
 */
static bool psp_media_wants_paired_submit(const void *opaque, int track_kind)
{
    const PspMediaBackend *backend = opaque;
    if (backend == NULL || track_kind != MEDIA_MP4_TRACK_AUDIO) return false;
    if (!backend->have_audio || backend->admissions_closed) return false;
    /* A refusal recovery needs one stable, observable backend snapshot. New
       audio jobs could otherwise keep the shared slot continuously RUNNING,
       making "stats unavailable" indistinguishable from "no refusal" to the
       browser thread. An already-prepared job may finish; no new one starts. */
    if (atomic_load_explicit(
            &psp_media_video_refusal_hold, memory_order_acquire)
        || atomic_load_explicit(
               &backend->video_refusal_dirty, memory_order_acquire))
        return false;
    bool staging_wants_partner = backend->audio_staged_count > 0u
        && backend->audio_staged_count < PSP_MEDIA_AUDIO_BATCH_MAXIMUM;
    unsigned pending = psp_media_audio_pending_count(backend);
    bool pending_has_room = pending > 0u
        && pending < PSP_MEDIA_AUDIO_PENDING_SLOTS;
    if (!staging_wants_partner && !pending_has_room)
        return false;
    if (psp_media_video_admissible(backend)) {
        atomic_fetch_add_explicit(
            &psp_media_audio_batch_video_declines, 1u, memory_order_relaxed);
        return false;
    }
    return true;
}

/* Hand the staged batch to the worker. The job's presentation time is the
   first unit's; every later unit carries its own in audio_staged_pts_us. */
static MediaBackendResult psp_media_queue_staged_audio(
    PspMediaBackend *backend, char *error, size_t error_size)
{
    if (atomic_load_explicit(
            &psp_media_video_refusal_hold, memory_order_acquire)
        || atomic_load_explicit(
               &backend->video_refusal_dirty, memory_order_acquire))
        return MEDIA_BACKEND_WOULD_BLOCK;
    int state = atomic_load_explicit(
        &backend->codec_job_state, memory_order_acquire);
    if (state == PSP_MEDIA_CODEC_JOB_RUNNING) {
        return psp_media_prepare_staged_audio(
            backend, error, error_size);
    }
    if (state == PSP_MEDIA_CODEC_JOB_DONE) {
        int collected = psp_media_collect_codec_job(
            backend, error, error_size);
        if (collected < 0) return MEDIA_BACKEND_ERROR;
        if (collected > 0) return MEDIA_BACKEND_WOULD_BLOCK;
    }
    backend->codec_job_audio_pts_us = backend->audio_staged_pts_us[
        backend->audio_staged_index];
    return psp_media_queue_codec_job(
        backend, PSP_MEDIA_CODEC_KIND_AUDIO, error, error_size);
}

/*
 * Send whatever is still staged, at the end of a pump.
 *
 * This is what bounds the deferral. A unit held for a partner that never
 * arrives -- the stream ran out, the horizon closed, the window went back on
 * the wire -- would otherwise sit in the staging until the next audio sample,
 * which on a stalled source is never. One visit is the whole exposure, and a
 * visit is what the browser thread makes at the end of every pump anyway.
 *
 * It also collects the other case: a job that stopped on its own time budget
 * with a unit left over. Same state, same answer.
 */
static void psp_media_flush_staged_audio(PspMediaBackend *backend)
{
    if (backend == NULL || backend->admissions_closed) return;
    if (backend->audio_staged_count == 0u) {
        unsigned pending = psp_media_audio_pending_count(backend);
        unsigned queue_depth = (unsigned) (
            atomic_load_explicit(
                &backend->audio_queue_write, memory_order_acquire)
            - atomic_load_explicit(
                &backend->audio_queue_read, memory_order_acquire));
        uint32_t age_us = psp_media_stamp_us()
            - backend->audio_pending_since_us;
        if (psp_media_audio_pending_should_wait(
                pending, queue_depth,
                atomic_load_explicit(
                    &backend->playing, memory_order_acquire),
                age_us)) {
            backend->stats.audio_pending_holds++;
            return;
        }
        if (pending == 1u
            && age_us >= PSP_MEDIA_AUDIO_PENDING_HOLD_US)
            backend->stats.audio_pending_timeouts++;
        psp_media_promote_pending_audio(backend);
    }
    if (backend->audio_staged_index >= backend->audio_staged_count) return;
    char error[128] = {0};
    backend->stats.audio_batch_flushes++;
    (void) psp_media_queue_staged_audio(backend, error, sizeof(error));
}

/* Close the per-picture period and the interval since the presenter let go.
   Called on the accepted path only, immediately before the job is queued, so
   the period shares its origin with codec_job_started_us. */
static void psp_media_note_video_submit(PspMediaBackend *backend)
{
    uint32_t submit_us = psp_media_stamp_us();
    /* The first access unit the decoder accepts after a reset. */
    if (atomic_load_explicit(
            &psp_media_recovery_stage, memory_order_relaxed)
        == PSP_MEDIA_RECOVERY_RESET) {
        atomic_fetch_add_explicit(
            &psp_media_recovery_au_total_us,
            submit_us - atomic_load_explicit(
                &psp_media_recovery_reset_us, memory_order_relaxed),
            memory_order_relaxed);
        atomic_store_explicit(
            &psp_media_recovery_au_us, submit_us, memory_order_relaxed);
        atomic_store_explicit(
            &psp_media_recovery_stage, PSP_MEDIA_RECOVERY_FED,
            memory_order_relaxed);
    }
#if defined(TILEFINCH_PSP_VALIDATION_LOG)
    psp_media_trace_open_au = ++psp_media_trace_next_au;
    psp_media_trace_open_submit_us = submit_us;
    {
        /* Close the claimed picture's record: this is the access unit its
           claim released into the decoder, which is the cycle the whole
           investigation is about. */
        PspMediaPictureTrace *claimed =
            psp_media_trace_slot(psp_media_claim_key.identity);
        if (claimed != NULL && claimed->next_submit_us == 0)
            claimed->next_submit_us = submit_us;
    }
#endif
    if (backend->video_submit_last_us != 0u) {
        uint32_t period_us = submit_us - backend->video_submit_last_us;
        backend->cadence.submit_periods++;
        backend->cadence.submit_period_total_us += period_us;
        if (period_us > backend->cadence.submit_period_max_us)
            backend->cadence.submit_period_max_us = period_us;
    }
    backend->video_submit_last_us = submit_us;
    /*
     * The opportunity a free slot created, closed by the access unit that
     * took it.
     *
     * slot_free_idle_us says how long a free slot went unused; this says what
     * the pipeline was waiting for when it did. A long delay here with a
     * short idle means the bytes had not arrived -- the supply side. A long
     * idle with a short delay means the unit went in promptly and the
     * conversion behind it is what took the time.
     */
    if (backend->slot_free_since_us != 0u) {
        uint32_t delay_us = submit_us - backend->slot_free_since_us;
        backend->slot_free_since_us = 0u;
        backend->stats.slot_free_to_au++;
        backend->stats.slot_free_to_au_total_us += (uint64_t) delay_us;
        if (delay_us > backend->stats.slot_free_to_au_max_us)
            backend->stats.slot_free_to_au_max_us = delay_us;
    }
    uint32_t released_us = atomic_exchange_explicit(
        &psp_media_present_done_us, 0u, memory_order_relaxed);
    if (released_us == 0u) return;
    uint32_t reoffer_us = submit_us - released_us;
    backend->cadence.reoffers++;
    backend->cadence.reoffer_total_us += reoffer_us;
    if (reoffer_us > backend->cadence.reoffer_max_us)
        backend->cadence.reoffer_max_us = reoffer_us;
}

static MediaBackendResult psp_media_submit(
    void *opaque, const MediaMp4Sample *sample,
    const unsigned char *payload, size_t length,
    char *error, size_t error_size)
{
    PspMediaBackend *backend = opaque;
    if (backend == NULL || sample == NULL || payload == NULL
        || length != sample->size || length > INT_MAX) {
        psp_media_error(error, error_size, "PSP media packet invalid");
        return MEDIA_BACKEND_ERROR;
    }
    /* The first step of a reset, and the reason it is first: everything the
       reset does after it -- the epoch bump, the quiesce, the discard -- is
       reasoning about a pipeline nothing is still feeding. */
    if (backend->admissions_closed) return MEDIA_BACKEND_WOULD_BLOCK;
    int collected = psp_media_collect_codec_job(
        backend, error, error_size);
    if (collected > 0) {
        if (sample->kind != MEDIA_MP4_TRACK_AUDIO) {
            if (sample->kind == MEDIA_MP4_TRACK_VIDEO) {
                psp_media_note_video_hold(
                    backend, &backend->cadence.hold_job_slot);
                psp_media_note_job_holder(backend);
            }
            return MEDIA_BACKEND_WOULD_BLOCK;
        }
        /* AAC only enters the browser-owned pending ring below. It never
           aliases audio_staging while the worker owns that buffer. */
    }
    if (collected < 0) return MEDIA_BACKEND_ERROR;
    if (sample->kind == MEDIA_MP4_TRACK_VIDEO) {
        if (!backend->have_video) return MEDIA_BACKEND_ACCEPTED;
        /*
         * Not while something is reading the decoded surface.
         *
         * A borrowed frame is valid until the next submit, and this is why:
         * accepting a video access unit can emit a picture the previous decode
         * left pending, and that emission is a colour conversion straight into
         * the surface -- as can the decode this call queues, from the worker,
         * a millisecond later. The presenter's stage copy is reading those
         * exact bytes while the pump that overlaps it feeds. Hold video for
         * that window and let audio, which owns none of that memory, keep
         * going. The pending access unit stays pending: the pump's next unit
         * re-offers it once the copy has been joined.
         */
        if (!psp_media_advance_may_submit_video(psp_media_advance_mode)) {
            psp_media_note_video_hold(
                backend, &backend->cadence.hold_stage_copy);
            return MEDIA_BACKEND_WOULD_BLOCK;
        }
        /* And the refusal window, which composes with the stage-copy hold
           above rather than replacing it: either alone is enough to hold the
           unit, and the pump re-offers it on its next unit. */
        if (atomic_load_explicit(
                &psp_media_video_refusal_hold, memory_order_acquire)) {
            psp_media_video_refusal_blocked++;
            psp_media_note_video_hold(
                backend, &backend->cadence.hold_refusal);
            return MEDIA_BACKEND_WOULD_BLOCK;
        }
        /* A device run showed that a refused AU can leave firmware healthy
           for dozens of calls OR hang on the very next one. The browser
           thread owns recovery; until it recreates the decoder, no video unit
           may cross this boundary. */
        if (atomic_load_explicit(
                &backend->video_refusal_dirty, memory_order_acquire)) {
            psp_media_note_video_hold(
                backend, &backend->cadence.hold_refusal);
            return MEDIA_BACKEND_WOULD_BLOCK;
        }
        /* Source size first for the refusal/hold structural boundary. The
           compatibility pass below updates this to the staged wire size
           before any worker can observe it. */
        backend->last_packet_bytes = length;
        backend->last_packet_offset = sample->offset;
        backend->last_packet_dts_us =
            psp_media_timestamp_us(sample->dts, sample->timescale);
        backend->last_packet_pts_us = psp_sample_pts_us(sample);
        /*
         * The gate that used to be the rate.
         *
         * It asked "is the one surface holding a picture nobody has claimed",
         * and with one surface the answer was yes for about half of wall time:
         * a conversion had to wait a median 26.4ms for its claim before the
         * next access unit could even be offered, which held a 24 fps stream
         * at 19 pictures a second while the Media Engine sat idle. It now asks
         * whether ANY slot is free, so a conversion targets the free one while
         * the other holds an unclaimed picture. It is deliberately still a
         * gate: when both slots are legitimately occupied -- one claimed, one
         * converted and waiting -- there is nowhere to put a picture and the
         * unit stays where it is.
         */
        if (psp_media_slot_free_count(
                backend->slots, PSP_MEDIA_SURFACE_SLOTS) == 0) {
            psp_media_note_video_hold(
                backend, &backend->cadence.hold_frame_ready);
            return MEDIA_BACKEND_WOULD_BLOCK;
        }
        /*
         * sceMpegAvcDecode can expose more than one buffered picture. Detail2
         * retains parallel info/YUV arrays until the next decode, so drain
         * that bounded batch into every free slot before accepting the next
         * compressed access unit -- the retention is exactly what makes this
         * safe, and accepting a unit now would invalidate the arrays the
         * surplus pictures still live in.
         */
        if (psp_media_batch_pending(backend)) {
            unsigned converted = 0;
            int status = psp_media_emit_batch_into_free_slots(
                backend, backend->session_epoch, &converted,
                error, error_size);
            if (status < 0) {
                backend->stats.last_native_error = status;
                return MEDIA_BACKEND_ERROR;
            }
            backend->cadence.emits_submit += converted;
            backend->cadence.hold_batch_pending++;
            return MEDIA_BACKEND_WOULD_BLOCK;
        }
        if (backend->video_timestamps.count
            >= PSP_MEDIA_VIDEO_TIMESTAMP_SLOTS) {
            backend->cadence.hold_timestamps++;
            return MEDIA_BACKEND_WOULD_BLOCK;
        }
        if (!media_h264_avcc_sample_is_admitted(
                payload, length, backend->nal_length_size,
                backend->decoded_width, backend->decoded_height,
                backend->video.codec_config,
                backend->video.codec_config_length)) {
            psp_media_log_failure(
                backend, "avc-sample-admission", -1);
            psp_media_error(
                error, error_size,
                "AVC SPS/geometry rejected");
            return MEDIA_BACKEND_ERROR;
        }
        if (length > SIZE_MAX - MEDIA_H264_PSP_COMPAT_EXTRA_BYTES
            || !psp_media_ensure_packet_staging(
                backend, length + MEDIA_H264_PSP_COMPAT_EXTRA_BYTES)) {
            psp_media_error(error, error_size,
                            "PSP aligned video packet exceeded reserve");
            return MEDIA_BACKEND_ERROR;
        }
        /*
         * Arm the extent canary on every slot this submission's pictures could
         * land in, which is every FREE one.
         *
         * The check happens at the conversion, on the worker, in whichever
         * slot it chose -- and the arm has to have named that same slot or the
         * proof is worthless. This thread cannot know the choice: the worker
         * picks when firmware returns, and a multi-picture batch picks more
         * than once. Arming all the candidates is what makes the pair
         * indexed-alike without predicting anything. It costs one row per
         * unproven slot and stops entirely once both are proven.
         */
        for (unsigned slot = 0; slot < PSP_MEDIA_SURFACE_SLOTS; slot++) {
            PspMediaSurfaceSlot *state = &backend->slots[slot];
            if (state->extent_validated
                || psp_media_slot_observe(state) != PSP_MEDIA_SLOT_FREE)
                continue;
            uint32_t *extent_canary = backend->surfaces[slot]
                + (size_t) (backend->csc_rows - 1u)
                    * backend->frame_stride;
            psp_media_surface_canary_fill(
                extent_canary, backend->decoded_width);
            sceKernelDcacheWritebackRange(
                extent_canary,
                backend->decoded_width * sizeof(*extent_canary));
            state->canary_armed = true;
        }
        if (backend->video_parameter_sets == NULL
            || backend->video_sps_bytes <= 0
            || backend->video_pps_bytes <= 0) {
            psp_media_error(
                error, error_size, "PSP AVC config invalid");
            return MEDIA_BACKEND_ERROR;
        }
        memcpy(backend->packet_staging, payload, length);
        size_t staged_length = length;
        MediaH264PspCompatResult compat_result =
            media_h264_psp_compat_transform(
                &backend->h264_compat, backend->packet_staging,
                &staged_length, backend->packet_staging_bytes);
        if (compat_result == MEDIA_H264_PSP_COMPAT_ERROR) {
            psp_media_log_failure(
                backend, "avc-recovery-compat", -1);
            psp_media_error(
                error, error_size,
                "PSP AVC recovery-point rewrite rejected syntax");
            return MEDIA_BACKEND_ERROR;
        }
        backend->last_packet_bytes = staged_length;
        /* Exactly what firmware is about to be handed, from the byte range it
           came from -- recorded here rather than on the worker so a unit that
           never returns is already in the buffer when it wedges. */
        psp_media_au_dump_record(sample->offset, backend->packet_staging,
                                 staged_length);
        /*
         * A Baseline stream can return its first picture from the same
         * sceMpegAvcDecode call which accepts the access unit.  Retain that
         * unit's timestamp before entering firmware so Detail2 admission has
         * one timestamp for every immediately returned picture.  The mature
         * PSP MP4 path uses the same ordering.  Preserve the prior queue so a
         * native submission failure cannot leave metadata for an AU the
         * backend did not accept.
         */
        backend->codec_job_timestamps_before = backend->video_timestamps;
        if (!psp_media_timestamp_push(
                &backend->video_timestamps,
                psp_sample_pts_us(sample),
                psp_sample_duration_us(sample))) {
            psp_media_log_failure(
                backend, "video-timestamp-queue", -1);
            psp_media_error(error, error_size,
                            "PSP AVC timestamp queue full");
            return MEDIA_BACKEND_ERROR;
        }
        psp_media_note_video_submit(backend);
        return psp_media_queue_codec_job(
            backend, PSP_MEDIA_CODEC_KIND_VIDEO, error, error_size);
    }
    if (sample->kind == MEDIA_MP4_TRACK_AUDIO) {
        if (!backend->have_audio) return MEDIA_BACKEND_ACCEPTED;
        /*
         * Finish an audio job the worker had already claimed, but do not keep
         * replacing it while a video refusal awaits the browser's recovery
         * decision. This guarantees a bounded idle snapshot for that decision
         * and closes the prepared-job variant of the historical
         * refusal-then-next-AU Media Engine wedge.
         */
        if (atomic_load_explicit(
                &psp_media_video_refusal_hold, memory_order_acquire)
            || atomic_load_explicit(
                   &backend->video_refusal_dirty, memory_order_acquire))
            return MEDIA_BACKEND_WOULD_BLOCK;
        int worker_error = psp_media_audio_worker_health(backend);
        if (worker_error < 0) {
            backend->stats.last_native_error =
                worker_error;
            psp_media_log_failure(
                backend,
                psp_media_audio_worker_stage_name(
                    atomic_load(&backend->audio_worker_stage)),
                worker_error);
            psp_media_error(
                error, error_size, "PCM worker: 0x%08X",
                (unsigned) worker_error);
            return MEDIA_BACKEND_ERROR;
        }
        if ((uint32_t) (backend->audio_queue_write
                        - backend->audio_queue_read)
                >= PSP_MEDIA_AUDIO_QUEUE_SLOTS) {
            return MEDIA_BACKEND_WOULD_BLOCK;
        }
        if (backend->audio_staging == NULL
            || length > PSP_MEDIA_AUDIO_AU_BYTES) {
            psp_media_error(error, error_size,
                            "PSP aligned audio packet exceeded reserve");
            return MEDIA_BACKEND_ERROR;
        }
        MediaBackendResult pending_result = psp_media_enqueue_pending_audio(
            backend, sample, payload, length);
        if (pending_result != MEDIA_BACKEND_ACCEPTED) return pending_result;
        unsigned queue_depth = (unsigned) (backend->audio_queue_write
                                           - backend->audio_queue_read);
        unsigned pending = psp_media_audio_pending_count(backend);
        if (pending < PSP_MEDIA_AUDIO_BATCH_MAXIMUM) {
            backend->stats.audio_batch_deferrals++;
            return MEDIA_BACKEND_ACCEPTED;
        }
        if (queue_depth + PSP_MEDIA_AUDIO_BATCH_MAXIMUM
            > PSP_MEDIA_AUDIO_QUEUE_SLOTS) {
            backend->stats.audio_batch_blocked_queue++;
            return MEDIA_BACKEND_ACCEPTED;
        }
        if (backend->audio_staged_count != 0u)
            return MEDIA_BACKEND_ACCEPTED;
        psp_media_promote_pending_audio(backend);
        MediaBackendResult queued = psp_media_queue_staged_audio(
            backend, error, error_size);
        /* The ring already owns the access unit. A temporarily busy native
           slot defers the batch; it must not ask the demuxer to submit the
           same sample again. */
        return queued == MEDIA_BACKEND_WOULD_BLOCK
            ? MEDIA_BACKEND_ACCEPTED : queued;
    }
    return MEDIA_BACKEND_ACCEPTED;
}

static MediaBackendResult psp_media_drain(
    void *opaque, char *error, size_t error_size)
{
    PspMediaBackend *backend = opaque;
    if (backend == NULL) {
        psp_media_error(error, error_size,
                        "PSP media drain unavailable");
        return MEDIA_BACKEND_ERROR;
    }
    int collected = psp_media_collect_codec_job(
        backend, error, error_size);
    if (collected > 0) {
        psp_media_note_video_hold(
            backend, &backend->cadence.drain_hold_job_slot);
        return MEDIA_BACKEND_WOULD_BLOCK;
    }
    if (collected < 0) return MEDIA_BACKEND_ERROR;
    /* A drain publishes pictures into the same surface a video submission
       does, on this thread or on the worker. It waits for the same window --
       and for the refusal window, for the same reason: it is another way to
       reach the Media Engine's video path. */
    if (!psp_media_advance_may_submit_video(psp_media_advance_mode)) {
        psp_media_note_video_hold(
            backend, &backend->cadence.drain_hold_stage_copy);
        return MEDIA_BACKEND_WOULD_BLOCK;
    }
    if (atomic_load_explicit(
            &psp_media_video_refusal_hold, memory_order_acquire)) {
        psp_media_note_video_hold(
            backend, &backend->cadence.drain_hold_refusal);
        return MEDIA_BACKEND_WOULD_BLOCK;
    }
    unsigned pending_pictures =
        backend->video_picture_index < backend->video_picture_count
          ? backend->video_picture_count - backend->video_picture_index
          : 0;
    PspMediaVideoDrainAction action = psp_media_video_drain_action(
        backend->video_timestamps.count, pending_pictures,
        psp_media_slot_free_count(backend->slots, PSP_MEDIA_SURFACE_SLOTS),
        backend->video_drain_calls, backend->video_drain_surface_waits);
    if (action == PSP_MEDIA_VIDEO_DRAIN_WAIT_FOR_SURFACE) {
        backend->video_drain_surface_waits++;
        psp_media_note_video_hold(
            backend, &backend->cadence.drain_hold_surface);
        return MEDIA_BACKEND_WOULD_BLOCK;
    }
    if (action == PSP_MEDIA_VIDEO_DRAIN_DROP_SURFACE) {
        /*
         * The end of the stream, with every slot still occupied and nobody
         * coming back for them. Dropping a READY picture is a decision the
         * browser thread is entitled to make here and nowhere else; a READING
         * one is still being displayed and is left exactly as it is, because
         * the frontend's pointer into it outlives this call.
         */
        for (unsigned slot = 0; slot < PSP_MEDIA_SURFACE_SLOTS; slot++) {
            if (psp_media_slot_observe(&backend->slots[slot])
                != PSP_MEDIA_SLOT_READY) continue;
            /* Erased, then published free -- the order every hand-back in
               this file uses, so the conversion this invites in cannot have
               its own metadata cleared out from under it. */
            backend->slots[slot].identity = 0;
            psp_media_slot_charge(backend, slot);
            psp_media_slot_publish(
                &backend->slots[slot], PSP_MEDIA_SLOT_FREE);
            backend->stats.dropped_video_frames++;
        }
        psp_media_slot_sample(backend);
        backend->stats.dropped_video_frames +=
            backend->video_timestamps.count;
        backend->video_timestamps.count = 0;
        backend->video_detail = NULL;
        backend->video_picture_count = 0;
        backend->video_picture_index = 0;
        return MEDIA_BACKEND_END;
    }
    if (action == PSP_MEDIA_VIDEO_DRAIN_EMIT_PENDING) {
        unsigned converted = 0;
        int status = psp_media_emit_batch_into_free_slots(
            backend, backend->session_epoch, &converted, error, error_size);
        if (status < 0) {
            backend->stats.last_native_error = status;
            return MEDIA_BACKEND_ERROR;
        }
        backend->cadence.emits_drain += converted;
        return MEDIA_BACKEND_ACCEPTED;
    }
    if (action == PSP_MEDIA_VIDEO_DRAIN_COMPLETE) {
        /*
         * A successful terminal call may still reject corrupt/incomplete
         * queued access units. Retire that bounded metadata rather than let
         * it leak into a later stream.
         */
        backend->stats.dropped_video_frames +=
            backend->video_timestamps.count;
        backend->video_timestamps.count = 0;
        return MEDIA_BACKEND_END;
    }

    return psp_media_queue_codec_job(
        backend, PSP_MEDIA_CODEC_KIND_DRAIN, error, error_size);
}

#if defined(TILEFINCH_PSP_VALIDATION_LOG)
/*
 * Publish the audio worker's output-side counters from the browser thread.
 *
 * The worker may never log, and until now nothing else reported it either:
 * every audio line the device produced was a decode-side
 * event=codec-job-complete kind=2, which says firmware accepted an access
 * unit and the worker was signalled -- not that one sample reached the DAC.
 * The final line names the verdict of the first firmware output call and says
 * whether the queue drained or starved. It is deliberately deferred until
 * teardown: host0 writes during playback have produced hundreds of
 * milliseconds of browser-thread delay in otherwise healthy device runs.
 */
static void psp_media_report_audio_output_final(PspMediaBackend *backend)
{
    if (!backend->have_audio) return;
    int first_status = atomic_load_explicit(
        &backend->audio_output_first_status, memory_order_acquire);
    uint32_t blocks = atomic_load(&backend->audio_output_blocks);
    if (atomic_exchange(&backend->audio_output_reported, true))
        return;
    psp_media_log(
        "tilefinch-media-decoder: event=audio-output final=1 blocks=%u "
        "samples=%llu queued=%u starved=%u first=0x%08X kind=%s "
        "channel=%d rate=%u",
        blocks,
        (unsigned long long) blocks * PSP_MEDIA_AUDIO_SAMPLES,
        atomic_load(&backend->audio_queue_write)
            - atomic_load(&backend->audio_queue_read),
        atomic_load(&backend->audio_output_starves),
        (unsigned) first_status,
        backend->audio_channel == -2 ? "src" : "standard",
        backend->audio_channel, backend->audio.sample_rate);
}
#else
#define psp_media_report_audio_output_final(backend) ((void) 0)
#endif

static bool psp_media_advance(void *opaque, uint64_t clock_us,
                              char *error, size_t error_size)
{
    PspMediaBackend *backend = opaque;
    if (backend == NULL) return false;
    backend->presentation_clock_us = clock_us;
    int collected = psp_media_collect_codec_job(
        backend, error, error_size);
    /*
     * Give the in-flight job a bounded slice to publish rather than leaving it
     * for the next browser frame. sceKernelDelayThread yields to the
     * lower-priority codec worker, so this converts collection latency from a
     * whole main-loop iteration into the firmware call's own cost -- which is
     * what lets a playing session submit more than one access unit per frame.
     * Only while playing: the first-frame and seek pumps run with the backend
     * paused and keep their existing nonblocking behaviour.
     *
     * And only where the thread had nothing else to do. This wait was written
     * before the presenter had dead time to spend, so the frame's own advance
     * paid for it: a device soak measured 962 waits costing 2.65 seconds
     * across 300 playing frames -- 8.8ms of every 46.4ms frame, on the one
     * path where nothing is overlapped. The pumps inside the engine's draw and
     * the stage copy make the same call for free, and they are the ones that
     * now carry the multi-access-unit feeding this bought.
     */
    if (collected > 0 && atomic_load(&backend->playing)
        && psp_media_advance_may_wait(psp_media_advance_mode)) {
        uint32_t waited_us = 0;
        uint32_t wait_limit_us = PSP_MEDIA_CODEC_COLLECT_WAIT_US;
        if (psp_media_advance_wait_limit_us != 0
            && psp_media_advance_wait_limit_us < wait_limit_us)
            wait_limit_us = psp_media_advance_wait_limit_us;
        bool published = psp_media_wait_codec_job(
            backend, wait_limit_us, &waited_us);
        backend->stats.codec_wait_calls++;
        backend->stats.codec_wait_us += waited_us;
        if (published) {
            backend->stats.codec_wait_collected++;
            collected = psp_media_collect_codec_job(
                backend, error, error_size);
        }
    }
    if (collected < 0) return false;
    /* An access unit held for a partner that never came, or one a job's time
       budget left behind. Either way the pump is ending and it goes now. */
    psp_media_flush_staged_audio(backend);
    int worker_error = psp_media_audio_worker_health(backend);
    if (worker_error < 0) {
        backend->stats.last_native_error =
            worker_error;
        psp_media_log_failure(
            backend,
            psp_media_audio_worker_stage_name(
                atomic_load(&backend->audio_worker_stage)),
            worker_error);
        psp_media_error(
            error, error_size, "PCM worker: 0x%08X",
            (unsigned) worker_error);
        return false;
    }
    return true;
}

static bool psp_media_reset(void *opaque, char *error, size_t error_size)
{
    PspMediaBackend *backend = opaque;
    if (backend == NULL || !backend->mpeg_created) {
        psp_media_error(error, error_size,
                        "PSP media decoder cannot reset");
        return false;
    }
    /*
     * A refusal reset is not an ordinary seek-mode experiment. A device run
     * proved the refused AU can leave firmware hung on the very next call.
     * Capture the reason before clearing it below; this reset must recreate
     * the decoder even when ordinary seeks use the reference-shaped NO_TOUCH
     * path.
     */
    bool refusal_rebuild = atomic_load_explicit(
        &backend->video_refusal_dirty, memory_order_acquire);
    /* The browser thread acting on a refusal. Closes the first interval of
       the outage: how long the refusal sat before anything was done about it,
       which is a scheduling cost and not a decoder one. */
    if (atomic_load_explicit(
            &psp_media_recovery_stage, memory_order_relaxed)
        == PSP_MEDIA_RECOVERY_REFUSED) {
        uint32_t now_us = psp_media_stamp_us();
        atomic_fetch_add_explicit(
            &psp_media_recovery_reset_total_us,
            now_us - atomic_load_explicit(
                &psp_media_recovery_refusal_us, memory_order_relaxed),
            memory_order_relaxed);
        atomic_store_explicit(
            &psp_media_recovery_reset_us, now_us, memory_order_relaxed);
        atomic_store_explicit(
            &psp_media_recovery_stage, PSP_MEDIA_RECOVERY_RESET,
            memory_order_relaxed);
    }
    /*
     * A reset is the browser thread processing the refusal, so it lifts the
     * hold itself rather than waiting for the advance to reach its release.
     * Without this a seek arriving in the same frame as a refusal would carry
     * the hold into its own decode leg -- which early-returns from the advance
     * and so never reaches that release -- and hold video against a job whose
     * deadline is five seconds.
     */
    media_psp_backend_release_refusal_hold();
    /*
     * The order below is the whole of this function's correctness while slots
     * can be in different states, and it is not the order that reads most
     * naturally. Step by step:
     *
     *   1 stop admissions, so nothing enters the decoder from here on;
     *   2 move the epoch, so everything already in flight is identifiable as
     *     belonging to the stream being left rather than the one arriving;
     *   3 quiesce the Media Engine writer;
     *   4 join the readers;
     *   5 discard what the old stream published -- READY slots, the captured
     *     batch arrays, the timestamp queues;
     *   6 reposition or rebuild;
     *   7 and only now hand the slots back as FREE.
     *
     * Two and seven are the ones with teeth. The epoch has to move BEFORE the
     * quiesce, because a worker that never comes back must not be able to
     * publish a picture of the old stream as though it were of the new one --
     * the stamp it copied is already stale by then. And the slots must not
     * become FREE until the hardware has provably stopped, because FREE is
     * permission for the Media Engine to write there: handing back a slot a
     * timed-out writer is still inside is the aliasing hazard the whole-pool
     * quarantine exists for, and a partial free would be strictly worse than
     * failing the seek.
    */
    backend->admissions_closed = true;
    atomic_store_explicit(
        &backend->video_refusal_dirty, false, memory_order_release);
    psp_media_cancel_prepared_job(backend);
    backend->session_epoch = psp_media_epoch_advance(
        backend->session_epoch, PSP_MEDIA_EPOCH_WIDTH_MASK);
    /* The demuxer resumes at a random-access sample. A frame-number base
       belongs to the GOP being left and must not cross a seek or recovery. */
    media_h264_psp_compat_reset(&backend->h264_compat);
    uint32_t codec_wait_started = sceKernelGetSystemTimeLow();
    while (atomic_load_explicit(
               &backend->codec_job_state, memory_order_acquire)
           == PSP_MEDIA_CODEC_JOB_RUNNING) {
        if (sceKernelGetSystemTimeLow() - codec_wait_started
            >= PSP_MEDIA_CODEC_QUIESCE_WAIT_US) {
            /*
             * The writer did not stop. Nothing below this line runs, and in
             * particular no slot is freed and the pool is not rewound: the
             * Media Engine may still be writing one of these surfaces, and a
             * bump-pool reset would let the next carve alias it. The seek
             * fails and the session retries; if the worker never returns at
             * all, destroy's own quiesce quarantines the whole pool, which is
             * the only discipline that covers a buffer firmware still owns.
             */
            backend->stats.last_native_error = (int) PSP_MEDIA_ERROR_BUSY;
            psp_media_log_failure(
                backend, "codec-quiesce-reset",
                (int) PSP_MEDIA_ERROR_BUSY);
            psp_media_error(
                error, error_size,
                "PSP codec is still finishing; try seek again");
            backend->admissions_closed = false;
            return false;
        }
        sceKernelDelayThread(1000);
    }
    /* The readers, which the presenter has already joined on every ordinary
       path. Bounded, and a failure here is the same class as the one above:
       something is still reading a slot, so nothing may be freed. */
    uint32_t reader_wait_us = 0;
    if (!psp_media_surface_quiesce_readers(&reader_wait_us)) {
        backend->stats.last_native_error = (int) PSP_MEDIA_ERROR_BUSY;
        psp_media_log_failure(
            backend, "surface-quiesce-reset", (int) PSP_MEDIA_ERROR_BUSY);
        psp_media_error(
            error, error_size,
            "PSP decoded surface is still being read; try seek again");
        backend->admissions_closed = false;
        return false;
    }
    psp_media_log(
        "tilefinch-media-decoder: event=surface-reset-quiesced wait-us=%u "
        "epoch=%llu",
        reader_wait_us, (unsigned long long) backend->session_epoch);
    (void) reader_wait_us;
    uint32_t codec_reset_wait_us =
        sceKernelGetSystemTimeLow() - codec_wait_started;
    psp_media_log(
        "tilefinch-media-decoder: event=codec-reset-quiesced wait-us=%u",
        codec_reset_wait_us);
    (void) codec_reset_wait_us;
    if (psp_media_collect_codec_job(
            backend, error, error_size) < 0) return false;
    atomic_store_explicit(
        &backend->audio_resetting, true, memory_order_release);
    atomic_store(&backend->playing, false);
    uint32_t wait_started = sceKernelGetSystemTimeLow();
    uint32_t reset_wait_us = 0;
    for (;;) {
        bool in_flight = atomic_load_explicit(
            &backend->audio_output_in_flight_slot,
            memory_order_acquire) != 0;
        uint32_t elapsed = sceKernelGetSystemTimeLow() - wait_started;
        reset_wait_us = elapsed;
        PspMediaAudioResetAction action =
            psp_media_audio_reset_action(in_flight, elapsed);
        if (action == PSP_MEDIA_AUDIO_RESET_READY) break;
        if (action == PSP_MEDIA_AUDIO_RESET_TIMEOUT) {
            int wait_error = (int) PSP_MEDIA_ERROR_BUSY;
            backend->stats.last_native_error = wait_error;
            psp_media_log_failure(
                backend, "audio-output-quiesce", wait_error);
            psp_media_error(
                error, error_size,
                "PSP audio output did not quiesce for seek");
            atomic_store_explicit(
                &backend->audio_resetting, false, memory_order_release);
            backend->admissions_closed = false;
            return false;
        }
        sceKernelDelayThread(1000);
    }
    psp_media_log(
        "tilefinch-media-decoder: event=audio-reset-quiesced wait-us=%u",
        reset_wait_us);
    (void) reset_wait_us;
    atomic_fetch_add(&backend->audio_queue_generation, 1u);
    atomic_store(
        &backend->audio_queue_read,
        atomic_load(&backend->audio_queue_write));
    backend->audio_output_origin_blocks =
        atomic_load(&backend->audio_queue_write);
    atomic_store(&backend->audio_first_output_us, 0u);
    backend->audio_cursor_last_us = 0u;
    backend->audio_cursor_elapsed_us = 0u;
    /*
     * Step 5 and step 7 together, and they are safe together only because the
     * writer and the readers have both provably stopped above. Every slot goes
     * back to FREE carrying a bumped generation, so a reader still holding
     * {slot, generation} from before the seek is refused rather than handed a
     * picture of the new stream, and the extent proof is retained: it is a
     * property of the surface and the decoder's geometry, which a reposition
     * does not change.
     */
    for (unsigned slot = 0; slot < PSP_MEDIA_SURFACE_SLOTS; slot++) {
        psp_media_surface_lease_publish(slot, 0, false);
        atomic_store(&psp_media_surface_borrowed[slot], 0);
        atomic_store(&psp_media_surface_writing[slot], 0);
        backend->slots[slot].generation++;
        backend->slots[slot].epoch = PSP_MEDIA_EPOCH_INVALID;
        backend->slots[slot].identity = 0;
        backend->slots[slot].pts_us = 0;
        backend->slots[slot].duration_us = 0;
        backend->slots[slot].emitted_us = 0;
        backend->slots[slot].signature = 0;
        backend->slots[slot].canary_armed = false;
        /* Published last here too. Nothing is running that could take the
           invitation up -- the writer and the readers stopped above -- but a
           hand-back that is ordered differently from every other hand-back is
           the kind of exception a later change reads as permission. */
        psp_media_slot_charge(backend, slot);
        psp_media_slot_publish(&backend->slots[slot], PSP_MEDIA_SLOT_FREE);
    }
    psp_media_slot_sample(backend);
    /* A reset's own stamps belong to the stream being left. The cumulative
       dwell is kept -- readers diff consecutive lines and a reset is a thing
       they should be able to see in the window it happened in -- but nothing
       may carry an opportunity across it: the access unit that closes a
       free-slot interval must be one this session asked for. */
    backend->slot_free_since_us = 0u;
    backend->batch_convert_us = 0u;
    /* Compressed audio staged for a stream this backend has left. The quiesce
       above has already collected or abandoned the job that would have
       decoded it, and a post-seek job must never carry a pre-seek unit. */
    backend->audio_staged_count = 0u;
    backend->audio_staged_index = 0u;
    backend->audio_pending_read = 0u;
    backend->audio_pending_write = 0u;
    backend->audio_pending_since_us = 0u;
    backend->reading_slot = -1;
    atomic_store_explicit(
        &backend->audio_origin_initialized, false, memory_order_release);
    backend->audio_origin_us = 0;
    backend->video_status = 0;
    backend->video_detail = NULL;
    backend->video_picture_count = 0;
    backend->video_picture_index = 0;
    backend->maximum_video_picture_batch = 0;
    backend->presentation_clock_us = 0;
    backend->presented_frame_pts_us = 0;
    backend->video_timestamps.count = 0;
    backend->video_drain_calls = 0;
    backend->video_drain_surface_waits = 0;
    /*
     * Primed means firmware has been given a sequence to decode against, and
     * the bridge reads it as "send mode 0 rather than mode 3". Modes 0 and 1
     * take that sequence away -- a flush or a new object -- so the next unit
     * has to reintroduce it. Mode 2 takes nothing away, so claiming otherwise
     * would send a mid-stream access unit as though it were the first of a
     * file. The reference only ever sends mode 3 for the packet at timestamp
     * zero; every frame after a seek goes as mode 0, which is exactly what
     * leaving this alone produces.
     */
    if (refusal_rebuild
        || psp_media_reset_mode != PSP_MEDIA_RESET_MODE_NO_TOUCH)
        backend->decoder_primed = false;
#if defined(TILEFINCH_PSP_VALIDATION_LOG)
    /* The sequence worth keeping starts here: a reset is what a seek does, and
       the wedge arrives about forty access units later. */
    psp_media_au_dump_armed = true;
#endif
    /*
     * The probe asks one question -- whether this firmware's raw-NAL bridge
     * produces pictures at all -- and a session that has decoded one has
     * answered it. Re-arming across a seek asks it again against mid-stream
     * data, where an access unit that produces no picture is ordinary: the
     * device log shows output arriving in batches of two and three, so the
     * units in between produce none. Past the probe's sixteen-packet bound
     * the first of those is fatal, and a soak died two seconds after a
     * forward seek having decoded 245 of 248 access units -- a working
     * decoder, declared unavailable. Ask only while unanswered.
     */
    if (!psp_media_surface_proven(backend)) {
        backend->raw_nal_probe_pending = true;
        backend->raw_nal_probe_packets = 0;
    }
    const char *native_stage = "sceMpegAvcDecodeFlush";
    int status = 0;
    /*
     * Two ways to give the decoder a clean slate, and which one runs is the
     * experiment. In place is what shipping does and what every wedge so far
     * has followed by four to six seconds. The rebuild hands the object back to
     * firmware and takes a new one, which is the only sequence this project has
     * ever seen produce a decoder that does not wedge -- the one an open runs.
     *
     * The rebuild goes to the codec worker because sceMpegDelete and
     * sceMpegCreate are unbounded Media Engine calls, and this function runs on
     * the browser thread. Everything above has already quiesced that worker, so
     * the job slot is free and the wait is the same bounded one destroy uses.
     * A worker that has died takes the in-place path instead: it cannot make
     * matters worse, and an unbounded firmware call on the browser thread
     * could.
     */
    /* A flush or a rebuild leaves firmware holding nothing, so only the
       no-touch branch below re-opens this window. */
    backend->video_reposition_drain_units = 0u;
    bool no_touch = !refusal_rebuild
        && psp_media_reset_mode == PSP_MEDIA_RESET_MODE_NO_TOUCH;
    bool rebuild_requested = refusal_rebuild
        || psp_media_reset_mode == PSP_MEDIA_RESET_MODE_RECREATE;
    bool rebuild = rebuild_requested
        && psp_media_codec_worker_dispatchable(backend);
    if (rebuild_requested && !rebuild) {
        native_stage = refusal_rebuild
            ? "codec-recreate-refusal-worker"
            : "codec-recreate-worker";
        status = (int) PSP_MEDIA_ERROR_BUSY;
        psp_media_log(
            "tilefinch-media-decoder: event=mpeg-recreate status=0x%08X "
            "stage=worker-unreachable required=-1 held=%d mode=%d "
            "refusal=%d",
            (unsigned) status, backend->mpeg_memory_bytes,
            backend->mpeg_mode, refusal_rebuild ? 1 : 0);
    } else if (no_touch) {
        /*
         * The reference's shape: firmware is told nothing at all. Everything
         * above this point still ran, because none of it speaks to firmware --
         * the in-flight job was collected, the audio output was quiesced, our
         * PCM queue was dropped, and our picture batch and timestamp queue were
         * cleared. What is skipped is only the pair of calls PMPlayer never
         * makes on a reposition, and the audio re-init below.
         *
         * The engine keeps whatever its reorder pipeline was holding, so open
         * the window in which an over-long batch is read as those held
         * pictures arriving rather than as a decoder returning nonsense.
         */
        native_stage = "no-touch";
        backend->video_reposition_drain_units =
            PSP_MEDIA_VIDEO_TIMESTAMP_SLOTS;
        psp_media_log(
            "tilefinch-media-decoder: event=reposition-no-touch "
            "primed=%d drain-units=%u mode=%d",
            backend->decoder_primed ? 1 : 0,
            backend->video_reposition_drain_units, backend->mpeg_mode);
    } else if (rebuild) {
        native_stage = "codec-recreate-queue";
        psp_media_log(
            "tilefinch-media-decoder: event=mpeg-recreate-request "
            "refusal=%d mode=%d",
            refusal_rebuild ? 1 : 0, psp_media_reset_mode);
        uint32_t recreate_wait_us = 0;
        backend->codec_recreate_ran = false;
        if (psp_media_queue_codec_job(
                backend, PSP_MEDIA_CODEC_KIND_RECREATE, NULL, 0)
            != MEDIA_BACKEND_QUEUED) {
            status = (int) PSP_MEDIA_ERROR_BUSY;
        } else if (!psp_media_wait_codec_job(
                       backend, PSP_MEDIA_CODEC_QUIESCE_WAIT_US,
                       &recreate_wait_us)
                   || !backend->codec_recreate_ran) {
            /* The worker is inside sceMpegDelete or sceMpegCreate and has not
               come back. The object's ownership is unknown, so this reset
               fails; destroy's own quiesce is what quarantines the backend
               rather than freeing memory firmware may still be reading. */
            native_stage = "codec-recreate-wait";
            status = (int) PSP_MEDIA_ERROR_BUSY;
        } else {
            atomic_store_explicit(
                &backend->codec_job_state, PSP_MEDIA_CODEC_JOB_IDLE,
                memory_order_release);
            atomic_store_explicit(
                &backend->codec_job_kind, PSP_MEDIA_CODEC_KIND_NONE,
                memory_order_release);
            status = backend->codec_recreate_status;
            native_stage = "codec-recreate";
        }
    } else {
        status = sceMpegAvcDecodeFlush(&backend->mpeg);
        if (status >= 0) {
            memset(backend->video_au, 0xFF, PSP_MEDIA_VIDEO_AU_BYTES);
            sceKernelDcacheWritebackInvalidateRange(
                backend->video_au, PSP_MEDIA_VIDEO_AU_BYTES);
            native_stage = "sceMpegInitAu-reset";
            status = sceMpegInitAu(
                &backend->mpeg, backend->video_es, backend->video_au);
            /* sceMpegInitAu fills the descriptor from the main CPU, so its
               stores are sitting dirty in the data cache. Write them back
               before the lines are dropped: a pure invalidate here is what left
               RAM holding the 0xFF fill and handed the raw-NAL bridge a garbage
               descriptor. */
            if (status >= 0) {
                sceKernelDcacheWritebackInvalidateRange(
                    backend->video_au, PSP_MEDIA_VIDEO_AU_BYTES);
            }
        }
    }
    if (status >= 0 && backend->have_audio) {
        /*
         * The audio program is re-initialised on both paths, and deliberately
         * no further than that. The run that wedged sceAudiocodecDecode is why
         * it is re-initialised at all -- whatever a reset disturbs reaches the
         * AAC side too. What is not re-run is sceAudiocodecCheckNeedMem and the
         * work-buffer grant: control word 3 still points at a live pool block,
         * the pool has no per-block free, and its whole budget for that line is
         * 64 KiB, so a handful of resets that re-granted would strand every
         * earlier buffer and exhaust it. Word 3 and word 4 survive untouched;
         * only the input/output descriptors below are cleared.
         *
         * The PCM zero-fill leaves dirty lines behind on purpose; the decode
         * path writeback-invalidates that range before every firmware call,
         * so they reach RAM rather than being discarded.
         */
        memset(backend->audio_pcm, 0, PSP_MEDIA_AUDIO_PCM_BYTES);
        /*
         * Mode 2 stops here. The reference mutes its output across a
         * reposition rather than stopping or re-initialising its decoder, and
         * everything that actually prevents stale sound has already happened
         * above without firmware being told anything: the output was quiesced,
         * the queue generation was bumped so a block in flight is discarded on
         * arrival, the read cursor was snapped to the write cursor, and the
         * PCM staging was zeroed just now. Re-initialising the AAC program is
         * the audio half of the very sequence this mode exists to remove --
         * and it is a reset that once left sceAudiocodecDecode not returning.
         * Control words 6 to 9 are the input and output descriptors, and the
         * decode path rewrites all four before every call, so leaving them is
         * not leaving anything stale behind.
         */
        if (!no_touch) {
            backend->audio_codec[6] = 0;
            backend->audio_codec[7] = 0;
            backend->audio_codec[8] = 0;
            backend->audio_codec[9] = 0;
            sceKernelDcacheWritebackInvalidateRange(
                backend->audio_codec,
                psp_media_cache_extent(PSP_MEDIA_AUDIO_CODEC_BYTES));
            native_stage = "sceAudiocodecInit-reset";
            status = sceAudiocodecInit(
                backend->audio_codec, PSP_CODEC_AAC);
            /* Main-CPU firmware call writing the control block: write back,
               then drop. See the creation-path sceAudiocodecInit for why. */
            if (status >= 0) {
                sceKernelDcacheWritebackInvalidateRange(
                    backend->audio_codec,
                    psp_media_cache_extent(PSP_MEDIA_AUDIO_CODEC_BYTES));
            }
        }
    }
    if (status < 0) {
        backend->stats.last_native_error = status;
        psp_media_log_failure(
            backend, native_stage, status);
        psp_media_error(
            error, error_size,
            "PSP media decoder reset %s failed: 0x%08X",
            native_stage, (unsigned) status);
        atomic_store_explicit(
            &backend->audio_resetting, false, memory_order_release);
        backend->admissions_closed = false;
        return false;
    }
    atomic_store_explicit(
        &backend->audio_resetting, false, memory_order_release);
    /* Last, and only here: the pipeline is coherent again, so it may be fed.
       A failed reset re-opens it too -- the session retries the seek, and a
       stream that could never be fed again would be a worse answer than one
       that tries. */
    backend->admissions_closed = false;
    return true;
}

static bool psp_media_take_frame(void *opaque, MediaVideoFrame *frame)
{
    PspMediaBackend *backend = opaque;
    if (backend == NULL || frame == NULL) return false;
    /*
     * There is no global job gate here any more, and its removal is the whole
     * point of this function's current shape.
     *
     * It used to refuse every claim for the length of ANY running video or
     * drain job -- a rule inherited from the single-surface pipeline, where a
     * running conversion and the one surface were the same statement. With two
     * independently owned slots they are not: a job converting slot 1 says
     * nothing whatsoever about a picture sitting READY in slot 0. The device
     * measured what that cost, 342 to 405 refusals of a due picture per soak,
     * about 13% of all take attempts, and take_blocked_other_slot said in every
     * one of those samples that the job holding the gate shut was writing the
     * other slot.
     *
     * What replaces it is per-slot, and it is the slot's own state word: the
     * worker publishes READY with a release store after the whole picture is
     * written, and psp_media_slot_take_index observes it with an acquire load
     * before any field is read. A slot that reads READY is one the worker has
     * finished with and will not touch again until this thread publishes it
     * FREE, so its metadata is not merely visible but immutable for as long as
     * the claim lasts. That is the ordering the codec-job handoff used to
     * supply for both slots at once, at the granularity it should always have
     * had.
     */
    backend->cadence.take_calls++;
    /*
     * Earliest presentation time first, across every slot holding a picture of
     * this epoch.
     *
     * With one surface there was nothing to choose and the order was whatever
     * the decoder produced. With two there is, and the choice is not free:
     * firmware returns reordered batches, so the slot converted most recently
     * is routinely NOT the picture due next. Taking the earliest is what keeps
     * a two-slot pipeline presenting a stream in stream order.
     */
    int chosen = psp_media_slot_take_index(
        backend->slots, PSP_MEDIA_SURFACE_SLOTS, backend->session_epoch);
    /* Audio is the presentation clock when it exists. If startup decoding
       falls behind it, retire at most one already-converted late head when a
       due successor is waiting. This avoids permanently slowing the clock (and
       leaving sound ahead) while preserving PTS order and never dropping the
       only picture available. READY is not enough by itself: the writer lease
       must also be down before FREE can be published. */
    if (chosen >= 0
        && atomic_load(&psp_media_surface_writing[chosen]) == 0) {
        int successor = -1;
        for (unsigned at = 0; at < PSP_MEDIA_SURFACE_SLOTS; at++) {
            if ((int) at == chosen
                || !psp_media_slot_holds_picture(
                    &backend->slots[at], backend->session_epoch)) continue;
            if (successor < 0
                || psp_media_slot_precedes(
                    &backend->slots[at], &backend->slots[successor]))
                successor = (int) at;
        }
        size_t displayed_frames = (size_t) atomic_load_explicit(
            &psp_media_claims_displayed, memory_order_acquire);
        if (backend->video_startup_catchup
            && psp_media_startup_catchup_settled(
                displayed_frames)) {
            backend->video_startup_catchup = false;
#if defined(TILEFINCH_PSP_VALIDATION_LOG)
            psp_media_log(
                "tilefinch-media-video: event=startup-catchup-settled "
                "clock=%lluus video=%lluus displayed=%zu",
                (unsigned long long) backend->presentation_clock_us,
                (unsigned long long) backend->slots[chosen].pts_us,
                displayed_frames);
#endif
        }
        if (successor >= 0
            && psp_media_video_should_drop_late(
                backend->presentation_clock_us,
                &backend->slots[chosen], &backend->slots[successor],
                backend->video_startup_catchup)) {
#if defined(TILEFINCH_PSP_VALIDATION_LOG)
            psp_media_log(
                "tilefinch-media-video: event=late-drop clock=%lluus "
                "current=%lluus successor=%lluus current-slot=%d "
                "successor-slot=%d emitted-age=%uus",
                (unsigned long long) backend->presentation_clock_us,
                (unsigned long long) backend->slots[chosen].pts_us,
                (unsigned long long) backend->slots[successor].pts_us,
                chosen, successor,
                backend->slots[chosen].emitted_us == 0u
                    ? 0u
                    : psp_media_stamp_us()
                        - backend->slots[chosen].emitted_us);
#endif
            backend->slots[chosen].identity = 0;
            psp_media_slot_charge(backend, (unsigned) chosen);
            psp_media_slot_publish(
                &backend->slots[chosen], PSP_MEDIA_SLOT_FREE);
            backend->stats.dropped_video_frames++;
            psp_media_slot_sample(backend);
            chosen = successor;
        }
    }
    /*
     * The one refusal left, and it is about this slot rather than about the
     * pipeline: the conversion that filled it has published READY but has not
     * yet dropped its write lease.
     *
     * Nothing more will be written -- the colour conversion completed and was
     * validated before READY was published -- so this is not a correctness
     * requirement on the pixels. It is the standing invariant that a slot is
     * never claimed and writable at the same instant, kept true by
     * construction at the one place a claim is granted. The Media Engine, the
     * DMA controller and the graphics engine genuinely overlap on this device;
     * "the write has surely finished by now" is exactly the reasoning that is
     * not available here.
     *
     * The window is the handful of instructions between the publish and
     * psp_media_surface_end_write, so this is expected to read zero. It is
     * what take_busy now counts: genuine per-slot contention, where the
     * counter previously carried the global gate's refusals as well.
     */
    bool slot_busy = chosen >= 0
        && atomic_load(&psp_media_surface_writing[chosen]) != 0;
    if (chosen < 0 || slot_busy) {
        /*
         * Three different failures wearing one shape. "empty" is a pipeline
         * that has never converted a picture; "stale" is every slot already
         * claimed, with nothing newer converted behind them -- the due-but-
         * empty case, and the one a pipeline running below the stream's frame
         * rate spends its missed takes in; "busy" is the lease above.
         */
        if (slot_busy) backend->cadence.take_busy++;
        else if (backend->reading_slot >= 0
                 || backend->frame_identity != 0)
            backend->cadence.take_stale++;
        else backend->cadence.take_empty++;
        psp_media_trace_take(
            backend->reading_slot >= 0
                ? backend->slots[backend->reading_slot].identity : 0,
            slot_busy ? PSP_MEDIA_TAKE_BUSY
                : (backend->reading_slot >= 0 || backend->frame_identity != 0)
                    ? PSP_MEDIA_TAKE_STALE : PSP_MEDIA_TAKE_EMPTY);
        return false;
    }
    PspMediaSurfaceSlot *picture = &backend->slots[chosen];
    /*
     * Not yet due. Wait for it -- never reach past it for the other slot: a
     * picture whose time has not come is still the next picture, and claiming
     * a later one because it happens to be ready is how a pipeline silently
     * reorders a stream. Displaying late is recoverable; displaying out of
     * order is not.
     */
    if (atomic_load(&backend->playing)
        && picture->duration_us != 0
        && picture->pts_us > backend->presentation_clock_us
        && picture->pts_us - backend->presentation_clock_us
           > picture->duration_us / 2u) {
        backend->cadence.take_early++;
        psp_media_trace_take(picture->identity, PSP_MEDIA_TAKE_EARLY);
        return false;
    }
    /*
     * This is where the H.264 crop rectangle is applied, and applying it costs
     * nothing: the CSC filled macroblock-padded rows (432 pixels wide for a
     * 426-wide stream) at the surface stride, and publishing the DISPLAY size
     * with that stride tells the presenter to read the first 426 pixels of
     * each row and the first 240 rows. The invented padding is never sampled.
     * Do not "fix" these to the firmware-reported coded size.
     */
    *frame = (MediaVideoFrame) {
        .pixels = backend->surfaces[chosen],
        .width = backend->decoded_width,
        .height = backend->decoded_height,
        .stride_pixels = (int) backend->frame_stride,
        .format = MEDIA_PIXEL_RGBA8888,
        .pts_us = picture->pts_us,
        .duration_us = picture->duration_us,
        .identity = picture->identity,
        .slot = chosen,
        .generation = picture->generation,
        .epoch = picture->epoch
    };
    /*
     * The claim, and the release of the one it supersedes.
     *
     * This is the only place a slot becomes writable again, and it is the
     * whole safety argument: nothing outside the backend can still be pointing
     * at the previous picture, because the frontend's pointer is the frame
     * struct being overwritten right now. Freeing at the reader's release
     * instead would leave the loop's redraw of a still-current picture
     * sampling a slot the Media Engine had been handed back.
     *
     * The lease is dropped with it. The browser thread is the only borrower
     * and it is the thread running this, so a lease still standing here is one
     * this same thread failed to release -- clearing it makes a leaked release
     * cost nothing rather than stalling the writer forever.
     */
    /*
     * ...unless an abandoned DMA is still inside it. A quarantined slot is
     * kept exactly as it is: still READING, still holding its picture, and
     * still refused to the Media Engine. It costs the pipeline one of its two
     * slots until the presenter observes the transfer end, which is the whole
     * price of not converting over a live reader.
     */
    if (backend->reading_slot >= 0
        && backend->reading_slot != chosen
        && atomic_load(
               &psp_media_surface_quarantined[backend->reading_slot]) == 0) {
        unsigned previous = (unsigned) backend->reading_slot;
        psp_media_surface_lease_publish(previous, 0, false);
        atomic_store(&psp_media_surface_borrowed[previous], 0);
        /*
         * Erased first, published free second, and with the gate gone that
         * order is load-bearing rather than tidy: the worker may be inside
         * psp_media_slot_free_index at this instant, and FREE is the word it
         * is waiting for. A clear that landed after the publish would be a
         * store into a slot the Media Engine had already been given.
         */
        backend->slots[previous].identity = 0;
        psp_media_slot_charge(backend, previous);
        psp_media_slot_publish(
            &backend->slots[previous], PSP_MEDIA_SLOT_FREE);
    }
    psp_media_slot_charge(backend, (unsigned) chosen);
    /* And the claim itself, published so the worker's next free-slot scan
       cannot pick the picture this thread is about to hand to the frontend. */
    psp_media_slot_publish(picture, PSP_MEDIA_SLOT_READING);
    backend->reading_slot = chosen;
    /* One sample for the pair: they are adjacent, and it is the state after
       both that describes the pipeline. */
    psp_media_slot_sample(backend);
    psp_media_surface_lease_publish(
        (unsigned) chosen, picture->generation, true);
    backend->stats.video_slot_claims[chosen]++;
    backend->presented_frame_pts_us = picture->pts_us;
    /* Segment three closes here, and segment four is armed for the presenter's
       borrow release to close. The age is how long this picture sat converted
       in the surface before the loop came back for it -- a whole browser frame
       of it means the conversion finished just after the take that wanted it,
       which is the missed-take shape. */
    backend->cadence.take_satisfied++;
    uint32_t taken_us = psp_media_stamp_us();
    /*
     * A claim, and the verdict on the one before it. An identity that was
     * claimed and then replaced in the surface before the presenter drew it
     * never reached the screen, and counting it as a presented picture is
     * what let every rate read off take-ok be an upper bound.
     */
    {
        PspMediaFrameKey previous = psp_media_claim_key;
        psp_media_claim_key = psp_media_frame_key_of(frame);
        if (psp_media_frame_key_armed(&previous)
            && !psp_media_frame_key_equal(
                   &previous, &psp_media_display_key)) {
            atomic_fetch_add_explicit(
                &psp_media_claims_dropped, 1u, memory_order_relaxed);
        }
    }
    psp_media_trace_take(picture->identity, PSP_MEDIA_TAKE_OK);
    if (picture->emitted_us != 0u) {
        uint32_t age_us = taken_us - picture->emitted_us;
        backend->cadence.take_age_total_us += age_us;
        if (age_us > backend->cadence.take_age_max_us)
            backend->cadence.take_age_max_us = age_us;
    }
    atomic_store_explicit(
        &psp_media_frame_taken_us, taken_us, memory_order_relaxed);
    return true;
}

static size_t psp_media_discard_video_before(
    void *opaque, uint64_t floor_us)
{
    PspMediaBackend *backend = opaque;
    if (backend == NULL || floor_us == 0) return 0;
    size_t dropped = 0;
    /* At most one pass over the fixed two-slot pool. More prerequisite
       pictures arrive through later bounded advances; no call can turn a long
       GOP into an unbounded browser-thread unit. */
    for (unsigned pass = 0; pass < PSP_MEDIA_SURFACE_SLOTS; pass++) {
        int chosen = psp_media_slot_take_index(
            backend->slots, PSP_MEDIA_SURFACE_SLOTS,
            backend->session_epoch);
        if (chosen < 0) break;
        PspMediaSurfaceSlot *picture = &backend->slots[chosen];
        if (picture->pts_us >= floor_us
            || atomic_load(&psp_media_surface_writing[chosen]) != 0)
            break;
        /* The slot is READY, never borrowed, and has not been claimed. Freeing
           it here cannot race a frontend reader and deliberately does not arm
           the claim/display funnel. */
        picture->identity = 0;
        psp_media_slot_charge(backend, (unsigned) chosen);
        psp_media_slot_publish(picture, PSP_MEDIA_SLOT_FREE);
        backend->stats.discarded_seek_video_frames++;
        dropped++;
        psp_media_slot_sample(backend);
    }
    return dropped;
}

/*
 * The staged path is finished with its source. Hand the slot back.
 *
 * This is the release that actually moves the rate, and getting its scope
 * right is the whole of the change. A claim used to hold its slot until a
 * later claim superseded it, on every path -- which made the pipeline exactly
 * one conversion per claim and left the submit gate shut for as long as it had
 * been with a single surface. The device said so precisely: hold-video
 * frame:15683 unchanged at about 131/s, conversions at 18.3/s, the second slot
 * reserved and paid for and doing nothing.
 *
 * The supersede rule was protecting re-presents, and on the staged path there
 * is nothing there to protect. The frontend redraws a picture it has already
 * staged from the staged copy -- psp_media_present_texture_for skips the whole
 * staging branch when the identity has not changed, and the texture it hands
 * back points at the staging, not at the slot. The slot is read exactly once,
 * by the copy itself, inside a borrow lease that is held until the transfer is
 * joined. After that join nothing will ever read it again.
 *
 * The unstaged draw and the software scaler are the opposite case: they sample
 * the slot for every draw, so their claim genuinely has to outlive the frame,
 * and they do not call this.
 *
 * Refuses unless the slot is still READING at the generation the caller read.
 * A late call naming a picture the slot has moved past is a reader that lost
 * its race and must not free somebody else's claim.
 *
 * The lease check is about OTHER readers: the staged path drops its own lease
 * immediately before calling this, so a lease still standing here belongs to
 * something else that is mid-read. It is deliberately not the guard against a
 * DMA transfer that outlived its join -- that transfer no longer holds a lease
 * either, so the caller makes that decision, and psp_media_present_texture_finish
 * keeps the claim rather than calling this at all when its join failed.
 */
static bool psp_media_release_video_slot(
    void *opaque, unsigned slot, uint32_t generation)
{
    PspMediaBackend *backend = opaque;
    if (backend == NULL || slot >= PSP_MEDIA_SURFACE_SLOTS) return false;
    PspMediaSurfaceSlot *state = &backend->slots[slot];
    if (psp_media_slot_observe(state) != PSP_MEDIA_SLOT_READING
        || state->generation != generation) return false;
    if (atomic_load(&psp_media_surface_borrowed[slot]) != 0) return false;
    /* And never while an abandoned transfer is still inside it. This is the
       path the presenter uses to hand the slot back once the quarantine has
       lifted, so it has to refuse for as long as it stands -- a release that
       ran early would free the slot to the Media Engine under a live DMA,
       which is the substitution the quarantine exists to make unbuildable. */
    if (atomic_load(&psp_media_surface_quarantined[slot]) != 0) return false;
    psp_media_surface_lease_publish(slot, 0, false);
    /* Erased, then published free. This release has always run without any
       codec-job gate above it -- it is called from the presenter, mid-frame,
       whatever the worker is doing -- so it was already the site where the
       clear could land behind the invitation. The order is the fix, and it is
       now the same order at all five hand-backs. */
    state->identity = 0;
    psp_media_slot_charge(backend, slot);
    psp_media_slot_publish(state, PSP_MEDIA_SLOT_FREE);
    psp_media_slot_sample(backend);
    if (backend->reading_slot == (int) slot) backend->reading_slot = -1;
    return true;
}

/*
 * Publish every surplus picture there is now a slot free to hold.
 *
 * A decode returns up to four pictures, so a batch can outrun the slots that
 * exist to receive it and the remainder stays captured until some visit is
 * allowed to convert it. Every such visit is a submit or a drain, and both are
 * held off while a read window is open -- so a surplus picture used to wait on
 * a window its own predecessor was holding shut, and the wait landed on the
 * wrong side of the frame's take. A device run measured the cost exactly: 5281
 * visits turned away with a picture pending, and 25.1ms of mean emit-to-take
 * against a 51.9ms per-picture period.
 *
 * With two slots the worker already converts as far as the free slots go
 * before it publishes, so this fires far less often -- but it is still the
 * right trigger for what is left: the moment a claim frees the slot the next
 * surplus picture was waiting for, the browser thread is already inside the
 * backend. Convert here.
 *
 * This is the same synchronous conversion psp_media_submit already performs for
 * a pending batch on this same thread -- 846 of them in that run -- so it is
 * the trigger that moves, not the work and not the thread. Nothing is queued to
 * the codec worker: a job kind for this would serialize the conversion behind
 * whatever the worker is doing and reintroduce the wait it exists to remove.
 *
 * On the stage-copy window, explicitly: this does NOT consult
 * psp_media_advance_mode, and must not. That hold fences SUBMISSIONS, because a
 * submission queues a worker decode whose conversion can land in the middle of
 * a copy the browser thread has already started. This conversion is not a
 * submission -- it runs to completion on the browser thread, and the borrow
 * protocol orders it against every reader: psp_media_surface_begin_write claims
 * the writer flag for one slot and refuses while any reader holds that same
 * slot's psp_media_surface_borrowed. The DMA copy holds that claim until it is
 * joined, and the release this runs behind happens after the join, so an emit
 * into a surface the copy is still reading cannot be constructed. A caller
 * that releases while some other reader is still live is refused by
 * begin_write and loses nothing: the picture stays captured for the visit that
 * would have taken it anyway.
 */
static bool psp_media_emit_pending_video(
    void *opaque, char *error, size_t error_size)
{
    PspMediaBackend *backend = opaque;
    if (backend == NULL || !psp_media_batch_pending(backend)) return false;
    /* The worker owns the surface and the firmware picture arrays for the
       whole of a job. Read-only observation on purpose -- collecting the job
       here would surface its errors at a point no caller is prepared to
       handle them, and the pump collects it a moment later regardless. */
    if (atomic_load_explicit(
            &backend->codec_job_state, memory_order_acquire)
        != PSP_MEDIA_CODEC_JOB_IDLE) return false;
    /* A refusal nobody has looked at yet holds the whole video path, and this
       is part of it. */
    if (atomic_load_explicit(
            &psp_media_video_refusal_hold, memory_order_acquire)) return false;
    /* Never over the top of a picture somebody still owns. With two slots that
       is no longer a question about "the surface" but about which of them are
       free, and it is the same question the submit path asks -- both of them
       answered by the slot states rather than by a pair of flags that could
       only describe one picture. */
    unsigned converted = 0;
    /* No codec_native_stage here, exactly like the pending-batch conversion
       psp_media_submit performs: that field names the stage a WORKER job is
       inside for the job watchdog, and nothing watches the browser thread.
       The epoch is the session's, because this thread IS the session. */
    int status = psp_media_emit_batch_into_free_slots(
        backend, backend->session_epoch, &converted, error, error_size);
    if (status < 0) {
        /* Reported through the next submit or drain, which owns the failure
           ladder. Recording it here keeps the cause rather than the symptom. */
        backend->stats.last_native_error = status;
        return false;
    }
    if (converted == 0) return false;
    backend->cadence.emits_release += converted;
    return true;
}

static bool psp_media_stats(const void *opaque, MediaBackendStats *stats)
{
    const PspMediaBackend *backend = opaque;
    if (backend == NULL || stats == NULL
        || atomic_load_explicit(
               &backend->codec_job_state, memory_order_acquire)
           == PSP_MEDIA_CODEC_JOB_RUNNING) return false;
    *stats = backend->stats;
    /* The surface handshake belongs to the session rather than to a decoder
       instance, so it is published here rather than accumulated in stats. */
    stats->surface_borrow_waits = atomic_load_explicit(
        &psp_media_surface_borrow_waits, memory_order_relaxed);
    stats->surface_borrow_wait_us = atomic_load_explicit(
        &psp_media_surface_borrow_wait_us, memory_order_relaxed);
    stats->surface_borrow_timeouts = atomic_load_explicit(
        &psp_media_surface_borrow_timeouts, memory_order_relaxed);
    stats->surface_emit_deferrals = atomic_load_explicit(
        &psp_media_surface_emit_deferrals, memory_order_relaxed);
    stats->surface_quarantines = atomic_load_explicit(
        &psp_media_surface_quarantines, memory_order_relaxed);
    stats->audio_batch_blocked_video = atomic_load_explicit(
        &psp_media_audio_batch_video_declines, memory_order_relaxed);
    stats->refused_video_packets = backend->video_refusals_total;
    stats->overrun_video_batches = backend->video_batch_overruns_total;
    stats->presented_video_us = backend->presented_frame_pts_us;
    stats->audio_origin_us = backend->audio_origin_us;
    /*
     * The SRC channel has no remaining-sample query. Its blocking producer
     * can have at most one accepted block in hardware, so this counter lets
     * device validation bound the real audio cursor to one 1024-sample
     * interval instead of pretending the UI wall clock is an audio cursor.
     */
    stats->audio_output_blocks =
        atomic_load(&backend->audio_queue_read)
        - backend->audio_output_origin_blocks;
    stats->audio_sample_rate = backend->have_audio
        ? backend->audio.sample_rate : 0;
    /*
     * The browser thread's half of the cadence, and the presented segment,
     * which is measured where no decoder is in scope. Assembled here for the
     * same reason the surface handshake is: this call already refuses while a
     * job is running, so it is the one point at which both halves are still.
     */
    stats->video_take_calls = backend->cadence.take_calls;
    stats->video_take_satisfied = backend->cadence.take_satisfied;
    stats->video_take_empty = backend->cadence.take_empty;
    stats->video_take_stale = backend->cadence.take_stale;
    stats->video_take_busy = backend->cadence.take_busy;
    stats->video_take_early = backend->cadence.take_early;
    stats->video_take_age_total_us = backend->cadence.take_age_total_us;
    stats->video_take_age_max_us = backend->cadence.take_age_max_us;
    stats->video_emits_submit = backend->cadence.emits_submit;
    stats->video_emits_drain = backend->cadence.emits_drain;
    stats->video_emits_release = backend->cadence.emits_release;
    stats->video_claims = backend->cadence.take_satisfied;
    stats->video_claims_staged = atomic_load_explicit(
        &psp_media_claims_staged, memory_order_relaxed);
    stats->video_claims_displayed = atomic_load_explicit(
        &psp_media_claims_displayed, memory_order_relaxed);
    stats->video_claims_dropped = atomic_load_explicit(
        &psp_media_claims_dropped, memory_order_relaxed);
    stats->video_claims_quiesced = atomic_load_explicit(
        &psp_media_claims_quiesced, memory_order_relaxed);
    stats->video_recoveries = atomic_load_explicit(
        &psp_media_recoveries, memory_order_relaxed);
    stats->recovery_reset_total_us = atomic_load_explicit(
        &psp_media_recovery_reset_total_us, memory_order_relaxed);
    stats->recovery_first_au_total_us = atomic_load_explicit(
        &psp_media_recovery_au_total_us, memory_order_relaxed);
    stats->recovery_first_csc_total_us = atomic_load_explicit(
        &psp_media_recovery_csc_total_us, memory_order_relaxed);
    stats->recovery_first_present_total_us = atomic_load_explicit(
        &psp_media_recovery_present_total_us, memory_order_relaxed);
    stats->recovery_first_present_max_us = atomic_load_explicit(
        &psp_media_recovery_present_max_us, memory_order_relaxed);
    stats->recovery_media_skip_total_us =
        (uint64_t) atomic_load_explicit(
            &psp_media_recovery_skip_total_ms, memory_order_relaxed)
        * UINT64_C(1000);
    stats->video_emit_visit_starved = backend->cadence.emit_visit_starved;
    stats->video_reoffers = backend->cadence.reoffers;
    stats->video_reoffer_total_us = backend->cadence.reoffer_total_us;
    stats->video_reoffer_max_us = backend->cadence.reoffer_max_us;
    stats->video_submit_periods = backend->cadence.submit_periods;
    stats->video_submit_period_total_us =
        backend->cadence.submit_period_total_us;
    stats->video_submit_period_max_us =
        backend->cadence.submit_period_max_us;
    stats->video_hold_job_slot = backend->cadence.hold_job_slot;
    stats->video_hold_stage_copy = backend->cadence.hold_stage_copy;
    stats->video_hold_refusal = backend->cadence.hold_refusal;
    stats->video_hold_frame_ready = backend->cadence.hold_frame_ready;
    stats->video_hold_batch_pending = backend->cadence.hold_batch_pending;
    stats->video_hold_timestamps = backend->cadence.hold_timestamps;
    stats->drain_hold_job_slot = backend->cadence.drain_hold_job_slot;
    stats->drain_hold_stage_copy = backend->cadence.drain_hold_stage_copy;
    stats->drain_hold_refusal = backend->cadence.drain_hold_refusal;
    stats->drain_hold_surface = backend->cadence.drain_hold_surface;
    stats->video_presents = atomic_load_explicit(
        &psp_media_presents, memory_order_relaxed);
    stats->video_present_total_us = atomic_load_explicit(
        &psp_media_present_total_us, memory_order_relaxed);
    stats->video_present_max_us = atomic_load_explicit(
        &psp_media_present_max_us, memory_order_relaxed);
    /* The ownership machine's own accounting. The per-slot claim counts are
       accumulated in backend->stats and arrive with the copy above; the three
       process-wide integrity counters live beside the handshake and are
       published here for the same reason it is. */
    stats->surface_borrow_stale = atomic_load_explicit(
        &psp_media_surface_borrow_stale, memory_order_relaxed);
    stats->stage_mismatches = atomic_load_explicit(
        &psp_media_stage_mismatches, memory_order_relaxed);
    stats->signature_mismatches = atomic_load_explicit(
        &psp_media_signature_mismatches, memory_order_relaxed);
    /*
     * The occupancy ladder, with the interval still running folded in.
     *
     * Dwell and the two pipeline conditions are charged at transitions, so
     * whatever is happening right now has not been charged to anything yet --
     * and at a 5-second cadence against transitions tens of milliseconds
     * apart that is a rounding error, except in exactly the case the fields
     * exist to catch. A pipeline wedged with a slot FREE and nothing
     * converting makes no transitions at all, so an unfolded free-idle total
     * would read as zero for the whole of the outage it is meant to name.
     *
     * Added to the caller's copy and not to the backend, which is what keeps
     * this a const observation: the residual is re-derived at every
     * publication rather than accumulated twice.
     */
    uint32_t now_us = psp_media_stamp_us();
    for (unsigned slot = 0; slot < PSP_MEDIA_SURFACE_SLOTS; slot++) {
        for (unsigned at = 0; at < PSP_MEDIA_SLOT_STATES; at++) {
            stats->video_slot_dwell_us[slot][at] =
                backend->slot_dwell[slot].state_us[at];
        }
        int state = psp_media_slot_peek(&backend->slots[slot]);
        if (state >= 0 && state < (int) PSP_MEDIA_SLOT_STATES
            && backend->slot_dwell[slot].since_us != 0u) {
            stats->video_slot_dwell_us[slot][state] +=
                (uint64_t) (now_us - backend->slot_dwell[slot].since_us);
        }
    }
    if (backend->slot_sample_us != 0u) {
        uint32_t open_us = now_us - backend->slot_sample_us;
        if (backend->slot_no_free)
            stats->slot_no_free_us += (uint64_t) open_us;
        if (backend->slot_free_idle)
            stats->slot_free_idle_us += (uint64_t) open_us;
    }
    return true;
}

static bool psp_media_audio_cursor_us(const void *opaque, uint64_t *cursor_us)
{
    PspMediaBackend *backend = (PspMediaBackend *) opaque;
    if (backend == NULL || cursor_us == NULL || !backend->have_audio
        || backend->audio.sample_rate == 0
        || !atomic_load_explicit(
               &backend->audio_origin_initialized, memory_order_acquire)) {
        return false;
    }
    uint32_t blocks = atomic_load_explicit(
        &backend->audio_queue_read, memory_order_acquire)
        - backend->audio_output_origin_blocks;
    if (blocks == 0) return false;
    uint32_t first_output_us = atomic_load_explicit(
        &backend->audio_first_output_us, memory_order_acquire);
    if (first_output_us == 0u) return false;
    uint32_t now_us = psp_media_stamp_us();
    uint32_t previous_us = backend->audio_cursor_last_us == 0u
        ? first_output_us : backend->audio_cursor_last_us;
    uint32_t delta_us = now_us - previous_us;
    backend->audio_cursor_last_us = now_us;
    /* Exact quotient/remainder form keeps the ordinary device path on the
       Allegrex 32-bit divider instead of paying __udivdi3 every UI frame. */
    uint32_t frame_numerator =
        PSP_MEDIA_AUDIO_SAMPLES * UINT32_C(1000000);
    uint32_t whole_us = frame_numerator / backend->audio.sample_rate;
    uint32_t remainder = frame_numerator % backend->audio.sample_rate;
    uint64_t accepted_us = (uint64_t) blocks * whole_us;
    uint64_t remainder_us = remainder == 0 ? 0
        : blocks <= UINT32_MAX / remainder
        ? (uint32_t) (blocks * remainder)
            / backend->audio.sample_rate
        : (uint64_t) blocks * remainder
            / backend->audio.sample_rate;
    accepted_us = remainder_us > UINT64_MAX - accepted_us
        ? UINT64_MAX : accepted_us + remainder_us;
    backend->audio_cursor_elapsed_us =
        psp_media_audio_cursor_advance_us(
            backend->audio_cursor_elapsed_us, delta_us,
            atomic_load_explicit(&backend->playing, memory_order_acquire)
                && !atomic_load_explicit(
                    &backend->buffering, memory_order_acquire),
            accepted_us);
    uint64_t elapsed_us = backend->audio_cursor_elapsed_us;
    *cursor_us = elapsed_us > UINT64_MAX - backend->audio_origin_us
        ? UINT64_MAX : backend->audio_origin_us + elapsed_us;
    return true;
}

static unsigned psp_media_ready_video_frames(const void *opaque)
{
    const PspMediaBackend *backend = opaque;
    return backend == NULL ? 0 : psp_media_slot_ready_count(
        backend->slots, PSP_MEDIA_SURFACE_SLOTS);
}

static bool psp_media_ready_video_start_us(
    const void *opaque, uint64_t *start_us)
{
    const PspMediaBackend *backend = opaque;
    if (backend == NULL || start_us == NULL) return false;
    int chosen = psp_media_slot_take_index(
        backend->slots, PSP_MEDIA_SURFACE_SLOTS,
        backend->session_epoch);
    if (chosen < 0) return false;
    *start_us = backend->slots[chosen].pts_us;
    return true;
}

static size_t psp_media_displayed_video_frames(const void *opaque)
{
    return opaque == NULL ? 0 : (size_t) atomic_load_explicit(
        &psp_media_claims_displayed, memory_order_acquire);
}

static void psp_media_set_presentation_clock_us(
    void *opaque, uint64_t clock_us)
{
    PspMediaBackend *backend = opaque;
    if (backend != NULL) backend->presentation_clock_us = clock_us;
}

static void psp_media_set_playing(void *opaque, bool playing)
{
    PspMediaBackend *backend = opaque;
    if (backend != NULL) {
        uint64_t ignored = 0;
        (void) psp_media_audio_cursor_us(backend, &ignored);
        atomic_store(&backend->playing, playing);
        if (backend->audio_event >= 0) {
            int status = sceKernelSetEventFlag(
                backend->audio_event, PSP_MEDIA_AUDIO_EVENT_READY);
            if (status < 0) {
                backend->stats.last_native_error = status;
                psp_media_log_failure(
                    backend, "sceKernelSetEventFlag-playing", status);
            }
        }
    }
}

static void psp_media_set_buffering(void *opaque, bool buffering)
{
    PspMediaBackend *backend = opaque;
    if (backend == NULL) return;
    uint64_t ignored = 0;
    (void) psp_media_audio_cursor_us(backend, &ignored);
    atomic_store(&backend->buffering, buffering);
    if (backend->audio_event >= 0) {
        int status = sceKernelSetEventFlag(
            backend->audio_event, PSP_MEDIA_AUDIO_EVENT_READY);
        if (status < 0) {
            backend->stats.last_native_error = status;
            psp_media_log_failure(
                backend, "sceKernelSetEventFlag-buffering", status);
        }
    }
}

/* Bounded poll for a published codec-job result. The worker's release store on
   codec_job_state makes every firmware side effect of the job visible before
   this returns true. Never used with a zero limit: a wedged firmware call must
   always get at least one deadline. */
static bool psp_media_wait_codec_job(
    PspMediaBackend *backend, uint32_t wait_limit_us, uint32_t *waited_us)
{
    uint32_t started = sceKernelGetSystemTimeLow();
    for (;;) {
        int state = atomic_load_explicit(
            &backend->codec_job_state, memory_order_acquire);
        uint32_t elapsed = sceKernelGetSystemTimeLow() - started;
        if (waited_us != NULL) *waited_us = elapsed;
        if (state != PSP_MEDIA_CODEC_JOB_RUNNING) return true;
        if (elapsed >= wait_limit_us) return false;
        if (sceKernelDelayThread(1000) < 0) return false;
    }
}

/* Ask the codec worker to leave its loop. Deleting the event flag is the only
   safe wake-up for an idle waiter when signalling the flag itself fails; a
   worker inside firmware is unaffected and still takes a bounded quarantine
   path. Every teardown exit runs this so a leaked backend never also leaks a
   live worker that could have been stopped. */
static void psp_media_signal_codec_stop(
    PspMediaBackend *backend, int *stop_signal, int *emergency_delete)
{
    atomic_store_explicit(
        &backend->codec_stop, true, memory_order_release);
    if (backend->codec_event < 0) return;
    *stop_signal = sceKernelSetEventFlag(
        backend->codec_event, PSP_MEDIA_CODEC_EVENT_STOP);
    if (*stop_signal >= 0) return;
    *emergency_delete = sceKernelDeleteEventFlag(backend->codec_event);
    if (*emergency_delete >= 0) backend->codec_event = -1;
}

/* A worker that already exited cannot run the Media Engine teardown job, and
   waiting the full quiesce window for a job it will never pick up would leak
   an otherwise reclaimable backend. Observation is nonblocking. */
static bool psp_media_codec_worker_dispatchable(
    const PspMediaBackend *backend)
{
    if (backend->codec_thread < 0 || backend->codec_event < 0) return false;
    PspThreadObservation observation;
    int referred = psp_thread_observe(backend->codec_thread, &observation);
    return psp_module_worker_poll_disposition(referred, observation.status)
        == PSP_MODULE_WORKER_POLL_PENDING;
}

/*
 * Teardown ordering, every step bounded:
 *   1. stop presenting, then wait up to PSP_MEDIA_CODEC_QUIESCE_WAIT_US for any
 *      in-flight codec job to publish a result (quarantine on timeout);
 *   2. stop the audio worker with its existing 250 ms / channel-release /
 *      100 ms / terminate / refer ladder (quarantine on timeout);
 *   3. release the audio channel with its 160 ms drain window (quarantine on
 *      timeout) so the Media Engine tail runs after the DMA owner is gone;
 *   4. dispatch the Media Engine teardown job -- ReleaseEDRAM, sceMpegDelete,
 *      the conditional sceMpegFinish -- to the still-alive codec worker and
 *      wait PSP_MEDIA_CODEC_QUIESCE_WAIT_US for it (mpeg-quarantine on
 *      timeout). Only a backend with no dispatchable worker runs that tail
 *      inline, which is the pre-worker create-failure path;
 *   5. signal codec_stop, join the worker with the same bounded wait and
 *      terminal-race check as before (quarantine on timeout);
 *   6. delete the event flags, apply the process-wide MPEG statics, log, free.
 * Every quarantine returns without freeing any firmware-visible buffer, for
 * the same leak-rather-than-UAF reason as before.
 */
static void psp_media_destroy(void *opaque)
{
    PspMediaBackend *backend = opaque;
    if (backend == NULL) return;
    psp_media_log(
        "tilefinch-media-decoder: event=h264-recovery-compat-summary "
        "enabled=%d recovery-idr=%u marking-clears=%u frame-rebases=%u",
        backend->h264_compat.enabled ? 1 : 0,
        backend->h264_compat.recovery_points_rewritten,
        backend->h264_compat.reference_markings_removed,
        backend->h264_compat.frame_numbers_rebased);
    backend->admissions_closed = true;
    psp_media_cancel_prepared_job(backend);
    atomic_store(&backend->playing, false);
    /* Before the teardown ladder, which can quarantine and never return the
       memory: the recording is worth more than the buffer it sits in. */
    psp_media_au_dump_flush();
    int stop_signal = 0;
    int thread_wait = 0;
    int thread_release_wait = 0;
    int thread_terminate = 0;
    int thread_refer = 0;
    int thread_delete = 0;
    int event_delete = 0;
    int emergency_event_delete = 0;
    int channel_release = 0;
    unsigned channel_release_attempts = 0;
    uint32_t channel_release_wait_us = 0;
    int edram_release = 0;
    int codec_stop_signal = 0;
    int codec_thread_wait = 0;
    int codec_thread_refer = 0;
    int codec_thread_delete = 0;
    int codec_event_delete = 0;
    int codec_emergency_event_delete = 0;
    uint32_t codec_quiesce_wait_us = 0;
    uint32_t teardown_wait_us = 0;
    const char *teardown_route = "none";
    int teardown_queue = (int) MEDIA_BACKEND_ACCEPTED;
    /* An in-flight decode owns the MPEG object the teardown job is about to
       delete. Give it the same bounded window a stuck worker already gets
       before declaring the Media Engine unrecoverable. */
    if (!psp_media_wait_codec_job(
            backend, PSP_MEDIA_CODEC_QUIESCE_WAIT_US,
            &codec_quiesce_wait_us)) {
        psp_media_signal_codec_stop(
            backend, &codec_stop_signal, &codec_emergency_event_delete);
        psp_media_log(
            "tilefinch-media-decoder: event=codec-quarantine "
            "phase=in-flight kind=%d state=%d native-stage=%d "
            "wait-us=%u elapsed-us=%u",
            atomic_load_explicit(
                &backend->codec_job_kind, memory_order_acquire),
            atomic_load_explicit(
                &backend->codec_job_state, memory_order_acquire),
            atomic_load_explicit(
                &backend->codec_native_stage, memory_order_acquire),
            codec_quiesce_wait_us,
            sceKernelGetSystemTimeLow()
                - atomic_load_explicit(
                    &backend->codec_job_started_us, memory_order_acquire));
        psp_media_quarantine();
        return;
    }
    /* The published result is not interesting to teardown, but the slot has to
       be idle before the Media Engine teardown job can be queued into it. */
    atomic_store_explicit(
        &backend->codec_job_state, PSP_MEDIA_CODEC_JOB_IDLE,
        memory_order_release);
    atomic_store_explicit(
        &backend->codec_job_kind, PSP_MEDIA_CODEC_KIND_NONE,
        memory_order_release);
    if (backend->audio_thread >= 0) {
        /* Last chance to say whether this session was ever audible: the
           worker is about to be stopped and its counters go with it. */
        psp_media_report_audio_output_final(backend);
        backend->audio_stop = true;
        if (backend->audio_event >= 0)
            stop_signal = sceKernelSetEventFlag(
                backend->audio_event, PSP_MEDIA_AUDIO_EVENT_STOP);
        if (stop_signal < 0 && backend->audio_event >= 0) {
            /* Deleting an event flag releases firmware waiters with a
               terminal wait error. This is the only safe wake-up fallback if
               signalling the flag itself fails; otherwise teardown can wait
               forever for a worker asleep in sceKernelWaitEventFlag. */
            emergency_event_delete =
                sceKernelDeleteEventFlag(backend->audio_event);
            if (emergency_event_delete >= 0)
                backend->audio_event = -1;
        }
        psp_media_log(
            "tilefinch-media-decoder: event=destroy-begin "
            "stop=0x%08X event-delete=0x%08X in-flight=%u",
            (unsigned) stop_signal,
            (unsigned) emergency_event_delete,
            atomic_load_explicit(
                &backend->audio_output_in_flight_slot,
                memory_order_acquire));
        /* The worker normally exits within one 1024-sample output block
           (128 ms at the lowest admitted rate). Never let a wedged firmware
           audio call turn Back, retry, suspend, or exit into an unbounded UI
           freeze. Releasing the channel is the least destructive wake-up;
           forced termination is reserved for a worker that still cannot
           prove it has stopped. */
        thread_wait = psp_thread_wait_end_bounded(
            backend->audio_thread, PSP_MEDIA_AUDIO_STOP_WAIT_US);
        bool worker_gone = thread_wait >= 0;
        if (!worker_gone) {
            (void) psp_media_release_audio_channel(
                backend, 0, &channel_release,
                &channel_release_attempts, &channel_release_wait_us);
            thread_release_wait = psp_thread_wait_end_bounded(
                backend->audio_thread, PSP_MEDIA_AUDIO_RELEASE_WAIT_US);
            worker_gone = thread_release_wait >= 0;
        }
        if (worker_gone) {
            thread_delete = sceKernelDeleteThread(backend->audio_thread);
        } else {
            thread_terminate = sceKernelTerminateDeleteThread(
                backend->audio_thread);
            worker_gone = thread_terminate >= 0;
            if (!worker_gone) {
                /* The worker can become terminal between the bounded wait
                   and forced termination. Confirm that race before leaking
                   an otherwise reclaimable decoder. */
                PspThreadObservation observation;
                thread_refer = psp_thread_observe(
                    backend->audio_thread, &observation);
                if (thread_refer >= 0
                    && psp_module_worker_poll_disposition(
                           thread_refer, observation.status)
                           == PSP_MODULE_WORKER_POLL_TERMINAL) {
                    thread_delete = sceKernelDeleteThread(
                        backend->audio_thread);
                    worker_gone = thread_delete >= 0;
                }
            }
        }
        if (!worker_gone) {
            /* The thread still owns backend/audio-queue pointers. Leaking
               this one failed backend is preferable to a deterministic UAF.
               The classified log gives hardware validation the exact three
               firmware results needed to diagnose the boundary. */
            psp_media_signal_codec_stop(
                backend, &codec_stop_signal,
                &codec_emergency_event_delete);
            psp_media_log(
                "tilefinch-media-decoder: event=destroy-quarantine "
                "wait=0x%08X release=0x%08X release-wait=0x%08X "
                "terminate=0x%08X refer=0x%08X",
                (unsigned) thread_wait, (unsigned) channel_release,
                (unsigned) thread_release_wait,
                (unsigned) thread_terminate, (unsigned) thread_refer);
            psp_media_quarantine();
            return;
        }
    }
    /* The Media Engine tail must run after the audio DMA owner is gone, so the
       channel release moves ahead of it rather than after libmpeg teardown. */
    if (backend->audio_channel != -1) {
        bool channel_released = psp_media_release_audio_channel(
            backend, PSP_MEDIA_AUDIO_CHANNEL_DRAIN_WAIT_US,
            &channel_release, &channel_release_attempts,
            &channel_release_wait_us);
        if (!channel_released) {
            psp_media_signal_codec_stop(
                backend, &codec_stop_signal,
                &codec_emergency_event_delete);
            psp_media_log(
                "tilefinch-media-decoder: event=audio-quarantine "
                "release=0x%08X attempts=%u waited=%uus",
                (unsigned) channel_release, channel_release_attempts,
                channel_release_wait_us);
            /* The firmware may still own a PCM queue pointer. Preserve the
               whole backend rather than freeing only the obvious buffer and
               leaving an indirect hardware UAF. */
            psp_media_quarantine();
            return;
        }
    }
    backend->codec_teardown_finish_runtime =
        psp_media_runtime_reset_after_failure(
            backend->stats.last_native_error,
            psp_mpeg_runtime_initialized);
    if (psp_media_codec_worker_dispatchable(backend)) {
        teardown_route = "worker";
        char teardown_error[64];
        teardown_error[0] = '\0';
        teardown_queue = (int) psp_media_queue_codec_job(
            backend, PSP_MEDIA_CODEC_KIND_TEARDOWN,
            teardown_error, sizeof(teardown_error));
        if (teardown_queue == (int) MEDIA_BACKEND_QUEUED) {
            if (!psp_media_wait_codec_job(
                    backend, PSP_MEDIA_CODEC_QUIESCE_WAIT_US,
                    &teardown_wait_us)
                || !backend->codec_teardown_ran) {
                /* sceMpegDelete/sceMpegFinish never returned, so libmpeg and
                   the Media Engine still own mpeg_memory, the DDR arena, the
                   surface, packet staging, the AU object, and the parameter
                   sets. Leak the whole backend rather than free memory the
                   firmware is still reading. */
                psp_media_signal_codec_stop(
                    backend, &codec_stop_signal,
                    &codec_emergency_event_delete);
                psp_media_log(
                    "tilefinch-media-decoder: event=mpeg-quarantine "
                    "native-stage=%d last-native-error=0x%08X mode=%d "
                    "state=%d wait-us=%u finish=%d",
                    atomic_load_explicit(
                        &backend->codec_native_stage, memory_order_acquire),
                    (unsigned) backend->stats.last_native_error,
                    backend->mpeg_mode,
                    atomic_load_explicit(
                        &backend->codec_job_state, memory_order_acquire),
                    teardown_wait_us,
                    backend->codec_teardown_finish_runtime ? 1 : 0);
                psp_media_quarantine();
                return;
            }
            atomic_store_explicit(
                &backend->codec_job_state, PSP_MEDIA_CODEC_JOB_IDLE,
                memory_order_release);
            atomic_store_explicit(
                &backend->codec_job_kind, PSP_MEDIA_CODEC_KIND_NONE,
                memory_order_release);
        } else {
            /* The worker is alive but unreachable through its event flag, so
               nothing would ever pick the job up. Fall back to the historical
               inline tail rather than wait out a deadline for certain. */
            teardown_route = "inline-unreachable";
            (void) psp_media_run_teardown_job(backend, NULL, 0);
        }
    } else {
        /* No worker to dispatch to: an early create failure, which is exactly
           the case that predates the worker and never reached the decoder. */
        teardown_route = "inline";
        (void) psp_media_run_teardown_job(backend, NULL, 0);
    }
    edram_release = backend->codec_teardown_edram_release;
    if (backend->codec_teardown_finish_runtime) {
        psp_mpeg_runtime_initialized = false;
        /* A failed stream can leave the selected Media Engine program in a
           stream-specific state after libmpeg teardown. Force the same
           firmware bridge used for profile transitions on the next retry. */
        psp_media_me_boot_type = -1;
        psp_media_me_boot_resolved_nid = 0;
    }
    psp_media_signal_codec_stop(
        backend, &codec_stop_signal, &codec_emergency_event_delete);
    if (backend->codec_thread >= 0) {
        codec_thread_wait = psp_thread_wait_end_bounded(
            backend->codec_thread, PSP_MEDIA_CODEC_QUIESCE_WAIT_US);
        if (codec_thread_wait < 0) {
            /* Close the race where the worker becomes terminal immediately
               after the bounded wait expires. Do not quarantine reclaimable
               state merely because the deadline and return crossed. */
            PspThreadObservation observation;
            codec_thread_refer = psp_thread_observe(
                backend->codec_thread, &observation);
            if (codec_thread_refer >= 0
                && psp_module_worker_poll_disposition(
                       codec_thread_refer, observation.status)
                       == PSP_MODULE_WORKER_POLL_TERMINAL) {
                codec_thread_delete = sceKernelDeleteThread(
                    backend->codec_thread);
                if (codec_thread_delete >= 0)
                    backend->codec_thread = -1;
            }
        }
        if (backend->codec_thread >= 0 && codec_thread_wait < 0) {
            /* A firmware codec call is not safely preemptible. The browser
               has already detached this backend, so preserve the worker's
               complete object graph and return immediately. This makes Back
               bounded even if Media Engine firmware never returns, without
               turning recovery into a use-after-free. */
            psp_media_log(
                "tilefinch-media-decoder: event=codec-quarantine "
                "kind=%d state=%d native-stage=%d wait=0x%08X "
                "stop=0x%08X refer=0x%08X event-delete=0x%08X "
                "elapsed-us=%u",
                atomic_load_explicit(
                    &backend->codec_job_kind, memory_order_acquire),
                atomic_load_explicit(
                    &backend->codec_job_state, memory_order_acquire),
                atomic_load_explicit(
                    &backend->codec_native_stage, memory_order_acquire),
                (unsigned) codec_thread_wait,
                (unsigned) codec_stop_signal,
                (unsigned) codec_thread_refer,
                (unsigned) codec_emergency_event_delete,
                sceKernelGetSystemTimeLow()
                    - atomic_load_explicit(
                        &backend->codec_job_started_us,
                        memory_order_acquire));
            psp_media_quarantine();
            return;
        }
        if (backend->codec_thread >= 0) {
            codec_thread_delete = sceKernelDeleteThread(
                backend->codec_thread);
            backend->codec_thread = -1;
        }
    }
    if (backend->codec_event >= 0) {
        codec_event_delete = sceKernelDeleteEventFlag(
            backend->codec_event);
        backend->codec_event = -1;
    }
    if (backend->audio_event >= 0)
        event_delete = sceKernelDeleteEventFlag(backend->audio_event);
    psp_media_log(
        "tilefinch-media-decoder: event=destroy stop=0x%08X "
        "wait=0x%08X release-wait=0x%08X terminate=0x%08X "
        "refer=0x%08X "
        "thread-delete=0x%08X event-delete=0x%08X "
        "emergency-event-delete=0x%08X channel-release=0x%08X "
        "release-attempts=%u release-wait=%uus "
        "codec-stop=0x%08X codec-wait=0x%08X "
        "codec-refer=0x%08X codec-thread-delete=0x%08X "
        "codec-event-delete=0x%08X codec-emergency-event-delete=0x%08X "
        "codec-quiesce-wait=%uus "
        "mpeg-teardown=%s queue=%d ran=%d wait=%uus "
        "edram-release=0x%08X runtime-reset=%d",
        (unsigned) stop_signal, (unsigned) thread_wait,
        (unsigned) thread_release_wait, (unsigned) thread_terminate,
        (unsigned) thread_refer,
        (unsigned) thread_delete, (unsigned) event_delete,
        (unsigned) emergency_event_delete,
        (unsigned) channel_release, channel_release_attempts,
        channel_release_wait_us,
        (unsigned) codec_stop_signal, (unsigned) codec_thread_wait,
        (unsigned) codec_thread_refer,
        (unsigned) codec_thread_delete, (unsigned) codec_event_delete,
        (unsigned) codec_emergency_event_delete,
        codec_quiesce_wait_us,
        teardown_route, teardown_queue,
        backend->codec_teardown_ran ? 1 : 0, teardown_wait_us,
        (unsigned) edram_release,
        backend->codec_teardown_finish_runtime ? 1 : 0);
    (void) codec_quiesce_wait_us;
    (void) teardown_route;
    (void) teardown_queue;
    (void) teardown_wait_us;
    (void) stop_signal;
    (void) thread_wait;
    (void) thread_release_wait;
    (void) thread_terminate;
    (void) thread_refer;
    (void) thread_delete;
    (void) event_delete;
    (void) emergency_event_delete;
    (void) channel_release;
    (void) codec_stop_signal;
    (void) codec_thread_wait;
    (void) codec_thread_refer;
    (void) codec_thread_delete;
    (void) codec_event_delete;
    (void) codec_emergency_event_delete;
    (void) edram_release;
    for (unsigned slot = 0; slot < PSP_MEDIA_SURFACE_SLOTS; slot++)
        psp_media_release(backend->surfaces[slot]);
    psp_media_release(backend->audio_queue);
    psp_media_release(backend->audio_pcm);
    psp_media_release(backend->audio_codec);
    psp_media_release(backend->audio_pending);
    psp_media_release(backend->audio_staging);
    psp_media_release(backend->packet_staging);
    psp_media_release(backend->video_parameter_sets);
    psp_media_release(backend->video_au);
    psp_media_release(backend->ddr_memory);
    psp_media_release(backend->mpeg_memory);
    /* Only a teardown that ran to completion reaches this line: every
       quarantine returns earlier and has already poisoned the pool. Rewinding
       here is what lets the next stream reuse the same low, Media Engine
       visible storage instead of allocating wherever the heap has grown to. */
    (void) psp_media_pool_reset(&psp_media_pool);
    budget_reservation_release(&backend->external);
    Budget *budget = backend->budget;
    memset(backend, 0, sizeof(*backend));
    budget_free(budget, backend);
}

bool media_psp_backend_create_split(
    Budget *budget, const MediaMp4Demux *video_demux,
    const MediaMp4Demux *audio_demux,
    MediaBackend *backend_out, char *error, size_t error_size)
{
    if (error != NULL && error_size != 0) error[0] = '\0';
    if (budget == NULL || video_demux == NULL || backend_out == NULL) {
        psp_media_error(error, error_size,
                        "PSP media request invalid");
        return false;
    }
    if (psp_media_backend_is_quarantined) {
        psp_media_error(
            error, error_size,
            "PSP codec teardown was quarantined; restart Tilefinch");
        psp_media_log(
            "tilefinch-media-decoder: event=create-refused "
            "reason=prior-teardown-quarantine");
        return false;
    }
    PspMediaBackend *backend = budget_calloc_category(
        budget, BUDGET_CATEGORY_NAVIGATION, 1, sizeof(*backend));
    if (backend == NULL) {
        psp_media_error(error, error_size,
                        "PSP backend state exceeded page budget");
        return false;
    }
    backend->budget = budget;
    backend->audio_channel = -1;
    backend->audio_event = -1;
    backend->audio_thread = -1;
    backend->codec_event = -1;
    backend->codec_thread = -1;
    backend->last_nal_mode = -1;
    backend->video_startup_catchup = true;
    atomic_init(&backend->playing, true);
    atomic_init(&backend->buffering, false);
    atomic_init(&backend->audio_origin_initialized, false);
    atomic_init(&backend->audio_stop, false);
    atomic_init(&backend->audio_resetting, false);
    atomic_init(&backend->codec_stop, false);
    atomic_init(
        &backend->codec_job_state, PSP_MEDIA_CODEC_JOB_IDLE);
    atomic_init(
        &backend->codec_prepared_state, PSP_MEDIA_CODEC_PREPARED_EMPTY);
    atomic_init(&backend->video_refusal_dirty, false);
    atomic_init(
        &backend->codec_completion_state, PSP_MEDIA_CODEC_COMPLETION_EMPTY);
    atomic_init(&backend->codec_job_kind, PSP_MEDIA_CODEC_KIND_NONE);
    atomic_init(&backend->codec_job_started_us, 0u);
    atomic_init(
        &backend->codec_native_stage, PSP_MEDIA_CODEC_STAGE_NONE);
    atomic_init(&backend->audio_output_in_flight_slot, 0);
    atomic_init(&backend->audio_queue_read, 0);
    atomic_init(&backend->audio_queue_write, 0);
    atomic_init(&backend->audio_queue_generation, 0);
    atomic_init(&backend->audio_first_output_us, 0u);
    backend->audio_cursor_last_us = 0u;
    backend->audio_cursor_elapsed_us = 0u;
    atomic_init(&backend->audio_output_blocks, 0);
    atomic_init(&backend->audio_output_starves, 0);
    atomic_init(
        &backend->audio_output_first_status,
        PSP_MEDIA_AUDIO_OUTPUT_PENDING);
    atomic_init(&backend->audio_output_reported, false);
    atomic_init(&backend->audio_worker_error, 0);
    atomic_init(&backend->audio_worker_stage, 0);
    const MediaMp4Demux *sources[2] = {video_demux, audio_demux};
    size_t source_count = audio_demux == NULL ? 1u : 2u;
    for (size_t source = 0; source < source_count; source++) {
        for (size_t i = 0;
             i < media_mp4_track_count(sources[source]); i++) {
            MediaMp4TrackInfo info;
            if (!media_mp4_track_info(sources[source], i, &info)) continue;
            if (!backend->have_video && info.kind == MEDIA_MP4_TRACK_VIDEO
                && info.codec == MEDIA_MP4_FOURCC('a','v','c','1')) {
                backend->video = info;
                backend->have_video = true;
            } else if (!backend->have_audio
                       && info.kind == MEDIA_MP4_TRACK_AUDIO
                       && info.codec == MEDIA_MP4_FOURCC('m','p','4','a')) {
                backend->audio = info;
                backend->have_audio = true;
            }
        }
    }
    if (!backend->have_video || backend->video.width > 640
        || backend->video.height > 360) {
        psp_media_error(error, error_size,
                        "PSP AVC exceeds 640x360");
        psp_media_destroy(backend);
        return false;
    }
    if (backend->video.largest_sample == 0
        || backend->video.largest_sample
             > PSP_MEDIA_MAXIMUM_PACKET_BYTES
        || (backend->have_audio
            && (backend->audio.largest_sample == 0
                || backend->audio.largest_sample
                     > PSP_MEDIA_MAXIMUM_PACKET_BYTES))) {
        psp_media_error(error, error_size,
                        "PSP media packet exceeds 512KiB");
        psp_media_destroy(backend);
        return false;
    }
    if (audio_demux != NULL && !backend->have_audio) {
        psp_media_error(error, error_size,
                        "adaptive AAC missing");
        psp_media_destroy(backend);
        return false;
    }
    if (backend->have_audio
        && (backend->audio.sample_rate == 0
            || backend->audio.sample_rate
               > PSP_MEDIA_AUDIO_MAXIMUM_RATE)) {
        psp_media_error(error, error_size,
                        "PSP AAC rate exceeds 48kHz");
        psp_media_destroy(backend);
        return false;
    }
    if (backend->have_audio && backend->audio.channels != 2) {
        psp_media_error(error, error_size,
                        "PSP AAC output requires stereo");
        psp_media_destroy(backend);
        return false;
    }
    MediaAacStreamInfo aac_info = {0};
    if (backend->have_audio
        && !media_aac_esds_stream_info(
            backend->audio.codec_config,
            backend->audio.codec_config_length, &aac_info)) {
        psp_media_error(
            error, error_size,
            "PSP requires AAC-LC; HE-AAC/SBR is unsupported");
        psp_media_destroy(backend);
        return false;
    }
    if (backend->have_audio
        && !psp_media_audio_stream_admitted(
            backend->audio.sample_rate, backend->audio.channels,
            aac_info.sample_rate, aac_info.channels,
            aac_info.samples_per_frame)) {
        psp_media_error(
            error, error_size,
            "PSP AAC config mismatch: %uHz/%uch/%u samples",
            (unsigned) aac_info.sample_rate,
            (unsigned) aac_info.channels,
            (unsigned) aac_info.samples_per_frame);
        psp_media_destroy(backend);
        return false;
    }
    if (!media_h264_avcc_dimensions(
            backend->video.codec_config,
            backend->video.codec_config_length,
            &backend->decoded_width, &backend->decoded_height,
            &backend->nal_length_size)
        || backend->decoded_width > 640
        || backend->decoded_height > 360) {
        psp_media_error(
            error, error_size,
            "PSP AVC SPS exceeds 640x360");
        psp_media_destroy(backend);
        return false;
    }
    bool h264_compat_enabled = media_h264_psp_compat_init(
        &backend->h264_compat, backend->video.codec_config,
        backend->video.codec_config_length);
#if defined(TILEFINCH_PSP_VALIDATION_LOG)
    psp_media_log(
        "tilefinch-media-decoder: event=h264-recovery-compat enabled=%d "
        "frame-bits=%u poc-bits=%u",
        h264_compat_enabled ? 1 : 0,
        (unsigned) backend->h264_compat.log2_max_frame_num,
        (unsigned) backend->h264_compat.log2_max_pic_order_cnt_lsb);
#else
    (void) h264_compat_enabled;
#endif
    PspAvcNal admitted_sets;
    memset(&admitted_sets, 0, sizeof(admitted_sets));
    if (!psp_avcc_sets(&backend->video, &admitted_sets)) {
        psp_media_error(
            error, error_size,
            "PSP AVC requires exactly one SPS and PPS");
        psp_media_destroy(backend);
        return false;
    }
    PspMediaSurfacePolicy surface_policy = {0};
    if (!psp_media_surface_policy(
            backend->decoded_width, backend->decoded_height,
            &surface_policy)) {
        psp_media_error(error, error_size,
                        "PSP AVC surface policy rejected geometry");
        psp_media_destroy(backend);
        return false;
    }
    PspMediaDecoderPolicy decoder_policy = {0};
    unsigned avc_profile = backend->video.codec_config_length > 1u
        ? backend->video.codec_config[1] : 0u;
    unsigned avc_level = backend->video.codec_config_length > 3u
        ? backend->video.codec_config[3] : 0u;
    bool wide_program_enabled =
        psp_media_wide_program_enabled(psp_media_wide_program_mode);
    /*
     * Refuse the wide program before it costs a Media Engine boot, an MPEG
     * workspace, and a doomed first access unit. The session already asks the
     * resolver for a stream the proven program can decode; a source that is
     * still wide here (an anamorphic rendition, an offline library entry, or
     * another MP4) has no admissible program and must say so.
     */
    if (psp_media_wide_program_required(
            backend->decoded_width, backend->decoded_height)
        && !wide_program_enabled) {
        psp_media_log(
            "tilefinch-media-decoder: event=wide-program-clamped "
            "source=%ux%u knob=%s program=mode4-type3",
            backend->decoded_width, backend->decoded_height,
            psp_media_wide_program_name(psp_media_wide_program_mode));
        psp_media_error(
            error, error_size,
            "PSP video %ux%u needs the wide decoder program",
            backend->decoded_width, backend->decoded_height);
        psp_media_destroy(backend);
        return false;
    }
    if (!psp_media_decoder_policy(
            avc_profile, avc_level, backend->decoded_width,
            backend->decoded_height, wide_program_enabled,
            psp_media_baseline_boot_call_enabled(
                psp_media_wide_program_mode),
            &decoder_policy)) {
        psp_media_error(
            error, error_size,
            "PSP AVC profile/level unsupported: %u/%u",
            avc_profile, avc_level);
        psp_media_destroy(backend);
        return false;
    }
    backend->frame_stride = surface_policy.stride_pixels;
    backend->surface_rows = surface_policy.surface_rows;
    /* The CSC writes whole macroblocks, so its extent is the padded picture:
       padded-height rows of padded-width pixels, laid out at the surface
       stride. The surface has to hold that, not the display size. */
    backend->csc_rows =
        psp_media_macroblock_align(backend->decoded_height);
    backend->surface_bytes = surface_policy.surface_bytes;
    if (!psp_media_surface_covers_decoded(
            &surface_policy, backend->decoded_width,
            backend->decoded_height)
        || backend->csc_rows == 0
        || backend->csc_rows > backend->surface_rows) {
        psp_media_error(
            error, error_size,
            "PSP AVC CSC extent exceeds protected surface");
        psp_media_destroy(backend);
        return false;
    }
    if (!budget_reservation_acquire(
            &backend->external, budget, BUDGET_CATEGORY_RESOURCE,
            surface_policy.external_reserve_bytes)) {
        psp_media_error(
            error, error_size,
            "PSP %up codec working set exceeded page budget",
            backend->decoded_height > 272 ? 360u : 240u);
        psp_media_destroy(backend);
        return false;
    }
    backend->stats.external_bytes =
        surface_policy.external_reserve_bytes;
    if (!psp_media_modules_ready) {
        psp_media_error(
            error, error_size,
            "PSP AV modules were not prepared");
        psp_media_destroy(backend);
        return false;
    }
    int status = 0;
    const char *native_stage = "media-engine-boot";
    uint32_t me_boot_nid = 0;
    /*
     * The Media Engine program is part of the process-wide MPEG runtime.
     * Mature raw-MP4 players finish and reinitialize MPEG when changing
     * between wide Main, ordinary Main, and Baseline programs. Preserve the
     * cheap long-lived runtime for same-profile seeks/replays, but do not
     * carry a type-1 runtime into the automatic 240p type-3 fallback.
     */
    if (psp_mpeg_runtime_initialized
        && psp_media_me_boot_type >= 0
        && psp_media_me_boot_type != decoder_policy.me_boot_type) {
        psp_media_log(
            "tilefinch-media-decoder: runtime-restart old-me-type=%d "
            "new-me-type=%d\n",
            psp_media_me_boot_type, decoder_policy.me_boot_type);
        sceMpegFinish();
        psp_mpeg_runtime_initialized = false;
        psp_media_me_boot_type = -1;
        psp_media_me_boot_resolved_nid = 0;
    }
    /* psp_media_boot_media_engine short-circuits when a real call has already
       put the requested program on the engine, which on the first open of a
       process is never true. Sample the condition here so the log below can
       say which of the two happened, because that is the whole difference the
       device evidence turns on. */
    bool me_boot_skipped =
        psp_media_me_boot_type == decoder_policy.me_boot_type;
    status = psp_media_boot_media_engine(
        decoder_policy.me_boot_type, &me_boot_nid);
    /*
     * Say what the Media Engine boot did before anything can reach the
     * raw-NAL bridge, and commit it to the Memory Stick. Every device run so
     * far has ended with a freeze or a power-off somewhere after this point,
     * which is exactly when an unsynced tail of the log is lost -- and the
     * boot outcome is the first thing the next run has to be able to read.
     */
    if (me_boot_skipped) {
        psp_media_log(
            "tilefinch-media-decoder: event=me-boot type=%d skipped=1",
            decoder_policy.me_boot_type);
    } else {
        psp_media_log(
            "tilefinch-media-decoder: event=me-boot type=%d "
            "status=0x%08X nid=0x%08X devkit=0x%08X",
            decoder_policy.me_boot_type, (unsigned) status,
            (unsigned) me_boot_nid,
            (unsigned) sceKernelDevkitVersion());
        psp_media_commit_log();
    }
    if (status < 0) {
        backend->stats.last_native_error = status;
        psp_media_log_failure(
            backend, native_stage, status);
        psp_media_error(
            error, error_size,
            "PSP Media Engine boot type %d failed: 0x%08X",
            decoder_policy.me_boot_type, (unsigned) status);
        psp_media_destroy(backend);
        return false;
    }
    if (!psp_mpeg_runtime_initialized) {
        native_stage = "sceMpegInit";
        status = sceMpegInit();
        if (status < 0) goto native_failure;
        psp_mpeg_runtime_initialized = true;
    }
    backend->mpeg_mode = decoder_policy.mpeg_mode;
    backend->me_boot_type = decoder_policy.me_boot_type;
    native_stage = "sceMpegQueryMemSize";
    backend->mpeg_memory_bytes =
        sceMpegQueryMemSize(backend->mpeg_mode);
    if (backend->mpeg_memory_bytes <= 0) {
        /* Zero is not a usable workspace and must not masquerade as a
           successful native status.  A zero result is a broken firmware
           runtime just as surely as a negative one: force the failure path to
           reset process-global MPEG state before the 240p retry. */
        status = backend->mpeg_memory_bytes < 0
            ? backend->mpeg_memory_bytes : -1;
        goto native_failure;
    }
    /*
     * PMPlayer's hardware-proven raw MP4 path uses a 2 MiB Media Engine DDR
     * arena on a 4 MiB boundary, including for its wide decoder mode. Take the
     * largest and most constrained block first: the boot-time pool is itself
     * 4 MiB aligned, so the arena's alignment is free at offset 0 and no
     * almost-4 MiB gap is wasted the way a heap memalign here would.
     *
     * The pool is a bump allocator with no per-block free, which is sound
     * because at most one backend exists at a time: firmware's MPEG runtime
     * and audio channel are themselves single-instance, every create failure
     * below funnels through psp_media_destroy, and destroy is what rewinds
     * the cursor.
     */
    backend->ddr_memory = psp_media_alloc_shared(
        PSP_MEDIA_DDR_BYTES, PSP_MEDIA_DDR_ALIGNMENT);
    for (unsigned slot = 0; slot < PSP_MEDIA_SURFACE_SLOTS; slot++) {
        backend->surfaces[slot] =
            psp_media_alloc64(backend->surface_bytes);
        /* Nobody owns either of them yet, and the first generation is 1 so
           that a zeroed reader tuple can never match a live slot. */
        backend->slots[slot] = (PspMediaSurfaceSlot) {
            .state = PSP_MEDIA_SLOT_FREE,
            .generation = 1u,
            .epoch = PSP_MEDIA_EPOCH_INVALID
        };
        psp_media_surface_lease_publish(slot, 0, false);
        /* Both slots start FREE, so the dwell clock starts here rather than
           at the first transition -- otherwise the whole of the open would be
           charged to whichever state the first conversion happened to leave,
           and the four totals would not sum to the session. */
        backend->slot_dwell[slot] = (PspMediaSlotDwell) {
            .since_us = psp_media_stamp_us()
        };
    }
    /* And the pipeline conditions, from the same instant. Both slots free
       and nothing converting is the honest description of a decoder that has
       not been fed yet; the first sample charges nothing to it. */
    backend->slot_sample_us = psp_media_stamp_us();
    backend->slot_no_free = false;
    backend->slot_free_idle = true;
    backend->reading_slot = -1;
    /* The session's first epoch. Never zero: zero is the reserved value a
       never-stamped job reads as, and it must stay distinguishable from a
       stream that is genuinely running. */
    backend->session_epoch = PSP_MEDIA_EPOCH_FIRST;
    backend->codec_job_epoch = PSP_MEDIA_EPOCH_INVALID;
    backend->mpeg_memory = psp_media_alloc64(backend->mpeg_memory_bytes);
    /* Lazy fragmented streams reveal future sample sizes only when their
       sidx window is opened. Reserve the admitted ceiling before firmware
       heaps and playback churn fragment user memory. */
    backend->packet_staging_bytes = PSP_MEDIA_PACKET_STAGING_BYTES;
    backend->packet_staging =
        psp_media_alloc64(backend->packet_staging_bytes);
    if (backend->have_audio) {
        backend->audio_codec =
            psp_media_alloc64(PSP_MEDIA_AUDIO_CODEC_BYTES);
        backend->audio_pcm = psp_media_alloc64(PSP_MEDIA_AUDIO_PCM_BYTES);
        backend->audio_queue =
            psp_media_alloc64(PSP_MEDIA_AUDIO_QUEUE_BYTES);
        /* Firmware DMAs the compressed unit out of here, so it takes the same
           64-byte alignment and the same pool as every other Media Engine
           buffer -- an audio staging above PSP_MEDIA_ME_VISIBLE_LIMIT would
           fail the AAC decode on structurally perfect input, exactly as the
           packet staging would. */
        backend->audio_staging =
            psp_media_alloc64(PSP_MEDIA_AUDIO_STAGING_BYTES);
        backend->audio_pending =
            psp_media_alloc64(PSP_MEDIA_AUDIO_PENDING_BYTES);
    }
    /* PMPlayer's proven raw decoder allocates a 64-byte AU object rather than
       embedding the 24-byte public struct in ordinary heap storage. */
    backend->video_au = psp_media_alloc64(PSP_MEDIA_VIDEO_AU_BYTES);
    (void) psp_media_copy_parameter_sets(backend);
    /*
     * Say where every Media Engine buffer actually landed, before anything
     * can reach sceMpegCreate or the raw-NAL bridge.
     *
     * `high` counts the buffers at or above PSP_MEDIA_ME_VISIBLE_LIMIT, which
     * the Media Engine cannot address at all (media_backend_psp_pool.h). A
     * nonzero count is the whole explanation for two independent codecs
     * rejecting structurally perfect input, and it has to survive whatever
     * the decode attempt does to the machine, so it is committed to the card
     * here rather than reported afterwards.
     */
    {
        const void *const shared[] = {
            backend->ddr_memory, backend->mpeg_memory,
            backend->packet_staging,
            backend->surfaces[0], backend->surfaces[1],
            backend->video_au, backend->video_parameter_sets,
            backend->audio_codec, backend->audio_pcm, backend->audio_queue,
            backend->audio_staging
        };
        /* Every slot is a Media Engine write target, so every slot has to
           pass the same addressability test the single one did. */
        _Static_assert(PSP_MEDIA_SURFACE_SLOTS == 2u,
                       "me-memory audit enumerates the surface slots");
        psp_media_log(
            "tilefinch-media-decoder: event=me-memory ddr=0x%08X/%uB "
            "workspace=0x%08X/%dB staging=0x%08X/%uB surface=0x%08X/0x%08X/%uB "
            "au=0x%08X sets=0x%08X audio=0x%08X pcm=0x%08X queue=0x%08X "
            "astage=0x%08X apending=0x%08X "
            "high=%d",
            (unsigned) (uintptr_t) backend->ddr_memory,
            (unsigned) PSP_MEDIA_DDR_BYTES,
            (unsigned) (uintptr_t) backend->mpeg_memory,
            backend->mpeg_memory_bytes,
            (unsigned) (uintptr_t) backend->packet_staging,
            (unsigned) backend->packet_staging_bytes,
            (unsigned) (uintptr_t) backend->surfaces[0],
            (unsigned) (uintptr_t) backend->surfaces[1],
            (unsigned) backend->surface_bytes,
            (unsigned) (uintptr_t) backend->video_au,
            (unsigned) (uintptr_t) backend->video_parameter_sets,
            (unsigned) (uintptr_t) backend->audio_codec,
            (unsigned) (uintptr_t) backend->audio_pcm,
            (unsigned) (uintptr_t) backend->audio_queue,
            (unsigned) (uintptr_t) backend->audio_staging,
            (unsigned) (uintptr_t) backend->audio_pending,
            (int) psp_media_me_invisible_count(
                shared, sizeof(shared) / sizeof(shared[0])));
        psp_media_commit_log();
        (void) shared;
    }
    if (backend->mpeg_memory == NULL || backend->video_au == NULL
        || backend->video_parameter_sets == NULL
        || backend->packet_staging == NULL
        || (backend->have_audio
            && (backend->audio_codec == NULL
                || backend->audio_pcm == NULL
                || backend->audio_queue == NULL
                || backend->audio_staging == NULL
                || backend->audio_pending == NULL))
        || backend->ddr_memory == NULL
        || backend->surfaces[0] == NULL
        || backend->surfaces[1] == NULL) {
        psp_media_log(
            "tilefinch-media-decoder: event=allocation-failure "
            "mpeg=%d/%dB au=%d/64B sets=%d/%zuB packet=%d/%zuB "
            "audio=%d/%zuB ddr=%d/%uB surface=%d/%zuB "
            "system-free=%u system-largest=%u",
            backend->mpeg_memory != NULL ? 1 : 0,
            backend->mpeg_memory_bytes,
            backend->video_au != NULL ? 1 : 0,
            backend->video_parameter_sets != NULL ? 1 : 0,
            backend->video_parameter_sets_bytes,
            backend->packet_staging != NULL ? 1 : 0,
            backend->packet_staging_bytes,
            !backend->have_audio
                || (backend->audio_codec != NULL
                    && backend->audio_pcm != NULL
                    && backend->audio_queue != NULL
                    && backend->audio_staging != NULL
                    && backend->audio_pending != NULL) ? 1 : 0,
            backend->have_audio
                ? (size_t) PSP_MEDIA_AUDIO_CODEC_BYTES
                    + PSP_MEDIA_AUDIO_PCM_BYTES
                    + PSP_MEDIA_AUDIO_QUEUE_BYTES
                : 0,
            backend->ddr_memory != NULL ? 1 : 0,
            PSP_MEDIA_DDR_BYTES,
            (backend->surfaces[0] != NULL
             && backend->surfaces[1] != NULL) ? 1 : 0,
            backend->surface_bytes,
            sceKernelTotalFreeMemSize(), sceKernelMaxFreeMemSize());
        psp_media_error(error, error_size,
                        "PSP aligned codec allocation failed");
        psp_media_destroy(backend);
        return false;
    }
    memset(&backend->ringbuffer, 0, sizeof(backend->ringbuffer));
    native_stage = "sceMpegCreate";
    status = sceMpegCreate(
        &backend->mpeg, backend->mpeg_memory,
        backend->mpeg_memory_bytes, &backend->ringbuffer,
        (SceInt32) PSP_MEDIA_MPEG_DECODE_WIDTH, backend->mpeg_mode,
        (SceInt32) (uintptr_t) backend->ddr_memory);
    if (status < 0) goto native_failure;
    backend->mpeg_created = true;
    backend->video_es = (unsigned char *) backend->ddr_memory + 0x10000;
    memset(backend->video_au, 0xFF, PSP_MEDIA_VIDEO_AU_BYTES);
    sceKernelDcacheWritebackInvalidateRange(
        backend->video_au, PSP_MEDIA_VIDEO_AU_BYTES);
    native_stage = "sceMpegInitAu";
    status = sceMpegInitAu(
        &backend->mpeg, backend->video_es, backend->video_au);
    if (status < 0) goto native_failure;
    /* Written by sceMpegInitAu on the main CPU. Write the descriptor back
       before dropping the lines -- a pure invalidate here discarded exactly
       these stores, and every device run then entered the raw-NAL bridge with
       the 0xFF fill still in RAM. */
    sceKernelDcacheWritebackInvalidateRange(
        backend->video_au, PSP_MEDIA_VIDEO_AU_BYTES);
    backend->raw_nal_probe_pending = true;

    if (backend->have_audio) {
        memset(backend->audio_codec, 0, PSP_MEDIA_AUDIO_CODEC_BYTES);
        sceKernelDcacheWritebackInvalidateRange(
            backend->audio_codec,
            psp_media_cache_extent(PSP_MEDIA_AUDIO_CODEC_BYTES));
        native_stage = "sceAudiocodecCheckNeedMem";
        status = sceAudiocodecCheckNeedMem(
            backend->audio_codec, PSP_CODEC_AAC);
        /*
         * CheckNeedMem is a main-CPU firmware call, not a Media Engine one:
         * its whole effect is a store of the needed size into control word 4,
         * made through the CPU's own data cache. A pure invalidate here threw
         * that line away before it reached memory, the reread missed and
         * refilled the memset era zeros, and the pool grant refused a
         * zero-size buffer -- `event=audio-edram mode=pool size=0
         * status=0xFFFFFFFF` on a PSP-3000 with the engine pool otherwise
         * healthy. Write back first, then drop.
         */
        if (status >= 0) {
            sceKernelDcacheWritebackInvalidateRange(
                backend->audio_codec,
                psp_media_cache_extent(PSP_MEDIA_AUDIO_CODEC_BYTES));
        }
        /*
         * Attach the AAC work buffer. PMPlayer never calls the firmware
         * grant for it -- see psp_media_pool_audiocodec_edram -- so neither
         * does the default; `experimental_wide_video=edram-real` restores the
         * sceAudiocodecGetEDRAM/ReleaseEDRAM pair for the A/B.
         *
         * Only the real grant is followed by a cache op, and it has to be a
         * writeback-invalidate: sceAudiocodecGetEDRAM writes the buffer
         * pointer into control word 3 from the main CPU, so a pure invalidate
         * discards it exactly the way it discarded CheckNeedMem's size, and
         * the decode then fails with 0x807F0001 on a block whose work buffer
         * reads back as zero. The pool substitute is a plain CPU store into
         * that same block and needs nothing at all here -- the writeback
         * below publishes it.
         */
        unsigned edram_bytes = 0;
        bool real_edram =
            psp_media_real_edram_enabled(psp_media_wide_program_mode);
        if (status >= 0) {
            if (real_edram) {
                native_stage = "sceAudiocodecGetEDRAM";
                status = sceAudiocodecGetEDRAM(
                    backend->audio_codec, PSP_CODEC_AAC);
                if (status >= 0) {
                    sceKernelDcacheWritebackInvalidateRange(
                        backend->audio_codec,
                        psp_media_cache_extent(
                            PSP_MEDIA_AUDIO_CODEC_BYTES));
                    backend->audio_edram_real = true;
                }
            } else {
                native_stage = "audiocodec-pool-edram";
                status = psp_media_pool_audiocodec_edram(
                    backend, &edram_bytes);
            }
        }
        /*
         * Say which mechanism supplied the buffer, how big firmware asked for
         * it to be, and where it landed, before sceAudiocodecInit hands the
         * control block to the Media Engine. Committed to the card for the
         * same reason event=me-memory is: every device session so far has
         * ended in a freeze or a power-off somewhere past this point.
         */
        psp_media_log(
            "tilefinch-media-decoder: event=audio-edram mode=%s size=%u "
            "status=0x%08X buffer=0x%08X high=%d",
            real_edram ? "real" : "pool",
            real_edram ? (unsigned) backend->audio_codec[4] : edram_bytes,
            (unsigned) status,
            (unsigned) backend->audio_codec[3],
            psp_media_me_invisible(
                (uintptr_t) backend->audio_codec[3]) ? 1 : 0);
        psp_media_commit_log();
        if (status < 0) goto native_failure;
        backend->audio_edram = true;
        backend->audio_codec[10] = backend->audio.sample_rate;
        sceKernelDcacheWritebackInvalidateRange(
            backend->audio_codec,
            psp_media_cache_extent(PSP_MEDIA_AUDIO_CODEC_BYTES));
        native_stage = "sceAudiocodecInit";
        status = sceAudiocodecInit(
            backend->audio_codec, PSP_CODEC_AAC);
        if (status < 0) goto native_failure;
        /* sceAudiocodecInit rewrites the block's firmware-owned header from
           the main CPU before it ever reaches the engine. Write those lines
           back, then drop them, so the decode reads what Init left rather
           than what the CPU stored before it. */
        sceKernelDcacheWritebackInvalidateRange(
            backend->audio_codec,
            psp_media_cache_extent(PSP_MEDIA_AUDIO_CODEC_BYTES));
        /*
         * -1 means "not attempted", and event=audio-channel prints it as
         * src=0xFFFFFFFF. It is not a firmware error code: at 44.1 kHz the
         * standard channel is the preferred output and sceAudioSRCChReserve
         * is never called. A device log reading kind=standard
         * standard=0x00000007 src=0xFFFFFFFF describes a successful reserve
         * of channel 7, and has already been misread once as an SRC failure.
         */
        int standard_status = -1;
        int src_status = -1;
        if (psp_media_audio_prefer_standard_channel(
                backend->audio.sample_rate)) {
            native_stage = "sceAudioChReserve";
            standard_status = sceAudioChReserve(
                PSP_AUDIO_NEXT_CHANNEL, PSP_MEDIA_AUDIO_SAMPLES,
                PSP_AUDIO_FORMAT_STEREO);
            if (standard_status >= 0) {
                status = standard_status;
                backend->audio_channel = standard_status;
            }
        }
        if (backend->audio_channel == -1) {
            native_stage = "sceAudioSRCChReserve";
            src_status = sceAudioSRCChReserve(
                PSP_MEDIA_AUDIO_SAMPLES, backend->audio.sample_rate, 2);
            status = src_status;
            if (status < 0) {
                psp_media_log(
                    "tilefinch-media-decoder: event=audio-channel-failure "
                    "rate=%u standard=0x%08X src=0x%08X",
                    backend->audio.sample_rate,
                    (unsigned) standard_status, (unsigned) src_status);
                goto native_failure;
            }
            backend->audio_channel = -2;
        }
        psp_media_log(
            "tilefinch-media-decoder: event=audio-channel rate=%u "
            "kind=%s standard=0x%08X src=0x%08X channel=%d",
            backend->audio.sample_rate,
            backend->audio_channel == -2 ? "src" : "standard",
            (unsigned) standard_status, (unsigned) src_status,
            backend->audio_channel);
        native_stage = "sceKernelCreateEventFlag-audio";
        backend->audio_event = sceKernelCreateEventFlag(
            "tilefinch_media_audio", 0, 0, NULL);
        if (backend->audio_event < 0) {
            backend->audio_thread = backend->audio_event;
        } else {
            native_stage = "sceKernelCreateThread-audio";
            backend->audio_thread = sceKernelCreateThread(
                "tilefinch_media_audio", psp_media_audio_thread,
                TILEFINCH_PSP_THREAD_PRIORITY_AUDIO, 16u * 1024u,
                PSP_THREAD_ATTR_USER, NULL);
        }
        PspMediaBackend *thread_backend = backend;
        if (backend->audio_thread < 0) {
            status = backend->audio_thread;
        } else {
            native_stage = "sceKernelStartThread-audio";
            status = sceKernelStartThread(
                backend->audio_thread, sizeof(thread_backend),
                &thread_backend);
            if (status < 0) {
                sceKernelDeleteThread(backend->audio_thread);
                backend->audio_thread = -1;
            }
        }
        if (status < 0) goto native_failure;
    }
    psp_media_surface_handshake_reset();
    native_stage = "sceKernelCreateEventFlag-codec";
    backend->codec_event = sceKernelCreateEventFlag(
        "tilefinch_media_codec", 0, 0, NULL);
    if (backend->codec_event < 0) {
        status = backend->codec_event;
        goto native_failure;
    }
    native_stage = "sceKernelCreateThread-codec";
    backend->codec_thread = sceKernelCreateThread(
        "tilefinch_media_codec", psp_media_codec_thread,
        TILEFINCH_PSP_THREAD_PRIORITY_CODEC, 32u * 1024u,
        PSP_THREAD_ATTR_USER | PSP_THREAD_ATTR_VFPU, NULL);
    if (backend->codec_thread < 0) {
        status = backend->codec_thread;
        goto native_failure;
    }
    PspMediaBackend *codec_thread_backend = backend;
    native_stage = "sceKernelStartThread-codec";
    status = sceKernelStartThread(
        backend->codec_thread, sizeof(codec_thread_backend),
        &codec_thread_backend);
    if (status < 0) {
        sceKernelDeleteThread(backend->codec_thread);
        backend->codec_thread = -1;
        goto native_failure;
    }
    psp_media_log(
        "tilefinch-media-decoder: module=mpeg_vsh mode=%d me-type=%d "
        "me-nid=0x%08X decode-width=%u source=%ux%u csc-stride=%u "
        "rows=%u surface=%zuB audio=%d/%uHz/%uch/%us output=%s/%d "
        "dma-aligned=%d "
        "ddr-aligned=%d packet-cap=%zuB sps=%dB pps=%dB\n",
        backend->mpeg_mode, decoder_policy.me_boot_type,
        (unsigned) me_boot_nid, PSP_MEDIA_MPEG_DECODE_WIDTH,
        backend->decoded_width, backend->decoded_height,
        backend->frame_stride,
        backend->surface_rows, backend->surface_bytes,
        backend->have_audio ? 1 : 0,
        backend->have_audio ? (unsigned) aac_info.sample_rate : 0u,
        backend->have_audio ? (unsigned) aac_info.channels : 0u,
        backend->have_audio ? (unsigned) aac_info.samples_per_frame : 0u,
        !backend->have_audio ? "none"
            : backend->audio_channel == -2 ? "src" : "standard",
        backend->audio_channel,
        ((uintptr_t) backend->mpeg_memory % 64u) == 0
            && ((uintptr_t) backend->video_au % 64u) == 0
            && ((uintptr_t) backend->video_parameter_sets % 64u) == 0
            && ((uintptr_t) backend->packet_staging % 64u) == 0
            && ((uintptr_t) backend->surfaces[0] % 64u) == 0
            && ((uintptr_t) backend->surfaces[1] % 64u) == 0
            && (!backend->have_audio
                || (((uintptr_t) backend->audio_codec % 64u) == 0
                    && ((uintptr_t) backend->audio_pcm % 64u) == 0
                    && ((uintptr_t) backend->audio_queue % 64u) == 0)),
        ((uintptr_t) backend->ddr_memory
             % PSP_MEDIA_DDR_ALIGNMENT) == 0,
        backend->packet_staging_bytes,
        backend->video_sps_bytes, backend->video_pps_bytes);
    *backend_out = (MediaBackend) {
        .opaque = backend,
        .submit = psp_media_submit,
        .drain = psp_media_drain,
        .advance = psp_media_advance,
        .take_video_frame = psp_media_take_frame,
        .discard_video_before = psp_media_discard_video_before,
        .stats = psp_media_stats,
        .audio_cursor_us = psp_media_audio_cursor_us,
        .ready_video_frames = psp_media_ready_video_frames,
        .ready_video_start_us = psp_media_ready_video_start_us,
        .displayed_video_frames = psp_media_displayed_video_frames,
        .set_presentation_clock_us =
            psp_media_set_presentation_clock_us,
        .set_playing = psp_media_set_playing,
        .set_buffering = psp_media_set_buffering,
        .reset = psp_media_reset,
        .destroy = psp_media_destroy,
        .emit_pending_video = psp_media_emit_pending_video,
        .release_video_slot = psp_media_release_video_slot,
        .wants_paired_submit = psp_media_wants_paired_submit
    };
    return true;

native_failure:
    backend->stats.last_native_error = status;
    psp_media_log_failure(
        backend, native_stage, status);
    psp_media_error(error, error_size,
                    "PSP codec %s failed: 0x%08X",
                    native_stage, (unsigned) status);
    psp_media_destroy(backend);
    return false;
}

bool media_psp_backend_create(
    Budget *budget, const MediaMp4Demux *demux,
    MediaBackend *backend, char *error, size_t error_size)
{
    return media_psp_backend_create_split(
        budget, demux, NULL, backend, error, error_size);
}

#ifdef TILEFINCH_PSP_VALIDATION_LOG
int media_psp_backend_csc_order_probe(
    void *opaque, int mode0, int mode1,
    uint32_t *head, size_t head_count)
{
    PspMediaBackend *backend = opaque;
    if (backend == NULL || !backend->csc_probe_request_valid
        || backend->surfaces[0] == NULL
        || head_count > backend->decoded_width) return -1;
    /* The surface is the codec worker's while a job owns it. A probe runs
       after playback has stopped, so this refuses rather than racing. */
    if (atomic_load_explicit(
            &backend->codec_job_state, memory_order_acquire)
        == PSP_MEDIA_CODEC_JOB_RUNNING) return -1;
    PspAvcCsc csc = backend->csc_probe_request;
    csc.mode0 = (SceInt32) mode0;
    csc.mode1 = (SceInt32) mode1;
    /*
     * Byte for byte the shipping conversion's cache handshake
     * (psp_media_convert_avc_picture): write back anything the CPU still holds
     * over the destination, publish the descriptor the Media Engine reads by
     * DMA, and after a successful write invalidate the destination so the
     * reads below see RAM rather than a stale line. The pure invalidate is
     * legitimate for the same reason it is there: the engine is the only
     * writer of this range, so it discards nothing.
     */
    sceKernelDcacheWritebackInvalidateRange(
        backend->surfaces[0], backend->surface_bytes);
    sceKernelDcacheWritebackRange(&csc, sizeof(csc));
    int status = sceMpegBaseCscAvc(
        backend->surfaces[0], NULL, (int) backend->frame_stride, &csc);
    if (status < 0) return status;
    sceKernelDcacheInvalidateRange(
        backend->surfaces[0], backend->surface_bytes);
    if (head != NULL) {
        for (size_t at = 0; at < head_count; at++)
            head[at] = backend->surfaces[0][at];
    }
    return status;
}
#endif

#endif
