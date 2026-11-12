#include "tilefinch/media_backend.h"

#include <pspaudio.h>
#include <pspkernel.h>
#include <pspkerror.h>

#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef TILEFINCH_PSP_VALIDATION_LOG
#include <malloc.h>
#endif

#include "media_backend_psp_policy.h"
#include "psp_thread_contract.h"
#include "swdec/swdec_audio_fence.h"
#include "swdec/swdec_audio_clock.h"
#ifdef TILEFINCH_PSP_VALIDATION_LOG
#include "swdec/swdec_audio_diag.h"
#include "swdec/swdec_audio_gap.h"
#include "swdec/swdec_hold_trace.h"
#include "swdec/swdec_window.h"
#include "tilefinch/psp_swdec_component.h"
#endif
#include "swdec/swdec_bounds.h"
#include "swdec/swdec_grow.h"
#include "swdec/swdec_job_ring.h"
#include "swdec/swdec_publish.h"
#include "swdec/swdec_reset_gate.h"
#include "tilefinch/psp_threads.h"
#include "tilefinch/psp_log.h"

#ifdef TILEFINCH_PSP_VALIDATION_LOG
/* The cumulative figures the steady window reports as differences
   (swdec_window_take), taken by the presenter thread when the window opens
   and closes. Each is written by one thread; 64-bit ones another thread
   writes are read until two reads agree. */
typedef struct {
    uint64_t at_us, audio_cursor_us, buffering_us, decode_us, csc_us;
    uint64_t job_gap_us, audio_gap_us;
    uint64_t cpu_main_us, cpu_worker_us, cpu_audio_us;
    uint32_t submitted, decoded, displayed, repeats, dropped;
    uint32_t submit_drops, skip_drops, full_drops, worker_late, csc_failures;
    uint32_t reduced, queue_full, audio_blocks, underruns, underruns_queued;
    uint32_t sent_errors, buffering_events, job_gaps, resets, audio_gaps;
    uint32_t publishes, no_picture, ready;
} SwdecWindowSnap;
#endif

#define SWDEC_RING_SLOTS TILEFINCH_SWDEC_CSC_SLOT_COUNT
/* Grown packets and queues stop at what the player can hand over. */
_Static_assert(SWDEC_SAMPLE_MAX_BYTES == PSP_MEDIA_MAXIMUM_PACKET_BYTES,
               "swdec sample bound must match the player's packet bound");
#define SWDEC_AU_MAP_SLOTS 64u
#define SWDEC_AUDIO_PACKET_SLOTS 48u
/* About 460 ms of decoded audio before the clock starts. */
#define SWDEC_AUDIO_PREBUFFER_FRAMES 20u
/* Consecutive pictures the colour conversion may refuse before playback
   fails: about a second of video. */
#define SWDEC_CSC_FAILURE_LIMIT 30u
/* worker_error value for an AAC frame the output cannot play as decoded:
   more than 1024 samples a channel, which is implicit SBR (HE-AAC), whose
   configuration does not announce it. */
#define SWDEC_AUDIO_ERROR_FRAME_SHAPE (-0x5342)
#define SWDEC_TRACE_AUDIO_BLOCKS 48u
/* Video access units held between the demuxer and the decoder. A unit that
   must wait for the decoder no longer holds back the audio interleaved behind
   it in the file. Bytes are sized by swdec_au_queue_plan, entries bounded by
   this count. */
#define SWDEC_AU_QUEUE_ENTRIES 64u
/* When the oldest queued unit is this far behind the audio clock and a
   keyframe is queued, skip to it rather than decode a backlog of late
   pictures. */
#define SWDEC_SKIP_BEHIND_US UINT64_C(400000)
/* The audio decoder's arena, used by the Media Engine: its 512 KiB PCM ring
   and FFmpeg's aac_fixed decoder. The decoder takes about 115 KB to open
   but about 786 KB once it has decoded a frame (output configuration,
   channel elements, transform tables); measured with this FFmpeg build on
   the host (64-bit, so an upper bound), 1,328,568 bytes with the ring. An
   832 KiB arena, sized from the open alone, failed every frame with ENOMEM
   and played silence. It comes from the reserved Media Engine pool when
   there is room (beside a 416x234 decoder arena there is), else from a heap
   block the Media Engine can reach. */
#define SWDEC_AUX_ARENA_BYTES (1408u * 1024u)
/* Everything the Media Engine reads or writes must sit below 0x0A000000
   (swdec_me_address_visible): it cannot reach the extra bank
   (media_backend_psp_pool.h). The arenas and audio packets come from the
   reserved pool or a low heap block, or the open is refused; picture slots
   prefer them too, and one left above is converted on the main CPU by the
   component. */
#define SWDEC_WORKER_STACK_BYTES (512u * 1024u)
#define SWDEC_AUDIO_STACK_BYTES (16u * 1024u)
#define SWDEC_EVENT_WAKE 1u
/* Budget left over after the optional second packet buffer. */
#define SWDEC_PREFETCH_HEADROOM_BYTES ((size_t) 512u * 1024u)
/* How long a pending seek reset may wait for in-flight decodes, borrowed
   surfaces, the audio output and the Media Engine's audio queue before
   playback fails. */
#define SWDEC_RESET_DEADLINE_US 2000000u
/* How long close waits for the decode worker before quarantining the
   software route for the process. It must outlast the worker's longest
   legitimate job: the component gives up on a stalled Media Engine slice
   after 2 s, tested once every 1024 polls of 100 us sleeps, so a job can
   return 2.1 s or more after its slice began. A shorter wait turned a
   close during such a stall into a process-long refusal of the route
   (MB-05). */
#define SWDEC_WORKER_STOP_WAIT_US 3000000u
#define SWDEC_MAX_WIDTH 432u
#define SWDEC_MAX_HEIGHT 240u
#define SWDEC_PARAMETER_SET_SLACK 1024u
#define SWDEC_CACHE_LINE_BYTES 64u

typedef enum {
    SWDEC_SLOT_FREE = 0,
    SWDEC_SLOT_WRITING,
    SWDEC_SLOT_READY,
    SWDEC_SLOT_READING
} SwdecSlotState;

typedef struct {
    atomic_int state;
    atomic_uint readers;
    atomic_bool retire_when_unborrowed;
    uint32_t generation;
    uint64_t identity;
    uint64_t pts_us;
    uint64_t duration_us;
    bool claimed;
    bool quarantined;
#ifdef TILEFINCH_PSP_VALIDATION_LOG
    uint32_t trace_ready_us;                 /* worker: when published */
    /* The job whose decode call returned this picture (with B-frames,
       usually a later unit than the picture's own): when that decode
       ended, and its unit's pts. */
    uint32_t trace_output_us;
    uint64_t trace_output_pts_us;
#endif
} SwdecVideoSlot;

typedef struct {
    uint64_t au;
    uint64_t pts_us;
    uint64_t duration_us;
} SwdecAuSlot;

/* One access unit handed to the worker (see swdec_job_ring.h). The browser
   writes the request fields before publishing; the worker writes the result
   fields before finishing. */
typedef struct {
    unsigned char *packet;
    /* Grown by the browser while the job is idle (swdec_job_packet_fit). */
    size_t packet_capacity;
    size_t bytes;
    int slot;
    uint64_t au;
    uint64_t pts_us;
    uint64_t duration_us;
    bool flush;
    bool may_drop_late;      /* non-reference, with audio as the clock */
    bool csc_prebound;       /* worker: conversion bound before decoding */
    bool dropped_late;       /* result: skipped at start, already late */
    bool csc_failed;         /* result: the conversion refused it */
    int result;
    int error;
#ifdef TILEFINCH_PSP_VALIDATION_LOG
    uint32_t trace_start_us, trace_end_us;   /* worker: the decode call */
    uint32_t trace_cpu_us, trace_browser_us, trace_clock_bracket_us;
#endif
} SwdecJob;

typedef struct {
    Budget *budget;
    const TilefinchSwdecComponentApi *api;
    void *decoder;
    void *decoder_arena;
    /* The arena is the reserved Media Engine pool's when it fits there
       (media_psp_me_pool_claim), charged through this reservation. */
    bool decoder_arena_pooled;
    BudgetReservation decoder_arena_reservation;
    size_t decoder_arena_bytes;
    void *aux_arena;
    size_t aux_arena_bytes;
    bool aux_arena_pooled;
    BudgetReservation aux_arena_reservation;
    /* One block per slot: after a page has run, the heap rarely has the
       ring's 5 MB in one piece. */
    uint16_t *rgb_slots[SWDEC_RING_SLOTS];
    size_t rgb_slot_bytes;
    size_t rgb_ring_bytes;
    unsigned rgb_stride;
    unsigned width;
    unsigned height;
    unsigned nal_length_size;
    /* Held pictures the decoder arena is sized for (swdec_arena_refs). */
    int arena_refs;
    /* The stream never reorders pictures (Baseline, or a VUI declaring no
       reordering), so the picture a job decodes is the one it outputs: its
       colour conversion is bound before the decode and the Media Engine
       converts finished rows while the CPU still reconstructs. */
    bool csc_prebind;
    unsigned char *parameter_sets;
    size_t parameter_sets_bytes;
    unsigned char *audio_packets;
    /* Audio decode results, kept by the audio thread (swdec_bounds.h). Once
       `audio_failed` is set the track is given up: packets are no longer
       submitted and the clock runs on silence while video plays. A seek
       gives the track a bounded number of fresh chances
       (swdec_audio_errors_seek). */
    SwdecAudioErrors audio_errors;
    atomic_bool audio_failed;
    /* audio_origin_us holds the first audio packet's time since the last
       seek (set by the browser even for a given-up track). */
    atomic_bool audio_origin_ready;
#ifdef TILEFINCH_PSP_VALIDATION_LOG
    /* Browser thread: the first packet the decoder failed on, and what
       each packet slot holds (swdec_audio_diag.h). */
    SwdecAudioBadPacket trace_audio_bad;
    SwdecAudioPacketMeta trace_audio_meta[SWDEC_AUDIO_PACKET_SLOTS];
    unsigned trace_audio_checked;
#endif
    bool audio_packets_pooled;
    BudgetReservation audio_packets_reservation;
    /* Ring slots taken from the reserved pool, charged together. */
    bool rgb_slot_pooled[SWDEC_RING_SLOTS];
    /* Slots placed (the rest stay NULL; see swdec_take_ring_slots). */
    unsigned rgb_slot_count;
    BudgetReservation rgb_pool_reservation;
    size_t audio_packet_stride;
    MediaAacStreamInfo audio_info;
    bool have_audio;
    bool audio_arena_bound;

    SceUID worker_event;
    SceUID ready_event;
    SceUID worker_thread;
    SceUID audio_thread;
    bool worker_started;
    bool audio_started;
    /* The output channel the audio thread reserved: -1 none, -2 the SRC
       channel, else a standard channel. Written by that thread; released by
       destroy once the thread has ended (MB-03). */
    atomic_int audio_channel;
    atomic_bool stop;
    atomic_bool playing;
    atomic_bool buffering;
    /* Every source has ended (drain was called): the audio thread starts
       with whatever is decoded, even under the prebuffer. */
    atomic_bool audio_input_ended;
    atomic_uint ready_count;
    /* Distinct pictures presented (a picture presented again, a redraw,
       is not counted; see swdec_note_displayed). */
    atomic_uint displayed_count;
    uint64_t last_displayed_identity;   /* presenter thread only */
    uint32_t displayed_repeats;         /* presenter thread only */
#ifdef TILEFINCH_PSP_VALIDATION_LOG
    SwdecWindow window;                 /* presenter thread only */
    SwdecWindowSnap window_open;
    uint32_t trace_resets;
#endif
    atomic_uint audio_submitted;
    atomic_uint audio_output;
    atomic_uint audio_epoch;
    /* Written by the audio thread (and by a seek reset while the fence
       holds that thread parked); read by the browser. */
    SwdecPublishedU64 audio_cursor_us;
    SwdecAudioClock audio_dac_clock;
#ifdef TILEFINCH_PSP_VALIDATION_LOG
    unsigned trace_dac_reads, trace_dac_errors, trace_dac_max_samples;
#endif
    /* Written by the browser; read by the audio thread. */
    SwdecPublishedU64 audio_origin_us;
    SwdecPublishedU64 shown_end_us;
    /* The PCM ring's owner while a seek replaces it (swdec_audio_fence.h). */
    SwdecAudioFence audio_fence;
    /* A seek's reset, completed on later pumps (swdec_reset_gate.h). */
    SwdecResetGate reset_gate;
    unsigned csc_failures_in_row;        /* worker thread only */
    bool reset_failed;           /* a reset stopped half way: play nothing */

    /* Video access units waiting for the decoder (browser thread only). */
    struct {
        size_t offset;
        size_t length;
        uint64_t pts_us;
        uint64_t duration_us;
        bool keyframe;
        bool annex_b;
        bool non_reference;
    } au_queue[SWDEC_AU_QUEUE_ENTRIES];
    unsigned au_head;
    unsigned au_count;
    size_t au_write;
    unsigned char *au_bytes;
    size_t au_bytes_capacity;
    bool au_skipping;
    bool au_skip_incoming;   /* drop arriving units until a keyframe */

    /* Two jobs when the second packet buffer fits, so the worker can start
       the next unit without waiting for the browser loop; one otherwise. */
    SwdecJobRing ring;
    SwdecJob jobs[SWDEC_JOB_RING_MAX];
    uint64_t last_job_pts_us;
    uint64_t last_job_duration_us;
    /* The clock the dispatcher's late test uses, published for the worker's
       recheck of a unit queued ahead. */
    SwdecPublishedU64 dispatch_clock_us;
    atomic_int worker_error;     /* the audio worker's channel failure */
    /* The decode worker's failed event wait: the worker has ended, so a
       reset never clears it. */
    atomic_int worker_wait_error;
    bool drained;

    SwdecVideoSlot slots[SWDEC_RING_SLOTS];
    SwdecAuSlot au_slots[SWDEC_AU_MAP_SLOTS];
    int last_claimed_slot;
    uint64_t next_au;
    uint64_t next_identity;
    uint64_t epoch;
    uint64_t presentation_clock_us;
    uint64_t clock_slip_us;
    int speed;
    MediaBackendStats stats;
    BudgetReservation worker_stack_reservation;
#ifdef TILEFINCH_PSP_VALIDATION_LOG
    /* Page-video device pass: software-decoder speed. Decode figures are
       written by the worker only; the lateness figures by the browser
       thread's take only. Read once both threads have stopped. */
    uint32_t trace_decode_jobs;
    SceUID trace_browser_thread;
    /* Decode jobs that gave no picture. Like the worker-late and
       conversion drops below, counted by the browser thread when it
       collects the job, so a window snapshot taken between two of the
       worker's increments cannot split one job across two counters. */
    uint32_t trace_no_picture;
    /* Decode results: a picture, none yet, an error (and the first one). */
    uint32_t trace_decode_pictures;
    uint32_t trace_csc_prebound;
    uint32_t trace_prebind_jobs;
    uint64_t trace_audio_next_pts_us;
    uint32_t trace_audio_pts_jumps;
    uint32_t trace_prebind_mismatch;
    uint32_t trace_prebind_close_zero;
    uint32_t trace_decode_empty;
    uint32_t trace_decode_errors;
    int trace_decode_first_error;
    uint64_t trace_decode_total_us;
    uint32_t trace_decode_max_us;
    /* The worker's idle time between finishing one job and starting the
       next (hand-off through the browser thread), and the time spent
       converting a decoded picture for the presenter (csc_picture). */
    uint64_t trace_job_last_end_us;
    uint32_t trace_job_chained;          /* started straight after another */
    uint32_t trace_worker_late_drops;    /* dropped by the worker's recheck */
    uint32_t trace_csc_failures;         /* pictures the conversion refused */
    /* (both counted at collect, on the browser thread) */
    uint32_t trace_me_recoveries;        /* Media Engine restarts */
    uint64_t trace_job_gap_total_us;
    uint32_t trace_job_gaps;
    uint32_t trace_job_gap_max_us;
    uint64_t trace_publish_total_us;
    uint32_t trace_publish_count;
    uint32_t trace_reduced_decodes;
    uint32_t trace_speed_changes;
    uint32_t trace_late_reanchors;
    uint64_t trace_late_max_us;
    /* Video units dropped at submit (late and non-reference), and the audio
       thread's output: blocks played, silence blocks while running and
       started (underruns), and the first blocks' ME CRC, sample count and
       peak, compared offline with a host decode of the same stream. */
    uint32_t trace_submit_drops;
    uint32_t trace_skip_drops;
    uint32_t trace_queue_full;
    uint32_t trace_queue_peak;
    uint64_t trace_created_us;
    uint64_t trace_audio_started_us;      /* first real audio block played */
    uint64_t trace_audio_last_us;         /* the latest real block returned */
    /* Holes between consecutive output returns while playing. */
    SwdecAudioGapStats trace_gap;
    uint32_t trace_audio_paused_blocks;   /* silence while started but paused */
    /* Whether the per-block PCM scans below ran (validation_swdec_audio_scan);
       off, they cost nothing on the audio thread. */
    bool trace_audio_scan;
    /* What was actually handed to the output call, for every block: blocks
       whose samples are all (near) zero, the output call's failures, and the
       loudest sample of each second of sound. */
    uint32_t trace_sent_silent;
    uint32_t trace_sent_errors;
    int trace_sent_first_error;
    uint16_t trace_sent_second_peak[64];
    unsigned trace_sent_seconds;
    uint64_t trace_buffering_since_us;
    uint64_t trace_buffering_total_us;
    uint32_t trace_buffering_events;
    uint32_t trace_full_drops;
    uint32_t trace_audio_blocks;
    uint32_t trace_audio_underruns;
    uint32_t trace_audio_underruns_queued;   /* packets waiting on the ME */
    unsigned trace_audio_first;
    struct {
        uint32_t crc;
        uint16_t samples;
        uint16_t peak;
    } trace_audio[SWDEC_TRACE_AUDIO_BLOCKS];
#endif
} PspSwdecBackend;

#ifdef TILEFINCH_PSP_VALIDATION_LOG
/* The window's per-picture hold records (swdec_hold_trace.h), browser
   thread only; one video plays at a time. */
static SwdecHoldTrace swdec_holds;
static uint32_t swdec_hold_last_take_us;
static uint32_t swdec_hold_take_gap_us;
static uint32_t swdec_presenter_wait_total, swdec_take_wait_at;
static uint32_t swdec_hold_gap_wait_us;
static uint32_t swdec_hold_repeat_us;
void media_psp_swdec_note_presenter_wait(uint32_t waited_us)
{
    swdec_presenter_wait_total += waited_us;
}
static uint64_t swdec_thread_run_us(SceUID thread);

static uint32_t swdec_hold_cpu_delta(uint64_t start, uint64_t end)
{
    return start != 0 && end >= start && end - start < UINT32_MAX
        ? (uint32_t) (end - start) : UINT32_MAX;
}

static uint32_t swdec_hold_now(void)
{
    return (uint32_t) sceKernelGetSystemTimeWide();
}

static void swdec_hold_fate(uint64_t pts_us, SwdecHoldFate fate)
{
    SwdecHoldPic *picture = swdec_hold_find(&swdec_holds, pts_us);
    if (picture != NULL) picture->fate = (uint8_t) fate;
}

/* A picture's readiness, and the decode that produced it: ready minus
   output is the conversion and publication of that same picture, while
   output minus the picture's own decode end is the decoder's reordering. */
static void swdec_hold_output(SwdecHoldPic *picture,
                              const SwdecVideoSlot *slot)
{
    picture->ready_us = slot->trace_ready_us;
    picture->output_us = slot->trace_output_us;
    int64_t by = ((int64_t) slot->trace_output_pts_us
                  - (int64_t) picture->pts_us) / 1000;
    picture->output_by_ms = by > INT16_MAX ? INT16_MAX
        : by < INT16_MIN ? INT16_MIN : (int16_t) by;
}
#endif

static bool swdec_quarantined;
/* What the last video open asked the Budget for (see
   media_psp_swdec_backend_last_working_set). */
static size_t swdec_last_working_set_bytes;
#ifdef TILEFINCH_PSP_VALIDATION_LOG
/* validation_swdec_audio_scan, published at boot before any video opens. */
static bool swdec_audio_scan_enabled;
#endif

#ifdef TILEFINCH_PSP_VALIDATION_LOG
static bool swdec_full_quality;
void media_psp_swdec_set_full_quality(bool enabled)
{
    swdec_full_quality = enabled;
}
static bool swdec_keep_high_slots;
static bool swdec_fixed_nine_slots;
void media_psp_swdec_set_fixed_nine_slots(bool enabled)
{
    swdec_fixed_nine_slots = enabled;
}
void media_psp_swdec_set_keep_high_slots(bool enabled)
{
    swdec_keep_high_slots = enabled;
}
static bool swdec_drop_ladder;
void media_psp_swdec_set_drop_ladder(bool enabled)
{
    swdec_drop_ladder = enabled;
}
static bool swdec_legacy_deadline;
static bool swdec_legacy_audio_clock;
static bool swdec_legacy_due_wait;
void media_psp_swdec_set_legacy_due_wait(bool enabled)
{
    swdec_legacy_due_wait = enabled;
}
bool media_psp_swdec_legacy_due_wait(void)
{
    return swdec_legacy_due_wait;
}
void media_psp_swdec_set_legacy_audio_clock(bool enabled)
{
    swdec_legacy_audio_clock = enabled;
}
bool media_psp_swdec_legacy_audio_clock(void)
{
    return swdec_legacy_audio_clock;
}
void media_psp_swdec_set_legacy_deadline(bool enabled)
{
    swdec_legacy_deadline = enabled;
}
#endif

static bool swdec_drop_late(uint64_t pts_us, uint64_t duration_us,
                            uint64_t clock_us)
{
#ifdef TILEFINCH_PSP_VALIDATION_LOG
    if (swdec_legacy_deadline)
        return pts_us <= clock_us || pts_us - clock_us <= duration_us;
#else
    (void) duration_us;
#endif
    return swdec_au_late(pts_us, clock_us);
}

void media_psp_swdec_set_audio_scan(bool enabled)
{
#ifdef TILEFINCH_PSP_VALIDATION_LOG
    swdec_audio_scan_enabled = enabled;
#else
    (void) enabled;
#endif
}

static size_t swdec_cache_extent(size_t bytes)
{
    return (bytes + SWDEC_CACHE_LINE_BYTES - 1u)
        & ~(size_t) (SWDEC_CACHE_LINE_BYTES - 1u);
}

static bool swdec_me_visible(const void *pointer, size_t bytes)
{
    return swdec_me_address_visible((uintptr_t) pointer, bytes);
}

/* One block from the reserved Media Engine pool, charged through
   `reservation`; NULL when the pool has no room or the Budget refuses. */
static void *swdec_take_pool_block(Budget *budget, size_t bytes,
                                   BudgetReservation *reservation)
{
    if (media_psp_me_pool_claimable() < bytes
        || !budget_reservation_acquire(
               reservation, budget, BUDGET_CATEGORY_RESOURCE, bytes))
        return NULL;
    void *block = media_psp_me_pool_claim(bytes);
    if (block == NULL) budget_reservation_release(reservation);
    return block;
}

typedef struct {
    Budget *budget;
    BudgetCategory category;
} SwdecHeapContext;

static void *swdec_heap_allocate(void *context, size_t bytes)
{
    SwdecHeapContext *heap = context;
    return budget_malloc_cacheline_category(heap->budget, heap->category,
                                            bytes);
}

static void swdec_heap_release(void *context, void *block)
{
    budget_free(((SwdecHeapContext *) context)->budget, block);
}

/* A heap block the Media Engine can reach, or NULL (logged) when the heap
   has none: the arenas and audio packets must never be placed where the ME
   would read or write another bank. */
static void *swdec_take_low_heap_block(Budget *budget, BudgetCategory category,
                                       size_t bytes, const char *what)
{
    SwdecHeapContext heap = {budget, category};
    unsigned held = 0;
    void *block = swdec_alloc_me_visible(
        swdec_heap_allocate, swdec_heap_release, &heap, bytes,
        SWDEC_LOW_SEARCH_HELD, false, &held);
#ifdef TILEFINCH_PSP_VALIDATION_LOG
    if (block == NULL || held != 0)
        psp_log_printf("tilefinch-swdec: event=low-heap what=%s bytes=%u "
                       "found=%d passed-over=%u\n",
                       what, (unsigned) bytes, block != NULL ? 1 : 0, held);
#else
    (void) what;
#endif
    return block;
}

/* The audio arena: a pool block when there is room, else a heap block the
   Media Engine can reach. */
static bool swdec_take_aux_arena(PspSwdecBackend *backend, Budget *budget)
{
    backend->aux_arena = swdec_take_pool_block(
        budget, SWDEC_AUX_ARENA_BYTES, &backend->aux_arena_reservation);
    backend->aux_arena_pooled = backend->aux_arena != NULL;
    if (backend->aux_arena == NULL)
        backend->aux_arena = swdec_take_low_heap_block(
            budget, BUDGET_CATEGORY_RESOURCE, SWDEC_AUX_ARENA_BYTES,
            "aux-arena");
    backend->aux_arena_bytes = backend->aux_arena != NULL
        ? SWDEC_AUX_ARENA_BYTES : 0;
    return backend->aux_arena != NULL;
}

/* The ME reads compressed audio from these packets. */
static bool swdec_take_audio_packets(PspSwdecBackend *backend, Budget *budget)
{
    size_t bytes = backend->audio_packet_stride * SWDEC_AUDIO_PACKET_SLOTS;
    backend->audio_packets = swdec_take_pool_block(
        budget, bytes, &backend->audio_packets_reservation);
    if (backend->audio_packets != NULL) {
        backend->audio_packets_pooled = true;
        return true;
    }
    backend->audio_packets = swdec_take_low_heap_block(
        budget, BUDGET_CATEGORY_RESOURCE, bytes, "audio-packets");
    return backend->audio_packets != NULL;
}

/* Picture slots: as many as fit in the pool's remaining room, charged
   exactly, then the heap, preferring blocks the Media Engine can reach.
   A slot the Media Engine cannot reach (above 0x0A000000) is converted on
   the main CPU by the component and then scaled by the main CPU too (the
   graphics engine cannot read it either), so once the low blocks run out
   the ring stops there if it already holds SWDEC_LOW_SLOT_FLOOR slots: a
   deep ring only runs further ahead of the clock. A NULL slot is never
   handed out. Below the floor, high slots fill the ring as before. */
#define SWDEC_LOW_SLOT_FLOOR 8u
/* Pictures the presenter can still hold: the one shown and one the
   graphics engine is drawing. */
#define SWDEC_PRESENTER_HELD_SLOTS 2u
static bool swdec_take_ring_slots(PspSwdecBackend *backend, Budget *budget)
{
    size_t bytes = backend->rgb_slot_bytes;
    unsigned pooled = swdec_pool_slot_count(
        media_psp_me_pool_claimable(), bytes, SWDEC_RING_SLOTS);
    if (pooled != 0
        && budget_reservation_acquire(
               &backend->rgb_pool_reservation, budget,
               BUDGET_CATEGORY_RENDER, bytes * pooled)) {
        unsigned claimed = 0;
        for (; claimed < pooled; claimed++) {
            backend->rgb_slots[claimed] = media_psp_me_pool_claim(bytes);
            if (backend->rgb_slots[claimed] == NULL) break;
            backend->rgb_slot_pooled[claimed] = true;
        }
        if (claimed != pooled) {
            /* Charge exactly what the pool gave. */
            budget_reservation_release(&backend->rgb_pool_reservation);
            if (claimed != 0
                && !budget_reservation_acquire(
                       &backend->rgb_pool_reservation, budget,
                       BUDGET_CATEGORY_RENDER, bytes * claimed)) {
                for (unsigned i = 0; i < claimed; i++) {
                    media_psp_me_pool_unclaim(backend->rgb_slots[i]);
                    backend->rgb_slots[i] = NULL;
                    backend->rgb_slot_pooled[i] = false;
                }
            }
        }
    }
    /* One search for the whole ring: the high blocks it passes over stay
       held until every slot is placed, so each slot does not walk past
       them again. */
    SwdecHeapContext heap = {budget, BUDGET_CATEGORY_RENDER};
    SwdecLowSearch search;
    swdec_low_search_begin(&search, SWDEC_LOW_SEARCH_HELD);
    bool accept_high = false;
#ifdef TILEFINCH_PSP_VALIDATION_LOG
    accept_high = swdec_keep_high_slots;
#endif
    unsigned placed = 0;
    bool complete = true;
    for (unsigned i = 0; i < SWDEC_RING_SLOTS; i++) {
        if (backend->rgb_slots[i] != NULL) {
            placed++;
            continue;
        }
        backend->rgb_slots[i] = swdec_low_search_take(
            &search, swdec_heap_allocate, swdec_heap_release, &heap, bytes,
            accept_high);
        if (backend->rgb_slots[i] == NULL && !accept_high) {
            if (placed >= SWDEC_LOW_SLOT_FLOOR) break;
            accept_high = true;
            backend->rgb_slots[i] = swdec_low_search_take(
                &search, swdec_heap_allocate, swdec_heap_release, &heap,
                bytes, true);
        }
        if (backend->rgb_slots[i] == NULL) {
            complete = false;
            break;
        }
        placed++;
    }
    swdec_low_search_end(&search, swdec_heap_release, &heap);
    backend->rgb_slot_count = placed;
#ifdef TILEFINCH_PSP_VALIDATION_LOG
    /* Preserve normal admission/recovery and all allocation ownership.
       Only the producer's usable reservoir is smaller in this comparison. */
    backend->rgb_slot_count = swdec_picture_slot_limit(placed, swdec_fixed_nine_slots);
#endif
    return complete;
}

#ifdef TILEFINCH_PSP_VALIDATION_LOG
/* The largest block malloc can hand out now, to 64 KiB, probed by halving
   the step (validation only: about forty trial allocations). */
static size_t swdec_trace_largest_block(void)
{
    /* Through a volatile pointer: GCC folds a malloc whose block is only
       freed, which made every probe "succeed". */
    void *(*volatile allocate)(size_t) = malloc;
    size_t low = 0, step = (size_t) 32u * 1024u * 1024u;
    for (; step >= 64u * 1024u; step /= 2u) {
        void *volatile probe = allocate(low + step);
        if (probe != NULL) {
            free(probe);
            low += step;
        }
    }
    return low;
}
#endif

static uint16_t *swdec_rgb_slot(PspSwdecBackend *backend, int slot)
{
    return backend->rgb_slots[slot];
}

bool media_psp_swdec_backend_quarantined(void)
{
    return swdec_quarantined;
}

static void swdec_error(
    char *error, size_t error_size, const char *format, ...)
{
    if (error == NULL || error_size == 0) return;
    va_list arguments;
    va_start(arguments, format);
    vsnprintf(error, error_size, format, arguments);
    va_end(arguments);
}

static uint64_t swdec_sample_time_us(const MediaMp4Sample *sample)
{
    if (sample == NULL || sample->timescale == 0) return 0;
    uint64_t value = sample->pts >= 0
        ? (uint64_t) sample->pts : sample->dts;
    return media_ticks_to_us(value, sample->timescale);
}

static uint64_t swdec_sample_duration_us(const MediaMp4Sample *sample)
{
    if (sample == NULL || sample->timescale == 0) return 0;
    if (sample->timescale == 90000u
        && sample->duration <= UINT32_MAX / 100u)
        return (uint32_t) (sample->duration * 100u / 9u);
    return (uint64_t) sample->duration * UINT64_C(1000000)
        / sample->timescale;
}

static bool swdec_append_start_code(
    unsigned char *output, size_t capacity, size_t *used,
    const unsigned char *nal, size_t bytes)
{
    if (output == NULL || used == NULL || nal == NULL
        || *used > capacity || bytes > capacity - *used
        || capacity - *used - bytes < 4u) return false;
    output[(*used)++] = 0;
    output[(*used)++] = 0;
    output[(*used)++] = 0;
    output[(*used)++] = 1;
    memcpy(output + *used, nal, bytes);
    *used += bytes;
    return true;
}

static bool swdec_build_parameter_sets(
    const unsigned char *config, size_t length,
    unsigned char *output, size_t capacity, size_t *written,
    unsigned *nal_length_size)
{
    if (written != NULL) *written = 0;
    if (nal_length_size != NULL) *nal_length_size = 0;
    if (config == NULL || length < 7u || output == NULL || written == NULL
        || nal_length_size == NULL || config[0] != 1u) return false;
    unsigned nls = (config[4] & 3u) + 1u;
    if (nls < 1u || nls > 4u) return false;
    size_t at = 6u;
    unsigned sps_count = config[5] & 31u;
    if (sps_count == 0) return false;
    for (unsigned i = 0; i < sps_count; i++) {
        if (at > length || length - at < 2u) return false;
        size_t bytes = ((size_t) config[at] << 8) | config[at + 1u];
        at += 2u;
        if (bytes == 0 || bytes > length - at
            || !swdec_append_start_code(
                   output, capacity, written, config + at, bytes)) return false;
        at += bytes;
    }
    if (at >= length) return false;
    unsigned pps_count = config[at++];
    if (pps_count == 0) return false;
    for (unsigned i = 0; i < pps_count; i++) {
        if (at > length || length - at < 2u) return false;
        size_t bytes = ((size_t) config[at] << 8) | config[at + 1u];
        at += 2u;
        if (bytes == 0 || bytes > length - at
            || !swdec_append_start_code(
                   output, capacity, written, config + at, bytes)) return false;
        at += bytes;
    }
    *nal_length_size = nls;
    return true;
}

static bool swdec_convert_avcc(
    PspSwdecBackend *backend, unsigned char *packet, size_t capacity,
    const unsigned char *payload, size_t length,
    bool keyframe, size_t *written)
{
    if (written != NULL) *written = 0;
    if (backend == NULL || packet == NULL || payload == NULL
        || written == NULL) return false;
    size_t used = 0;
    if (keyframe && backend->parameter_sets_bytes != 0) {
        if (backend->parameter_sets_bytes > capacity)
            return false;
        memcpy(packet, backend->parameter_sets,
               backend->parameter_sets_bytes);
        used = backend->parameter_sets_bytes;
    }
    size_t at = 0;
    while (at < length) {
        if (backend->nal_length_size > length - at) return false;
        size_t bytes = 0;
        for (unsigned i = 0; i < backend->nal_length_size; i++)
            bytes = (bytes << 8) | payload[at + i];
        at += backend->nal_length_size;
        if (bytes == 0 || bytes > length - at
            || !swdec_append_start_code(
                   packet, capacity, &used, payload + at, bytes))
            return false;
        at += bytes;
    }
    *written = used;
    return true;
}

static bool swdec_annexb_has_nal(
    const unsigned char *bytes, size_t length, unsigned wanted)
{
    if (bytes == NULL) return false;
    for (size_t i = 0; i + 4u < length; i++) {
        size_t prefix = bytes[i] == 0 && bytes[i + 1u] == 0
            && bytes[i + 2u] == 1u ? 3u
            : bytes[i] == 0 && bytes[i + 1u] == 0
              && bytes[i + 2u] == 0 && bytes[i + 3u] == 1u ? 4u : 0u;
        if (prefix != 0 && (bytes[i + prefix] & 0x1fu) == wanted)
            return true;
    }
    return false;
}

static int swdec_find_free_slot(PspSwdecBackend *backend)
{
    unsigned limit = SWDEC_RING_SLOTS;
#ifdef TILEFINCH_PSP_VALIDATION_LOG
    limit = backend->rgb_slot_count;
#endif
    for (unsigned i = 0; i < limit; i++) {
        int expected = SWDEC_SLOT_FREE;
        if (backend->rgb_slots[i] != NULL && !backend->slots[i].quarantined
            && atomic_compare_exchange_strong_explicit(
                &backend->slots[i].state, &expected, SWDEC_SLOT_WRITING,
                memory_order_acq_rel, memory_order_acquire)) return (int) i;
    }
    return -1;
}

static void swdec_free_slot(PspSwdecBackend *backend, int slot)
{
    if (backend == NULL || slot < 0 || slot >= (int) SWDEC_RING_SLOTS)
        return;
    SwdecVideoSlot *state = &backend->slots[slot];
    state->claimed = false;
    atomic_store_explicit(
        &state->retire_when_unborrowed, false, memory_order_release);
    if (!state->quarantined) {
        atomic_store_explicit(
            &state->state, SWDEC_SLOT_FREE, memory_order_release);
    }
}

static void swdec_publish_picture(
    PspSwdecBackend *backend, SwdecJob *job,
    const TilefinchSwdecPicture *picture)
{
    uint64_t source_au = picture->pts;
    SwdecAuSlot *mapping = &backend->au_slots[source_au & 63u];
    if (mapping->au != source_au || job->slot < 0
        || job->slot >= (int) SWDEC_RING_SLOTS) {
        /* Take a bound conversion back from the Media Engine before the
           slot is free for another picture. */
        if (job->csc_prebound) backend->api->csc_off();
        if (job->slot >= 0 && job->slot < (int) SWDEC_RING_SLOTS)
            swdec_free_slot(backend, job->slot);
        job->error = -20;
        return;
    }
    int slot = job->slot;
    if (picture->width != (int) backend->width
        || picture->height != (int) backend->height
        || !swdec_rgb565_destination_fits(
            picture->width, picture->height, (int) backend->rgb_stride,
            backend->rgb_slot_bytes)) {
        backend->api->csc_off();
        swdec_free_slot(backend, slot);
        job->error = -23;
        return;
    }
    uint64_t pts_us = mapping->pts_us;
    uint64_t duration_us = mapping->duration_us;
#ifdef TILEFINCH_PSP_VALIDATION_LOG
    uint64_t trace_publish_started_us = sceKernelGetSystemTimeWide();
#endif
    /* A conversion bound before the decode is this picture's when the
       picture is the job's own access unit: finish it (the rows the Media
       Engine has not converted yet). Otherwise, or if it held nothing,
       convert the picture now. */
    int converted = 0;
    if (job->csc_prebound) {
        if (source_au == job->au) {
            converted = backend->api->csc_close();
#ifdef TILEFINCH_PSP_VALIDATION_LOG
            if (converted) backend->trace_csc_prebound++;
            else backend->trace_prebind_close_zero++;
#endif
        } else {
            backend->api->csc_off();
#ifdef TILEFINCH_PSP_VALIDATION_LOG
            backend->trace_prebind_mismatch++;
#endif
        }
    }
    if (!converted)
        converted = backend->api->csc_picture(
            slot, swdec_rgb_slot(backend, slot),
            (int) backend->rgb_stride, backend->rgb_slot_bytes, picture);
#ifdef TILEFINCH_PSP_VALIDATION_LOG
    backend->trace_publish_total_us +=
        sceKernelGetSystemTimeWide() - trace_publish_started_us;
    backend->trace_publish_count++;
#endif
    if (!converted) {
        /* One picture the colour conversion refused is a dropped frame,
           not the end of playback; only a run of them is an error. */
        swdec_free_slot(backend, slot);
        job->csc_failed = true;
        if (++backend->csc_failures_in_row >= SWDEC_CSC_FAILURE_LIMIT)
            job->error = -21;
        return;
    }
    backend->csc_failures_in_row = 0;
    /* The ME and the bounded CPU tail have both written this isolated
       surface and published their writes before csc_close returns. Discard
       any stale CPU cache lines before the browser stages the frame. */
    sceKernelDcacheInvalidateRange(
        swdec_rgb_slot(backend, slot), (unsigned) backend->rgb_slot_bytes);
    SwdecVideoSlot *state = &backend->slots[slot];
    state->generation++;
    if (state->generation == 0) state->generation = 1;
    state->identity = ++backend->next_identity;
    state->pts_us = pts_us;
    state->duration_us = duration_us;
    state->claimed = false;
#ifdef TILEFINCH_PSP_VALIDATION_LOG
    state->trace_ready_us = (uint32_t) sceKernelGetSystemTimeWide();
    state->trace_output_us = job->trace_end_us;
    state->trace_output_pts_us = job->pts_us;
#endif
    atomic_store_explicit(
        &state->retire_when_unborrowed, false, memory_order_release);
    atomic_fetch_add_explicit(
        &backend->ready_count, 1u, memory_order_relaxed);
    atomic_store_explicit(
        &state->state, SWDEC_SLOT_READY, memory_order_release);
    backend->stats.decoded_video_frames++;
    /* Last publication step: a waiting presenter may preempt this worker.
       No output pixels, metadata, or counters may remain half-published. */
    if (backend->ready_event >= 0)
        (void) sceKernelSetEventFlag(backend->ready_event, 1u);
}

static void swdec_select_speed(PspSwdecBackend *backend)
{
    unsigned ready = atomic_load_explicit(
        &backend->ready_count, memory_order_relaxed);
    unsigned displayed = atomic_load_explicit(
        &backend->displayed_count, memory_order_relaxed);
    int speed = backend->speed;
    if (displayed == 0) speed = 0;
#ifdef TILEFINCH_PSP_VALIDATION_LOG
    else if (swdec_full_quality) speed = 0;
#endif
    else {
        speed = swdec_speed_for_ready(
            speed, ready, backend->rgb_slot_count, SWDEC_JOB_RING_MAX,
            SWDEC_PRESENTER_HELD_SLOTS);
#ifdef TILEFINCH_PSP_VALIDATION_LOG
        if (!swdec_drop_ladder)
#endif
            speed = swdec_rung_decoder_speed(speed);
    }
    if (speed != backend->speed) {
        backend->api->set_speed(backend->decoder, speed);
        backend->speed = speed;
#ifdef TILEFINCH_PSP_VALIDATION_LOG
        backend->trace_speed_changes++;
#endif
    }
}

static void swdec_decode_job(PspSwdecBackend *backend, SwdecJob *job)
{
    if (job->slot >= 0) {
        SwdecAuSlot *mapping = &backend->au_slots[job->au & 63u];
        mapping->au = job->au;
        mapping->pts_us = job->pts_us;
        mapping->duration_us = job->duration_us;
    }
    swdec_select_speed(backend);
    job->csc_prebound = backend->csc_prebind && !job->flush
        && job->slot >= 0 && job->slot < (int) SWDEC_RING_SLOTS;
#ifdef TILEFINCH_PSP_VALIDATION_LOG
    if (job->csc_prebound) backend->trace_prebind_jobs++;
#endif
    if (job->csc_prebound)
        backend->api->csc_begin(
            job->slot, swdec_rgb_slot(backend, job->slot),
            (int) backend->rgb_stride, backend->rgb_slot_bytes);
    TilefinchSwdecPicture picture = {0};
#ifdef TILEFINCH_PSP_VALIDATION_LOG
    if (!job->flush && backend->trace_decode_jobs < 3u
        && job->packet != NULL) {
        /* The NAL unit types of the first access units the decoder gets
           (Annex-B by now), e.g. "9,7,8,6,5". */
        char types[96];
        size_t used = 0;
        types[0] = '\0';
        for (size_t i = 0; i + 3u < job->bytes && used + 4u < sizeof(types);
             i++) {
            if (job->packet[i] == 0 && job->packet[i + 1u] == 0
                && job->packet[i + 2u] == 1u) {
                int written = snprintf(types + used, sizeof(types) - used,
                                       "%s%u", used == 0 ? "" : ",",
                                       (unsigned) (job->packet[i + 3u] & 0x1fu));
                if (written > 0) used += (size_t) written;
                i += 2u;
            }
        }
        psp_log_printf("tilefinch-swdec: event=decoder-au n=%u bytes=%u "
                       "nal-types=%s\n",
                       (unsigned) backend->trace_decode_jobs,
                       (unsigned) job->bytes, used == 0 ? "none" : types);
    }
    uint64_t trace_bracket_started_us = sceKernelGetSystemTimeWide();
    uint64_t trace_cpu_started_us = swdec_thread_run_us(backend->worker_thread);
    uint64_t trace_browser_started_us =
        swdec_thread_run_us(backend->trace_browser_thread);
    uint64_t trace_started_us = sceKernelGetSystemTimeWide();
#endif
    int result = backend->api->decode(
        backend->decoder,
        job->flush ? NULL : job->packet,
        job->flush ? 0 : job->bytes,
        job->au, &picture);
#ifdef TILEFINCH_PSP_VALIDATION_LOG
    uint64_t trace_ended_us = sceKernelGetSystemTimeWide();
    uint64_t trace_cpu_ended_us = swdec_thread_run_us(backend->worker_thread);
    uint64_t trace_browser_ended_us =
        swdec_thread_run_us(backend->trace_browser_thread);
    job->trace_clock_bracket_us = (uint32_t) (
        trace_started_us - trace_bracket_started_us
        + sceKernelGetSystemTimeWide() - trace_ended_us);
    job->trace_cpu_us = swdec_hold_cpu_delta(trace_cpu_started_us, trace_cpu_ended_us);
    job->trace_browser_us = swdec_hold_cpu_delta(
        trace_browser_started_us, trace_browser_ended_us);
    uint64_t trace_elapsed_us = trace_ended_us - trace_started_us;
    job->trace_start_us = (uint32_t) trace_started_us;
    job->trace_end_us = (uint32_t) (trace_started_us + trace_elapsed_us);
    backend->trace_decode_jobs++;
    backend->trace_decode_total_us += trace_elapsed_us;
    if (trace_elapsed_us > backend->trace_decode_max_us)
        backend->trace_decode_max_us = trace_elapsed_us > UINT32_MAX
            ? UINT32_MAX : (uint32_t) trace_elapsed_us;
    if (backend->speed != 0) backend->trace_reduced_decodes++;
    if (result == 1) backend->trace_decode_pictures++;
    else if (result == 0) backend->trace_decode_empty++;
    else if (backend->trace_decode_errors++ == 0)
        backend->trace_decode_first_error = result;
#endif
    job->result = result;
    if (result == 1) swdec_publish_picture(backend, job, &picture);
    else {
        if (job->csc_prebound) backend->api->csc_off();
        if (job->slot >= 0) swdec_free_slot(backend, job->slot);
        if (result < 0) job->error = result;
    }
    if (job->flush && result == 0) backend->drained = true;
}

/* A non-reference unit queued ahead may have become late while the previous
   unit decoded. Drop it now, as the dispatcher would have, rather than spend
   a decode on a picture the presenter will discard. */
static bool swdec_job_dropped_late(PspSwdecBackend *backend, SwdecJob *job)
{
    if (!job->may_drop_late) return false;
    /* Without a reading (the browser kept publishing), decode it. */
    uint64_t clock_us = swdec_published_read(&backend->dispatch_clock_us, 0);
    if (!swdec_drop_late(job->pts_us, job->duration_us, clock_us)) return false;
    if (job->slot >= 0) swdec_free_slot(backend, job->slot);
    job->result = 0;
    job->dropped_late = true;
    return true;
}

static int swdec_worker_main(SceSize arguments, void *argument)
{
    (void) arguments;
    PspSwdecBackend *backend = *(PspSwdecBackend **) argument;
    while (!atomic_load_explicit(&backend->stop, memory_order_acquire)) {
        u32 bits = 0;
        int waited = sceKernelWaitEventFlag(
            backend->worker_event, SWDEC_EVENT_WAKE,
            PSP_EVENT_WAITOR | PSP_EVENT_WAITCLEAR, &bits, NULL);
        if (waited < 0) {
            /* A failing wait returns at once: looping would spin this
               thread, which outranks the browser. Report it (the next pump
               fails playback, as the firmware workers do) and end; close
               then finds the thread already stopped. */
            atomic_store_explicit(
                &backend->worker_wait_error, waited, memory_order_release);
            break;
        }
        /* Run every published job in order, then sleep. A job the browser
           queued while the previous one decoded starts at once. */
#ifdef TILEFINCH_PSP_VALIDATION_LOG
        bool chained = false;
#endif
        while (!atomic_load_explicit(&backend->stop, memory_order_acquire)) {
            int index = swdec_job_ring_begin(&backend->ring);
            if (index < 0) break;
            SwdecJob *job = &backend->jobs[index];
            if (swdec_job_dropped_late(backend, job)) {
                swdec_job_ring_finish(&backend->ring, index);
                continue;
            }
#ifdef TILEFINCH_PSP_VALIDATION_LOG
            uint64_t trace_job_started_us = sceKernelGetSystemTimeWide();
            if (chained) backend->trace_job_chained++;
            if (backend->trace_job_last_end_us != 0) {
                uint64_t gap_us =
                    trace_job_started_us - backend->trace_job_last_end_us;
                backend->trace_job_gap_total_us += gap_us;
                backend->trace_job_gaps++;
                if (gap_us > backend->trace_job_gap_max_us)
                    backend->trace_job_gap_max_us = gap_us > UINT32_MAX
                        ? UINT32_MAX : (uint32_t) gap_us;
            }
#endif
            swdec_decode_job(backend, job);
#ifdef TILEFINCH_PSP_VALIDATION_LOG
            backend->trace_job_last_end_us = sceKernelGetSystemTimeWide();
#endif
#ifdef TILEFINCH_PSP_VALIDATION_LOG
            chained = true;
#endif
            swdec_job_ring_finish(&backend->ring, index);
        }
    }
    return 0;
}

#ifdef TILEFINCH_PSP_VALIDATION_LOG
/* Validation, with validation_swdec_audio_scan only: the loudest sample of a
   2048-sample stereo block. Off by default, so timing runs do not pay for
   a full pass over every block on the audio thread. */
static unsigned swdec_trace_block_peak(const short *pcm)
{
    unsigned peak = 0;
    for (unsigned i = 0; i < 2048u; i++) {
        int v = pcm[i] < 0 ? -(int) pcm[i] : pcm[i];
        if ((unsigned) v > peak) peak = (unsigned) v;
    }
    return peak;
}
#endif

/* Audio thread: an output call failed. Report it to the browser (the next
   collect fails playback) and wait out one block: a failing call returns at
   once, and this thread outranks the browser, so retrying immediately would
   spin and starve it. */
static void swdec_audio_output_failed(
    PspSwdecBackend *backend, int result, uint32_t block_us)
{
    int expected = 0;
    (void) atomic_compare_exchange_strong_explicit(
        &backend->worker_error, &expected, result,
        memory_order_acq_rel, memory_order_acquire);
    (void) sceKernelDelayThread(block_us);
}

static void swdec_audio_publish_cursor(PspSwdecBackend *backend,
    uint64_t cursor, int channel, bool use_src, int output_result)
{
    /* SRC has no documented per-channel remaining-sample query. Keep its
       block clock rather than guessing from an unrelated output channel. */
    int remaining = !use_src && output_result >= 0
        ? sceAudioGetChannelRestLen(channel) : -1;
    uint64_t now = sceKernelGetSystemTimeWide();
    uint64_t origin = swdec_published_read(&backend->audio_origin_us, cursor);
    uint32_t pending = swdec_audio_clock_pending(remaining,
        backend->audio_info.sample_rate, cursor > origin ? cursor - origin : 0);
    swdec_audio_clock_write(&backend->audio_dac_clock, cursor, now, pending);
    swdec_published_write(&backend->audio_cursor_us, cursor);
#ifdef TILEFINCH_PSP_VALIDATION_LOG
    backend->trace_dac_reads++;
    if (remaining < 0 || remaining > 2048) backend->trace_dac_errors++;
    else if ((unsigned) remaining > backend->trace_dac_max_samples)
        backend->trace_dac_max_samples = (unsigned) remaining;
#endif
}

static int swdec_audio_main(SceSize arguments, void *argument)
{
    (void) arguments;
    PspSwdecBackend *backend = *(PspSwdecBackend **) argument;
    static short silence[2048];
    /* The standard channels play at 44.1 kHz only. Any other rate needs the
       process-wide SRC channel, as on the firmware decoder path; otherwise a
       48 kHz track plays 8% slow and low. */
    bool use_src = !psp_media_audio_prefer_standard_channel(
        backend->audio_info.sample_rate);
    int channel = use_src
        ? sceAudioSRCChReserve(1024, (int) backend->audio_info.sample_rate, 2)
        : sceAudioChReserve(
              PSP_AUDIO_NEXT_CHANNEL, 1024, PSP_AUDIO_FORMAT_STEREO);
    if (channel < 0) {
        atomic_store_explicit(
            &backend->worker_error, channel, memory_order_release);
        swdec_audio_fence_set_thread(&backend->audio_fence, false);
        return 0;
    }
    atomic_store_explicit(&backend->audio_channel, use_src ? -2 : channel,
                          memory_order_release);
    unsigned output = 0;
    unsigned audio_epoch = atomic_load_explicit(
        &backend->audio_epoch, memory_order_acquire);
    bool started = false;
    uint64_t cursor = 0;
    uint64_t shown_end = 0;
    uint32_t numerator = UINT32_C(1024000000);
    uint32_t step = numerator / backend->audio_info.sample_rate;
    uint32_t remainder_step = numerator % backend->audio_info.sample_rate;
    uint32_t remainder = 0;
    while (!atomic_load_explicit(&backend->stop, memory_order_acquire)) {
        unsigned current_epoch = atomic_load_explicit(
            &backend->audio_epoch, memory_order_acquire);
        if (current_epoch != audio_epoch) {
            audio_epoch = current_epoch;
            output = 0;
            started = false;
            cursor = 0;
            shown_end = 0;
            remainder = 0;
        }
        /* While a seek replaces the PCM ring, take nothing from it: play
           silence, and acknowledge once that silence has returned (the
           hardware then holds no ring block). */
        unsigned hold = swdec_audio_fence_hold(&backend->audio_fence);
        unsigned done = backend->api->audio_done();
        bool running = atomic_load_explicit(
            &backend->playing, memory_order_acquire)
            && !atomic_load_explicit(
                &backend->buffering, memory_order_acquire);
        /* Never waits for the browser: a publication it interrupted leaves
           the previous value readable (swdec_publish.h). */
        shown_end = swdec_published_read(&backend->shown_end_us, shown_end);
        bool audio_only = backend->decoder == NULL;
        /* At the end of the stream (a short clip, or a seek into its last
           half second) there may never be a whole prebuffer: start with
           what the ME has decoded once it has decoded everything. */
        bool prebuffered = swdec_audio_prebuffered(
            done,
            atomic_load_explicit(
                &backend->audio_submitted, memory_order_acquire),
            atomic_load_explicit(
                &backend->audio_input_ended, memory_order_acquire),
            SWDEC_AUDIO_PREBUFFER_FRAMES);
        if (hold == 0u && !started && prebuffered
            && (audio_only || shown_end != 0)) {
            cursor = swdec_published_read(&backend->audio_origin_us, 0);
            started = true;
        }
        /* Audio is the clock and never waits for video: when decoding
           falls behind, the presenter drops late frames instead. */
        bool refused_shape = false;
        if (hold == 0u && running && started && done > output
            && backend->api->audio_poll != NULL) {
            /* samples holds the decoder's error code (negative) for a frame
               that failed; the ME wrote silence for it, which plays. */
            unsigned block_crc = 0, block_samples = 1024u;
            (void) backend->api->audio_poll(
                output, &block_crc, &block_samples);
            if (swdec_audio_errors_note(&backend->audio_errors,
                                        block_samples)) {
                atomic_store_explicit(&backend->audio_failed, true,
                                      memory_order_release);
#ifdef TILEFINCH_PSP_VALIDATION_LOG
                psp_log_printf("tilefinch-swdec-audio: event=failed "
                               "errors=%u first-error=%d decoded=%u "
                               "(video continues on a silent clock)\n",
                               backend->audio_errors.errors,
                               backend->audio_errors.first_error,
                               backend->audio_errors.decoded);
#endif
            }
            refused_shape = swdec_audio_block_refused(block_samples);
            if (refused_shape) {
                /* Implicit SBR: the ME produced twice the samples the
                   output plays per block. Refuse it rather than play
                   half of every frame. */
                int expected = 0;
                (void) atomic_compare_exchange_strong_explicit(
                    &backend->worker_error, &expected,
                    SWDEC_AUDIO_ERROR_FRAME_SHAPE,
                    memory_order_acq_rel, memory_order_acquire);
            }
        }
        if (hold == 0u && running
            && atomic_load_explicit(&backend->audio_failed,
                                    memory_order_acquire)) {
            /* The track was given up: keep the clock on silence. After a
               seek it starts from the new position, not from zero. */
            if (!started) {
                uint64_t origin = 0;
                if (swdec_silent_clock_start(
                        atomic_load_explicit(&backend->audio_origin_ready,
                                             memory_order_acquire),
                        swdec_published_read(&backend->audio_origin_us, 0),
                        atomic_load_explicit(&backend->audio_input_ended,
                                             memory_order_acquire),
                        shown_end, audio_only, &origin)) {
                    cursor = origin;
                    started = true;
                }
            }
            int silence_result = use_src
                ? sceAudioSRCOutputBlocking(PSP_AUDIO_VOLUME_MAX, silence)
                : sceAudioOutputBlocking(
                      channel, PSP_AUDIO_VOLUME_MAX, silence);
            if (silence_result < 0)
                swdec_audio_output_failed(backend, silence_result, step);
            if (!started) continue;
            cursor += step;
            remainder += remainder_step;
            if (remainder >= backend->audio_info.sample_rate) {
                cursor++;
                remainder -= backend->audio_info.sample_rate;
            }
            swdec_audio_publish_cursor(backend, cursor, channel, use_src,
                                      silence_result);
            continue;
        }
        if (hold == 0u && running && started && done > output
            && !refused_shape) {
#ifdef TILEFINCH_PSP_VALIDATION_LOG
            uint64_t block_started_us = sceKernelGetSystemTimeWide();
            if (backend->trace_audio_blocks == 0)
                backend->trace_audio_started_us = block_started_us;
            backend->trace_audio_blocks++;
            if (output < SWDEC_TRACE_AUDIO_BLOCKS
                && backend->api->audio_poll != NULL) {
                unsigned crc = 0, samples = 0;
                (void) backend->api->audio_poll(output, &crc, &samples);
                backend->trace_audio[output].crc = crc;
                backend->trace_audio[output].samples = (uint16_t) samples;
                backend->trace_audio[output].peak = backend->trace_audio_scan
                    ? (uint16_t) swdec_trace_block_peak(
                          backend->api->audio_pcm(output))
                    : 0u;
                if (output + 1u > backend->trace_audio_first)
                    backend->trace_audio_first = output + 1u;
            }
#endif
            /* The component hands out the uncached alias the ME wrote
               through. Give the output call the ordinary cached alias of the
               same memory, as every other PSP audio client does, after
               dropping any stale CPU lines over the block. */
            void *pcm = (void *) ((uintptr_t) backend->api->audio_pcm(output)
                                  & ~(uintptr_t) 0x40000000u);
            sceKernelDcacheInvalidateRange(pcm, 2048u * sizeof(short));
#ifdef TILEFINCH_PSP_VALIDATION_LOG
            if (backend->trace_audio_scan) {
                unsigned sent_peak = swdec_trace_block_peak(pcm);
                if (sent_peak < 4u) backend->trace_sent_silent++;
                unsigned second = (unsigned) (((uint64_t) output * 1024u)
                    / backend->audio_info.sample_rate);
                if (second < 64u) {
                    if (second + 1u > backend->trace_sent_seconds)
                        backend->trace_sent_seconds = second + 1u;
                    if (sent_peak > backend->trace_sent_second_peak[second])
                        backend->trace_sent_second_peak[second] =
                            (uint16_t) (sent_peak > 65535u ? 65535u : sent_peak);
                }
            }
#endif
            int sent_result = use_src
                ? sceAudioSRCOutputBlocking(PSP_AUDIO_VOLUME_MAX, pcm)
                : sceAudioOutputBlocking(channel, PSP_AUDIO_VOLUME_MAX, pcm);
            if (sent_result < 0)
                swdec_audio_output_failed(backend, sent_result, step);
#ifdef TILEFINCH_PSP_VALIDATION_LOG
            if (sent_result < 0) {
                if (backend->trace_sent_errors++ == 0)
                    backend->trace_sent_first_error = sent_result;
            }
            backend->trace_audio_last_us = sceKernelGetSystemTimeWide();
            swdec_audio_gap_note_return(
                &backend->trace_gap, backend->trace_audio_last_us, step);
#else
            (void) sent_result;
#endif
            output++;
            cursor += step;
            remainder += remainder_step;
            if (remainder >= backend->audio_info.sample_rate) {
                cursor++;
                remainder -= backend->audio_info.sample_rate;
            }
            atomic_store_explicit(
                &backend->audio_output, output, memory_order_release);
            swdec_audio_publish_cursor(backend, cursor, channel, use_src,
                                      sent_result);
        } else {
#ifdef TILEFINCH_PSP_VALIDATION_LOG
            bool underrun = hold == 0u && running && started;
            if (hold == 0u && started && !running)
                backend->trace_audio_paused_blocks++;
            if (underrun) {
                backend->trace_audio_underruns++;
                /* Packets submitted but not yet decoded: the ME is behind.
                   None queued: the demuxer did not deliver audio in time. */
                if (atomic_load_explicit(&backend->audio_submitted,
                                         memory_order_acquire) > done)
                    backend->trace_audio_underruns_queued++;
            }
#endif
            int silence_result = use_src
                ? sceAudioSRCOutputBlocking(PSP_AUDIO_VOLUME_MAX, silence)
                : sceAudioOutputBlocking(
                      channel, PSP_AUDIO_VOLUME_MAX, silence);
            /* Only an output call that succeeded proves the hardware let go
               of the block before it. A failed one leaves the fence up (the
               pending seek then times out and reports) and reports the
               channel failure. */
            if (silence_result >= 0) {
                swdec_audio_fence_acknowledge(&backend->audio_fence, hold);
            } else {
                swdec_audio_output_failed(backend, silence_result, step);
            }
#ifdef TILEFINCH_PSP_VALIDATION_LOG
            /* An underrun inside a playing run is a hole the gap figures
               include; a pause, rebuffer or seek is a deliberate silence. */
            if (underrun)
                swdec_audio_gap_note_return(
                    &backend->trace_gap, sceKernelGetSystemTimeWide(), step);
            else
                swdec_audio_gap_break(&backend->trace_gap);
#endif
        }
    }
    /* The channel is released by destroy, after this thread has ended: the
       release straight after a blocking output can report busy, and has to
       be retried rather than dropped (MB-03). */
    swdec_audio_fence_set_thread(&backend->audio_fence, false);
    return 0;
}

static int swdec_audio_channel_release_once(void *context)
{
    int channel = *(const int *) context;
    return channel == -2 ? sceAudioSRCChRelease()
                         : sceAudioChRelease(channel);
}

static uint32_t swdec_audio_release_now_us(void)
{
    return (uint32_t) sceKernelGetSystemTimeLow();
}

static int swdec_audio_release_delay_us(uint32_t us)
{
    return sceKernelDelayThread((SceUInt) us);
}

/* Destroy only, after the audio thread has ended. False when the channel
   is still held: the audio service may then still own the last PCM block,
   so the caller keeps everything. */
static bool swdec_release_audio_channel(PspSwdecBackend *backend)
{
    int channel = atomic_load_explicit(
        &backend->audio_channel, memory_order_acquire);
    if (channel == -1) return true;
    const PspMediaAudioReleaseOps ops = {
        swdec_audio_channel_release_once, &channel,
        swdec_audio_release_now_us, swdec_audio_release_delay_us
    };
    int status = 0;
    unsigned attempts = 0;
    uint32_t waited_us = 0;
    bool released = psp_media_audio_release_bounded(
        &ops, PSP_MEDIA_AUDIO_CHANNEL_DRAIN_WAIT_US,
        &status, &attempts, &waited_us);
    /* Only the abnormal releases: a busy answer, or none at all. */
    if (!released || attempts > 1u)
        psp_log_printf("tilefinch-swdec-audio: event=channel-release "
                       "channel=%d released=%d status=0x%08X attempts=%u "
                       "waited=%uus\n", channel, released ? 1 : 0,
                       (unsigned) status, attempts, (unsigned) waited_us);
    if (!released) return false;
    atomic_store_explicit(&backend->audio_channel, -1, memory_order_release);
    return true;
}

/* Browser thread: retire every finished job in order. WOULD_BLOCK while the
   oldest job is still queued or decoding, ACCEPTED when nothing is in
   flight, ERROR when a finished job failed. */
static MediaBackendResult swdec_collect(
    PspSwdecBackend *backend, char *error, size_t error_size)
{
    for (;;) {
        int index = swdec_job_ring_collect(&backend->ring);
        if (index == SWDEC_JOB_RING_EMPTY) return MEDIA_BACKEND_ACCEPTED;
        if (index == SWDEC_JOB_RING_BUSY) return MEDIA_BACKEND_WOULD_BLOCK;
        const SwdecJob *job = &backend->jobs[index];
#ifdef TILEFINCH_PSP_VALIDATION_LOG
        if (!job->flush) {
            SwdecHoldPic *picture = swdec_hold_find(&swdec_holds, job->pts_us);
            if (picture != NULL && job->dropped_late)
                picture->fate = SWDEC_HOLD_WORKER_LATE;
            else if (picture != NULL) {
                picture->decode_start_us = job->trace_start_us;
                picture->decode_end_us = job->trace_end_us;
                picture->decode_cpu_us = job->trace_cpu_us;
                picture->browser_cpu_us = job->trace_browser_us;
                picture->clock_bracket_us = job->trace_clock_bracket_us;
            }
        }
#endif
        /* The browser thread does all the drop accounting here, so the
           shared stats never take a racing increment from the worker and
           a window snapshot sees each job's counters change together. */
        if (job->dropped_late) {
            backend->stats.dropped_video_frames++;
#ifdef TILEFINCH_PSP_VALIDATION_LOG
            backend->trace_submit_drops++;
            backend->trace_worker_late_drops++;
#endif
            continue;
        }
        if (job->csc_failed) {
            backend->stats.dropped_video_frames++;
#ifdef TILEFINCH_PSP_VALIDATION_LOG
            backend->trace_csc_failures++;
#endif
        }
        int result = job->result;
        int worker_error = job->error != 0 ? job->error
            : atomic_load_explicit(
                  &backend->worker_error, memory_order_acquire);
        if (result < 0 || worker_error != 0) {
            backend->stats.last_native_error = worker_error != 0
                ? worker_error : result;
            swdec_error(error, error_size,
                        "software H.264 decode failed (%d/%d)",
                        result, worker_error);
            return MEDIA_BACKEND_ERROR;
        }
#ifdef TILEFINCH_PSP_VALIDATION_LOG
        if (!job->flush && result == 0) backend->trace_no_picture++;
#endif
    }
}

/* Browser thread: hand a prepared job to the worker. False (job withdrawn,
   its slot freed) when the worker cannot be woken. */
static bool swdec_publish_job(PspSwdecBackend *backend, int index)
{
    SwdecJob *job = &backend->jobs[index];
    job->dropped_late = false;
    job->csc_failed = false;
    job->result = 0;
    job->error = 0;
    backend->last_job_pts_us = job->pts_us;
    backend->last_job_duration_us = job->duration_us;
    swdec_job_ring_publish(&backend->ring, index);
    if (sceKernelSetEventFlag(backend->worker_event, SWDEC_EVENT_WAKE) < 0
        && swdec_job_ring_unpublish(&backend->ring, index)) {
        swdec_free_slot(backend, job->slot);
        return false;
    }
    return true;
}

static bool swdec_adts_header(
    const MediaAacStreamInfo *info, size_t payload_bytes,
    unsigned char output[7])
{
    static const unsigned rates[] = {
        96000, 88200, 64000, 48000, 44100, 32000, 24000,
        22050, 16000, 12000, 11025, 8000, 7350
    };
    unsigned rate_index = 13u;
    for (unsigned i = 0; i < sizeof(rates) / sizeof(rates[0]); i++)
        if (rates[i] == info->sample_rate) { rate_index = i; break; }
    size_t frame_bytes = payload_bytes + 7u;
    if (rate_index >= 13u || info->channels == 0 || info->channels > 2
        || frame_bytes > 0x1fffu) return false;
    unsigned channels = info->channels;
    output[0] = 0xffu;
    output[1] = 0xf1u;
    output[2] = (unsigned char) ((1u << 6) | (rate_index << 2)
                                 | (channels >> 2));
    output[3] = (unsigned char) (((channels & 3u) << 6)
                                 | (frame_bytes >> 11));
    output[4] = (unsigned char) (frame_bytes >> 3);
    output[5] = (unsigned char) ((frame_bytes << 5) | 0x1fu);
    output[6] = 0xfcu;
    return true;
}

static size_t swdec_au_head_offset(const PspSwdecBackend *backend)
{
    return backend->au_queue[backend->au_head].offset;
}

/* Copy a video sample into the byte ring. False when entries or bytes are
   exhausted; the sample then stays with the demuxer. */
static bool swdec_au_enqueue(
    PspSwdecBackend *backend, const MediaMp4Sample *sample,
    const unsigned char *payload, size_t length)
{
    if (backend->au_bytes == NULL || length == 0
        || length >= backend->au_bytes_capacity
        || backend->au_count >= SWDEC_AU_QUEUE_ENTRIES) return false;
    size_t capacity = backend->au_bytes_capacity;
    size_t at;
    if (backend->au_count == 0) {
        backend->au_write = 0;
        at = 0;
    } else {
        size_t read = swdec_au_head_offset(backend);
        size_t write = backend->au_write;
        if (write >= read) {
            if (length <= capacity - write) at = write;
            else if (length < read) at = 0;
            else return false;
        } else {
            if (length < read - write) at = write;
            else return false;
        }
    }
    memcpy(backend->au_bytes + at, payload, length);
    unsigned index = (backend->au_head + backend->au_count)
        % SWDEC_AU_QUEUE_ENTRIES;
    backend->au_queue[index].offset = at;
    backend->au_queue[index].length = length;
    backend->au_queue[index].pts_us = swdec_sample_time_us(sample);
    backend->au_queue[index].duration_us = swdec_sample_duration_us(sample);
    backend->au_queue[index].keyframe = sample->keyframe;
    backend->au_queue[index].annex_b =
        sample->packet_format == MEDIA_PACKET_FORMAT_H264_ANNEX_B;
    backend->au_queue[index].non_reference = swdec_au_is_non_reference(
        payload, length,
        backend->au_queue[index].annex_b ? 0u : backend->nal_length_size);
#ifdef TILEFINCH_PSP_VALIDATION_LOG
    {
        uint64_t pts_us = backend->au_queue[index].pts_us;
        SwdecHoldPic *picture = swdec_hold_add(&swdec_holds, pts_us);
        int64_t lead = (int64_t) pts_us
            - (int64_t) backend->presentation_clock_us;
        picture->bytes = (uint32_t) length;
        picture->lead_us = lead > INT32_MAX ? INT32_MAX
            : lead < INT32_MIN ? INT32_MIN : (int32_t) lead;
        picture->queued_us = swdec_hold_now();
        picture->depth = (uint8_t) backend->au_count;
        picture->keyframe = sample->keyframe;
        picture->reference = !backend->au_queue[index].non_reference;
        size_t slice_bytes = 0;
        const unsigned char *slice = swdec_au_first_slice(
            payload, length,
            backend->au_queue[index].annex_b ? 0u : backend->nal_length_size,
            &slice_bytes);
        if (slice != NULL)
            picture->slice = (int8_t) swdec_hold_slice_type(
                slice, slice_bytes);
    }
#endif
    backend->au_count++;
    backend->au_write = at + length;
#ifdef TILEFINCH_PSP_VALIDATION_LOG
    if (backend->au_count > backend->trace_queue_peak)
        backend->trace_queue_peak = backend->au_count;
#endif
    return true;
}

static void swdec_au_pop(PspSwdecBackend *backend)
{
    if (backend->au_count == 0) return;
    backend->au_head = (backend->au_head + 1u) % SWDEC_AU_QUEUE_ENTRIES;
    backend->au_count--;
    if (backend->au_count == 0) {
        backend->au_head = 0;
        backend->au_write = 0;
    }
}

static void swdec_au_clear(PspSwdecBackend *backend)
{
    backend->au_head = 0;
    backend->au_count = 0;
    backend->au_write = 0;
    backend->au_skipping = false;
    backend->au_skip_incoming = false;
}

/* Playback is under way: a picture has been shown and the clock runs,
   either because the audio has played a block or because the track was
   given up and the clock runs on silence (which never counts output). */
static bool swdec_playback_started(PspSwdecBackend *backend)
{
    return atomic_load_explicit(
               &backend->displayed_count, memory_order_relaxed) != 0
        && (atomic_load_explicit(
                &backend->audio_output, memory_order_acquire) != 0
            || atomic_load_explicit(
                &backend->audio_failed, memory_order_acquire));
}

static bool swdec_au_keyframe_queued_after_head(const PspSwdecBackend *backend)
{
    for (unsigned i = 1; i < backend->au_count; i++) {
        unsigned index = (backend->au_head + i) % SWDEC_AU_QUEUE_ENTRIES;
        if (backend->au_queue[index].keyframe) return true;
    }
    return false;
}

#ifdef TILEFINCH_PSP_VALIDATION_LOG
/* Why and where a skip to the next keyframe began (validation). */
static void swdec_trace_skip_start(PspSwdecBackend *backend,
                                   const char *reason)
{
    uint64_t head_pts = backend->au_count != 0
        ? backend->au_queue[backend->au_head].pts_us : 0;
    uint64_t keyframe_pts = 0;
    for (unsigned i = 1; i < backend->au_count; i++) {
        unsigned index = (backend->au_head + i) % SWDEC_AU_QUEUE_ENTRIES;
        if (backend->au_queue[index].keyframe) {
            keyframe_pts = backend->au_queue[index].pts_us;
            break;
        }
    }
    psp_log_printf("tilefinch-swdec-skip: event=start reason=%s head-pts=%llu "
                   "keyframe-pts=%llu clock=%llu queued=%u ready=%u "
                   "displayed=%u audio-cursor=%llu\n",
                   reason, (unsigned long long) head_pts,
                   (unsigned long long) keyframe_pts,
                   (unsigned long long) backend->presentation_clock_us,
                   backend->au_count,
                   atomic_load_explicit(&backend->ready_count,
                                        memory_order_relaxed),
                   atomic_load_explicit(&backend->displayed_count,
                                        memory_order_relaxed),
                   (unsigned long long) swdec_published_read(
                       &backend->audio_cursor_us, 0));
}
#endif

/* Remove one queued unit to make room for an arriving one, preferring the
   oldest non-reference picture; failing that, if a keyframe is queued after
   the head, the oldest unit before it (the dispatcher then skips to that
   keyframe). Returns false when neither exists: every queued unit is a
   reference that later ones depend on. */
static bool swdec_au_make_room(PspSwdecBackend *backend)
{
    for (unsigned i = 0; i < backend->au_count; i++) {
        unsigned index = (backend->au_head + i) % SWDEC_AU_QUEUE_ENTRIES;
        if (!backend->au_queue[index].non_reference) continue;
#ifdef TILEFINCH_PSP_VALIDATION_LOG
        swdec_hold_fate(backend->au_queue[index].pts_us, SWDEC_HOLD_FULL);
#endif
        /* Remove entry i by shifting the later entries down one place; their
           bytes stay where they are. */
        for (unsigned j = i; j + 1u < backend->au_count; j++) {
            unsigned to = (backend->au_head + j) % SWDEC_AU_QUEUE_ENTRIES;
            unsigned from = (backend->au_head + j + 1u)
                % SWDEC_AU_QUEUE_ENTRIES;
            backend->au_queue[to] = backend->au_queue[from];
        }
        backend->au_count--;
        if (backend->au_count == 0) {
            backend->au_head = 0;
            backend->au_write = 0;
        }
        backend->stats.dropped_video_frames++;
#ifdef TILEFINCH_PSP_VALIDATION_LOG
        backend->trace_full_drops++;
#endif
        return true;
    }
    if (backend->au_count != 0 && !backend->au_queue[backend->au_head].keyframe
        && swdec_au_keyframe_queued_after_head(backend)) {
#ifdef TILEFINCH_PSP_VALIDATION_LOG
        if (!backend->au_skipping)
            swdec_trace_skip_start(backend, "queue-full");
#endif
        backend->au_skipping = true;
#ifdef TILEFINCH_PSP_VALIDATION_LOG
        swdec_hold_fate(backend->au_queue[backend->au_head].pts_us,
                        SWDEC_HOLD_SKIPPED);
#endif
        swdec_au_pop(backend);
        backend->stats.dropped_video_frames++;
#ifdef TILEFINCH_PSP_VALIDATION_LOG
        backend->trace_skip_drops++;
#endif
        return true;
    }
    return false;
}

/* Browser thread, `job` idle: make its packet big enough for a
   `length`-byte unit once rewritten (swdec_packet_bytes). The packet was
   sized at open from the track's largest sample then, which a later
   fragment can exceed (SWDEC_SAMPLE_MAX_BYTES); it grows towards what the
   largest admitted sample needs (swdec_packet_grow: half again as big, or
   exactly what it needs when that is refused). False, with the packet
   unchanged, when the unit is past that bound or the Budget refuses even
   the exact size. */
static bool swdec_job_packet_fit(
    PspSwdecBackend *backend, SwdecJob *job, size_t length, bool annex_b,
    char *error, size_t error_size)
{
    unsigned nal_length_size = annex_b ? 0u : backend->nal_length_size;
    size_t needed = 0, ceiling = 0;
    SwdecGrowResult grown = SWDEC_GROW_OVER_BOUND;
    if (length <= SWDEC_SAMPLE_MAX_BYTES
        && swdec_packet_bytes(length, nal_length_size,
                              backend->parameter_sets_bytes, &needed)
        && swdec_packet_bytes(SWDEC_SAMPLE_MAX_BYTES, nal_length_size,
                              backend->parameter_sets_bytes, &ceiling))
        grown = swdec_packet_grow(backend->budget, &job->packet,
                                  &job->packet_capacity, needed, ceiling);
    if (grown == SWDEC_GROW_OVER_BOUND) {
        swdec_error(error, error_size, "H.264 AU exceeds swdec bound");
        return false;
    }
    if (grown == SWDEC_GROW_REFUSED) {
        swdec_error(error, error_size, "swdec packet exceeds budget");
        return false;
    }
    return true;
}

/* Browser thread: hand queued units to the worker while it has a free job
   entry (two with the second packet buffer: one decoding, one waiting). With
   audio as the clock, late units are dropped here: a non-reference picture
   that cannot be ready before it is due, or, when the backlog is far behind
   and a keyframe is queued, everything up to that keyframe. A non-reference
   unit queued behind a running one is checked again when the worker starts
   it. Returns QUEUED when a job was handed over, ACCEPTED when nothing could
   be, ERROR on a bad unit. */
static MediaBackendResult swdec_dispatch_queued(
    PspSwdecBackend *backend, char *error, size_t error_size)
{
    bool queued = false;
    int index;
    /* A failed ME is restarted between jobs (swdec_recover_me_when_idle);
       hand out nothing until it has been, so the ring can drain. */
    if (backend->api->me_failed != NULL && backend->api->me_failed())
        return MEDIA_BACKEND_ACCEPTED;
    while (backend->au_count != 0
           && (index = swdec_job_ring_reserve(&backend->ring)) >= 0) {
        unsigned head = backend->au_head;
        uint64_t pts_us = backend->au_queue[head].pts_us;
        uint64_t duration_us = backend->au_queue[head].duration_us;
        bool keyframe = backend->au_queue[head].keyframe;
        bool non_reference = backend->au_queue[head].non_reference;
        /* Until the first picture is on screen and the audio has started,
           the clock the browser publishes runs on its own while startup
           fills the ring: every unit would look late, and skipping them
           kept startup from ever gathering its pictures (one clip skipped 134
           in a row and showed nothing for 6 s). Lateness counts once
           playback is under way. */
        bool started = swdec_playback_started(backend);
        if (backend->have_audio && started) {
            uint64_t clock_us = backend->presentation_clock_us;
#ifdef TILEFINCH_PSP_VALIDATION_LOG
            if (keyframe && backend->au_skipping)
                psp_log_printf("tilefinch-swdec-skip: event=end keyframe-pts=%llu "
                               "clock=%llu skipped=%u\n",
                               (unsigned long long) pts_us,
                               (unsigned long long) clock_us,
                               (unsigned) backend->trace_skip_drops);
#endif
            if (keyframe) backend->au_skipping = false;
            if (!backend->au_skipping && !keyframe
                && pts_us + SWDEC_SKIP_BEHIND_US < clock_us
                && swdec_au_keyframe_queued_after_head(backend)) {
#ifdef TILEFINCH_PSP_VALIDATION_LOG
                swdec_trace_skip_start(backend, "behind");
#endif
                backend->au_skipping = true;
            }
            bool drop = backend->au_skipping
                || (non_reference
                    && swdec_drop_late(pts_us, duration_us, clock_us));
            if (drop) {
                backend->stats.dropped_video_frames++;
#ifdef TILEFINCH_PSP_VALIDATION_LOG
                if (backend->au_skipping) backend->trace_skip_drops++;
                else backend->trace_submit_drops++;
                swdec_hold_fate(pts_us, backend->au_skipping
                                    ? SWDEC_HOLD_SKIPPED : SWDEC_HOLD_LATE);
#endif
                swdec_au_pop(backend);
                continue;
            }
        }
        int slot = swdec_find_free_slot(backend);
        if (slot < 0) break;
        SwdecJob *job = &backend->jobs[index];
        const unsigned char *payload =
            backend->au_bytes + backend->au_queue[head].offset;
        size_t length = backend->au_queue[head].length;
        size_t written = 0;
        if (!swdec_job_packet_fit(backend, job, length,
                                  backend->au_queue[head].annex_b,
                                  error, error_size)) {
            swdec_free_slot(backend, slot);
            return MEDIA_BACKEND_ERROR;
        }
        if (backend->au_queue[head].annex_b) {
            bool prepend = keyframe && backend->parameter_sets_bytes != 0
                && !swdec_annexb_has_nal(payload, length, 7u);
            size_t prefix = prepend ? backend->parameter_sets_bytes : 0u;
            if (length > job->packet_capacity
                || prefix > job->packet_capacity - length) {
                swdec_free_slot(backend, slot);
                swdec_error(error, error_size,
                            "Annex-B H.264 AU exceeds bound");
                return MEDIA_BACKEND_ERROR;
            }
            if (prefix != 0)
                memcpy(job->packet, backend->parameter_sets, prefix);
            memcpy(job->packet + prefix, payload, length);
            written = prefix + length;
        } else if (!swdec_convert_avcc(
                       backend, job->packet, job->packet_capacity,
                       payload, length, keyframe, &written)) {
            swdec_free_slot(backend, slot);
            swdec_error(error, error_size,
                        "invalid length-prefixed H.264 AU");
            return MEDIA_BACKEND_ERROR;
        }
        /* Every in-band SPS keeps the geometry and the coding the decoder
           was admitted for at open: one switching to field coding, another
           chroma format or bit depth is refused here, before the decoder
           and its Media Engine split see a form they were never run on. */
        MediaH264SampleVerdict verdict =
            media_h264_annexb_software_sample_verdict(
                job->packet, written,
                (uint16_t) backend->width, (uint16_t) backend->height);
        if (verdict != MEDIA_H264_SAMPLE_ADMITTED) {
            swdec_free_slot(backend, slot);
            if (verdict == MEDIA_H264_SAMPLE_CODING_UNSUPPORTED)
                swdec_error(error, error_size, "%s",
                            MEDIA_H264_SOFTWARE_CODING_REFUSAL);
            else
                swdec_error(error, error_size,
                            "H.264 access unit changed decoded geometry");
            return MEDIA_BACKEND_ERROR;
        }
        swdec_au_pop(backend);
        job->bytes = written;
        job->slot = slot;
        job->au = backend->next_au++;
        job->pts_us = pts_us;
        job->duration_us = duration_us;
        job->flush = false;
        job->may_drop_late = backend->have_audio && non_reference && started;
        if (!swdec_publish_job(backend, index)) {
            swdec_error(error, error_size, "swdec worker wake failed");
            return MEDIA_BACKEND_ERROR;
        }
        backend->stats.submitted_video_packets++;
#ifdef TILEFINCH_PSP_VALIDATION_LOG
        {
            SwdecHoldPic *picture = swdec_hold_find(&swdec_holds, pts_us);
            if (picture != NULL) picture->dispatched_us = swdec_hold_now();
        }
#endif
        queued = true;
    }
    return queued ? MEDIA_BACKEND_QUEUED : MEDIA_BACKEND_ACCEPTED;
}

static MediaBackendResult swdec_reset_progress(
    PspSwdecBackend *backend, char *error, size_t error_size);

static MediaBackendResult swdec_submit(
    void *opaque, const MediaMp4Sample *sample,
    const unsigned char *payload, size_t length,
    char *error, size_t error_size)
{
    PspSwdecBackend *backend = opaque;
    /* A seek's reset still pending holds the new stream's units. */
    MediaBackendResult resetting =
        swdec_reset_progress(backend, error, error_size);
    if (resetting != MEDIA_BACKEND_ACCEPTED) return resetting;
    MediaBackendResult collected = swdec_collect(backend, error, error_size);
    if (collected == MEDIA_BACKEND_ERROR) return collected;
    if (sample->kind == MEDIA_MP4_TRACK_AUDIO) {
        if (!backend->have_audio) return MEDIA_BACKEND_ACCEPTED;
        if (atomic_load_explicit(&backend->audio_failed,
                                 memory_order_acquire)) {
            /* Given up: nothing is decoded, but the first packet after a
               seek still says where the silent clock starts. */
            if (!atomic_load_explicit(&backend->audio_origin_ready,
                                      memory_order_relaxed)) {
                swdec_published_write(
                    &backend->audio_origin_us, swdec_sample_time_us(sample));
                atomic_store_explicit(&backend->audio_origin_ready, true,
                                      memory_order_release);
            }
            return MEDIA_BACKEND_ACCEPTED;
        }
        unsigned submitted = atomic_load_explicit(
            &backend->audio_submitted, memory_order_acquire);
        unsigned done = backend->api->audio_done();
#ifdef TILEFINCH_PSP_VALIDATION_LOG
        /* Before any slot is written again (submitted - done stays under
           the slot count, so every reused slot has been decoded). */
        swdec_audio_bad_scan(
            &backend->trace_audio_bad, &backend->trace_audio_checked, done,
            backend->api->audio_poll, backend->audio_packets,
            backend->audio_packet_stride, SWDEC_AUDIO_PACKET_SLOTS,
            backend->trace_audio_meta);
#endif
        unsigned played = atomic_load_explicit(
            &backend->audio_output, memory_order_acquire);
        /* Bound both the packet slots and the decoded PCM ring: with the
           demuxer no longer held back by video, audio can run far enough
           ahead to overwrite PCM that has not been played yet. */
        if (submitted - done >= SWDEC_AUDIO_PACKET_SLOTS
            || submitted - played >= TILEFINCH_SWDEC_PCM_SLOT_COUNT - 16u)
            return MEDIA_BACKEND_WOULD_BLOCK;
        bool adts = sample->packet_format == MEDIA_PACKET_FORMAT_AAC_ADTS;
        size_t header_bytes = adts ? 0u : 7u;
        if (length > backend->audio_packet_stride
            || header_bytes > backend->audio_packet_stride - length) {
            swdec_error(error, error_size, "AAC packet exceeds swdec bound");
            return MEDIA_BACKEND_ERROR;
        }
        unsigned char *packet = backend->audio_packets
            + (size_t) (submitted % SWDEC_AUDIO_PACKET_SLOTS)
              * backend->audio_packet_stride;
        if (!adts
            && !swdec_adts_header(&backend->audio_info, length, packet)) {
            swdec_error(error, error_size, "AAC config cannot form ADTS");
            return MEDIA_BACKEND_ERROR;
        }
        memcpy(packet + header_bytes, payload, length);
        /* The audio-only path never enters the video slice bridge whose
           whole-cache maintenance used to mask this missing handoff. Every
           packet is line-isolated, so a range writeback cannot touch Budget
           metadata or a neighbouring in-flight packet. */
        sceKernelDcacheWritebackRange(
            packet, (unsigned) swdec_cache_extent(length + header_bytes));
        if (submitted == 0) {
            swdec_published_write(
                &backend->audio_origin_us, swdec_sample_time_us(sample));
            atomic_store_explicit(&backend->audio_origin_ready, true,
                                  memory_order_release);
        }
#ifdef TILEFINCH_PSP_VALIDATION_LOG
        backend->trace_audio_meta[submitted % SWDEC_AUDIO_PACKET_SLOTS] =
            (SwdecAudioPacketMeta) {
                (unsigned) (length + header_bytes),
                swdec_sample_time_us(sample)};
        {
            /* The audio clock counts samples played; a jump in the packets'
               timestamps moves sound against pictures (first few). */
            uint64_t pts = swdec_sample_time_us(sample);
            uint64_t expected = backend->trace_audio_next_pts_us;
            if (submitted != 0
                && (pts > expected + 100000u || pts + 100000u < expected)
                && backend->trace_audio_pts_jumps++ < 4u)
                psp_log_printf("tilefinch-swdec-audio-jump: packet=%u "
                               "expected=%llu pts=%llu\n", submitted,
                               (unsigned long long) expected,
                               (unsigned long long) pts);
            backend->trace_audio_next_pts_us =
                pts + swdec_sample_duration_us(sample);
        }
#endif
        if (backend->api->audio_submit(
                packet, (unsigned) (length + header_bytes)) != 0)
            return MEDIA_BACKEND_WOULD_BLOCK;
        atomic_store_explicit(
            &backend->audio_submitted, submitted + 1u,
            memory_order_release);
        backend->stats.submitted_audio_packets++;
        return MEDIA_BACKEND_ACCEPTED;
    }
    if (sample->kind != MEDIA_MP4_TRACK_VIDEO)
        return MEDIA_BACKEND_ACCEPTED;
    if (sample->keyframe) {
        /* A new SPS takes effect only at an IDR picture. One asking for more
           held pictures than the arena was sized for at open is refused
           here, before the decoder could run its arena dry. */
        unsigned frames = 0;
        MediaH264SpsFramesStatus status = media_h264_sample_sps_frames(
            payload, length,
            sample->packet_format == MEDIA_PACKET_FORMAT_H264_ANNEX_B
                ? 0u : backend->nal_length_size,
            &frames);
        if (status != MEDIA_H264_SPS_FRAMES_ABSENT
            && !swdec_sps_frames_admitted(frames, backend->arena_refs)) {
            swdec_error(error, error_size,
                        MEDIA_H264_UNSUPPORTED_STREAM
                        " (reference frames grew from %d to %u)",
                        backend->arena_refs, frames);
            return MEDIA_BACKEND_ERROR;
        }
    }
    if (backend->au_skip_incoming) {
        if (!sample->keyframe) {
            /* The queue overflowed past a reference picture: nothing until
               the next keyframe can be decoded. Keep showing the last
               picture while audio keeps the clock. */
            backend->stats.dropped_video_frames++;
#ifdef TILEFINCH_PSP_VALIDATION_LOG
            backend->trace_full_drops++;
#endif
            return MEDIA_BACKEND_ACCEPTED;
        }
        backend->au_skip_incoming = false;
    }
    /* A unit from a fragment after open can be past the whole queue. */
    SwdecGrowResult queue = swdec_au_queue_grow(
        backend->budget, &backend->au_bytes, &backend->au_bytes_capacity,
        length, backend->have_audio);
    if (queue == SWDEC_GROW_OVER_BOUND) {
        swdec_error(error, error_size, "H.264 AU exceeds swdec bound");
        return MEDIA_BACKEND_ERROR;
    }
    if (queue == SWDEC_GROW_REFUSED) {
        swdec_error(error, error_size, "swdec video queue exceeds budget");
        return MEDIA_BACKEND_ERROR;
    }
    if (!swdec_au_enqueue(backend, sample, payload, length)) {
        MediaBackendResult dispatched =
            swdec_dispatch_queued(backend, error, error_size);
        if (dispatched == MEDIA_BACKEND_ERROR) return dispatched;
        if (!swdec_au_enqueue(backend, sample, payload, length)) {
#ifdef TILEFINCH_PSP_VALIDATION_LOG
            backend->trace_queue_full++;
#endif
            /* Before playback starts nothing plays behind the queue: wait
               rather than drop pictures startup is still gathering. */
            if (!backend->have_audio || !swdec_playback_started(backend))
                return MEDIA_BACKEND_WOULD_BLOCK;
            /* With audio playing, a full video queue must never stop the
               demuxer: that stops the audio behind it. Make room by
               dropping video instead. */
            bool placed = false;
            for (unsigned attempt = 0;
                 attempt < SWDEC_AU_QUEUE_ENTRIES && !placed; attempt++) {
                if (!swdec_au_make_room(backend))
                    break;
                placed = swdec_au_enqueue(backend, sample, payload, length);
            }
            if (!placed && sample->keyframe) {
                /* A keyframe restarts decoding: everything queued before it
                   is expendable. */
#ifdef TILEFINCH_PSP_VALIDATION_LOG
                backend->trace_full_drops += backend->au_count;
#endif
                backend->stats.dropped_video_frames += backend->au_count;
                swdec_au_clear(backend);
                placed = swdec_au_enqueue(backend, sample, payload, length);
                if (!placed) return MEDIA_BACKEND_WOULD_BLOCK;
            }
            if (!placed) {
                backend->stats.dropped_video_frames++;
#ifdef TILEFINCH_PSP_VALIDATION_LOG
                backend->trace_full_drops++;
#endif
                if (!sample->keyframe
                    && !swdec_au_is_non_reference(
                           payload, length,
                           sample->packet_format
                                   == MEDIA_PACKET_FORMAT_H264_ANNEX_B
                               ? 0u : backend->nal_length_size))
                    backend->au_skip_incoming = true;
                return MEDIA_BACKEND_ACCEPTED;
            }
        }
    }
    MediaBackendResult dispatched =
        swdec_dispatch_queued(backend, error, error_size);
    if (dispatched == MEDIA_BACKEND_ERROR) return dispatched;
    return dispatched == MEDIA_BACKEND_QUEUED
        ? MEDIA_BACKEND_QUEUED : MEDIA_BACKEND_ACCEPTED;
}

static MediaBackendResult swdec_drain(
    void *opaque, char *error, size_t error_size)
{
    PspSwdecBackend *backend = opaque;
    MediaBackendResult resetting =
        swdec_reset_progress(backend, error, error_size);
    if (resetting != MEDIA_BACKEND_ACCEPTED) return resetting;
    atomic_store_explicit(
        &backend->audio_input_ended, true, memory_order_release);
    MediaBackendResult collected = swdec_collect(backend, error, error_size);
    if (collected == MEDIA_BACKEND_ERROR) return collected;
    if (collected == MEDIA_BACKEND_WOULD_BLOCK) return collected;
    if (backend->decoder == NULL) {
        unsigned submitted = atomic_load_explicit(
            &backend->audio_submitted, memory_order_acquire);
        return backend->api->audio_done() < submitted
            ? MEDIA_BACKEND_WOULD_BLOCK : MEDIA_BACKEND_END;
    }
    if (backend->drained) return MEDIA_BACKEND_END;
    if (backend->au_count != 0) {
        MediaBackendResult dispatched =
            swdec_dispatch_queued(backend, error, error_size);
        if (dispatched == MEDIA_BACKEND_ERROR) return dispatched;
        if (backend->au_count != 0 || dispatched == MEDIA_BACKEND_QUEUED)
            return MEDIA_BACKEND_WOULD_BLOCK;
    }
    int index = swdec_job_ring_reserve(&backend->ring);
    if (index < 0) return MEDIA_BACKEND_WOULD_BLOCK;
    int slot = swdec_find_free_slot(backend);
    if (slot < 0) return MEDIA_BACKEND_WOULD_BLOCK;
    SwdecJob *job = &backend->jobs[index];
    job->bytes = 0;
    job->slot = slot;
    job->au = backend->next_au++;
    job->pts_us = backend->last_job_pts_us + backend->last_job_duration_us;
    job->duration_us = backend->last_job_duration_us;
    job->flush = true;
    job->may_drop_late = false;
    if (!swdec_publish_job(backend, index)) {
        swdec_error(error, error_size, "swdec drain wake failed");
        return MEDIA_BACKEND_ERROR;
    }
    return MEDIA_BACKEND_QUEUED;
}

static void swdec_set_dispatch_clock(
    PspSwdecBackend *backend, uint64_t clock_us)
{
    backend->presentation_clock_us = clock_us;
    swdec_published_write(&backend->dispatch_clock_us, clock_us);
}

/* Browser thread: restart a Media Engine that failed, once the decode worker
   holds no job. Recovery reinstalls the slice hooks and re-enables the ME,
   which the patched decoder reads per row: done while a job is decoding on
   the CPU path, that job would switch modes mid-slice (ME cache operations
   on the CPU, a wait on a pipe nobody advances, the audio service on both
   cores). So no new job is dispatched while the ME is down, and recovery
   waits for the ring to drain. ACCEPTED when the ME is fine or restarted,
   WOULD_BLOCK while jobs are still in flight, ERROR when it cannot be
   restarted. */
static MediaBackendResult swdec_recover_me_when_idle(
    PspSwdecBackend *backend, char *error, size_t error_size)
{
    SwdecMeRecovery action = swdec_me_recovery_action(
        backend->api->me_failed != NULL && backend->api->me_failed(),
        swdec_job_ring_in_flight(&backend->ring));
    if (action == SWDEC_ME_RECOVERY_NONE) return MEDIA_BACKEND_ACCEPTED;
    if (action == SWDEC_ME_RECOVERY_WAIT) return MEDIA_BACKEND_WOULD_BLOCK;
    bool recovered = backend->api->recover_me() == 0
        && (!backend->have_audio || backend->api->audio_setup() == 0);
#ifdef TILEFINCH_PSP_VALIDATION_LOG
    backend->trace_me_recoveries++;
#endif
    psp_log_printf("tilefinch-swdec-me: event=recover ok=%d pending-reset=%d\n",
                   recovered ? 1 : 0, backend->reset_gate.pending ? 1 : 0);
    if (!recovered) {
        swdec_error(error, error_size,
                    "software decoder ME recovery failed");
        return MEDIA_BACKEND_ERROR;
    }
    return MEDIA_BACKEND_ACCEPTED;
}

static bool swdec_advance(
    void *opaque, uint64_t clock_us, char *error, size_t error_size)
{
    PspSwdecBackend *backend = opaque;
    swdec_set_dispatch_clock(backend, clock_us);
    MediaBackendResult resetting =
        swdec_reset_progress(backend, error, error_size);
    if (resetting == MEDIA_BACKEND_ERROR) return false;
    if (resetting == MEDIA_BACKEND_WOULD_BLOCK) {
        /* A pending reset waits for the ME's audio queue; a dead ME never
           drains it. Restart it (its jobs are collected by now) so the
           reset can complete instead of timing out. */
        return swdec_recover_me_when_idle(backend, error, error_size)
            != MEDIA_BACKEND_ERROR;
    }
    MediaBackendResult result = swdec_collect(backend, error, error_size);
    if (result == MEDIA_BACKEND_ERROR) return false;
    int wait_error = atomic_load_explicit(
        &backend->worker_wait_error, memory_order_acquire);
    if (wait_error != 0) {
        backend->stats.last_native_error = wait_error;
        swdec_error(error, error_size,
                    "software decoder worker wait failed (%d)", wait_error);
        return false;
    }
    int output_error = atomic_load_explicit(
        &backend->worker_error, memory_order_acquire);
    if (output_error != 0) {
        /* The audio thread's channel failed (reserve or output), or it
           refused the stream's frames: also reachable with no decode job to
           collect it, as audio-only. */
        backend->stats.last_native_error = output_error;
        if (output_error == SWDEC_AUDIO_ERROR_FRAME_SHAPE)
            swdec_error(error, error_size,
                        "HE-AAC (SBR) audio is not supported by the "
                        "software decoder");
        else
            swdec_error(error, error_size,
                        "software decoder audio output failed (%d)",
                        output_error);
        return false;
    }
    MediaBackendResult recovered =
        swdec_recover_me_when_idle(backend, error, error_size);
    if (recovered == MEDIA_BACKEND_ERROR) return false;
    if (recovered == MEDIA_BACKEND_WOULD_BLOCK) return true;
    return swdec_dispatch_queued(backend, error, error_size)
        != MEDIA_BACKEND_ERROR;
}

static void swdec_release_prior_claim(PspSwdecBackend *backend, int keep)
{
    int prior = backend->last_claimed_slot;
    if (prior < 0 || prior == keep) return;
    SwdecVideoSlot *state = &backend->slots[prior];
    /* Publish retirement before sampling readers. If the last reader raced
       just ahead of this store, the zero-reader recheck below performs the
       retire; if it races after, release performs it. */
    atomic_store_explicit(
        &state->retire_when_unborrowed, true, memory_order_release);
    state->claimed = false;
    if (atomic_load_explicit(&state->readers, memory_order_acquire) == 0) {
        if (atomic_exchange_explicit(
                &state->retire_when_unborrowed, false,
                memory_order_acq_rel))
            swdec_free_slot(backend, prior);
    }
    backend->last_claimed_slot = -1;
}

static bool swdec_take_video_frame(void *opaque, MediaVideoFrame *frame)
{
    PspSwdecBackend *backend = opaque;
#ifdef TILEFINCH_PSP_VALIDATION_LOG
    {
        uint32_t now = swdec_hold_now();
        uint32_t gap = now - swdec_hold_last_take_us;
        if (swdec_hold_last_take_us != 0 && gap > swdec_hold_take_gap_us) {
            swdec_hold_take_gap_us = gap;
            swdec_hold_gap_wait_us = swdec_presenter_wait_total - swdec_take_wait_at;
        }
        swdec_take_wait_at = swdec_presenter_wait_total;
        swdec_hold_last_take_us = now;
    }
#endif
    if (backend->reset_gate.pending || backend->reset_failed) return false;
    int selected = -1;
    uint64_t selected_pts = UINT64_MAX;
    if (backend->have_audio) {
        /* Audio is the clock and never waits. Show the newest frame that is
           due and drop the due frames behind it, so slow decoding costs
           frames rather than slowing the whole presentation. */
        uint64_t pts[SWDEC_RING_SLOTS];
        uint64_t duration[SWDEC_RING_SLOTS];
        bool eligible[SWDEC_RING_SLOTS];
        for (unsigned i = 0; i < SWDEC_RING_SLOTS; i++) {
            SwdecVideoSlot *candidate = &backend->slots[i];
            eligible[i] = atomic_load_explicit(
                    &candidate->state, memory_order_acquire)
                    == SWDEC_SLOT_READY
                && !candidate->claimed;
            pts[i] = candidate->pts_us;
            duration[i] = candidate->duration_us;
        }
        selected = swdec_pick_due_frame(
            pts, duration, eligible, SWDEC_RING_SLOTS,
            backend->presentation_clock_us);
#ifdef TILEFINCH_PSP_VALIDATION_LOG
        if (selected < 0) {
            uint64_t earliest = UINT64_MAX;
            for (unsigned i = 0; i < SWDEC_RING_SLOTS; i++)
                if (eligible[i] && pts[i] < earliest) earliest = pts[i];
            SwdecHoldPic *picture = swdec_hold_find(&swdec_holds, earliest);
            if (picture != NULL) {
                picture->declined_us = swdec_hold_now();
                picture->declined_clock_us = backend->presentation_clock_us;
            }
        }
#endif
        if (selected < 0) return false;
        selected_pts = pts[selected];
        for (unsigned i = 0; i < SWDEC_RING_SLOTS; i++) {
            if (!eligible[i] || (int) i == selected || pts[i] >= selected_pts)
                continue;
            atomic_fetch_sub_explicit(
                &backend->ready_count, 1u, memory_order_relaxed);
#ifdef TILEFINCH_PSP_VALIDATION_LOG
            {
                SwdecHoldPic *picture = swdec_hold_find(&swdec_holds, pts[i]);
                if (picture != NULL) {
                    picture->fate = SWDEC_HOLD_SUPERSEDED;
                    swdec_hold_output(picture, &backend->slots[i]);
                    picture->taken_us = swdec_hold_now();
                }
            }
#endif
            swdec_free_slot(backend, (int) i);
            backend->stats.dropped_video_frames++;
        }
    } else {
        uint64_t effective_clock = backend->presentation_clock_us
            >= backend->clock_slip_us
            ? backend->presentation_clock_us - backend->clock_slip_us : 0;
        for (unsigned i = 0; i < SWDEC_RING_SLOTS; i++) {
            SwdecVideoSlot *state = &backend->slots[i];
            if (atomic_load_explicit(&state->state, memory_order_acquire)
                  != SWDEC_SLOT_READY
                || state->claimed || state->pts_us >= selected_pts)
                continue;
            selected = (int) i;
            selected_pts = state->pts_us;
        }
        if (selected < 0) return false;
        SwdecVideoSlot *state = &backend->slots[selected];
        if (selected_pts > effective_clock
            && selected_pts - effective_clock > state->duration_us / 2u)
            return false;
        if (effective_clock > selected_pts + 8000u) {
#ifdef TILEFINCH_PSP_VALIDATION_LOG
            backend->trace_late_reanchors++;
            if (effective_clock - selected_pts > backend->trace_late_max_us)
                backend->trace_late_max_us = effective_clock - selected_pts;
#endif
            /* Video only: re-anchor the correction to this displayed frame.
               Accumulating lateness treats the same clock error as new on
               every take and can grow without bound; this absolute anchor
               makes effective_clock exactly selected_pts and carries only
               the measured clock offset. */
            backend->clock_slip_us = swdec_clock_reanchor_slip(
                backend->presentation_clock_us, selected_pts);
        }
    }
    SwdecVideoSlot *state = &backend->slots[selected];
#ifdef TILEFINCH_PSP_VALIDATION_LOG
    {
        SwdecHoldPic *picture = swdec_hold_find(&swdec_holds, state->pts_us);
        if (picture != NULL) {
            swdec_hold_output(picture, state);
            picture->taken_us = swdec_hold_now();
            picture->taken_clock_us = backend->presentation_clock_us;
        }
    }
#endif
    swdec_release_prior_claim(backend, selected);
    state->claimed = true;
    backend->last_claimed_slot = selected;
    atomic_fetch_sub_explicit(
        &backend->ready_count, 1u, memory_order_relaxed);
    *frame = (MediaVideoFrame) {
        .pixels = swdec_rgb_slot(backend, selected),
        .width = (int) backend->width,
        .height = (int) backend->height,
        .stride_pixels = (int) backend->rgb_stride,
        .format = MEDIA_PIXEL_RGB565,
        .pts_us = state->pts_us,
        .duration_us = state->duration_us,
        .identity = state->identity,
        .slot = selected,
        .generation = state->generation,
        .epoch = backend->epoch
    };
    return true;
}

static size_t swdec_discard_video_before(void *opaque, uint64_t floor_us)
{
    PspSwdecBackend *backend = opaque;
    size_t discarded = 0;
    if (backend->reset_gate.pending) return 0;
    for (unsigned i = 0; i < SWDEC_RING_SLOTS; i++) {
        SwdecVideoSlot *state = &backend->slots[i];
        if (atomic_load_explicit(&state->state, memory_order_acquire)
              != SWDEC_SLOT_READY
            || state->claimed || state->pts_us >= floor_us) continue;
        atomic_fetch_sub_explicit(
            &backend->ready_count, 1u, memory_order_relaxed);
        swdec_free_slot(backend, (int) i);
        discarded++;
    }
    backend->stats.discarded_seek_video_frames += discarded;
    return discarded;
}

static bool swdec_stats(const void *opaque, MediaBackendStats *stats)
{
    const PspSwdecBackend *backend = opaque;
    if (backend == NULL || stats == NULL) return false;
    *stats = backend->stats;
    stats->audio_output_blocks = backend->reset_gate.pending ? 0u
        : atomic_load_explicit(&backend->audio_output, memory_order_acquire);
    return true;
}

static bool swdec_audio_cursor(const void *opaque, uint64_t *cursor_us)
{
    const PspSwdecBackend *backend = opaque;
    if (backend == NULL || cursor_us == NULL || !backend->have_audio)
        return false;
    /* A seek's reset is pending: the cursor of the stream being left must
       not be mistaken for the new one's, as the synchronous reset never
       let it be. */
    *cursor_us = backend->reset_gate.pending ? 0
        : swdec_published_read(&backend->audio_cursor_us, 0);
    if (!backend->reset_gate.pending
#ifdef TILEFINCH_PSP_VALIDATION_LOG
        && !swdec_legacy_audio_clock
#endif
        )
        *cursor_us = swdec_audio_clock_read(&backend->audio_dac_clock,
            sceKernelGetSystemTimeWide(), *cursor_us);
    return true;
}

static unsigned swdec_ready_frames(const void *opaque)
{
    const PspSwdecBackend *backend = opaque;
    return backend == NULL || backend->reset_gate.pending ? 0
        : atomic_load_explicit(&backend->ready_count, memory_order_acquire);
}

static bool swdec_ready_start(const void *opaque, uint64_t *start_us)
{
    const PspSwdecBackend *backend = opaque;
    if (backend == NULL || start_us == NULL
        || backend->reset_gate.pending) return false;
    uint64_t earliest = UINT64_MAX;
    for (unsigned i = 0; i < SWDEC_RING_SLOTS; i++) {
        const SwdecVideoSlot *slot = &backend->slots[i];
        if (atomic_load_explicit(&slot->state, memory_order_acquire)
              == SWDEC_SLOT_READY
            && !slot->claimed && slot->pts_us < earliest)
            earliest = slot->pts_us;
    }
    if (earliest == UINT64_MAX) return false;
    *start_us = earliest;
    return true;
}

static bool swdec_wait_ready(void *opaque, uint32_t timeout_us)
{
    PspSwdecBackend *backend = opaque;
    if (backend == NULL || backend->ready_event < 0
        || backend->reset_gate.pending || timeout_us == 0)
        return false;
    /* Clear a notification from an already-consumed picture, then recheck
       the queue. A publication on either side of the clear cannot be lost:
       the recheck sees it or the event wakes the bounded wait. */
    if (sceKernelClearEventFlag(backend->ready_event, 0u) < 0) return false;
    if (swdec_ready_frames(backend) != 0) return true;
    SceUInt timeout = timeout_us;
    u32 bits = 0;
    int result = sceKernelWaitEventFlag(backend->ready_event, 1u,
        PSP_EVENT_WAITOR | PSP_EVENT_WAITCLEAR, &bits, &timeout);
    return result == 0 || result == (int) SCE_KERNEL_ERROR_WAIT_TIMEOUT;
}

static bool swdec_wait_due(void *opaque, uint32_t timeout_us)
{
    PspSwdecBackend *backend = opaque;
    if (backend == NULL || !backend->have_audio || backend->reset_gate.pending
        || backend->reset_failed || swdec_audio_fence_hold(&backend->audio_fence)
        || !atomic_load_explicit(&backend->playing, memory_order_acquire)
        || atomic_load_explicit(&backend->buffering, memory_order_acquire))
        return false;
#ifdef TILEFINCH_PSP_VALIDATION_LOG
    if (swdec_legacy_due_wait || swdec_legacy_audio_clock) return false;
#endif
    const SwdecVideoSlot *earliest = NULL;
    for (unsigned i = 0; i < SWDEC_RING_SLOTS; i++) {
        const SwdecVideoSlot *slot = &backend->slots[i];
        if (atomic_load_explicit(&slot->state, memory_order_acquire)
                == SWDEC_SLOT_READY && !slot->claimed
            && (earliest == NULL || slot->pts_us < earliest->pts_us))
            earliest = slot;
    }
    if (earliest == NULL) return false;
    uint64_t accepted_end = 0;
    uint64_t clock = swdec_audio_clock_sample(&backend->audio_dac_clock,
        sceKernelGetSystemTimeWide(), 0, &accepted_end);
    if (accepted_end == 0) return false;
    uint32_t delay = swdec_audio_due_wait_us(earliest->pts_us,
        earliest->duration_us, clock, accepted_end, timeout_us);
    return delay != 0 && sceKernelDelayThread(delay) >= 0;
}

static size_t swdec_displayed_frames(const void *opaque)
{
    const PspSwdecBackend *backend = opaque;
    return backend == NULL ? 0 : atomic_load_explicit(
        &backend->displayed_count, memory_order_acquire);
}

static void swdec_set_clock(void *opaque, uint64_t clock_us)
{
    swdec_set_dispatch_clock(opaque, clock_us);
}

static void swdec_set_playing(void *opaque, bool playing)
{
    atomic_store_explicit(
        &((PspSwdecBackend *) opaque)->playing, playing,
        memory_order_release);
}

static void swdec_set_buffering(void *opaque, bool buffering)
{
#ifdef TILEFINCH_PSP_VALIDATION_LOG
    PspSwdecBackend *traced = opaque;
    uint64_t now_us = sceKernelGetSystemTimeWide();
    bool was = atomic_load_explicit(&traced->buffering, memory_order_acquire);
    if (buffering && !was) {
        traced->trace_buffering_since_us = now_us;
        traced->trace_buffering_events++;
    } else if (!buffering && was && traced->trace_buffering_since_us != 0) {
        traced->trace_buffering_total_us +=
            now_us - traced->trace_buffering_since_us;
        traced->trace_buffering_since_us = 0;
    }
#endif
    atomic_store_explicit(
        &((PspSwdecBackend *) opaque)->buffering, buffering,
        memory_order_release);
}

static bool swdec_borrow(void *opaque, unsigned slot, uint32_t generation)
{
    PspSwdecBackend *backend = opaque;
    if (backend == NULL || slot >= SWDEC_RING_SLOTS) return false;
    SwdecVideoSlot *state = &backend->slots[slot];
    int slot_state = atomic_load_explicit(&state->state, memory_order_acquire);
    if ((slot_state != SWDEC_SLOT_READY && slot_state != SWDEC_SLOT_READING)
        || state->generation != generation || state->quarantined) return false;
    atomic_fetch_add_explicit(&state->readers, 1u, memory_order_acq_rel);
    atomic_store_explicit(
        &state->state, SWDEC_SLOT_READING, memory_order_release);
    return true;
}

static void swdec_release(void *opaque, unsigned slot)
{
    PspSwdecBackend *backend = opaque;
    if (backend == NULL || slot >= SWDEC_RING_SLOTS) return;
    SwdecVideoSlot *state = &backend->slots[slot];
    unsigned readers = atomic_load_explicit(
        &state->readers, memory_order_acquire);
    if (readers == 0) return;
    readers = atomic_fetch_sub_explicit(
        &state->readers, 1u, memory_order_acq_rel) - 1u;
    if (readers != 0 || state->quarantined) return;
    if (atomic_exchange_explicit(
            &state->retire_when_unborrowed, false,
            memory_order_acq_rel)) {
        swdec_free_slot(backend, (int) slot);
    } else {
        /* A concurrent supersession may already have freed this slot. Only
           return the exact READING generation to READY; never resurrect FREE. */
        int expected = SWDEC_SLOT_READING;
        (void) atomic_compare_exchange_strong_explicit(
            &state->state, &expected, SWDEC_SLOT_READY,
            memory_order_acq_rel, memory_order_acquire);
    }
}

static void swdec_quarantine(void *opaque, unsigned slot)
{
    PspSwdecBackend *backend = opaque;
    if (backend == NULL || slot >= SWDEC_RING_SLOTS) return;
    backend->slots[slot].quarantined = true;
    swdec_quarantined = true;
}

static void swdec_release_quarantine(void *opaque, unsigned slot)
{
    (void) opaque;
    (void) slot;
    /* A timed-out DMA may still read this memory. Process-lifetime only. */
}

static bool swdec_is_quarantined(const void *opaque, unsigned slot)
{
    const PspSwdecBackend *backend = opaque;
    return backend == NULL || slot >= SWDEC_RING_SLOTS
        || backend->slots[slot].quarantined;
}

#ifdef TILEFINCH_PSP_VALIDATION_LOG
static uint64_t swdec_read_u64(const uint64_t *value)
{
    const volatile uint64_t *shared = value;
    uint64_t first, second;
    do {
        first = *shared;
        second = *shared;
    } while (first != second);
    return first;
}

/* Main-thread CPU of the loop's idle and present phases by part
   (MediaSwdecCpuKind), added by the loop while a window is open (the
   presenter, which opens and reports the window, is that thread). */
static bool swdec_window_cpu_on;
static uint64_t swdec_window_cpu_us[MEDIA_SWDEC_CPU_KINDS];

bool media_psp_swdec_window_cpu_wanted(void)
{
    return swdec_window_cpu_on;
}

void media_psp_swdec_window_cpu_add(unsigned kind, uint64_t us)
{
    if (kind < MEDIA_SWDEC_CPU_KINDS) swdec_window_cpu_us[kind] += us;
}

static uint64_t swdec_thread_run_us(SceUID thread)
{
    SceKernelThreadRunStatus status = {.size = sizeof(status)};
    if (thread < 0 || sceKernelReferThreadRunStatus(thread, &status) < 0)
        return 0;
    return ((uint64_t) status.runClocks.hi << 32) | status.runClocks.low;
}

static void swdec_window_take(PspSwdecBackend *backend, SwdecWindowSnap *snap)
{
    snap->at_us = sceKernelGetSystemTimeWide();
    snap->audio_cursor_us = swdec_published_read(&backend->audio_cursor_us, 0);
    snap->buffering_us = backend->trace_buffering_total_us;
    snap->decode_us = swdec_read_u64(&backend->trace_decode_total_us);
    snap->csc_us = swdec_read_u64(&backend->trace_publish_total_us);
    snap->publishes = backend->trace_publish_count;
    snap->no_picture = backend->trace_no_picture;
    snap->ready = atomic_load_explicit(
        &backend->ready_count, memory_order_relaxed);
    snap->job_gap_us = swdec_read_u64(&backend->trace_job_gap_total_us);
    snap->audio_gap_us = swdec_read_u64(&backend->trace_gap.gap_total_us);
    snap->cpu_main_us = swdec_thread_run_us(sceKernelGetThreadId());
    snap->cpu_worker_us = swdec_thread_run_us(backend->worker_thread);
    snap->cpu_audio_us = backend->audio_started
        ? swdec_thread_run_us(backend->audio_thread) : 0;
    snap->submitted = backend->stats.submitted_video_packets;
    snap->decoded = backend->stats.decoded_video_frames;
    snap->displayed = atomic_load_explicit(
        &backend->displayed_count, memory_order_relaxed);
    snap->repeats = backend->displayed_repeats;
    snap->dropped = backend->stats.dropped_video_frames;
    snap->submit_drops = backend->trace_submit_drops;
    snap->skip_drops = backend->trace_skip_drops;
    snap->full_drops = backend->trace_full_drops;
    snap->worker_late = backend->trace_worker_late_drops;
    snap->csc_failures = backend->trace_csc_failures;
    snap->reduced = backend->trace_reduced_decodes;
    snap->queue_full = backend->trace_queue_full;
    snap->audio_blocks = backend->trace_audio_blocks;
    snap->underruns = backend->trace_audio_underruns;
    snap->underruns_queued = backend->trace_audio_underruns_queued;
    snap->sent_errors = backend->trace_sent_errors;
    snap->buffering_events = backend->trace_buffering_events;
    snap->job_gaps = backend->trace_job_gaps;
    snap->resets = backend->trace_resets;
    snap->audio_gaps = backend->trace_gap.gaps;
}

/* Times of one hold's pictures, in ms from the moment the hold ended (the
   picture that ended it shown); '-' where a step did not happen. */
static void swdec_hold_ms(char *out, size_t size, uint32_t at, uint32_t end)
{
    if (at == 0) snprintf(out, size, "-");
    else {
        int32_t us = (int32_t) (at - end);
        snprintf(out, size, "%s%d.%d", us < 0 ? "-" : "",
                 (int) ((us < 0 ? -us : us) / 1000),
                 (int) ((us < 0 ? -us : us) % 1000 / 100));
    }
}

/* The longest holds of the window, each with the pictures around the one
   that ended it. */
static void swdec_hold_report(void)
{
    static const char *const fates[] = {
        "pending", "shown", "superseded", "late", "skipped", "full",
        "worker-late"
    };
    static const char slices[] = "PBI??";
    for (unsigned h = 0; h < SWDEC_HOLD_KEEP; h++) {
        const SwdecHold *hold = &swdec_holds.holds[h];
        if (hold->hold_us == 0) break;
        psp_log_printf("tilefinch-swdec-hold: rank=%u hold-us=%u pts-ms=%llu "
                       "take-gap-us=%u ready=%u queued=%u pictures=%u at-us=%u gap-wait-us=%u repeat-age-us=%u\n",
                       h + 1u, (unsigned) hold->hold_us,
                       (unsigned long long) (hold->pts_us / 1000u),
                       (unsigned) hold->take_gap_us, (unsigned) hold->ready,
                       (unsigned) hold->queued, hold->count,
                       (unsigned) hold->shown_us, hold->gap_wait_us,
                       hold->repeat_age_us);
        for (unsigned i = 0; i < hold->count; i++) {
            const SwdecHoldPic *p = &hold->pictures[i];
            char queued[16], dispatched[16], start[16], end[16], ready[16],
                taken[16], shown[16], output[16], declined[16];
            uint32_t at = hold->shown_us;
            swdec_hold_ms(queued, sizeof(queued), p->queued_us, at);
            swdec_hold_ms(dispatched, sizeof(dispatched), p->dispatched_us, at);
            swdec_hold_ms(start, sizeof(start), p->decode_start_us, at);
            swdec_hold_ms(end, sizeof(end), p->decode_end_us, at);
            swdec_hold_ms(output, sizeof(output), p->output_us, at);
            swdec_hold_ms(ready, sizeof(ready), p->ready_us, at);
            swdec_hold_ms(taken, sizeof(taken), p->taken_us, at);
            swdec_hold_ms(shown, sizeof(shown), p->shown_us, at);
            swdec_hold_ms(declined, sizeof(declined), p->declined_us, at);
            psp_log_printf(
                "tilefinch-swdec-hold-pic: rank=%u pts-ms=%llu type=%c%s%s "
                "bytes=%u depth=%u lead-ms=%d queued=%s dispatched=%s "
                "decode=%s..%s decode-us=%u output=%s by=%+d ready=%s "
                "taken=%s shown=%s fate=%s declined=%s "
                "declined-clock-us=%llu taken-clock-us=%llu "
                "decode-cpu-us=%u browser-cpu-us=%u clock-bracket-us=%u\n",
                h + 1u, (unsigned long long) (p->pts_us / 1000u),
                p->slice < 0 ? '?' : slices[p->slice],
                p->keyframe ? "+idr" : "", p->reference ? "+ref" : "",
                (unsigned) p->bytes, (unsigned) p->depth,
                (int) (p->lead_us / 1000), queued, dispatched, start, end,
                p->decode_start_us == 0 ? 0u
                    : (unsigned) (p->decode_end_us - p->decode_start_us),
                output, (int) p->output_by_ms, ready, taken, shown,
                p->fate < sizeof(fates) / sizeof(fates[0])
                    ? fates[p->fate] : "?", declined,
                (unsigned long long) p->declined_clock_us,
                (unsigned long long) p->taken_clock_us,
                (unsigned) p->decode_cpu_us, (unsigned) p->browser_cpu_us,
                (unsigned) p->clock_bracket_us);
        }
    }
}

/* The window's three lines: timing (unique published pictures), the
   work it took, and what it ran on. Raw differences only; per-picture and
   per-second figures are the host's (tools/swdec-bench/window-pairs.py). */
static void swdec_window_report(PspSwdecBackend *backend, bool complete)
{
    SwdecWindowSnap now;
    swdec_window_take(backend, &now);
    const SwdecWindowSnap *was = &backend->window_open;
    const SwdecWindow *w = &backend->window;
    psp_log_printf("tilefinch-swdec-window: complete=%d lead-us=%llu "
                   "span-us=%llu displayed=%u repeats=%u intervals=%u "
                   "interval-ms=%u/%u/%u held=%u worst-us=%u@%llu,%u@%llu,"
                   "%u@%llu clock-us=%llu buffering=%u/%lluus resets=%u\n",
                   complete ? 1 : 0,
                   (unsigned long long) (was->at_us - w->first_us),
                   (unsigned long long) (now.at_us - was->at_us),
                   now.displayed - was->displayed, now.repeats - was->repeats,
                   (unsigned) w->intervals,
                   swdec_window_percentile_ms(w, 500u),
                   swdec_window_percentile_ms(w, 950u),
                   (unsigned) (w->max_us / 1000u), (unsigned) w->held,
                   (unsigned) w->worst_us[0],
                   (unsigned long long) w->worst_pts_us[0],
                   (unsigned) w->worst_us[1],
                   (unsigned long long) w->worst_pts_us[1],
                   (unsigned) w->worst_us[2],
                   (unsigned long long) w->worst_pts_us[2],
                   (unsigned long long) (now.audio_cursor_us
                                         - was->audio_cursor_us),
                   now.buffering_events - was->buffering_events,
                   (unsigned long long) (now.buffering_us - was->buffering_us),
                   now.resets - was->resets);
    /* Phase totals and the player's and presenter's parts (each part's
       own time; a phase total is its parts' sum). */
    const uint64_t *cpu = swdec_window_cpu_us;
    uint64_t player_us = 0, present_us = 0;
    for (unsigned kind = MEDIA_SWDEC_CPU_PLAYER;
         kind <= MEDIA_SWDEC_CPU_TAKE; kind++)
        player_us += cpu[kind];
    for (unsigned kind = MEDIA_SWDEC_CPU_ROUTE;
         kind <= MEDIA_SWDEC_CPU_SCRIPT; kind++)
        player_us += cpu[kind];
    for (unsigned kind = MEDIA_SWDEC_CPU_PRESENT;
         kind <= MEDIA_SWDEC_CPU_SUBTITLE; kind++)
        present_us += cpu[kind];
    psp_log_printf("tilefinch-swdec-window-work: submitted=%u no-picture=%u "
                   "ready=%u/%u decoded=%u "
                   "displayed=%u dropped=%u drops submit-late=%u skip=%u "
                   "full=%u worker-late=%u csc-failed=%u reduced=%u "
                   "queue-full=%u cpu-us main=%llu worker=%llu audio=%llu "
                   "main-split idle=%llu player=%llu present=%llu compose=%llu submit=%llu "
                   "p-transport=%llu p-clock=%llu p-feed=%llu p-take=%llu "
                   "s-entry=%llu s-probe=%llu s-presented=%llu s-subtitle=%llu p-route=%llu p-prep=%llu p-mid=%llu p-tail=%llu p-script=%llu worker-wall-us decode=%llu csc=%llu/%u job-gaps=%u/%lluus "
                   "audio blocks=%u underruns=%u/%u output-errors=%u "
                   "gaps=%u/%lluus\n",
                   now.submitted - was->submitted,
                   now.no_picture - was->no_picture,
                   was->ready, now.ready,
                   now.decoded - was->decoded,
                   now.displayed - was->displayed, now.dropped - was->dropped,
                   now.submit_drops - was->submit_drops,
                   now.skip_drops - was->skip_drops,
                   now.full_drops - was->full_drops,
                   now.worker_late - was->worker_late,
                   now.csc_failures - was->csc_failures,
                   now.reduced - was->reduced,
                   now.queue_full - was->queue_full,
                   (unsigned long long) (now.cpu_main_us - was->cpu_main_us),
                   (unsigned long long) (now.cpu_worker_us - was->cpu_worker_us),
                   (unsigned long long) (now.cpu_audio_us - was->cpu_audio_us),
                   (unsigned long long) (swdec_window_cpu_us[0] + player_us),
                   (unsigned long long) player_us,
                   (unsigned long long) present_us,
                   (unsigned long long) cpu[MEDIA_SWDEC_CPU_COMPOSE],
                   (unsigned long long) cpu[MEDIA_SWDEC_CPU_SUBMIT],
                   (unsigned long long) cpu[MEDIA_SWDEC_CPU_TRANSPORT],
                   (unsigned long long) cpu[MEDIA_SWDEC_CPU_CLOCK],
                   (unsigned long long) cpu[MEDIA_SWDEC_CPU_FEED],
                   (unsigned long long) cpu[MEDIA_SWDEC_CPU_TAKE],
                   (unsigned long long) cpu[MEDIA_SWDEC_CPU_ENTRY],
                   (unsigned long long) (cpu[MEDIA_SWDEC_CPU_PROBE]
                                         + cpu[MEDIA_SWDEC_CPU_SUBTITLE]),
                   (unsigned long long) cpu[MEDIA_SWDEC_CPU_PRESENTED],
                   (unsigned long long) cpu[MEDIA_SWDEC_CPU_SUBTITLE],
                   (unsigned long long) cpu[MEDIA_SWDEC_CPU_ROUTE],
                   (unsigned long long) cpu[MEDIA_SWDEC_CPU_PREP],
                   (unsigned long long) cpu[MEDIA_SWDEC_CPU_MID],
                   (unsigned long long) cpu[MEDIA_SWDEC_CPU_TAIL],
                   (unsigned long long) cpu[MEDIA_SWDEC_CPU_SCRIPT],
                   (unsigned long long) (now.decode_us - was->decode_us),
                   (unsigned long long) (now.csc_us - was->csc_us),
                   now.publishes - was->publishes,
                   now.job_gaps - was->job_gaps,
                   (unsigned long long) (now.job_gap_us - was->job_gap_us),
                   now.audio_blocks - was->audio_blocks,
                   now.underruns - was->underruns,
                   now.underruns_queued - was->underruns_queued,
                   now.sent_errors - was->sent_errors,
                   now.audio_gaps - was->audio_gaps,
                   (unsigned long long) (now.audio_gap_us - was->audio_gap_us));
    swdec_window_cpu_on = false;
    psp_swdec_component_window_step();
}

/* The window opens: what it runs on, then the baseline figures. */
static void swdec_window_open(PspSwdecBackend *backend)
{
    unsigned high = 0, pooled = 0, allocated = 0;
    for (unsigned i = 0; i < SWDEC_RING_SLOTS; i++) {
        if (backend->rgb_slots[i] != NULL) allocated++;
        if (i >= backend->rgb_slot_count) continue;
        if (backend->rgb_slots[i] != NULL
            && !swdec_me_visible(backend->rgb_slots[i],
                                 backend->rgb_slot_bytes)) high++;
        if (backend->rgb_slot_pooled[i]) pooled++;
    }
    psp_log_printf("tilefinch-swdec-window-id: video=%ux%u slots=%u high=%u "
                   "pooled=%u slot-bytes=%u full-quality=%d keep-high=%d "
                   "speed=%d budget-free=%u me-pool-free=%u allocated=%u\n",
                   (unsigned) backend->width, (unsigned) backend->height,
                   backend->rgb_slot_count, high, pooled,
                   (unsigned) backend->rgb_slot_bytes,
                   swdec_full_quality ? 1 : 0, swdec_keep_high_slots ? 1 : 0,
                   backend->speed, (unsigned) budget_remaining(backend->budget),
                   (unsigned) media_psp_me_pool_claimable(), allocated);
    swdec_window_take(backend, &backend->window_open);
    memset(swdec_holds.holds, 0, sizeof(swdec_holds.holds));
    memset(swdec_window_cpu_us, 0, sizeof(swdec_window_cpu_us));
    swdec_window_cpu_on = true;
    psp_swdec_component_window_step();
}
#endif

static void swdec_note_displayed(
    void *opaque, const MediaVideoFrame *frame, int present_path)
{
    (void) present_path;
    PspSwdecBackend *backend = opaque;
    if (backend == NULL || frame == NULL) return;
    uint64_t end = frame->pts_us > UINT64_MAX - frame->duration_us
        ? UINT64_MAX : frame->pts_us + frame->duration_us;
    swdec_published_write(&backend->shown_end_us, end);
    /* The presenter reports every present, including the same picture
       drawn again when something else redraws the screen. Only a new
       picture counts as displayed, so a redraw storm cannot look like
       faster playback; the redraws are counted apart. */
    if (frame->identity == backend->last_displayed_identity) {
        backend->displayed_repeats++;
#ifdef TILEFINCH_PSP_VALIDATION_LOG
        swdec_hold_repeat_us = swdec_hold_now();
#endif
        return;
    }
    backend->last_displayed_identity = frame->identity;
    atomic_fetch_add_explicit(
        &backend->displayed_count, 1u, memory_order_relaxed);
#ifdef TILEFINCH_PSP_VALIDATION_LOG
    uint64_t shown_us = sceKernelGetSystemTimeWide();
    {
        SwdecHoldPic *picture = swdec_hold_find(&swdec_holds, frame->pts_us);
        if (picture != NULL) {
            picture->fate = SWDEC_HOLD_SHOWN;
            picture->shown_us = (uint32_t) shown_us;
        }
        if (backend->window.phase == SWDEC_WINDOW_OPEN) {
            uint64_t hold = shown_us - backend->window.last_us;
            SwdecHold record = {
                .hold_us = hold > UINT32_MAX ? UINT32_MAX : (uint32_t) hold,
                .pts_us = frame->pts_us,
                .shown_us = (uint32_t) shown_us,
                .take_gap_us = swdec_hold_take_gap_us,
                .gap_wait_us = swdec_hold_gap_wait_us,
                .repeat_age_us = swdec_hold_repeat_us == 0 ? 0
                    : (uint32_t) shown_us - swdec_hold_repeat_us,
                .ready = atomic_load_explicit(
                    &backend->ready_count, memory_order_relaxed),
                .queued = backend->au_count
            };
            swdec_hold_capture(&swdec_holds, &record);
        }
        swdec_hold_take_gap_us = 0;
        swdec_hold_gap_wait_us = 0;
        swdec_hold_repeat_us = 0;
    }
    SwdecWindowNote note = swdec_window_note(
        &backend->window, shown_us, frame->pts_us,
        (uint32_t) frame->duration_us);
    if (note == SWDEC_WINDOW_NOTE_OPENED) swdec_window_open(backend);
    else if (note == SWDEC_WINDOW_NOTE_CLOSED)
        swdec_window_report(backend, true);
#endif
}

static void swdec_note_quiesced(
    void *opaque, const MediaVideoFrame *frame)
{
    PspSwdecBackend *backend = opaque;
    if (backend == NULL || frame == NULL || frame->slot < 0
        || frame->slot >= (int) SWDEC_RING_SLOTS) return;
    swdec_free_slot(backend, frame->slot);
    if (backend->last_claimed_slot == frame->slot)
        backend->last_claimed_slot = -1;
}

static bool swdec_release_slot(
    void *opaque, unsigned slot, uint32_t generation)
{
    PspSwdecBackend *backend = opaque;
    if (backend == NULL || slot >= SWDEC_RING_SLOTS) return false;
    SwdecVideoSlot *state = &backend->slots[slot];
    if (state->generation != generation
        || atomic_load_explicit(&state->readers, memory_order_acquire) != 0)
        return false;
    swdec_free_slot(backend, (int) slot);
    if (backend->last_claimed_slot == (int) slot)
        backend->last_claimed_slot = -1;
    return true;
}

static const MediaBackendPresentationOps swdec_presentation_ops = {
    .borrow = swdec_borrow,
    .release = swdec_release,
    .end_auxiliary_read = swdec_release,
    .quarantine = swdec_quarantine,
    .release_quarantine = swdec_release_quarantine,
    .is_quarantined = swdec_is_quarantined,
    .note_displayed = swdec_note_displayed,
    .note_quiesced = swdec_note_quiesced
};

static bool swdec_surfaces_unborrowed(const PspSwdecBackend *backend)
{
    for (unsigned i = 0; i < SWDEC_RING_SLOTS; i++) {
        if (atomic_load_explicit(
                &backend->slots[i].readers, memory_order_acquire) != 0)
            return false;
    }
    return true;
}

/* The body of a seek reset, run once nothing is in flight: no decode job,
   no borrowed surface, no PCM block the audio output may still read, no
   audio unit the Media Engine has yet to decode. Replaces the audio decoder
   and its PCM ring, reopens the video decoder and hands every surface back
   for the new epoch. */
static bool swdec_reset_perform(
    PspSwdecBackend *backend, char *error, size_t error_size)
{
    if (backend->decoder != NULL) {
        backend->api->csc_off();
    }
    if (backend->have_audio
        && backend->api->audio_shutdown(2000000u) != 0) {
        swdec_error(error, error_size,
                    "software decoder audio queue did not quiesce");
        return false;
    }
    if (backend->have_audio) {
#ifdef TILEFINCH_PSP_VALIDATION_LOG
        /* The queue is quiet: every packet submitted has been decoded. */
        swdec_audio_bad_scan(
            &backend->trace_audio_bad, &backend->trace_audio_checked,
            backend->api->audio_done(), backend->api->audio_poll,
            backend->audio_packets, backend->audio_packet_stride,
            SWDEC_AUDIO_PACKET_SLOTS, backend->trace_audio_meta);
        backend->trace_audio_checked = 0;
#endif
        backend->api->bind_aux_arena(
            backend->aux_arena, backend->aux_arena_bytes);
        if (backend->api->audio_setup() != 0) {
            swdec_error(error, error_size,
                        "software decoder audio reopen failed at seek");
            return false;
        }
        /* The audio thread is parked behind the fence: the error record
           is the browser's until the fence is lowered. A given-up track
           gets a fresh decoder and, within its retry budget, another
           chance; past it the silent clock restarts at the new position
           (swdec_silent_clock_start). */
        if (atomic_load_explicit(&backend->audio_failed,
                                 memory_order_acquire)
            && swdec_audio_errors_seek(&backend->audio_errors)) {
            atomic_store_explicit(&backend->audio_failed, false,
                                  memory_order_release);
#ifdef TILEFINCH_PSP_VALIDATION_LOG
            psp_log_printf("tilefinch-swdec-audio: event=retry-after-seek "
                           "retry=%u/%u\n",
                           backend->audio_errors.seek_retries,
                           SWDEC_AUDIO_SEEK_RETRIES);
#endif
        } else {
            backend->audio_errors.run = 0;
        }
        atomic_store_explicit(&backend->audio_origin_ready, false,
                              memory_order_release);
    }
    if (backend->decoder != NULL) {
        backend->api->close(backend->decoder);
        backend->decoder = NULL;
        backend->decoder = backend->api->open(
            backend->decoder_arena, backend->decoder_arena_bytes,
            (int) backend->width, (int) backend->height, backend->arena_refs);
        if (backend->decoder == NULL) {
            swdec_error(error, error_size,
                        "software decoder reopen failed at seek");
            return false;
        }
    }
    memset(backend->au_slots, 0, sizeof(backend->au_slots));
    for (unsigned i = 0; i < SWDEC_RING_SLOTS; i++) {
        SwdecVideoSlot *slot = &backend->slots[i];
        slot->claimed = false;
        slot->quarantined = false;
        atomic_store_explicit(
            &slot->retire_when_unborrowed, false, memory_order_release);
        atomic_store_explicit(
            &slot->state, SWDEC_SLOT_FREE, memory_order_release);
    }
    backend->last_claimed_slot = -1;
    backend->drained = false;
    swdec_au_clear(backend);
    backend->clock_slip_us = 0;
    backend->speed = -1;
    backend->epoch++;
    if (backend->epoch == 0) backend->epoch = 1;
    atomic_store_explicit(&backend->ready_count, 0u, memory_order_release);
    atomic_store_explicit(
        &backend->audio_input_ended, false, memory_order_release);
    atomic_store_explicit(
        &backend->audio_submitted, 0u, memory_order_release);
    atomic_store_explicit(&backend->audio_output, 0u, memory_order_release);
    atomic_fetch_add_explicit(
        &backend->audio_epoch, 1u, memory_order_acq_rel);
    /* The audio thread is parked behind the fence and publishes nothing
       until it is lowered, which orders these writes before its next. */
    swdec_published_write(&backend->audio_cursor_us, 0);
    swdec_audio_clock_write(&backend->audio_dac_clock, 0, 0, 0);
    swdec_published_write(&backend->audio_origin_us, 0);
    /* Sound resumes only once the first picture of the new position has
       reached the LCD, as at startup. Left set, the end of the last picture
       before the seek let audio restart as soon as 20 frames were decoded,
       while video was still decoding forward from the keyframe: the clock
       ran ahead and the catch-up pictures were dropped as late. */
    swdec_published_write(&backend->shown_end_us, 0);
    atomic_store_explicit(
        &backend->worker_error, 0, memory_order_release);
    return true;
}

/* Browser thread: drive a pending seek reset. ACCEPTED when none is pending
   (or it has just completed), WOULD_BLOCK while it waits, ERROR when it
   failed or waited past its deadline. */
static MediaBackendResult swdec_reset_progress(
    PspSwdecBackend *backend, char *error, size_t error_size)
{
    if (backend->reset_failed) {
        swdec_error(error, error_size, "software decoder reset failed");
        return MEDIA_BACKEND_ERROR;
    }
    if (!backend->reset_gate.pending) return MEDIA_BACKEND_ACCEPTED;
    /* Finished units belong to the stream being left: their pictures are
       discarded with the slots below and their errors are not reported. */
    int collected;
    do collected = swdec_job_ring_collect(&backend->ring);
    while (collected >= 0);
    bool audio = backend->have_audio;
    SwdecResetInputs inputs = {
        .jobs_idle = collected == SWDEC_JOB_RING_EMPTY,
        .surfaces_idle = swdec_surfaces_unborrowed(backend),
        .audio_parked = !audio
            || swdec_audio_fence_parked(&backend->audio_fence),
        .media_engine_idle = !audio
            || backend->api->audio_done() >= atomic_load_explicit(
                   &backend->audio_submitted, memory_order_acquire)
    };
    uint32_t now_us = sceKernelGetSystemTimeLow();
    switch (swdec_reset_gate_poll(&backend->reset_gate, inputs, now_us)) {
    case SWDEC_RESET_IDLE:
        return MEDIA_BACKEND_ACCEPTED;
    case SWDEC_RESET_WAIT:
        return MEDIA_BACKEND_WOULD_BLOCK;
    case SWDEC_RESET_TIMEOUT:
        psp_log_printf("tilefinch-swdec-reset: event=timeout waited=%uus "
                       "deadline=%uus coalesced=%u jobs=%d surfaces=%d "
                       "audio-parked=%d me-idle=%d\n",
                       (unsigned) (now_us - backend->reset_gate.requested_us),
                       (unsigned) backend->reset_gate.deadline_us,
                       backend->reset_gate.coalesced,
                       inputs.jobs_idle, inputs.surfaces_idle,
                       inputs.audio_parked, inputs.media_engine_idle);
        swdec_error(error, error_size,
                    !inputs.jobs_idle
                        ? "software decoder is still finishing an access unit"
                    : !inputs.surfaces_idle
                        ? "software decoder surface is still borrowed"
                    : !inputs.audio_parked
                        ? "software decoder audio output did not stop for seek"
                        : "software decoder audio queue did not quiesce");
        return MEDIA_BACKEND_ERROR;
    case SWDEC_RESET_READY:
        break;
    }
    if (!swdec_reset_perform(backend, error, error_size)) {
        /* Half replaced: nothing may play from here; the fence stays up so
           the audio thread keeps away from the ring. */
        backend->reset_failed = true;
        return MEDIA_BACKEND_ERROR;
    }
    psp_log_printf("tilefinch-swdec-reset: event=complete waited=%uus "
                   "coalesced=%u\n",
                   (unsigned) (now_us - backend->reset_gate.requested_us),
                   backend->reset_gate.coalesced);
    swdec_reset_gate_complete(&backend->reset_gate);
    swdec_audio_fence_lower(&backend->audio_fence);
    return MEDIA_BACKEND_ACCEPTED;
}

/* A seek. Requests the reset and returns without waiting: up to two
   decode jobs can be in flight (tens of milliseconds each), and the audio
   output must let go of the PCM ring first. Later submit, drain and advance
   calls complete it (swdec_reset_progress); until then they hold the
   stream being entered, and nothing of the stream being left is presented.
   A seek while one is pending folds into it. */
static bool swdec_reset(void *opaque, char *error, size_t error_size)
{
    PspSwdecBackend *backend = opaque;
    if (backend == NULL) return false;
#ifdef TILEFINCH_PSP_VALIDATION_LOG
    backend->trace_resets++;
#endif
    if (backend->reset_failed) {
        swdec_error(error, error_size, "software decoder reset failed");
        return false;
    }
    if (swdec_reset_gate_request(
            &backend->reset_gate, sceKernelGetSystemTimeLow())
        && backend->have_audio)
        (void) swdec_audio_fence_raise(&backend->audio_fence);
    /* Nothing more goes to the worker; what is queued belongs to the stream
       being left. */
    swdec_au_clear(backend);
    backend->drained = false;
    /* Often everything is already quiet (no audio, worker idle). */
    return swdec_reset_progress(backend, error, error_size)
        != MEDIA_BACKEND_ERROR;
}

static void swdec_destroy(void *opaque)
{
    PspSwdecBackend *backend = opaque;
    if (backend == NULL) return;
    atomic_store_explicit(&backend->stop, true, memory_order_release);
    if (backend->worker_started && backend->worker_event >= 0)
        (void) sceKernelSetEventFlag(backend->worker_event, SWDEC_EVENT_WAKE);
    bool worker_stopped = !backend->worker_started
        || psp_thread_wait_end_bounded(
               backend->worker_thread, SWDEC_WORKER_STOP_WAIT_US) >= 0;
    bool audio_stopped = !backend->audio_started
        || psp_thread_wait_end_bounded(
               backend->audio_thread, 2000000u) >= 0;
    if (!worker_stopped || !audio_stopped) {
        swdec_quarantined = true;
        return;
    }
    if (!swdec_release_audio_channel(backend)) {
        /* Never released: the audio service may still own the last PCM
           block, so nothing it could read is freed, as on the firmware
           backend. */
        swdec_quarantined = true;
        return;
    }
#ifdef TILEFINCH_PSP_VALIDATION_LOG
    /* A window still open when playback stops reports what it covered. */
    if (backend->window.phase == SWDEC_WINDOW_OPEN)
        swdec_window_report(backend, false);
    /* The saved holds are immutable after the window closes. Formatting
       their picture histories during playback can itself delay the next
       frame or overflow the bounded timing log. Print them only once the
       decoder and audio workers have stopped. */
    if (backend->window.phase >= SWDEC_WINDOW_OPEN)
        swdec_hold_report();
    /* decode-mean against the clip's frame period answers whether the
       software decoder keeps up; late-reanchors counts takes that found the
       oldest ready picture more than 8 ms behind the clock (the elastic
       clock slipped), and reduced-decodes counts decodes at a faster,
       lower-quality speed rung. */
    {
        TilefinchSwdecStats decoder_stats = {0};
        if (backend->decoder != NULL && backend->api->stats != NULL)
            backend->api->stats(backend->decoder, &decoder_stats);
        psp_log_printf("tilefinch-swdec-decode: pictures=%u empty=%u "
                       "errors=%u first-error=%d csc-prebound=%u/%d "
                       "prebind-jobs=%u prebind-mismatch=%u prebind-close-zero=%u "
                       "decoder-aus=%u "
                       "decoder-frames=%u decoder-errors=%u arena-peak=%u/%u\n",
                       (unsigned) backend->trace_decode_pictures,
                       (unsigned) backend->trace_decode_empty,
                       (unsigned) backend->trace_decode_errors,
                       backend->trace_decode_first_error,
                       (unsigned) backend->trace_csc_prebound,
                       backend->csc_prebind ? 1 : 0,
                       (unsigned) backend->trace_prebind_jobs,
                       (unsigned) backend->trace_prebind_mismatch,
                       (unsigned) backend->trace_prebind_close_zero,
                       (unsigned) decoder_stats.aus_in,
                       (unsigned) decoder_stats.frames_out,
                       (unsigned) decoder_stats.errors,
                       (unsigned) decoder_stats.peak_bytes,
                       (unsigned) decoder_stats.arena_bytes);
    }
    psp_log_printf("tilefinch-swdec-cadence: video=%ux%u decoded=%zu "
           "displayed=%u video-packets=%zu "
           "decode=%u/%lluus/%uus decode-mean=%lluus "
           "reduced-decodes=%u speed-changes=%u "
           "late-reanchors=%u late-max=%lluus slip=%lluus "
           "csc=%u/%lluus job-gap=%u/%lluus/%uus "
           "job-depth=%u chained=%u worker-late-drops=%u "
           "csc-failures=%u me-recoveries=%u\n",
           backend->width, backend->height,
           backend->stats.decoded_video_frames,
           atomic_load_explicit(
               &backend->displayed_count, memory_order_acquire),
           backend->stats.submitted_video_packets,
           (unsigned) backend->trace_decode_jobs,
           (unsigned long long) backend->trace_decode_total_us,
           (unsigned) backend->trace_decode_max_us,
           backend->trace_decode_jobs == 0 ? 0ull
               : (unsigned long long) (backend->trace_decode_total_us
                     / backend->trace_decode_jobs),
           (unsigned) backend->trace_reduced_decodes,
           (unsigned) backend->trace_speed_changes,
           (unsigned) backend->trace_late_reanchors,
           (unsigned long long) backend->trace_late_max_us,
           (unsigned long long) backend->clock_slip_us,
           (unsigned) backend->trace_publish_count,
           (unsigned long long) backend->trace_publish_total_us,
           (unsigned) backend->trace_job_gaps,
           (unsigned long long) backend->trace_job_gap_total_us,
           (unsigned) backend->trace_job_gap_max_us,
           backend->ring.depth, (unsigned) backend->trace_job_chained,
           (unsigned) backend->trace_worker_late_drops,
           (unsigned) backend->trace_csc_failures,
           (unsigned) backend->trace_me_recoveries);
    {
        /* Wall time from open to teardown against the audio clock reached:
           the ratio is 1.0 when playback keeps real time. */
        uint64_t wall_us = sceKernelGetSystemTimeWide()
            - backend->trace_created_us;
        uint64_t clock_us = backend->presentation_clock_us;
        unsigned displayed = atomic_load_explicit(
            &backend->displayed_count, memory_order_acquire);
        uint64_t now_us = sceKernelGetSystemTimeWide();
        uint64_t startup_us = backend->trace_audio_started_us != 0
            ? backend->trace_audio_started_us - backend->trace_created_us
            : backend->have_audio ? wall_us : 0;
        /* Playing time ends with the last audio block, not at teardown:
           the backend can outlive a closed player by seconds (a scripted
           run tears it down only at application exit). */
        uint64_t playing_us = wall_us > startup_us ? wall_us - startup_us : 0;
        if (backend->trace_audio_blocks != 0
            && backend->audio_info.sample_rate != 0)
            playing_us = backend->trace_audio_last_us
                - backend->trace_audio_started_us
                + UINT64_C(1024000000) / backend->audio_info.sample_rate;
        uint64_t buffering_us = backend->trace_buffering_total_us;
        if (backend->trace_buffering_since_us != 0)
            buffering_us += now_us - backend->trace_buffering_since_us;
        /* Sound actually produced against the time it took, from the first
           block to the last one: independent of how long teardown waited
           after the player was closed. */
        uint64_t block_us = backend->audio_info.sample_rate == 0 ? 0
            : UINT64_C(1024000000) / backend->audio_info.sample_rate;
        uint64_t sound_us = (uint64_t) backend->trace_audio_blocks * block_us;
        uint64_t span_us = backend->trace_audio_blocks == 0 ? 0
            : backend->trace_audio_last_us - backend->trace_audio_started_us;
        uint64_t after_us = backend->trace_audio_last_us != 0
            ? now_us - backend->trace_audio_last_us : 0;
        /* gaps: intervals between consecutive output returns while playing
           that exceeded one block by more than SWDEC_AUDIO_GAP_THRESHOLD_US;
           gap-time sums that excess; gap-hist buckets every interval's
           excess at <2, <5, <10, <23, <46 and >=46 ms. */
        const SwdecAudioGapStats *gap = &backend->trace_gap;
        psp_log_printf("tilefinch-swdec-audio-span: sound=%lluus span=%lluus "
                       "audio-ratio-permille=%llu gaps=%u gap-time=%lluus "
                       "gap-max=%uus gap-intervals=%u "
                       "gap-hist=%u,%u,%u,%u,%u,%u "
                       "paused-blocks=%u after-last-block=%lluus\n",
                       (unsigned long long) sound_us,
                       (unsigned long long) span_us,
                       span_us == 0 ? 0ull
                           : (unsigned long long) (sound_us * 1000u / span_us),
                       (unsigned) gap->gaps,
                       (unsigned long long) gap->gap_total_us,
                       (unsigned) gap->max_excess_us,
                       (unsigned) gap->intervals,
                       (unsigned) gap->histogram[0],
                       (unsigned) gap->histogram[1],
                       (unsigned) gap->histogram[2],
                       (unsigned) gap->histogram[3],
                       (unsigned) gap->histogram[4],
                       (unsigned) gap->histogram[5],
                       (unsigned) backend->trace_audio_paused_blocks,
                       (unsigned long long) after_us);
        psp_log_printf("tilefinch-swdec-realtime: wall=%lluus startup=%lluus "
                       "playing=%lluus clock=%lluus ratio-permille=%llu "
                       "buffering=%lluus/%u full-drops=%u "
                       "displayed=%u repeats=%u fps-x10=%llu "
                       "late-drops=%u skip-drops=%u queue-peak=%u "
                       "queue-full=%u queue-bytes=%zu\n",
                       (unsigned long long) wall_us,
                       (unsigned long long) startup_us,
                       (unsigned long long) playing_us,
                       (unsigned long long) clock_us,
                       playing_us == 0 ? 0ull
                           : (unsigned long long) (clock_us * 1000u
                                                   / playing_us),
                       (unsigned long long) buffering_us,
                       (unsigned) backend->trace_buffering_events,
                       (unsigned) backend->trace_full_drops,
                       displayed, (unsigned) backend->displayed_repeats,
                       playing_us == 0 ? 0ull
                           : (unsigned long long) ((uint64_t) displayed
                                 * 10000000u / playing_us),
                       (unsigned) backend->trace_submit_drops,
                       (unsigned) backend->trace_skip_drops,
                       (unsigned) backend->trace_queue_peak,
                       (unsigned) backend->trace_queue_full,
                       backend->au_bytes_capacity);
    }
    {
        char peaks[64u * 6u + 1u];
        size_t used = 0;
        peaks[0] = '\0';
        for (unsigned i = 0; i < backend->trace_sent_seconds; i++) {
            int wrote = snprintf(peaks + used, sizeof peaks - used, "%s%u",
                                 i == 0 ? "" : ",",
                                 (unsigned) backend->trace_sent_second_peak[i]);
            if (wrote <= 0 || (size_t) wrote >= sizeof peaks - used) break;
            used += (size_t) wrote;
        }
        /* silent-blocks and peak-per-second need the per-block scan
           (validation_swdec_audio_scan=1); scan=off leaves them empty. */
        psp_log_printf("tilefinch-swdec-audio-clock: reads=%u errors=%u max-samples=%u\n",
                       backend->trace_dac_reads, backend->trace_dac_errors,
                       backend->trace_dac_max_samples);
        psp_log_printf("tilefinch-swdec-audio-sent: channel=%s rate=%u "
                       "scan=%s silent-blocks=%u output-errors=%u "
                       "first-error=0x%08x peak-per-second=%s\n",
                       psp_media_audio_prefer_standard_channel(
                           backend->audio_info.sample_rate) ? "standard" : "src",
                       (unsigned) backend->audio_info.sample_rate,
                       backend->trace_audio_scan ? "on" : "off",
                       (unsigned) backend->trace_sent_silent,
                       (unsigned) backend->trace_sent_errors,
                       (unsigned) backend->trace_sent_first_error, peaks);
    }
    if (backend->have_audio) {
        /* Packets decoded since the last submit. */
        swdec_audio_bad_scan(
            &backend->trace_audio_bad, &backend->trace_audio_checked,
            backend->api->audio_done(), backend->api->audio_poll,
            backend->audio_packets, backend->audio_packet_stride,
            SWDEC_AUDIO_PACKET_SLOTS, backend->trace_audio_meta);
    }
    {
        const SwdecAudioBadPacket *bad = &backend->trace_audio_bad;
        char head[SWDEC_AUDIO_BAD_HEAD_BYTES * 2u + 1u];
        size_t shown = bad->bytes < SWDEC_AUDIO_BAD_HEAD_BYTES
            ? bad->bytes : SWDEC_AUDIO_BAD_HEAD_BYTES;
        head[0] = '\0';
        for (size_t i = 0; i < shown; i++)
            snprintf(head + i * 2u, 3u, "%02x", bad->head[i]);
        psp_log_printf("tilefinch-swdec-audio-decode: decoded=%u errors=%u "
                       "first-error=%d longest-run=%u failed=%d "
                       "seek-retries=%u first-bad-packet=%s "
                       "bad-index=%u bad-bytes=%u bad-pts-us=%llu\n",
                       backend->audio_errors.decoded,
                       backend->audio_errors.errors,
                       backend->audio_errors.first_error,
                       backend->audio_errors.longest_run,
                       atomic_load(&backend->audio_failed) ? 1 : 0,
                       backend->audio_errors.seek_retries,
                       bad->captured ? head : "none",
                       bad->index, bad->bytes,
                       (unsigned long long) bad->pts_us);
    }
    psp_log_printf("tilefinch-swdec-audio: blocks=%u underruns=%u "
                   "underruns-me-behind=%u submitted=%u decoded=%u "
                   "submit-drops=%u dropped=%zu\n",
                   (unsigned) backend->trace_audio_blocks,
                   (unsigned) backend->trace_audio_underruns,
                   (unsigned) backend->trace_audio_underruns_queued,
                   atomic_load_explicit(&backend->audio_submitted,
                                        memory_order_acquire),
                   backend->api->audio_done(),
                   (unsigned) backend->trace_submit_drops,
                   backend->stats.dropped_video_frames);
    for (unsigned i = 0; i < backend->trace_audio_first; i++)
        psp_log_printf("tilefinch-swdec-audio-block: n=%u crc=%08x "
                       "samples=%u peak=%u\n", i,
                       (unsigned) backend->trace_audio[i].crc,
                       (unsigned) backend->trace_audio[i].samples,
                       (unsigned) backend->trace_audio[i].peak);
#endif
    if (backend->worker_thread >= 0)
        sceKernelDeleteThread(backend->worker_thread);
    if (backend->audio_thread >= 0)
        sceKernelDeleteThread(backend->audio_thread);
    if (backend->worker_event >= 0)
        sceKernelDeleteEventFlag(backend->worker_event);
    if (backend->ready_event >= 0)
        sceKernelDeleteEventFlag(backend->ready_event);
    if (backend->decoder != NULL) {
        backend->api->csc_off();
    }
    if (backend->audio_arena_bound
        && backend->api->audio_shutdown(2000000u) != 0) {
        swdec_quarantined = true;
        return;
    }
    if (backend->decoder != NULL) backend->api->close(backend->decoder);
    budget_reservation_release(&backend->worker_stack_reservation);
    if (backend->audio_packets_pooled) {
        media_psp_me_pool_unclaim(backend->audio_packets);
        budget_reservation_release(&backend->audio_packets_reservation);
    } else {
        budget_free(backend->budget, backend->audio_packets);
    }
    for (unsigned i = 0; i < SWDEC_JOB_RING_MAX; i++)
        budget_free(backend->budget, backend->jobs[i].packet);
    budget_free(backend->budget, backend->au_bytes);
    budget_free(backend->budget, backend->parameter_sets);
    for (unsigned i = 0; i < SWDEC_RING_SLOTS; i++) {
        if (backend->rgb_slot_pooled[i])
            media_psp_me_pool_unclaim(backend->rgb_slots[i]);
        else
            budget_free(backend->budget, backend->rgb_slots[i]);
    }
    budget_reservation_release(&backend->rgb_pool_reservation);
    if (backend->aux_arena_pooled) {
        media_psp_me_pool_unclaim(backend->aux_arena);
        budget_reservation_release(&backend->aux_arena_reservation);
    } else {
        budget_free(backend->budget, backend->aux_arena);
    }
    if (backend->decoder_arena_pooled) {
        media_psp_me_pool_unclaim(backend->decoder_arena);
        budget_reservation_release(&backend->decoder_arena_reservation);
    } else {
        budget_free(backend->budget, backend->decoder_arena);
    }
    budget_free(backend->budget, backend);
}

bool media_psp_swdec_backend_create_sources(
    Budget *budget, const MediaSampleSource *video_source,
    const MediaSampleSource *audio_source,
    const TilefinchSwdecComponentApi *component,
    MediaBackend *backend_out, char *error, size_t error_size)
{
    if (error != NULL && error_size != 0) error[0] = '\0';
    swdec_last_working_set_bytes = 0;
    if (budget == NULL || video_source == NULL || video_source->ops == NULL
        || component == NULL
        || backend_out == NULL || swdec_quarantined) {
        swdec_error(error, error_size, "software decoder unavailable");
        return false;
    }
    MediaMp4TrackInfo video = {0};
    MediaMp4TrackInfo audio = {0};
    bool have_video = false, have_audio = false;
    const MediaSampleSource *sources[2] = {video_source, audio_source};
    size_t source_count = audio_source == NULL ? 1u : 2u;
    for (size_t source = 0; source < source_count; source++) {
        if (sources[source] == NULL || sources[source]->ops == NULL) continue;
        for (size_t i = 0;
             i < sources[source]->ops->track_count(sources[source]->opaque);
             i++) {
            MediaMp4TrackInfo info;
            if (!sources[source]->ops->track_info(
                    sources[source]->opaque, i, &info)) continue;
            if (!have_video && info.kind == MEDIA_MP4_TRACK_VIDEO
                && info.codec == MEDIA_MP4_FOURCC('a','v','c','1')) {
                video = info;
                have_video = true;
            } else if (!have_audio && info.kind == MEDIA_MP4_TRACK_AUDIO
                       && info.codec == MEDIA_MP4_FOURCC('m','p','4','a')) {
                audio = info;
                have_audio = true;
            }
        }
    }
    uint8_t profile = 0;
    uint16_t width = 0, height = 0;
    uint8_t nal_length = 0;
    bool annex_b = video.packet_format == MEDIA_PACKET_FORMAT_H264_ANNEX_B;
    if (annex_b) {
        width = video.width;
        height = video.height;
        for (size_t i = 0; i + 4u < video.codec_config_length; i++) {
            size_t prefix = video.codec_config[i] == 0
                && video.codec_config[i + 1u] == 0
                && video.codec_config[i + 2u] == 1u ? 3u
                : video.codec_config[i] == 0
                  && video.codec_config[i + 1u] == 0
                  && video.codec_config[i + 2u] == 0
                  && video.codec_config[i + 3u] == 1u ? 4u : 0u;
            if (prefix != 0
                && (video.codec_config[i + prefix] & 0x1fu) == 7u
                && i + prefix + 1u < video.codec_config_length) {
                profile = video.codec_config[i + prefix + 1u];
                break;
            }
        }
    }
    if (!have_video
        || (!annex_b
            && (media_h264_avcc_decoder_route(
                    video.codec_config, video.codec_config_length, &profile)
                  == MEDIA_H264_DECODER_ROUTE_UNSUPPORTED
                || !media_h264_avcc_dimensions(
                    video.codec_config, video.codec_config_length,
                    &width, &height, &nal_length)))
        || width == 0 || height == 0
        || width > SWDEC_MAX_WIDTH || height > SWDEC_MAX_HEIGHT) {
        swdec_error(error, error_size,
                    "software H.264 is limited to 432x240");
        return false;
    }
    (void) profile;
    if (media_h264_config_software_coding(
            video.codec_config, video.codec_config_length, annex_b)
        == MEDIA_H264_SOFTWARE_CODING_UNSUPPORTED) {
        swdec_error(error, error_size, "%s",
                    MEDIA_H264_SOFTWARE_CODING_REFUSAL);
        return false;
    }
    unsigned declared_frames = 0;
    MediaH264SpsFramesStatus declared = media_h264_config_sps_frames(
        video.codec_config, video.codec_config_length, annex_b,
        &declared_frames);
    int arena_refs = swdec_arena_refs(
        declared == MEDIA_H264_SPS_FRAMES_KNOWN, declared_frames);
    PspSwdecBackend *backend = budget_calloc_category(
        budget, BUDGET_CATEGORY_NAVIGATION, 1, sizeof(*backend));
    if (backend == NULL) {
        swdec_error(error, error_size, "swdec state exceeds budget");
        return false;
    }
    backend->budget = budget;
    backend->api = component;
    backend->worker_event = -1;
    backend->ready_event = -1;
    backend->worker_thread = -1;
    backend->audio_thread = -1;
    atomic_init(&backend->audio_channel, -1);
#ifdef TILEFINCH_PSP_VALIDATION_LOG
    memset(&swdec_holds, 0, sizeof(swdec_holds));
    swdec_hold_last_take_us = swdec_hold_take_gap_us = 0;
    swdec_presenter_wait_total = swdec_take_wait_at = swdec_hold_gap_wait_us = 0;
    swdec_hold_repeat_us = 0;
    backend->trace_browser_thread = sceKernelGetThreadId();
#endif
    backend->last_claimed_slot = -1;
    backend->width = width;
    backend->height = height;
    backend->rgb_stride = (width + 15u) & ~15u;
    backend->nal_length_size = nal_length;
    backend->arena_refs = arena_refs;
    backend->csc_prebind = media_h264_config_reorder_free(
        video.codec_config, video.codec_config_length, annex_b);
    backend->speed = -1;
    backend->epoch = 1;
    memset(backend->au_slots, 0, sizeof(backend->au_slots));
    for (unsigned i = 0; i < SWDEC_RING_SLOTS; i++) {
        atomic_init(&backend->slots[i].state, SWDEC_SLOT_FREE);
        atomic_init(&backend->slots[i].readers, 0u);
        atomic_init(&backend->slots[i].retire_when_unborrowed, false);
    }
    swdec_job_ring_init(&backend->ring, 1u);
    swdec_published_init(&backend->dispatch_clock_us, 0);
    atomic_init(&backend->stop, false);
    atomic_init(&backend->playing, false);
    atomic_init(&backend->buffering, false);
    atomic_init(&backend->audio_input_ended, false);
    atomic_init(&backend->audio_failed, false);
    atomic_init(&backend->audio_origin_ready, false);
    atomic_init(&backend->ready_count, 0u);
    atomic_init(&backend->displayed_count, 0u);
    atomic_init(&backend->audio_submitted, 0u);
    atomic_init(&backend->audio_output, 0u);
    atomic_init(&backend->audio_epoch, 1u);
    swdec_published_init(&backend->audio_cursor_us, 0);
    swdec_audio_clock_init(&backend->audio_dac_clock);
    swdec_published_init(&backend->audio_origin_us, 0);
    swdec_published_init(&backend->shown_end_us, 0);
    swdec_audio_fence_init(&backend->audio_fence);
    swdec_reset_gate_init(&backend->reset_gate, SWDEC_RESET_DEADLINE_US);
    atomic_init(&backend->worker_error, 0);
    atomic_init(&backend->worker_wait_error, 0);

    backend->decoder_arena_bytes = component->arena_bytes(
        (int) width, (int) height, arena_refs);
    backend->rgb_slot_bytes = swdec_cache_extent(
        (size_t) backend->rgb_stride * height * sizeof(uint16_t));
    backend->rgb_ring_bytes = backend->rgb_slot_bytes * SWDEC_RING_SLOTS;
    size_t parameter_capacity = video.codec_config_length
        + SWDEC_PARAMETER_SET_SLACK;
    {
        /* The whole open-time charge, so a caller refused for room knows
           how much to make (the optional second packet buffer aside). */
        size_t queue_bytes = 0, queue_floor = 0;
        (void) swdec_au_queue_plan(video.largest_sample, have_audio,
                                   &queue_bytes, &queue_floor);
        size_t audio_bytes = have_audio
            ? SWDEC_AUX_ARENA_BYTES + swdec_cache_extent(
                  swdec_aac_sample_bound(
                      audio.largest_sample, audio.channels) + 7u)
                * SWDEC_AUDIO_PACKET_SLOTS
            : 0u;
        swdec_last_working_set_bytes = sizeof(*backend) + parameter_capacity
            + backend->decoder_arena_bytes + backend->rgb_ring_bytes
            + video.largest_sample + parameter_capacity + queue_bytes
            + audio_bytes + SWDEC_WORKER_STACK_BYTES + SWDEC_AUDIO_STACK_BYTES;
    }
#ifdef TILEFINCH_PSP_VALIDATION_LOG
    psp_log_printf("tilefinch-swdec: event=working-set video=%ux%u "
                   "declared-frames=%u sps=%d arena-refs=%d arena=%u "
                   "ring=%u total=%u remaining=%u\n",
                   (unsigned) width, (unsigned) height, declared_frames,
                   (int) declared, arena_refs,
                   (unsigned) backend->decoder_arena_bytes,
                   (unsigned) backend->rgb_ring_bytes,
                   (unsigned) swdec_last_working_set_bytes,
                   (unsigned) budget_remaining(budget));
#endif
    /* Largest contiguous block first. The decoder arena (about 3 MB) goes
       to the reserved Media Engine pool when it fits: contiguous, low and
       visible to the Media Engine whatever the page did to the heap. When it
       does not (many references), the smaller audio arena takes the pool
       and the decoder arena a low heap block; both must be in reach. */
    const char *refused = NULL;
    size_t refused_bytes = 0;
    if (budget_reservation_acquire(
            &backend->decoder_arena_reservation, budget,
            BUDGET_CATEGORY_RESOURCE, backend->decoder_arena_bytes)) {
        backend->decoder_arena =
            media_psp_me_pool_claim(backend->decoder_arena_bytes);
        if (backend->decoder_arena != NULL)
            backend->decoder_arena_pooled = true;
        else
            budget_reservation_release(&backend->decoder_arena_reservation);
    }
    if (backend->decoder_arena == NULL && have_audio
        && !swdec_take_aux_arena(backend, budget)) {
        refused = "aux-arena";
        refused_bytes = SWDEC_AUX_ARENA_BYTES;
    }
    if (refused == NULL && backend->decoder_arena == NULL) {
        backend->decoder_arena = swdec_take_low_heap_block(
            budget, BUDGET_CATEGORY_RESOURCE, backend->decoder_arena_bytes,
            "decoder-arena");
        if (backend->decoder_arena == NULL) {
            refused = "arena";
            refused_bytes = backend->decoder_arena_bytes;
        }
    }
    if (refused == NULL && have_audio) {
        if (backend->aux_arena == NULL
            && !swdec_take_aux_arena(backend, budget)) {
            refused = "aux-arena";
            refused_bytes = SWDEC_AUX_ARENA_BYTES;
        } else if (audio.largest_sample <= 16384u) {
            /* Before the ring takes the rest of the pool (the stream's
               admission is checked again below). */
            backend->audio_packet_stride = swdec_cache_extent(
                swdec_aac_sample_bound(audio.largest_sample, audio.channels)
                + (audio.packet_format == MEDIA_PACKET_FORMAT_AAC_ADTS
                       ? 0u : 7u));
            if (!swdec_take_audio_packets(backend, budget)) {
                refused = "audio-packets";
                refused_bytes = backend->audio_packet_stride
                    * SWDEC_AUDIO_PACKET_SLOTS;
            }
        }
    }
    if (refused == NULL) {
        backend->parameter_sets = budget_malloc_category(
            budget, BUDGET_CATEGORY_RESOURCE, parameter_capacity);
        if (backend->parameter_sets == NULL) {
            refused = "parameter-sets";
            refused_bytes = parameter_capacity;
        }
    }
    /* From the largest sample known at open; a later fragment's larger one
       grows the packet (swdec_job_packet_fit). */
    size_t packet_capacity = 0;
    if (!swdec_packet_bytes(
               video.largest_sample, annex_b ? 0u : backend->nal_length_size,
               parameter_capacity, &packet_capacity)) {
        swdec_error(error, error_size, "swdec packet bound overflow");
        swdec_destroy(backend);
        return false;
    }
    if (refused == NULL) {
        backend->jobs[0].packet = budget_malloc_category(
            budget, BUDGET_CATEGORY_RESOURCE, packet_capacity);
        if (backend->jobs[0].packet == NULL) {
            refused = "packet";
            refused_bytes = packet_capacity;
        } else {
            backend->jobs[0].packet_capacity = packet_capacity;
        }
    }
    if (refused == NULL) {
        /* Eight units with audio, a couple without; smaller under memory
           pressure rather than refusing the video (swdec_au_queue_plan). */
        size_t queue_bytes = 0, queue_floor = 0;
        if (swdec_au_queue_plan(video.largest_sample, have_audio,
                                &queue_bytes, &queue_floor)) {
            for (; queue_bytes != 0 && backend->au_bytes == NULL;
                 queue_bytes = swdec_au_queue_step_down(
                     queue_bytes, queue_floor)) {
                backend->au_bytes = budget_malloc_category(
                    budget, BUDGET_CATEGORY_RESOURCE, queue_bytes);
                if (backend->au_bytes != NULL)
                    backend->au_bytes_capacity = queue_bytes;
            }
        }
        if (backend->au_bytes == NULL) {
            refused = "au-queue";
            refused_bytes = queue_floor;
        }
    }
    if (refused == NULL
        && !budget_reservation_acquire(
            &backend->worker_stack_reservation, budget,
            BUDGET_CATEGORY_RESOURCE,
            SWDEC_WORKER_STACK_BYTES + SWDEC_AUDIO_STACK_BYTES)) {
        refused = "worker-stacks";
        refused_bytes = SWDEC_WORKER_STACK_BYTES + SWDEC_AUDIO_STACK_BYTES;
    }
    /* The ring can shrink; the packet and access-unit queue cannot disappear.
       Claim those required contiguous buffers before optional picture slots
       split the native heap into smaller holes. Keep all existing ceilings
       and the ring's minimum-slot/refusal policy. */
    if (refused == NULL && !swdec_take_ring_slots(backend, budget)) {
        refused = "ring-slot";
        refused_bytes = backend->rgb_slot_bytes;
    }
#ifdef TILEFINCH_PSP_VALIDATION_LOG
    backend->trace_created_us = sceKernelGetSystemTimeWide();
    backend->trace_audio_scan = swdec_audio_scan_enabled;
    {
        unsigned high = 0, ring_pooled = 0;
        for (unsigned i = 0; i < SWDEC_RING_SLOTS; i++) {
            if (backend->rgb_slots[i] != NULL
                && !swdec_me_visible(backend->rgb_slots[i],
                                     backend->rgb_slot_bytes)) high++;
            if (backend->rgb_slot_pooled[i]) ring_pooled++;
        }
        struct mallinfo heap = mallinfo();
        psp_log_printf("tilefinch-swdec: event=working-set-placed "
                       "refused=%s bytes=%u arena=0x%08x pooled=%d "
                       "aux=0x%08x aux-bytes=%u aux-pooled=%d "
                       "ring-high=%u/%u/%u ring-pooled=%u audio-packets=0x%08x "
                       "heap-free=%u heap-top=%u "
                       "heap-largest=%u remaining=%u\n",
                       refused == NULL ? "none" : refused,
                       (unsigned) refused_bytes,
                       (unsigned) (uintptr_t) backend->decoder_arena,
                       backend->decoder_arena_pooled ? 1 : 0,
                       (unsigned) (uintptr_t) backend->aux_arena,
                       (unsigned) backend->aux_arena_bytes,
                       backend->aux_arena_pooled ? 1 : 0,
                       high, backend->rgb_slot_count,
                       (unsigned) SWDEC_RING_SLOTS, ring_pooled,
                       (unsigned) (uintptr_t) backend->audio_packets,
                       (unsigned) heap.fordblks, (unsigned) heap.keepcost,
                       (unsigned) (refused == NULL ? 0
                           : swdec_trace_largest_block()),
                       (unsigned) budget_remaining(budget));
    }
#endif
    if (refused != NULL) {
        swdec_error(error, error_size, "swdec working set exceeds budget");
        swdec_destroy(backend);
        return false;
    }
    /* Optional: a second packet buffer lets the worker start the next unit
       the moment it finishes one. It is taken only from clear headroom, never
       by asking the page to give memory back; without it, one job at a
       time. */
    size_t remaining = budget_remaining(budget);
    if (remaining >= packet_capacity
        && remaining - packet_capacity >= SWDEC_PREFETCH_HEADROOM_BYTES)
        backend->jobs[1].packet = budget_malloc_category(
            budget, BUDGET_CATEGORY_RESOURCE, packet_capacity);
    if (backend->jobs[1].packet != NULL)
        backend->jobs[1].packet_capacity = packet_capacity;
    swdec_job_ring_init(
        &backend->ring, backend->jobs[1].packet != NULL ? 2u : 1u);
    bool parameter_sets_ok;
    if (annex_b) {
        parameter_sets_ok = video.codec_config != NULL
            && video.codec_config_length != 0
            && video.codec_config_length <= parameter_capacity;
        if (parameter_sets_ok) {
            memcpy(backend->parameter_sets, video.codec_config,
                   video.codec_config_length);
            backend->parameter_sets_bytes = video.codec_config_length;
            backend->nal_length_size = 0;
        }
    } else {
        parameter_sets_ok = swdec_build_parameter_sets(
            video.codec_config, video.codec_config_length,
            backend->parameter_sets, parameter_capacity,
            &backend->parameter_sets_bytes, &backend->nal_length_size);
    }
    if (!parameter_sets_ok) {
        swdec_error(error, error_size, "invalid AVC decoder configuration");
        swdec_destroy(backend);
        return false;
    }
    backend->decoder = component->open(
        backend->decoder_arena, backend->decoder_arena_bytes,
        (int) width, (int) height, arena_refs);
    if (backend->decoder == NULL) {
        swdec_error(error, error_size, "software H.264 decoder open failed");
        swdec_destroy(backend);
        return false;
    }
    if (have_audio) {
        bool adts = audio.packet_format == MEDIA_PACKET_FORMAT_AAC_ADTS;
        bool audio_info_ok = adts
            ? audio.sample_rate != 0 && audio.channels != 0
            : media_aac_esds_stream_info(
                  audio.codec_config, audio.codec_config_length,
                  &backend->audio_info);
        if (adts) backend->audio_info = (MediaAacStreamInfo) {
            .sample_rate = audio.sample_rate,
            .channels = audio.channels,
            .samples_per_frame = 1024u
        };
        if (!audio_info_ok
            || backend->audio_info.channels == 0
            || backend->audio_info.channels > 2
            || backend->audio_info.sample_rate == 0
            || audio.largest_sample > 16384u) {
            swdec_error(error, error_size, "swdec AAC stream unsupported");
            swdec_destroy(backend);
            return false;
        }
        if (backend->audio_info.samples_per_frame != 1024u) {
            /* The PCM ring, the output blocks and the cursor all count
               1024 samples a frame; 960-sample frames would play padded
               and drift. */
            swdec_error(error, error_size,
                        "software decoder plays 1024-sample AAC frames only "
                        "(this stream uses %u)",
                        (unsigned) backend->audio_info.samples_per_frame);
            swdec_destroy(backend);
            return false;
        }
        if (backend->audio_packets == NULL) {
            backend->audio_packet_stride = swdec_cache_extent(
                swdec_aac_sample_bound(audio.largest_sample, audio.channels)
                + (adts ? 0u : 7u));
            (void) swdec_take_audio_packets(backend, budget);
        }
        component->bind_aux_arena(
            backend->aux_arena, backend->aux_arena_bytes);
        backend->audio_arena_bound = true;
        if (backend->audio_packets == NULL || component->audio_setup() != 0) {
            swdec_error(error, error_size, "swdec AAC setup failed");
            swdec_destroy(backend);
            return false;
        }
        backend->have_audio = true;
        backend->stats.audio_sample_rate = backend->audio_info.sample_rate;
    }
    backend->worker_event = sceKernelCreateEventFlag(
        "TilefinchSwdec", 0, 0, NULL);
    /* Optional: refusal retains ordinary vblank pacing, not a failed open. */
    backend->ready_event = sceKernelCreateEventFlag(
        "TilefinchSwdecReady", 0, 0, NULL);
    backend->worker_thread = sceKernelCreateThread(
        "TilefinchSwdec", swdec_worker_main,
        TILEFINCH_PSP_THREAD_PRIORITY_SWDEC,
        SWDEC_WORKER_STACK_BYTES, PSP_THREAD_ATTR_USER, NULL);
    if (backend->worker_event < 0 || backend->worker_thread < 0
        || sceKernelStartThread(
               backend->worker_thread, sizeof(backend), &backend) < 0) {
        swdec_error(error, error_size, "swdec worker start failed");
        swdec_destroy(backend);
        return false;
    }
    backend->worker_started = true;
    if (backend->have_audio) {
        backend->audio_thread = sceKernelCreateThread(
            "TilefinchSwdecAudio", swdec_audio_main,
            TILEFINCH_PSP_THREAD_PRIORITY_AUDIO,
            SWDEC_AUDIO_STACK_BYTES, PSP_THREAD_ATTR_USER, NULL);
        /* Present before it can run: it outranks this thread. */
        swdec_audio_fence_set_thread(
            &backend->audio_fence, backend->audio_thread >= 0);
        if (backend->audio_thread < 0
            || sceKernelStartThread(
                   backend->audio_thread, sizeof(backend), &backend) < 0) {
            swdec_audio_fence_set_thread(&backend->audio_fence, false);
            swdec_error(error, error_size, "swdec audio worker start failed");
            swdec_destroy(backend);
            return false;
        }
        backend->audio_started = true;
    }
    backend->stats.external_bytes =
        SWDEC_WORKER_STACK_BYTES + SWDEC_AUDIO_STACK_BYTES;
    *backend_out = (MediaBackend) {
        .opaque = backend,
        .presentation = &swdec_presentation_ops,
        .startup_ready_frames = swdec_startup_ready_frames(
            backend->rgb_slot_count, SWDEC_JOB_RING_MAX),
        .preferred_decode_lead_us = 1000000u,
        .submit = swdec_submit,
        .drain = swdec_drain,
        .advance = swdec_advance,
        .take_video_frame = swdec_take_video_frame,
        .discard_video_before = swdec_discard_video_before,
        .stats = swdec_stats,
        .audio_cursor_us = swdec_audio_cursor,
        .ready_video_frames = swdec_ready_frames,
        .ready_video_start_us = swdec_ready_start,
        .wait_video_ready = swdec_wait_ready,
        .wait_video_due = swdec_wait_due,
        .displayed_video_frames = swdec_displayed_frames,
        .set_presentation_clock_us = swdec_set_clock,
        .set_playing = swdec_set_playing,
        .set_buffering = swdec_set_buffering,
        .reset = swdec_reset,
        .destroy = swdec_destroy,
        .release_video_slot = swdec_release_slot
    };
    return true;
}

size_t media_psp_swdec_backend_last_working_set(void)
{
    return swdec_last_working_set_bytes;
}

bool media_psp_swdec_backend_create_split(
    Budget *budget, const MediaMp4Demux *video_demux,
    const MediaMp4Demux *audio_demux,
    const TilefinchSwdecComponentApi *component,
    MediaBackend *backend_out, char *error, size_t error_size)
{
    MediaSampleSource video = {0}, audio = {0};
    if (!media_sample_source_from_mp4(
            (MediaMp4Demux *) video_demux, &video)) return false;
    const MediaSampleSource *audio_pointer = NULL;
    if (audio_demux != NULL) {
        if (!media_sample_source_from_mp4(
                (MediaMp4Demux *) audio_demux, &audio)) return false;
        audio_pointer = &audio;
    }
    return media_psp_swdec_backend_create_sources(
        budget, &video, audio_pointer, component,
        backend_out, error, error_size);
}

bool media_psp_swdec_backend_create_audio(
    Budget *budget, const MediaMp4Demux *audio_demux,
    const TilefinchSwdecComponentApi *component,
    MediaBackend *backend_out, char *error, size_t error_size)
{
    if (error != NULL && error_size != 0) error[0] = '\0';
    if (budget == NULL || audio_demux == NULL || component == NULL
        || backend_out == NULL || swdec_quarantined) {
        swdec_error(error, error_size, "software AAC decoder unavailable");
        return false;
    }
    MediaMp4TrackInfo audio = {0};
    bool have_audio = false;
    for (size_t i = 0; i < media_mp4_track_count(audio_demux); i++) {
        MediaMp4TrackInfo info;
        if (media_mp4_track_info(audio_demux, i, &info)
            && info.kind == MEDIA_MP4_TRACK_AUDIO
            && info.codec == MEDIA_MP4_FOURCC('m','p','4','a')) {
            audio = info;
            have_audio = true;
            break;
        }
    }
    MediaAacStreamInfo audio_info = {0};
    if (!have_audio
        || !media_aac_esds_stream_info(
            audio.codec_config, audio.codec_config_length, &audio_info)
        || audio_info.channels == 0 || audio_info.channels > 2
        || audio_info.sample_rate == 0 || audio.largest_sample > 16384u) {
        swdec_error(error, error_size, "software AAC stream unsupported");
        return false;
    }
    if (audio_info.samples_per_frame != 1024u) {
        swdec_error(error, error_size,
                    "software decoder plays 1024-sample AAC frames only "
                    "(this stream uses %u)",
                    (unsigned) audio_info.samples_per_frame);
        return false;
    }
    PspSwdecBackend *backend = budget_calloc_category(
        budget, BUDGET_CATEGORY_NAVIGATION, 1, sizeof(*backend));
    if (backend == NULL) {
        swdec_error(error, error_size, "swdec audio state exceeds budget");
        return false;
    }
    backend->budget = budget;
    backend->api = component;
    backend->worker_event = -1;
    backend->ready_event = -1;
    backend->worker_thread = -1;
    backend->audio_thread = -1;
    atomic_init(&backend->audio_channel, -1);
    backend->last_claimed_slot = -1;
    backend->speed = -1;
    backend->epoch = 1;
    backend->have_audio = true;
    backend->audio_info = audio_info;
#ifdef TILEFINCH_PSP_VALIDATION_LOG
    backend->trace_audio_scan = swdec_audio_scan_enabled;
#endif
    backend->audio_packet_stride = swdec_cache_extent(
        swdec_aac_sample_bound(audio.largest_sample, audio.channels) + 7u);
    memset(backend->au_slots, 0, sizeof(backend->au_slots));
    for (unsigned i = 0; i < SWDEC_RING_SLOTS; i++) {
        atomic_init(&backend->slots[i].state, SWDEC_SLOT_FREE);
        atomic_init(&backend->slots[i].readers, 0u);
        atomic_init(&backend->slots[i].retire_when_unborrowed, false);
    }
    swdec_job_ring_init(&backend->ring, 1u);
    swdec_published_init(&backend->dispatch_clock_us, 0);
    atomic_init(&backend->stop, false);
    atomic_init(&backend->playing, false);
    atomic_init(&backend->buffering, false);
    atomic_init(&backend->audio_input_ended, false);
    atomic_init(&backend->audio_failed, false);
    atomic_init(&backend->audio_origin_ready, false);
    atomic_init(&backend->ready_count, 0u);
    atomic_init(&backend->displayed_count, 0u);
    atomic_init(&backend->audio_submitted, 0u);
    atomic_init(&backend->audio_output, 0u);
    atomic_init(&backend->audio_epoch, 1u);
    swdec_published_init(&backend->audio_cursor_us, 0);
    swdec_audio_clock_init(&backend->audio_dac_clock);
    swdec_published_init(&backend->audio_origin_us, 0);
    swdec_published_init(&backend->shown_end_us, 0);
    swdec_audio_fence_init(&backend->audio_fence);
    swdec_reset_gate_init(&backend->reset_gate, SWDEC_RESET_DEADLINE_US);
    atomic_init(&backend->worker_error, 0);
    atomic_init(&backend->worker_wait_error, 0);

    (void) swdec_take_aux_arena(backend, budget);
    (void) swdec_take_audio_packets(backend, budget);
    if (backend->aux_arena == NULL || backend->audio_packets == NULL
        || !budget_reservation_acquire(
            &backend->worker_stack_reservation, budget,
            BUDGET_CATEGORY_RESOURCE, SWDEC_AUDIO_STACK_BYTES)) {
        swdec_error(error, error_size,
                    "swdec audio working set exceeds budget");
        swdec_destroy(backend);
        return false;
    }
    component->bind_aux_arena(
        backend->aux_arena, backend->aux_arena_bytes);
    backend->audio_arena_bound = true;
    if (component->audio_setup() != 0) {
        swdec_error(error, error_size, "swdec AAC setup failed");
        swdec_destroy(backend);
        return false;
    }
    backend->audio_thread = sceKernelCreateThread(
        "TilefinchSwdecAudio", swdec_audio_main,
        TILEFINCH_PSP_THREAD_PRIORITY_AUDIO,
        SWDEC_AUDIO_STACK_BYTES, PSP_THREAD_ATTR_USER, NULL);
    /* Present before it can run: it outranks this thread. */
    swdec_audio_fence_set_thread(
        &backend->audio_fence, backend->audio_thread >= 0);
    if (backend->audio_thread < 0
        || sceKernelStartThread(
               backend->audio_thread, sizeof(backend), &backend) < 0) {
        swdec_audio_fence_set_thread(&backend->audio_fence, false);
        swdec_error(error, error_size, "swdec audio worker start failed");
        swdec_destroy(backend);
        return false;
    }
    backend->audio_started = true;
    backend->stats.audio_sample_rate = audio_info.sample_rate;
    backend->stats.external_bytes = SWDEC_AUDIO_STACK_BYTES;
    *backend_out = (MediaBackend) {
        .opaque = backend,
        .preferred_decode_lead_us = 1000000u,
        .submit = swdec_submit,
        .drain = swdec_drain,
        .advance = swdec_advance,
        .stats = swdec_stats,
        .audio_cursor_us = swdec_audio_cursor,
        .set_presentation_clock_us = swdec_set_clock,
        .set_playing = swdec_set_playing,
        .set_buffering = swdec_set_buffering,
        .reset = swdec_reset,
        .destroy = swdec_destroy
    };
    return true;
}
