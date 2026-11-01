#include "swdec_audio_diag.h"
#include "swdec_bounds.h"
#include "swdec_csc_mask.h"
#include "swdec_hold_trace.h"
#include "swdec_job_ring.h"
#include "swdec_window.h"

#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#include <limits.h>
#include <string.h>

#define CHECK(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "CHECK failed at %s:%d: %s\n", \
                __FILE__, __LINE__, #condition); \
        return 1; \
    } \
} while (0)

/* A worker thread that runs the ring's jobs in the order it receives them
   and records that order; the browser side publishes job numbers. */
#define RING_STRESS_JOBS 20000u
typedef struct {
    SwdecJobRing ring;
    unsigned payload[SWDEC_JOB_RING_MAX];
    unsigned result[SWDEC_JOB_RING_MAX];
    atomic_bool stop;
    unsigned out_of_order;
    unsigned ran;
} RingStress;

static void *ring_stress_worker(void *argument)
{
    RingStress *stress = argument;
    unsigned expected = 0;
    while (!atomic_load(&stress->stop) || expected < RING_STRESS_JOBS) {
        int index = swdec_job_ring_begin(&stress->ring);
        if (index < 0) { sched_yield(); continue; }
        if (stress->payload[index] != expected) stress->out_of_order++;
        stress->result[index] = stress->payload[index] * 3u + 1u;
        expected++;
        stress->ran++;
        swdec_job_ring_finish(&stress->ring, index);
    }
    return NULL;
}

static int ring_stress(unsigned depth)
{
    static RingStress stress;
    swdec_job_ring_init(&stress.ring, depth);
    atomic_init(&stress.stop, false);
    stress.out_of_order = 0;
    stress.ran = 0;
    pthread_t thread;
    CHECK(pthread_create(&thread, NULL, ring_stress_worker, &stress) == 0);
    unsigned next = 0, collected = 0, bad_results = 0, peak = 0;
    unsigned published_at[SWDEC_JOB_RING_MAX] = {0};
    while (collected < RING_STRESS_JOBS) {
        int index;
        while (next < RING_STRESS_JOBS
               && (index = swdec_job_ring_reserve(&stress.ring)) >= 0) {
            stress.payload[index] = next;
            published_at[index] = next;
            swdec_job_ring_publish(&stress.ring, index);
            next++;
            if (swdec_job_ring_in_flight(&stress.ring) > peak)
                peak = swdec_job_ring_in_flight(&stress.ring);
        }
        index = swdec_job_ring_collect(&stress.ring);
        if (index >= 0) {
            if (stress.result[index] != published_at[index] * 3u + 1u
                || published_at[index] != collected) bad_results++;
            collected++;
        } else {
            sched_yield();
        }
    }
    atomic_store(&stress.stop, true);
    pthread_join(thread, NULL);
    CHECK(stress.out_of_order == 0);
    CHECK(bad_results == 0);
    CHECK(stress.ran == RING_STRESS_JOBS);
    CHECK(peak <= depth);
    CHECK(swdec_job_ring_collect(&stress.ring) == SWDEC_JOB_RING_EMPTY);
    return 0;
}

/* A heap that hands out a fixed list of addresses. */
typedef struct {
    uintptr_t addresses[8];
    unsigned count;
    unsigned next;
    unsigned released;
} FakeHeap;

static void *fake_allocate(void *context, size_t bytes)
{
    FakeHeap *heap = context;
    (void) bytes;
    return heap->next < heap->count
        ? (void *) heap->addresses[heap->next++] : NULL;
}

static void fake_release(void *context, void *block)
{
    (void) block;
    ((FakeHeap *) context)->released++;
}

/* A heap that prefers its high (Media-Engine-invisible) blocks: each
   allocation takes the first free block in order, a release frees it
   again. Counts allocator calls. */
#define ORDERED_HEAP_BLOCKS 64u
typedef struct {
    uintptr_t addresses[ORDERED_HEAP_BLOCKS];
    bool taken[ORDERED_HEAP_BLOCKS];
    unsigned count;
    unsigned calls;
    unsigned outstanding;
} OrderedHeap;

static void *ordered_allocate(void *context, size_t bytes)
{
    OrderedHeap *heap = context;
    (void) bytes;
    heap->calls++;
    for (unsigned i = 0; i < heap->count; i++) {
        if (heap->taken[i]) continue;
        heap->taken[i] = true;
        heap->outstanding++;
        return (void *) heap->addresses[i];
    }
    return NULL;
}

static void ordered_release(void *context, void *block)
{
    OrderedHeap *heap = context;
    for (unsigned i = 0; i < heap->count; i++) {
        if (heap->addresses[i] == (uintptr_t) block && heap->taken[i]) {
            heap->taken[i] = false;
            heap->outstanding--;
            return;
        }
    }
}

/* `high` high blocks ahead of `low` low ones. */
static void ordered_heap_init(OrderedHeap *heap, unsigned high, unsigned low)
{
    memset(heap, 0, sizeof(*heap));
    for (unsigned i = 0; i < high; i++)
        heap->addresses[heap->count++] = 0x0A000000u + i * 0x40000u;
    for (unsigned i = 0; i < low; i++)
        heap->addresses[heap->count++] = 0x08800000u + i * 0x40000u;
}

#define TEST_RING_SLOTS 25u
static int ring_search_attempts(void)
{
    puts("test: a ring's low-memory search passes each high block once");
    OrderedHeap heap;
    void *slots[TEST_RING_SLOTS];
    /* Slot by slot (the old placement): every slot walks past the same 32
       high blocks again, 33 calls each. */
    ordered_heap_init(&heap, SWDEC_LOW_SEARCH_HELD, TEST_RING_SLOTS);
    for (unsigned i = 0; i < TEST_RING_SLOTS; i++) {
        slots[i] = swdec_alloc_me_visible(
            ordered_allocate, ordered_release, &heap, 4096u,
            SWDEC_LOW_SEARCH_HELD, true, NULL);
        CHECK(slots[i] != NULL);
    }
    CHECK(heap.calls == TEST_RING_SLOTS * (SWDEC_LOW_SEARCH_HELD + 1u));
    /* One search for the ring: the high blocks are held throughout. */
    ordered_heap_init(&heap, SWDEC_LOW_SEARCH_HELD, TEST_RING_SLOTS);
    SwdecLowSearch search;
    swdec_low_search_begin(&search, SWDEC_LOW_SEARCH_HELD);
    for (unsigned i = 0; i < TEST_RING_SLOTS; i++) {
        slots[i] = swdec_low_search_take(
            &search, ordered_allocate, ordered_release, &heap, 4096u, true);
        CHECK(slots[i] != NULL
              && swdec_me_address_visible((uintptr_t) slots[i], 4096u));
    }
    CHECK(search.count == SWDEC_LOW_SEARCH_HELD);
    swdec_low_search_end(&search, ordered_release, &heap);
    CHECK(heap.calls == SWDEC_LOW_SEARCH_HELD + TEST_RING_SLOTS
          && search.attempts == heap.calls && search.count == 0
          && heap.outstanding == TEST_RING_SLOTS);
    /* The cap still holds: more high blocks than it allows, and no low
       memory at all. Each slot then takes a held high block (converted on
       the CPU); nothing is held past the end and the calls stay bounded by
       the cap plus two per slot. */
    ordered_heap_init(&heap, 40u, 0u);
    swdec_low_search_begin(&search, SWDEC_LOW_SEARCH_HELD);
    for (unsigned i = 0; i < TEST_RING_SLOTS; i++) {
        slots[i] = swdec_low_search_take(
            &search, ordered_allocate, ordered_release, &heap, 4096u, true);
        CHECK(slots[i] != NULL);
        CHECK(search.count <= SWDEC_LOW_SEARCH_HELD);
    }
    swdec_low_search_end(&search, ordered_release, &heap);
    CHECK(heap.outstanding == TEST_RING_SLOTS
          && heap.calls <= SWDEC_LOW_SEARCH_HELD + 2u * TEST_RING_SLOTS);
    /* Without accept_high, an exhausted search returns NULL and its end
       gives every held block back. */
    ordered_heap_init(&heap, 3u, 1u);
    swdec_low_search_begin(&search, SWDEC_LOW_SEARCH_HELD);
    CHECK(swdec_low_search_take(&search, ordered_allocate, ordered_release,
                                &heap, 4096u, false) != NULL);
    CHECK(swdec_low_search_take(&search, ordered_allocate, ordered_release,
                                &heap, 4096u, false) == NULL);
    swdec_low_search_end(&search, ordered_release, &heap);
    CHECK(heap.outstanding == 1u);
    return 0;
}

/* The audio thread's clock for a given-up track, block by block. */
#define TEST_BLOCK_US 23220u
static int audio_seek_after_give_up(void)
{
    puts("test: a seek after audio gives up resumes audio or rebases the "
         "silent clock");
    SwdecAudioErrors errors = {0};
    for (unsigned i = 0; i < 40u; i++)
        CHECK(!swdec_audio_errors_note(&errors, 1024u));
    bool failed = false;
    for (unsigned i = 0; i < SWDEC_AUDIO_FAIL_RUN; i++)
        failed |= swdec_audio_errors_note(&errors, (unsigned) -3);
    CHECK(failed && errors.failed);
    /* Seek to 60 s: the decoder is replaced and the track tried again,
       from a clean run. The clock then starts at the first packet's
       time, as at any seek, so the first picture is not held back. */
    CHECK(swdec_audio_errors_seek(&errors) && !errors.failed
          && errors.run == 0 && errors.seek_retries == 1u);
    for (unsigned i = 0; i < 10u; i++)
        CHECK(!swdec_audio_errors_note(&errors, 1024u));
    CHECK(errors.decoded == 50u && errors.run == 0);
    /* A track that fails again needs a whole new run to be given up. */
    for (unsigned i = 0; i + 1u < SWDEC_AUDIO_FAIL_RUN; i++)
        CHECK(!swdec_audio_errors_note(&errors, (unsigned) -3));
    CHECK(swdec_audio_errors_note(&errors, (unsigned) -3) && errors.failed);
    CHECK(swdec_audio_errors_seek(&errors) && errors.seek_retries == 2u);
    for (unsigned i = 0; i < SWDEC_AUDIO_FAIL_RUN; i++)
        (void) swdec_audio_errors_note(&errors, (unsigned) -3);
    CHECK(errors.failed);
    /* Out of retries: the track stays given up. */
    CHECK(!swdec_audio_errors_seek(&errors) && errors.failed
          && errors.seek_retries == SWDEC_AUDIO_SEEK_RETRIES
          && errors.run == 0);
    CHECK(!swdec_audio_errors_seek(&errors));
    /* A seek on a healthy track changes nothing but the run. */
    SwdecAudioErrors healthy = {0};
    (void) swdec_audio_errors_note(&healthy, (unsigned) -3);
    CHECK(swdec_audio_errors_seek(&healthy) && healthy.run == 0
          && healthy.seek_retries == 0 && healthy.errors == 1u);

    /* The silent clock after that seek to 60 s. */
    const uint64_t target = UINT64_C(60000000);
    const uint64_t first_picture_end = target + 33367u;
    uint64_t cursor = 12345u;
    /* Nothing known yet: it waits rather than running from zero. */
    CHECK(!swdec_silent_clock_start(false, 0, false, 0, false, &cursor));
    CHECK(!swdec_silent_clock_start(false, 0, false, first_picture_end,
                                    false, &cursor));
    /* The first audio packet's time is known, but with video the clock
       waits for the first picture of the new position, as sound does. */
    CHECK(!swdec_silent_clock_start(true, target, false, 0, false, &cursor));
    CHECK(swdec_silent_clock_start(true, target, false, first_picture_end,
                                   false, &cursor) && cursor == target);
    /* Audio only: the packet's time alone starts it. */
    CHECK(swdec_silent_clock_start(true, target, false, 0, true, &cursor)
          && cursor == target);
    /* No audio arrives before the input ends: the shown picture rebases. */
    CHECK(swdec_silent_clock_start(false, 0, true, first_picture_end, false,
                                   &cursor) && cursor == first_picture_end);
    /* No stall: from the rebased start the pictures after the seek are
       due within a block of the clock, not a minute later. */
    CHECK(swdec_silent_clock_start(true, target, false, first_picture_end,
                                   false, &cursor));
    uint64_t clock = cursor;
    unsigned blocks_until_first_picture_due = 0;
    while (clock < target) {
        clock += TEST_BLOCK_US;
        blocks_until_first_picture_due++;
    }
    CHECK(blocks_until_first_picture_due == 0u);
    clock += 2u * TEST_BLOCK_US;
    CHECK(clock >= first_picture_end);
    return 0;
}

/* A Media Engine audio queue: result `samples` per packet; packets up to
   `done` are decoded. */
typedef struct {
    int samples[256];
    unsigned done;
} FakeAudioQueue;
static FakeAudioQueue fake_audio;

static int fake_audio_poll(unsigned index, unsigned *crc, unsigned *samples)
{
    if (index >= fake_audio.done) return 0;
    *crc = 0;
    *samples = (unsigned) fake_audio.samples[index];
    return 1;
}

#define TEST_PACKET_SLOTS 48u
#define TEST_PACKET_STRIDE 64u
static int audio_bad_packet_capture(void)
{
    puts("test: the failed audio packet is copied while its slot holds it");
    static unsigned char packets[TEST_PACKET_SLOTS * TEST_PACKET_STRIDE];
    SwdecAudioPacketMeta meta[TEST_PACKET_SLOTS];
    SwdecAudioBadPacket bad;
    memset(&bad, 0, sizeof(bad));
    memset(&fake_audio, 0, sizeof(fake_audio));
    unsigned checked = 0;
    const unsigned failed_packet = 10u;
    for (unsigned i = 0; i < 256u; i++)
        fake_audio.samples[i] = i == failed_packet || i == 20u ? -5 : 1024;
    /* The browser writes packet n into slot n % 48 once the decoder has
       finished packet n - 48; the decoder runs right behind submission,
       playback (where the old capture ran) about 112 packets behind. */
    for (unsigned n = 0; n < 200u; n++) {
        fake_audio.done = n;
        swdec_audio_bad_scan(&bad, &checked, fake_audio.done,
                             fake_audio_poll, packets, TEST_PACKET_STRIDE,
                             TEST_PACKET_SLOTS, meta);
        unsigned char *slot = packets + (n % TEST_PACKET_SLOTS)
            * TEST_PACKET_STRIDE;
        unsigned bytes = 20u + n % 30u;
        memset(slot, 0, TEST_PACKET_STRIDE);
        for (unsigned b = 0; b < bytes; b++)
            slot[b] = (unsigned char) (n * 7u + b);
        meta[n % TEST_PACKET_SLOTS] = (SwdecAudioPacketMeta) {
            bytes, (uint64_t) n * TEST_BLOCK_US};
    }
    CHECK(bad.captured && bad.index == failed_packet);
    CHECK(bad.bytes == 20u + failed_packet % 30u
          && bad.pts_us == (uint64_t) failed_packet * TEST_BLOCK_US);
    for (unsigned b = 0; b < SWDEC_AUDIO_BAD_HEAD_BYTES; b++)
        CHECK(bad.head[b] == (unsigned char) (failed_packet * 7u + b));
    /* By the time playback reaches it the slot holds another packet, which
       the old capture would have reported. */
    CHECK(packets[(failed_packet % TEST_PACKET_SLOTS) * TEST_PACKET_STRIDE]
          != bad.head[0]);
    /* Only the first failure is kept, and scanning stops at what the
       decoder has not finished. */
    SwdecAudioBadPacket later;
    memset(&later, 0, sizeof(later));
    checked = 15u;
    fake_audio.done = 30u;
    swdec_audio_bad_scan(&later, &checked, 40u, fake_audio_poll, packets,
                         TEST_PACKET_STRIDE, TEST_PACKET_SLOTS, meta);
    CHECK(later.captured && later.index == 20u && checked == 40u);
    SwdecAudioBadPacket none;
    memset(&none, 0, sizeof(none));
    checked = 21u;
    swdec_audio_bad_scan(&none, &checked, 40u, fake_audio_poll, packets,
                         TEST_PACKET_STRIDE, TEST_PACKET_SLOTS, meta);
    CHECK(!none.captured && checked == 30u);
    /* A short packet copies only its own bytes. */
    memset(&none, 0, sizeof(none));
    meta[3] = (SwdecAudioPacketMeta) {5u, 0u};
    fake_audio.samples[3] = -1;
    fake_audio.done = 4u;
    checked = 3u;
    memset(packets + 3u * TEST_PACKET_STRIDE, 0xAB, TEST_PACKET_STRIDE);
    swdec_audio_bad_scan(&none, &checked, 4u, fake_audio_poll, packets,
                         TEST_PACKET_STRIDE, TEST_PACKET_SLOTS, meta);
    CHECK(none.captured && none.bytes == 5u && none.head[4] == 0xABu
          && none.head[5] == 0u);
    return 0;
}

/* The fixed steady window: lead, span, intervals and holds. */
static int steady_window(void)
{
    puts("test: the steady-playback window opens and closes at fixed times");
    static SwdecWindow window;
    memset(&window, 0, sizeof(window));
    const uint32_t period = 40000u;
    uint64_t now = 1000000u, pts = 0;
    CHECK(swdec_window_note(&window, now, pts, period)
          == SWDEC_WINDOW_NOTE_NONE);
    CHECK(window.phase == SWDEC_WINDOW_LEADING);
    unsigned opened = 0, closed = 0, steps = 0;
    while (window.phase != SWDEC_WINDOW_CLOSED && steps < 2000u) {
        steps++;
        pts += period;
        /* every 50th picture is held for three periods */
        now += steps % 50u == 0 ? 3u * period : period;
        SwdecWindowNote note = swdec_window_note(&window, now, pts, period);
        if (note == SWDEC_WINDOW_NOTE_OPENED) {
            opened++;
            CHECK(now - window.first_us >= SWDEC_WINDOW_LEAD_US);
            CHECK(window.intervals == 0);
        }
        if (note == SWDEC_WINDOW_NOTE_CLOSED) {
            closed++;
            CHECK(now - window.open_us >= SWDEC_WINDOW_SPAN_US);
            CHECK(now - window.open_us < SWDEC_WINDOW_SPAN_US + 3u * period);
        }
    }
    CHECK(opened == 1u && closed == 1u);
    CHECK(swdec_window_percentile_ms(&window, 500u) == 40u);
    CHECK(swdec_window_percentile_ms(&window, 950u) == 40u);
    CHECK(swdec_window_percentile_ms(&window, 1000u) == 120u);
    CHECK(window.max_us == 3u * period);
    CHECK(window.held >= 14u && window.held <= 16u);
    CHECK(window.worst_us[0] == 3u * period
          && window.worst_us[2] == 3u * period);
    CHECK(window.worst_pts_us[0] < window.worst_pts_us[1]);
    /* nothing after it closed */
    uint32_t intervals = window.intervals;
    CHECK(swdec_window_note(&window, now + period, pts + period, period)
          == SWDEC_WINDOW_NOTE_NONE);
    CHECK(window.intervals == intervals);
    /* an empty window has no percentile */
    SwdecWindow empty;
    memset(&empty, 0, sizeof(empty));
    CHECK(swdec_window_percentile_ms(&empty, 500u) == 0u);
    return 0;
}

static int hold_trace(void)
{
    puts("test: a hold keeps the pictures around the one that ended it");
    static SwdecHoldTrace trace;
    memset(&trace, 0, sizeof(trace));
    /* 200 pictures at 24 fps: the ring keeps the newest 128. */
    for (unsigned i = 0; i < 200u; i++) {
        SwdecHoldPic *picture = swdec_hold_add(&trace, (uint64_t) i * 41667u);
        picture->bytes = i;
        picture->declined_us = i + 1u;
        picture->declined_clock_us = (uint64_t) i * 41667u - 100u;
        picture->taken_clock_us = (uint64_t) i * 41667u;
        picture->decode_cpu_us = 10000u + i;
        picture->browser_cpu_us = 1000u + i;
        picture->clock_bracket_us = 10u + i;
    }
    CHECK(swdec_hold_find(&trace, 199u * 41667u)->bytes == 199u);
    CHECK(swdec_hold_find(&trace, 72u * 41667u)->bytes == 72u);
    CHECK(swdec_hold_find(&trace, 71u * 41667u) == NULL);
    CHECK(swdec_hold_find(&trace, 5u) == NULL);
    /* A later record of the same time (after a seek) wins. */
    swdec_hold_add(&trace, 150u * 41667u)->bytes = 9999u;
    CHECK(swdec_hold_find(&trace, 150u * 41667u)->bytes == 9999u);

    SwdecHold hold = {.hold_us = 120000u, .pts_us = 120u * 41667u};
    swdec_hold_capture(&trace, &hold);
    /* 500 ms before to 130 ms after: pictures 109..123 (15). */
    CHECK(trace.holds[0].count == 15u);
    CHECK(trace.holds[0].pictures[0].bytes == 109u);
    CHECK(trace.holds[0].pictures[0].declined_us == 110u);
    CHECK(trace.holds[0].pictures[0].declined_clock_us == 109u * 41667u - 100u);
    CHECK(trace.holds[0].pictures[0].taken_clock_us == 109u * 41667u);
    CHECK(trace.holds[0].pictures[0].decode_cpu_us == 10109u);
    CHECK(trace.holds[0].pictures[0].browser_cpu_us == 1109u);
    CHECK(trace.holds[0].pictures[0].clock_bracket_us == 119u);
    CHECK(trace.holds[0].pictures[14].bytes == 123u);
    for (unsigned i = 1; i < trace.holds[0].count; i++)
        CHECK(trace.holds[0].pictures[i - 1u].pts_us
              < trace.holds[0].pictures[i].pts_us);
    /* Longer holds rank first; one shorter than every kept is dropped. */
    for (unsigned k = 0; k < SWDEC_HOLD_KEEP; k++) {
        SwdecHold longer = {.hold_us = 130000u + k,
                            .pts_us = (uint64_t) (130u + k) * 41667u};
        swdec_hold_capture(&trace, &longer);
    }
    CHECK(trace.holds[0].hold_us == 130000u + SWDEC_HOLD_KEEP - 1u);
    CHECK(trace.holds[SWDEC_HOLD_KEEP - 1u].hold_us == 130000u);
    SwdecHold shorter = {.hold_us = 50000u, .pts_us = 140u * 41667u};
    swdec_hold_capture(&trace, &shorter);
    CHECK(trace.holds[SWDEC_HOLD_KEEP - 1u].hold_us == 130000u);

    /* More pictures in range than a hold keeps: the nearest stay (all six
       after it, the thirteen before it and itself). */
    memset(&trace, 0, sizeof(trace));
    for (unsigned i = 0; i < 100u; i++)
        swdec_hold_add(&trace, (uint64_t) i * 20000u)->bytes = i;
    SwdecHold dense = {.hold_us = 1u, .pts_us = 50u * 20000u};
    swdec_hold_capture(&trace, &dense);
    CHECK(trace.holds[0].count == SWDEC_HOLD_PICTURES);
    for (unsigned i = 0; i < SWDEC_HOLD_PICTURES; i++) {
        unsigned n = trace.holds[0].pictures[i].bytes;
        CHECK(n >= 37u && n <= 56u);
    }

    puts("test: the first slice's type is read from its header");
    /* first_mb_in_slice 0 ('1'), slice_type 7 (I, '0001000'). */
    static const unsigned char idr[] = {
        0, 0, 0, 1, 0x67, 0x42, 0, 0, 0, 1, 0x65, 0x88, 0x80, 0x10
    };
    size_t bytes = 0;
    const unsigned char *slice = swdec_au_first_slice(idr, sizeof(idr), 0u,
                                                      &bytes);
    CHECK(slice == idr + 11 && bytes == 3u);
    CHECK(swdec_hold_slice_type(slice, bytes) == 2);
    /* slice_type 1 (B, '010') in a non-reference slice, length-prefixed. */
    static const unsigned char b_slice[] = {0, 0, 0, 3, 0x01, 0xa0, 0x00};
    slice = swdec_au_first_slice(b_slice, sizeof(b_slice), 4u, &bytes);
    CHECK(slice == b_slice + 5 && bytes == 2u);
    CHECK(swdec_hold_slice_type(slice, bytes) == 1);
    CHECK(swdec_au_is_non_reference(b_slice, sizeof(b_slice), 4u));
    static const unsigned char no_slice[] = {0, 0, 1, 0x67, 0x42};
    CHECK(swdec_au_first_slice(no_slice, sizeof(no_slice), 0u, &bytes)
          == NULL);
    static const unsigned char zeros[] = {0, 0};
    CHECK(swdec_hold_slice_type(zeros, sizeof(zeros)) == -1);
    return 0;
}

int main(void)
{
    puts("test: decoded video geometry remains inside admitted surfaces");
    CHECK(swdec_dimensions_admitted(432, 240, 432, 240));
    CHECK(swdec_dimensions_admitted(320, 180, 432, 240));
    CHECK(!swdec_dimensions_admitted(448, 240, 432, 240));
    CHECK(!swdec_dimensions_admitted(432, 256, 432, 240));
    CHECK(!swdec_dimensions_admitted(0, 240, 432, 240));

    puts("test: RGB565 capacity includes stride and every decoded row");
    CHECK(swdec_rgb565_destination_fits(
        432, 240, 432, 432u * 240u * 2u));
    CHECK(swdec_rgb565_destination_fits(
        426, 240, 432, 432u * 240u * 2u));
    CHECK(!swdec_rgb565_destination_fits(
        448, 240, 432, 432u * 240u * 2u));
    CHECK(!swdec_rgb565_destination_fits(
        431, 240, 432, 432u * 240u * 2u));
    CHECK(!swdec_rgb565_destination_fits(
        432, 256, 432, 432u * 240u * 2u));
    CHECK(!swdec_rgb565_destination_fits(
        432, 240, 432, 432u * 240u * 2u - 1u));

    puts("test: the startup picture target fits a short picture ring");
    CHECK(swdec_startup_ready_frames(25u, 2u) == 20u);
    CHECK(swdec_startup_ready_frames(22u, 2u) == 20u);
    CHECK(swdec_startup_ready_frames(13u, 2u) == 11u);
    CHECK(swdec_startup_ready_frames(8u, 2u) == 6u);
    CHECK(swdec_startup_ready_frames(2u, 2u) == 1u);

    puts("test: quality recovery stays reachable on a short picture ring");
    /* The full ring keeps the original rungs: 2, 4 and 8 ready. */
    CHECK(swdec_speed_for_ready(0, 2u, 25u, 2u, 2u) == 3);
    CHECK(swdec_speed_for_ready(0, 4u, 25u, 2u, 2u) == 4);
    CHECK(swdec_speed_for_ready(4, 7u, 25u, 2u, 2u) == 4);
    CHECK(swdec_speed_for_ready(4, 8u, 25u, 2u, 2u) == 0);
    CHECK(swdec_speed_for_ready(0, 5u, 25u, 2u, 2u) == 0);
    for (unsigned slots = 8u; slots <= 10u; slots++) {
        unsigned most = slots - 4u;
        /* The most the ring can hold ready recovers full quality from
           either reduced rung; an empty ring reduces most. */
        CHECK(swdec_speed_for_ready(4, most, slots, 2u, 2u) == 0);
        CHECK(swdec_speed_for_ready(3, most, slots, 2u, 2u) == 0);
        CHECK(swdec_speed_for_ready(0, 0u, slots, 2u, 2u) == 3);
        /* Hysteresis: one short of the most still holds the rung. */
        CHECK(swdec_speed_for_ready(4, most - 1u, slots, 2u, 2u) == 4);
    }
    /* 8 slots (capacity 4): reduce only when empty, recover at 4. */
    CHECK(swdec_speed_for_ready(0, 1u, 8u, 2u, 2u) == 4);
    CHECK(swdec_speed_for_ready(0, 2u, 8u, 2u, 2u) == 0);
    CHECK(swdec_speed_for_ready(4, 3u, 8u, 2u, 2u) == 4);
    CHECK(swdec_speed_for_ready(4, 4u, 8u, 2u, 2u) == 0);
    CHECK(swdec_speed_for_ready(4, 6u, 10u, 2u, 2u) == 0);
    CHECK(swdec_speed_for_ready(3, 5u, 10u, 2u, 2u) == 3);
    CHECK(swdec_speed_for_ready(0, 1u, 10u, 2u, 2u) == 3);
    /* 14 slots (capacity 10, the small talk video): a 0.3 s feed stall
       from a full ring leaves 3 ready and keeps full quality; 2 reduces,
       1 reduces most, 8 recovers. */
    CHECK(swdec_speed_for_ready(0, 3u, 14u, 2u, 2u) == 0);
    CHECK(swdec_speed_for_ready(0, 2u, 14u, 2u, 2u) == 4);
    CHECK(swdec_speed_for_ready(0, 1u, 14u, 2u, 2u) == 3);
    CHECK(swdec_speed_for_ready(4, 7u, 14u, 2u, 2u) == 4);
    CHECK(swdec_speed_for_ready(4, 8u, 14u, 2u, 2u) == 0);
    /* Every capacity: the rungs are ordered and recovery is reachable. */
    for (unsigned slots = 5u; slots <= 30u; slots++) {
        unsigned capacity = slots - 4u;
        CHECK(swdec_speed_for_ready(4, capacity, slots, 2u, 2u) == 0);
        CHECK(swdec_speed_for_ready(0, 0u, slots, 2u, 2u) == 3);
    }

    puts("test: a reduced rung skips deblocking and drops no picture");
    CHECK(swdec_rung_decoder_speed(0) == 0);
    CHECK(swdec_rung_decoder_speed(3) == 1);
    CHECK(swdec_rung_decoder_speed(4) == 1);
    CHECK(swdec_rung_decoder_speed(1) == 1);

    puts("test: the fixed two-plane AAC ABI admits mono and stereo only");
    CHECK(swdec_audio_channels_admitted(1));
    CHECK(swdec_audio_channels_admitted(2));
    CHECK(!swdec_audio_channels_admitted(0));
    CHECK(!swdec_audio_channels_admitted(3));
    CHECK(!swdec_audio_channels_admitted(8));

    puts("test: clock correction re-anchors instead of accumulating");
    CHECK(swdec_clock_reanchor_slip(1000000u, 925000u) == 75000u);
    CHECK(swdec_clock_reanchor_slip(1000000u, 925000u) == 75000u);
    CHECK(swdec_clock_reanchor_slip(900000u, 925000u) == 0u);

    puts("test: with audio, the latest due frame is shown and older ones dropped");
    {
        /* PSP-3000 run: video decoded at about half real time while audio
           was held for it, so playback crawled at 0.46x. With audio free
           running, the presenter must jump to the newest due frame. */
        const uint64_t pts[5] = {1200000u, 1233333u, 1266667u,
                                 1300000u, 1333333u};
        const uint64_t dur[5] = {33333u, 33333u, 33333u, 33333u, 33333u};
        bool eligible[5] = {true, true, true, true, true};
        /* clock 1.31 s: 1.3333 s is more than half a frame ahead */
        CHECK(swdec_pick_due_frame(pts, dur, eligible, 5, 1310000u) == 3);
        /* half a frame early is due */
        CHECK(swdec_pick_due_frame(pts, dur, eligible, 5, 1317000u) == 4);
        /* nothing due yet */
        CHECK(swdec_pick_due_frame(pts, dur, eligible, 5, 1100000u) == -1);
        /* claimed or empty slots are never picked */
        eligible[3] = false;
        eligible[4] = false;
        CHECK(swdec_pick_due_frame(pts, dur, eligible, 5, 2000000u) == 2);
        /* unordered ring slots: the picker compares pts, not slot order */
        const uint64_t mixed[4] = {1300000u, 1200000u, 1400000u, 1233333u};
        const bool all[4] = {true, true, true, true};
        CHECK(swdec_pick_due_frame(mixed, dur, all, 4, 1350000u) == 0);
    }

    puts("test: only all-non-reference access units may be dropped");
    {
        /* length-prefixed (4 bytes): SEI then a non-reference B slice */
        const unsigned char b_frame[] = {0, 0, 0, 2, 0x06, 0x05,
                                         0, 0, 0, 3, 0x01, 0x9e, 0x11};
        CHECK(swdec_au_is_non_reference(b_frame, sizeof b_frame, 4u));
        /* a reference P slice (nal_ref_idc 2) */
        const unsigned char p_frame[] = {0, 0, 0, 3, 0x41, 0x9a, 0x22};
        CHECK(!swdec_au_is_non_reference(p_frame, sizeof p_frame, 4u));
        /* a B-pyramid reference B slice (nal_ref_idc 1) */
        const unsigned char b_ref[] = {0, 0, 0, 3, 0x21, 0x9e, 0x22};
        CHECK(!swdec_au_is_non_reference(b_ref, sizeof b_ref, 4u));
        /* IDR is always kept */
        const unsigned char idr[] = {0, 0, 0, 3, 0x05, 0x88, 0x84};
        CHECK(!swdec_au_is_non_reference(idr, sizeof idr, 4u));
        /* Annex-B, two non-reference slices */
        const unsigned char annexb[] = {0, 0, 0, 1, 0x01, 0x9e, 0x11,
                                        0, 0, 1, 0x01, 0x9e, 0x12};
        CHECK(swdec_au_is_non_reference(annexb, sizeof annexb, 0u));
        /* Annex-B with one reference slice among them */
        const unsigned char annexb_ref[] = {0, 0, 1, 0x01, 0x9e, 0x11,
                                            0, 0, 1, 0x41, 0x9a, 0x12};
        CHECK(!swdec_au_is_non_reference(annexb_ref, sizeof annexb_ref, 0u));
        /* malformed length, no VCL, and empty input are kept */
        const unsigned char bad[] = {0, 0, 0, 9, 0x01, 0x9e};
        CHECK(!swdec_au_is_non_reference(bad, sizeof bad, 4u));
        const unsigned char sei_only[] = {0, 0, 0, 2, 0x06, 0x05};
        CHECK(!swdec_au_is_non_reference(sei_only, sizeof sei_only, 4u));
        CHECK(!swdec_au_is_non_reference(b_frame, 0, 4u));
    }

    puts("test: non-reference pictures with positive lead are not discarded");
    CHECK(!swdec_au_late(1000000u, 962000u));
    CHECK(!swdec_au_late(1000001u, 1000000u));
    CHECK(swdec_au_late(1000000u, 1000000u));
    CHECK(swdec_au_late(900000u, 1000000u));
    CHECK(!swdec_au_late(UINT64_MAX, 1000000u));
    CHECK(!swdec_au_late(UINT64_MAX - 1u, UINT64_MAX - 2u));
    CHECK(swdec_au_late(UINT64_MAX - 2u, UINT64_MAX - 1u));

    puts("test: the job ring runs jobs in order, one or two ahead");
    {
        SwdecJobRing ring;
        swdec_job_ring_init(&ring, 2u);
        CHECK(swdec_job_ring_collect(&ring) == SWDEC_JOB_RING_EMPTY);
        CHECK(swdec_job_ring_begin(&ring) == -1);
        int first = swdec_job_ring_reserve(&ring);
        CHECK(first == 0);
        swdec_job_ring_publish(&ring, first);
        /* the second unit is queued while the first is not yet started */
        int second = swdec_job_ring_reserve(&ring);
        CHECK(second == 1);
        swdec_job_ring_publish(&ring, second);
        CHECK(swdec_job_ring_reserve(&ring) == -1);
        CHECK(swdec_job_ring_collect(&ring) == SWDEC_JOB_RING_BUSY);
        CHECK(swdec_job_ring_begin(&ring) == 0);
        swdec_job_ring_finish(&ring, 0);
        /* the worker starts the queued unit without the browser */
        CHECK(swdec_job_ring_begin(&ring) == 1);
        CHECK(swdec_job_ring_begin(&ring) == -1);
        /* finished work is collected in order; the running one blocks */
        CHECK(swdec_job_ring_collect(&ring) == 0);
        CHECK(swdec_job_ring_collect(&ring) == SWDEC_JOB_RING_BUSY);
        CHECK(swdec_job_ring_reserve(&ring) == 0);
        swdec_job_ring_finish(&ring, 1);
        CHECK(swdec_job_ring_collect(&ring) == 1);
        CHECK(swdec_job_ring_collect(&ring) == SWDEC_JOB_RING_EMPTY);
        /* a job whose wake failed is withdrawn before the worker sees it */
        int withdrawn = swdec_job_ring_reserve(&ring);
        swdec_job_ring_publish(&ring, withdrawn);
        CHECK(swdec_job_ring_unpublish(&ring, withdrawn));
        CHECK(swdec_job_ring_in_flight(&ring) == 0);
        CHECK(swdec_job_ring_begin(&ring) == -1);
        /* ... but not once the worker has it */
        withdrawn = swdec_job_ring_reserve(&ring);
        swdec_job_ring_publish(&ring, withdrawn);
        CHECK(swdec_job_ring_begin(&ring) == withdrawn);
        CHECK(!swdec_job_ring_unpublish(&ring, withdrawn));

        /* depth 1 (no second packet buffer) is the old single hand-off */
        swdec_job_ring_init(&ring, 1u);
        CHECK(swdec_job_ring_reserve(&ring) == 0);
        swdec_job_ring_publish(&ring, 0);
        CHECK(swdec_job_ring_reserve(&ring) == -1);
        CHECK(swdec_job_ring_begin(&ring) == 0);
        swdec_job_ring_finish(&ring, 0);
        CHECK(swdec_job_ring_reserve(&ring) == -1);
        CHECK(swdec_job_ring_collect(&ring) == 0);
        CHECK(swdec_job_ring_reserve(&ring) == 0);
    }
    puts("test: the job ring keeps order across threads");
    if (ring_stress(1u) != 0 || ring_stress(2u) != 0) return 1;

    puts("test: CSC completion covers the bottom of a 272-row frame");
    SwdecCscMask mask = {{0, 0}};
    for (unsigned unit = 0; unit < 34u; unit++)
        swdec_csc_mask_set(&mask, unit);
    CHECK(swdec_csc_mask_test(&mask, 31u));
    CHECK(swdec_csc_mask_test(&mask, 32u)); /* rows 256..263 */
    CHECK(swdec_csc_mask_test(&mask, 33u)); /* rows 264..271 */
    CHECK(!swdec_csc_mask_test(&mask, SWDEC_CSC_MASK_UNITS));
    swdec_csc_mask_clear(&mask);
    CHECK(!swdec_csc_mask_test(&mask, 32u));

    puts("test: the decoder arena is sized for the stream's declared refs");
    /* The 416x234 fixture rendition declares 5. */
    CHECK(swdec_arena_estimate(416, 234, 16) == 5610496u);
    CHECK(swdec_arena_estimate(416, 234, 5) == 3076096u);
    /* Above the device-measured peaks: 2.43 MB (416x234, 5 refs), 2.41 MB
       (426x240, 4 refs with B-frames); and the host peak at 432x240 with
       16 references (4.77 MB) plus the device's few percent. */
    CHECK(swdec_arena_estimate(416, 234, 5) > 2427648u * 115u / 100u);
    CHECK(swdec_arena_estimate(426, 240, 4) == 2965504u
          && swdec_arena_estimate(426, 240, 4) > 2410000u * 115u / 100u);
    CHECK(swdec_arena_estimate(432, 240, 16) > 4768026u * 115u / 100u);
    /* With up to 8 references the arena and the 1408 KiB audio arena fit
       the 5,538,132-byte Media Engine pool together at 426x240. */
    CHECK(swdec_arena_estimate(426, 240, 8) + 1408u * 1024u < 5538132u);
    CHECK(swdec_arena_estimate(0, 240, 4) == 0
          && swdec_arena_estimate(426, 240, -1) == 0);
    CHECK(swdec_arena_refs(false, 0) == 16
          && swdec_arena_refs(false, 3) == 16);
    CHECK(swdec_arena_refs(true, 1) == 4 && swdec_arena_refs(true, 4) == 4
          && swdec_arena_refs(true, 5) == 5
          && swdec_arena_refs(true, 16) == 16
          && swdec_arena_refs(true, 40) == 16);
    /* A later SPS asking for more than the arena holds is refused. */
    CHECK(swdec_sps_frames_admitted(5u, 5) && swdec_sps_frames_admitted(2u, 5)
          && !swdec_sps_frames_admitted(6u, 5)
          && !swdec_sps_frames_admitted(1u, 0));
    puts("test: picture slots are counted and charged for the room there is");
    for (unsigned capacity = 0; capacity <= 25; capacity++) {
        CHECK(swdec_picture_slot_limit(capacity, false) == capacity);
        unsigned limit = swdec_picture_slot_limit(capacity, true);
        CHECK(limit == (capacity > 9u ? 9u : capacity));
        CHECK(swdec_pool_slot_count(100u * 207360u, 207360u, limit) == limit);
    }
    /* 426x240 slots (207,360 bytes): one fits in 400 KB, not 25. */
    CHECK(swdec_pool_slot_count(400000u, 207360u, 25u) == 1u);
    CHECK(swdec_pool_slot_count(207359u, 207360u, 25u) == 0u);
    CHECK(swdec_pool_slot_count(3u * 207360u, 207360u, 25u) == 3u);
    CHECK(swdec_pool_slot_count(100u * 207360u, 207360u, 25u) == 25u);
    CHECK(swdec_pool_slot_count(1000u, 0u, 25u) == 0u);

    puts("test: Media Engine buffers are never placed in the extra bank");
    CHECK(swdec_me_address_visible(0x09800000u, 4096u));
    CHECK(swdec_me_address_visible(0x49800000u, 4096u));   /* uncached alias */
    CHECK(!swdec_me_address_visible(0x0A000000u, 64u));
    CHECK(!swdec_me_address_visible(0x09FFFF00u, 512u));   /* straddles */
    CHECK(!swdec_me_address_visible(0u, 64u));
    {
        FakeHeap heap = {{0x0B000000u, 0x0A800000u, 0x09100000u}, 3u, 0u, 0u};
        unsigned held = 99u;
        void *block = swdec_alloc_me_visible(
            fake_allocate, fake_release, &heap, 4096u, 32u, false, &held);
        /* Two high blocks passed over and given back; the low one kept. */
        CHECK((uintptr_t) block == 0x09100000u && held == 2u
              && heap.released == 2u);
        FakeHeap high = {{0x0B000000u, 0x0A800000u}, 2u, 0u, 0u};
        block = swdec_alloc_me_visible(
            fake_allocate, fake_release, &high, 4096u, 32u, false, &held);
        /* Nothing low: refused, nothing kept. */
        CHECK(block == NULL && held == 2u && high.released == 2u);
        FakeHeap slots = {{0x0B000000u, 0x0A800000u}, 2u, 0u, 0u};
        block = swdec_alloc_me_visible(
            fake_allocate, fake_release, &slots, 4096u, 32u, true, &held);
        /* A picture slot may stay high (converted on the CPU). */
        CHECK((uintptr_t) block == 0x0A800000u && slots.released == 1u);
        FakeHeap bounded = {{0x0B000000u, 0x0B100000u, 0x0B200000u,
                             0x09000000u}, 4u, 0u, 0u};
        block = swdec_alloc_me_visible(
            fake_allocate, fake_release, &bounded, 4096u, 2u, false, &held);
        CHECK(block == NULL && bounded.released == 3u);
    }

    puts("test: audio batches yield when video runs out of parsed rows");
    {
        CHECK(swdec_audio_service_continue(0u, true));
        CHECK(swdec_audio_service_continue(0u, false));
        for (unsigned i = 1; i < 4u; i++) {
            CHECK(!swdec_audio_service_continue(i, true));
            CHECK(swdec_audio_service_continue(i, false));
        }
        CHECK(!swdec_audio_service_continue(4u, false));
        CHECK(!swdec_audio_service_continue(UINT_MAX, false));
        /* A CPU catching up during the second audio packet stops the
           third; the next visit still guarantees audio forward progress. */
        unsigned done = 0;
        while (swdec_audio_service_continue(done, done >= 2u)) done++;
        CHECK(done == 2u);
        CHECK(swdec_audio_service_continue(0u, true));
    }
    puts("test: failed audio frames are counted, and a long run gives up");
    {
        SwdecAudioErrors errors = {0};
        CHECK(!swdec_audio_errors_note(&errors, 1024u));
        for (unsigned i = 0; i + 1u < SWDEC_AUDIO_FAIL_RUN; i++)
            CHECK(!swdec_audio_errors_note(&errors, (unsigned) -3));
        CHECK(errors.errors == SWDEC_AUDIO_FAIL_RUN - 1u
              && errors.first_error == -3 && !errors.failed);
        /* A good frame ends the run. */
        CHECK(!swdec_audio_errors_note(&errors, 1024u) && errors.run == 0
              && errors.decoded == 2u
              && errors.longest_run == SWDEC_AUDIO_FAIL_RUN - 1u);
        bool failed = false;
        for (unsigned i = 0; i < SWDEC_AUDIO_FAIL_RUN; i++)
            failed |= swdec_audio_errors_note(&errors,
                                              (unsigned) -1094995529);
        CHECK(failed && errors.failed && errors.first_error == -3);
        /* Reported once. */
        CHECK(!swdec_audio_errors_note(&errors, (unsigned) -3));
        CHECK(SWDEC_AUDIO_RESET_RUN < SWDEC_AUDIO_FAIL_RUN);
    }
    if (ring_search_attempts() != 0) return 1;
    if (audio_seek_after_give_up() != 0) return 1;
    if (audio_bad_packet_capture() != 0) return 1;
    if (steady_window() != 0) return 1;
    if (hold_trace() != 0) return 1;
    puts("swdec bounds tests passed");
    return 0;
}
