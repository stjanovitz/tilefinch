/* Host tests for the software decoder's cross-thread protocols: the 64-bit
   publication the audio thread reads, the PCM-ring fence a seek waits on,
   the deferred seek reset, the audio-gap measurement and the compressed
   video queue sizing, and the colour-conversion slot claim between the
   decoding thread and the Media Engine. */
#include "swdec_audio_fence.h"
#include "swdec_audio_clock.h"
#include "swdec_audio_gap.h"
#include "swdec_bounds.h"
/* the fake Media Engine sometimes stalls between reading the offer and
   announcing its batch */
static void claim_take_gap(void);
#define SWDEC_CSC_CLAIM_TAKE_GAP() claim_take_gap()
#include "swdec_csc_claim.h"
#include "swdec_csc_mask.h"
#include "swdec_csc.h"
#include "swdec_occupancy.h"
#include "swdec_publish.h"
#include "swdec_reset_gate.h"

#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define CHECK(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "CHECK failed at %s:%d: %s\n", \
                __FILE__, __LINE__, #condition); \
        return 1; \
    } \
} while (0)

static uint64_t now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t) ts.tv_sec * 1000000000u + (uint64_t) ts.tv_nsec;
}

/* ---- publication: a reader never waits for a paused writer ---- */

typedef struct {
    SwdecPublishedU64 *published;
    uint64_t value;
    atomic_bool done;
} PausedRead;

static void *paused_reader(void *argument)
{
    PausedRead *read = argument;
    read->value = swdec_published_read(read->published, UINT64_MAX);
    atomic_store(&read->done, true);
    return NULL;
}

/* The reader runs on its own thread while the writer is stopped part-way
   through a publication, as a browser thread preempted by the audio thread
   would be. It must return the previous value promptly. A reader that waits
   for the writer never returns here; the test then fails rather than hangs
   (the stuck thread is abandoned at exit). */
static int published_read_with_writer_paused(
    SwdecPublishedU64 *published, uint64_t expected)
{
    PausedRead read = { .published = published, .value = 0 };
    atomic_init(&read.done, false);
    pthread_t thread;
    if (pthread_create(&thread, NULL, paused_reader, &read) != 0) return 1;
    uint64_t start = now_ns();
    while (!atomic_load(&read.done)) {
        if (now_ns() - start > UINT64_C(1000000000)) {
            fprintf(stderr, "reader did not return within 1 s while the "
                            "writer was paused mid-publication\n");
            fflush(stderr);
            _exit(1);
        }
        sched_yield();
    }
    pthread_join(thread, NULL);
    CHECK(read.value == expected);
    return 0;
}

#define PUBLISH_STRESS_WRITES 200000u
typedef struct {
    SwdecPublishedU64 published;
    atomic_bool stop;
} PublishStress;

static void *publish_stress_writer(void *argument)
{
    PublishStress *stress = argument;
    for (uint64_t i = 1; i <= PUBLISH_STRESS_WRITES; i++) {
        /* Both halves carry the counter, so a torn read is detectable. */
        swdec_published_write(&stress->published, (i << 32) | i);
    }
    atomic_store(&stress->stop, true);
    return NULL;
}

static int test_publication(void)
{
    puts("test: a paused publication leaves the previous value readable");
    SwdecPublishedU64 published;
    swdec_published_init(&published, 0);
    swdec_published_write(&published, UINT64_C(0x0000000500000007));
    CHECK(swdec_published_read(&published, 0) == UINT64_C(0x0000000500000007));

    /* Paused after choosing its slot. */
    unsigned slot = swdec_published_begin(&published);
    if (published_read_with_writer_paused(
            &published, UINT64_C(0x0000000500000007)) != 0) return 1;
    /* Paused between the two halves. */
    atomic_store_explicit(&published.low[slot & 1u], 9u, memory_order_relaxed);
    if (published_read_with_writer_paused(
            &published, UINT64_C(0x0000000500000007)) != 0) return 1;
    atomic_store_explicit(&published.high[slot & 1u], 8u,
                          memory_order_relaxed);
    if (published_read_with_writer_paused(
            &published, UINT64_C(0x0000000500000007)) != 0) return 1;
    swdec_published_commit(&published);
    CHECK(swdec_published_read(&published, 0) == UINT64_C(0x0000000800000009));

    puts("test: a reader retries at most a bounded number of times");
    uint64_t value = 0;
    CHECK(swdec_published_try_read(&published, &value));
    CHECK(value == UINT64_C(0x0000000800000009));

    puts("test: concurrent reads are never torn");
    PublishStress stress;
    swdec_published_init(&stress.published, 0);
    atomic_init(&stress.stop, false);
    pthread_t writer;
    CHECK(pthread_create(&writer, NULL, publish_stress_writer, &stress) == 0);
    uint64_t last = 0;
    unsigned reads = 0, torn = 0, backwards = 0;
    while (!atomic_load(&stress.stop) || reads == 0) {
        uint64_t seen = swdec_published_read(&stress.published, last);
        if ((seen >> 32) != (seen & UINT32_MAX)) torn++;
        if (seen < last) backwards++;
        last = seen;
        reads++;
    }
    pthread_join(writer, NULL);
    CHECK(torn == 0);
    CHECK(backwards == 0);
    CHECK(swdec_published_read(&stress.published, 0)
          == ((uint64_t) PUBLISH_STRESS_WRITES << 32 | PUBLISH_STRESS_WRITES));
    return 0;
}

/* ---- PCM ring fence ---- */

/* A model audio thread. `output` stands for the blocking output call: the
   block it is handed stays in the hardware's hands until the NEXT call
   returns. ring_reads counts ring blocks the hardware may still read. */
typedef struct {
    SwdecAudioFence fence;
    atomic_bool stop;
    atomic_int ring_in_hardware;   /* 0 or 1: a ring block may be read */
    atomic_uint ring_blocks;
    atomic_uint silence_blocks;
    atomic_bool use_fence;
    atomic_bool fail_output;       /* the output call returns an error */
    atomic_uint failed_calls;
} FenceModel;

/* False when the output call failed: then nothing is known about the
   hardware, which may still hold the previous block. */
static bool model_output(FenceModel *model, bool ring_block)
{
    struct timespec pause = { 0, 200000 };   /* the call blocks ~ a block */
    nanosleep(&pause, NULL);
    if (atomic_load(&model->fail_output)) {
        atomic_fetch_add(&model->failed_calls, 1u);
        return false;
    }
    /* The call returned: the previous block is drained, this one queued. */
    atomic_store(&model->ring_in_hardware, ring_block ? 1 : 0);
    return true;
}

static void *fence_audio_thread(void *argument)
{
    FenceModel *model = argument;
    while (!atomic_load(&model->stop)) {
        unsigned hold = atomic_load(&model->use_fence)
            ? swdec_audio_fence_hold(&model->fence) : 0u;
        if (hold == 0u) {
            (void) model_output(model, true);
            atomic_fetch_add(&model->ring_blocks, 1u);
        } else {
            /* As the backend: acknowledge only a silence call that
               succeeded. */
            if (model_output(model, false))
                swdec_audio_fence_acknowledge(&model->fence, hold);
            atomic_fetch_add(&model->silence_blocks, 1u);
        }
    }
    swdec_audio_fence_set_thread(&model->fence, false);
    return NULL;
}

static int test_audio_fence(void)
{
    puts("test: a seek frees the PCM ring only after the hardware let go");
    FenceModel model;
    swdec_audio_fence_init(&model.fence);
    atomic_init(&model.stop, false);
    atomic_init(&model.ring_in_hardware, 0);
    atomic_init(&model.ring_blocks, 0u);
    atomic_init(&model.silence_blocks, 0u);
    atomic_init(&model.use_fence, true);
    atomic_init(&model.fail_output, false);
    atomic_init(&model.failed_calls, 0u);
    CHECK(!swdec_audio_fence_parked(&model.fence));   /* not raised */
    swdec_audio_fence_set_thread(&model.fence, true);
    pthread_t thread;
    CHECK(pthread_create(&thread, NULL, fence_audio_thread, &model) == 0);

    for (unsigned seek = 0; seek < 50u; seek++) {
        while (atomic_load(&model.ring_blocks) < (seek + 1u) * 2u)
            sched_yield();
        unsigned generation = swdec_audio_fence_raise(&model.fence);
        CHECK(generation != 0u);
        /* Raising again while pending folds into the same raise. */
        CHECK(swdec_audio_fence_raise(&model.fence) == generation);
        uint64_t start = now_ns();
        while (!swdec_audio_fence_parked(&model.fence)) {
            CHECK(now_ns() - start < UINT64_C(2000000000));
            sched_yield();
        }
        /* Parked: the ring may be freed. Nothing of it may be in use. */
        CHECK(atomic_load(&model.ring_in_hardware) == 0);
        struct timespec pause = { 0, 500000 };
        nanosleep(&pause, NULL);                    /* "reinitialise" */
        CHECK(atomic_load(&model.ring_in_hardware) == 0);
        swdec_audio_fence_lower(&model.fence);
        CHECK(!swdec_audio_fence_parked(&model.fence));
    }
    CHECK(atomic_load(&model.silence_blocks) >= 50u);

    puts("test: a failed silence call does not open the fence");
    while (atomic_load(&model.ring_in_hardware) == 0) sched_yield();
    atomic_store(&model.fail_output, true);
    unsigned failing = swdec_audio_fence_raise(&model.fence);
    CHECK(failing != 0u);
    while (atomic_load(&model.failed_calls) < 20u) {
        CHECK(!swdec_audio_fence_parked(&model.fence));
        sched_yield();
    }
    CHECK(!swdec_audio_fence_parked(&model.fence));
    CHECK(atomic_load(&model.ring_in_hardware) == 1);   /* still held */
    atomic_store(&model.fail_output, false);   /* the channel recovers */
    uint64_t recovered = now_ns();
    while (!swdec_audio_fence_parked(&model.fence)) {
        CHECK(now_ns() - recovered < UINT64_C(2000000000));
        sched_yield();
    }
    CHECK(atomic_load(&model.ring_in_hardware) == 0);
    swdec_audio_fence_lower(&model.fence);

    puts("test: the model detects the hazard when no fence is used");
    atomic_store(&model.use_fence, false);
    while (atomic_load(&model.ring_in_hardware) == 0) sched_yield();
    CHECK(atomic_load(&model.ring_in_hardware) == 1);   /* what a seek
        without the fence would have freed underneath the hardware */

    atomic_store(&model.stop, true);
    pthread_join(thread, NULL);

    puts("test: an ended audio thread holds no block");
    CHECK(swdec_audio_fence_raise(&model.fence) != 0u);
    CHECK(swdec_audio_fence_parked(&model.fence));
    swdec_audio_fence_lower(&model.fence);
    return 0;
}

/* ---- deferred reset ---- */

static int test_reset_gate(void)
{
    puts("test: a seek requests the reset and returns at once");
    SwdecResetGate gate;
    swdec_reset_gate_init(&gate, 2000000u);
    SwdecResetInputs busy = { false, true, true, true };
    SwdecResetInputs quiet = { true, true, true, true };
    CHECK(swdec_reset_gate_poll(&gate, quiet, 0) == SWDEC_RESET_IDLE);
    CHECK(swdec_reset_gate_request(&gate, 1000u));
    CHECK(swdec_reset_gate_poll(&gate, busy, 1000u) == SWDEC_RESET_WAIT);
    CHECK(swdec_reset_gate_poll(&gate, busy, 900000u) == SWDEC_RESET_WAIT);

    puts("test: a seek while one is pending folds into it");
    CHECK(!swdec_reset_gate_request(&gate, 950000u));
    CHECK(gate.coalesced == 1u);
    CHECK(gate.requested_us == 1000u);

    puts("test: each quiet condition is required");
    SwdecResetInputs cases[4] = {
        { false, true, true, true }, { true, false, true, true },
        { true, true, false, true }, { true, true, true, false }
    };
    for (unsigned i = 0; i < 4u; i++)
        CHECK(swdec_reset_gate_poll(&gate, cases[i], 5000u)
              == SWDEC_RESET_WAIT);
    CHECK(swdec_reset_gate_poll(&gate, quiet, 5000u) == SWDEC_RESET_READY);
    swdec_reset_gate_complete(&gate);
    CHECK(!gate.pending);
    CHECK(swdec_reset_gate_poll(&gate, busy, 6000u) == SWDEC_RESET_IDLE);

    puts("test: a reset that never quiesces times out, across clock wrap");
    CHECK(swdec_reset_gate_request(&gate, UINT32_MAX - 1000u));
    CHECK(swdec_reset_gate_poll(&gate, busy, 500u) == SWDEC_RESET_WAIT);
    CHECK(swdec_reset_gate_poll(&gate, busy, 1999000u) == SWDEC_RESET_TIMEOUT);
    swdec_reset_gate_complete(&gate);
    return 0;
}

/* ---- audio gap measurement ---- */

/* What the previous validation build computed: the next block's START
   against the previous call's RETURN, counted only beyond two blocks. */
static unsigned legacy_gaps(const uint64_t *start, const uint64_t *ret,
                            unsigned count, uint32_t block_us)
{
    unsigned gaps = 0;
    for (unsigned i = 1; i < count; i++)
        if (start[i] - ret[i - 1] > 2u * (uint64_t) block_us) gaps++;
    return gaps;
}

static int test_audio_gaps(void)
{
    puts("test: a hole of a few milliseconds between returns is counted");
    const uint32_t block = 23219u;   /* 1024 samples at 44.1 kHz */
    /* Steady playback, then one call that returned 9 ms late (a higher
       priority thread held the CPU before the call was made). */
    uint64_t start[6], ret[6];
    uint64_t t = 1000000u;
    for (unsigned i = 0; i < 6u; i++) {
        start[i] = t + 50u;
        if (i == 3u) start[i] += 9000u;
        ret[i] = start[i] + block - 50u;
        t = ret[i];
    }
    SwdecAudioGapStats stats = {0};
    for (unsigned i = 0; i < 6u; i++)
        swdec_audio_gap_note_return(&stats, ret[i], block);
    CHECK(stats.intervals == 5u);
    CHECK(stats.gaps == 1u);
    CHECK(stats.max_excess_us >= 9000u && stats.max_excess_us < 9100u);
    CHECK(stats.histogram[2] == 1u);   /* 5..10 ms */
    CHECK(stats.histogram[0] == 4u);   /* on time */
    CHECK(legacy_gaps(start, ret, 6u, block) == 0u);   /* the old miss */

    puts("test: jitter under the threshold is not a gap");
    SwdecAudioGapStats jitter = {0};
    uint64_t at = 5000000u;
    for (unsigned i = 0; i < 20u; i++) {
        at += block + (i % 2u ? 3000u : 0u);
        swdec_audio_gap_note_return(&jitter, at, block);
    }
    CHECK(jitter.gaps == 0u);
    CHECK(jitter.histogram[1] == 10u);  /* 2..5 ms */
    CHECK(jitter.histogram[0] == 9u);

    puts("test: pauses and seeks do not count as holes");
    swdec_audio_gap_break(&jitter);
    swdec_audio_gap_note_return(&jitter, at + 5000000u, block);
    CHECK(jitter.gaps == 0u);
    swdec_audio_gap_note_return(&jitter, at + 5000000u + block + 60000u,
                                block);
    CHECK(jitter.gaps == 1u);
    CHECK(jitter.histogram[5] == 1u);   /* 46 ms and over */
    return 0;
}

/* ---- compressed video queue ---- */

static int test_queue_plan(void)
{
    puts("test: video-only playback keeps a small prefetch queue");
    size_t preferred = 0, floor = 0;
    CHECK(swdec_au_queue_plan(20u * 1024u, true, &preferred, &floor));
    CHECK(preferred == 256u * 1024u);                 /* unchanged */
    CHECK(swdec_au_queue_plan(20u * 1024u, false, &preferred, &floor));
    CHECK(preferred == 64u * 1024u);
    CHECK(floor == 32u * 1024u);
    CHECK(swdec_au_queue_plan(60u * 1024u, true, &preferred, &floor));
    CHECK(preferred == 480u * 1024u);                 /* unchanged */
    CHECK(swdec_au_queue_plan(60u * 1024u, false, &preferred, &floor));
    CHECK(preferred == 120u * 1024u);
    CHECK(floor == 60u * 1024u + 1u);
    CHECK(swdec_au_queue_plan(200u * 1024u, true, &preferred, &floor));
    CHECK(preferred == 1024u * 1024u);
    CHECK(swdec_au_queue_plan(600u * 1024u, false, &preferred, &floor));
    CHECK(preferred == 1024u * 1024u);
    CHECK(preferred > 600u * 1024u && floor > 600u * 1024u);
    CHECK(!swdec_au_queue_plan(1024u * 1024u, true, &preferred, &floor));

    puts("test: under memory pressure the queue steps down to its floor");
    CHECK(swdec_au_queue_plan(40u * 1024u, true, &preferred, &floor));
    size_t tried[16];
    unsigned n = 0;
    for (size_t bytes = preferred; bytes != 0 && n < 16u;
         bytes = swdec_au_queue_step_down(bytes, floor))
        tried[n++] = bytes;
    CHECK(n == 4u);
    CHECK(tried[0] == 320u * 1024u);
    CHECK(tried[1] == 160u * 1024u);
    CHECK(tried[2] == 80u * 1024u);
    CHECK(tried[3] == 40u * 1024u + 1u);
    for (unsigned i = 0; i < n; i++) CHECK(tried[i] > 40u * 1024u);
    return 0;
}

/* ---- audio start, frame shape, ME recovery ---- */

static int test_audio_and_recovery_policy(void)
{
    puts("test: a stream shorter than the prebuffer still starts at its end");
    /* 12 AAC frames in all (0.28 s): the old rule, decoded >= 20, never
       starts, so the clock stays at zero and the clip ends silent. */
    CHECK(!(12u >= 20u));
    CHECK(!swdec_audio_prebuffered(12u, 12u, false, 20u));
    CHECK(swdec_audio_prebuffered(12u, 12u, true, 20u));
    CHECK(!swdec_audio_prebuffered(11u, 12u, true, 20u));   /* ME behind */
    CHECK(!swdec_audio_prebuffered(0u, 0u, true, 20u));     /* no audio */
    CHECK(swdec_audio_prebuffered(20u, 40u, false, 20u));

    puts("test: implicit SBR blocks are refused, failed frames play");
    CHECK(!swdec_audio_block_refused(1024u));
    CHECK(swdec_audio_block_refused(2048u));
    CHECK(!swdec_audio_block_refused((unsigned) -5));        /* error code */
    CHECK(!swdec_audio_block_refused(960u));

    puts("test: a failed ME is restarted only between jobs");
    CHECK(swdec_me_recovery_action(false, 2u) == SWDEC_ME_RECOVERY_NONE);
    CHECK(swdec_me_recovery_action(true, 2u) == SWDEC_ME_RECOVERY_WAIT);
    CHECK(swdec_me_recovery_action(true, 1u) == SWDEC_ME_RECOVERY_WAIT);
    CHECK(swdec_me_recovery_action(true, 0u) == SWDEC_ME_RECOVERY_RESTART);
    return 0;
}

/* ---- colour-conversion claim: a late Media Engine batch never lands in a
   slot after the CPU took it back ---- */

#define CLAIM_SLOTS 3
#define CLAIM_UNITS 34u
typedef struct {
    volatile unsigned generation;          /* picture bound to the slot */
    volatile unsigned pixels[CLAIM_UNITS]; /* generation that converted each unit */
    unsigned me_limit;
    SwdecCscMask done;
} ClaimSlot;
typedef struct {
    SwdecCscClaim claim;
    ClaimSlot slots[CLAIM_SLOTS];
    atomic_bool stop;
    atomic_uint me_units;
    atomic_bool torn;
    atomic_bool cpu_owns;   /* the CPU took the slots back, none is offered */
    unsigned seed;
} ClaimWorld;

static unsigned claim_rand(unsigned *seed)
{
    *seed = *seed * 1103515245u + 12345u;
    return (*seed >> 16) & 0x7fffu;
}
static _Thread_local unsigned claim_gap_seed;
static void claim_take_gap(void)
{
    if (claim_gap_seed != 0u && claim_rand(&claim_gap_seed) % 2u == 0u)
        usleep(claim_rand(&claim_gap_seed) % 200u);
}

/* Fake ME: converts up to three units per batch and finishes late (sleeps
   between reading the binding and writing its pixels and mask bits). */
static void *claim_me_thread(void *argument)
{
    ClaimWorld *world = argument;
    unsigned seed = world->seed;
    claim_gap_seed = seed + 1u;
    while (!atomic_load(&world->stop)) {
        int slot = swdec_csc_claim_take(&world->claim);
        if (slot < 0) { sched_yield(); continue; }
        ClaimSlot *s = &world->slots[slot];
        unsigned generation = s->generation;
        /* a batch only ever runs on an offered slot, and its binding
           cannot change under it */
        if (atomic_load(&world->cpu_owns)) atomic_store(&world->torn, true);
        claim_take_gap();
        if (s->generation != generation) atomic_store(&world->torn, true);
        unsigned picked[3], n = 0;
        for (unsigned unit = 0; unit < s->me_limit && n < 3u; unit++)
            if (!swdec_csc_mask_test(&s->done, unit)) picked[n++] = unit;
        if (claim_rand(&seed) % 4u == 0u) usleep(claim_rand(&seed) % 300u);
        for (unsigned i = 0; i < n; i++) s->pixels[picked[i]] = generation;
        for (unsigned i = 0; i < n; i++) swdec_csc_mask_set(&s->done, picked[i]);
        atomic_fetch_add(&world->me_units, n);
        swdec_csc_claim_release(&world->claim);
    }
    return NULL;
}

static int claim_take_back(ClaimWorld *world)
{
    (void) swdec_csc_claim_withdraw(&world->claim);
    uint64_t started = now_ns();
    while (swdec_csc_claim_held(&world->claim)) {
        if (now_ns() - started > 2000000000u) return -1;
        sched_yield();
    }
    atomic_store(&world->cpu_owns, true);
    return 0;
}
static void claim_offer(ClaimWorld *world, int slot)
{
    atomic_store(&world->cpu_owns, false);
    swdec_csc_claim_offer(&world->claim, slot);
}

static int test_csc_claim(void)
{
    /* the state machine on one thread */
    static ClaimWorld world;
    swdec_csc_claim_init(&world.claim);
    CHECK(swdec_csc_claim_take(&world.claim) == -1);
    swdec_csc_claim_offer(&world.claim, 2);
    CHECK(swdec_csc_claim_take(&world.claim) == 2);
    CHECK(swdec_csc_claim_withdraw(&world.claim) == 2);
    CHECK(swdec_csc_claim_held(&world.claim));   /* the batch still runs */
    CHECK(swdec_csc_claim_take(&world.claim) == -1);
    CHECK(swdec_csc_claim_held(&world.claim));
    swdec_csc_claim_release(&world.claim);
    CHECK(!swdec_csc_claim_held(&world.claim));
    CHECK(swdec_csc_claim_withdraw(&world.claim) == -1);

    CHECK(swdec_csc_share_bucket(0u, 30u) == 0u);
    CHECK(swdec_csc_share_bucket(1u, 30u) == 1u);
    CHECK(swdec_csc_share_bucket(7u, 28u) == 1u);
    CHECK(swdec_csc_share_bucket(8u, 28u) == 2u);
    CHECK(swdec_csc_share_bucket(21u, 28u) == 3u);
    CHECK(swdec_csc_share_bucket(27u, 28u) == 4u);
    CHECK(swdec_csc_share_bucket(28u, 28u) == 5u);
    CHECK(swdec_csc_share_bucket(3u, 0u) == 0u);

    /* against a late fake ME: every published picture holds only its own
       pixels, and stays so until its slot is bound again */
    swdec_csc_claim_init(&world.claim);
    atomic_store(&world.stop, false);
    atomic_store(&world.me_units, 0u);
    atomic_store(&world.torn, false);
    atomic_store(&world.cpu_owns, true);
    world.seed = 7u;
    pthread_t me;
    CHECK(pthread_create(&me, NULL, claim_me_thread, &world) == 0);
    unsigned seed = 11u, published[CLAIM_SLOTS] = {0, 0, 0};
    unsigned cpu_units = 0;
    int failed = 0;
    for (unsigned picture = 1; picture <= 10000u && !failed; picture++) {
        int slot = (int) (picture % CLAIM_SLOTS);
        ClaimSlot *s = &world.slots[slot];
        /* bind: take the previous offer back first */
        if (claim_take_back(&world) != 0) { failed = 1; break; }
        for (unsigned unit = 0; unit < CLAIM_UNITS && published[slot]; unit++)
            if (s->pixels[unit] != published[slot]) failed = 2;
        s->generation = picture;
        swdec_csc_mask_clear(&s->done);
        s->me_limit = swdec_csc_cpu_first(
            CLAIM_UNITS, (const void *) (uintptr_t) 64u, 512u,
            (picture & 1u) != 0u);
        claim_offer(&world, slot);
        /* This range is never offered to the ME. Do not touch its mask
           until the old whole-slot claim has been withdrawn and joined. */
        for (unsigned unit = s->me_limit; unit < CLAIM_UNITS; unit++) {
            s->pixels[unit] = picture;
            cpu_units++;
        }
        if (claim_rand(&seed) % 2u) usleep(claim_rand(&seed) % 200u);
        /* close: take it back, finish on the CPU, publish */
        if (claim_take_back(&world) != 0) { failed = 1; break; }
        for (unsigned unit = s->me_limit; unit < CLAIM_UNITS; unit++)
            swdec_csc_mask_set(&s->done, unit);
        for (unsigned unit = 0; unit < CLAIM_UNITS; unit++) {
            if (swdec_csc_mask_test(&s->done, unit)) continue;
            s->pixels[unit] = picture;
            swdec_csc_mask_set(&s->done, unit);
            cpu_units++;
        }
        for (unsigned unit = 0; unit < CLAIM_UNITS; unit++)
            if (s->pixels[unit] != picture) failed = 3;
        published[slot] = picture;
    }
    /* a last late batch would land here */
    usleep(2000);
    for (int slot = 0; slot < CLAIM_SLOTS && !failed; slot++)
        for (unsigned unit = 0; unit < CLAIM_UNITS; unit++)
            if (world.slots[slot].pixels[unit] != published[slot]) failed = 2;
    atomic_store(&world.stop, true);
    pthread_join(me, NULL);
    if (failed) fprintf(stderr, "csc claim failure kind %d\n", failed);
    CHECK(failed == 0);
    CHECK(!atomic_load(&world.torn));
    CHECK(atomic_load(&world.me_units) > 0u);   /* the ME took part */
    CHECK(cpu_units > 0u);
    return 0;
}

static int test_csc_parallel_pixels(void)
{
    static uint8_t y[512 * 432], u[256 * 216], v[256 * 216];
    static _Alignas(64) uint16_t reference[512 * 432], actual[512 * 432];
    CHECK(swdec_csc_unit_count(0) == 0u);
    CHECK(swdec_csc_unit_count(-1) == 0u);
    CHECK(swdec_csc_unit_count(INT32_MAX) == 0u);
    CHECK(swdec_csc_unit_count(513) == 0u);
    CHECK(swdec_csc_unit_count(512) == 64u);
    CHECK(swdec_csc_unit_count(272) == 34u);
    CHECK(swdec_csc_cpu_first(30u, actual, 512u, true) == 10u);
    CHECK(swdec_csc_cpu_first(30u, actual, 512u, false) == 30u);
    CHECK(swdec_csc_cpu_first(30u, NULL, 512u, true) == 30u);
    CHECK(swdec_csc_cpu_first(30u, actual + 1, 512u, true) == 30u);
    CHECK(swdec_csc_cpu_first(30u, actual, 510u, true) == 30u);
    CHECK(swdec_csc_cpu_first(30u, actual, 0u, true) == 30u);
    CHECK(swdec_csc_cpu_first(5u, actual, 512u, true) == 5u);
    CHECK(swdec_csc_cpu_first(65u, actual, 512u, true) == 65u);
    for (unsigned i = 0; i < sizeof(y); i++) y[i] = (uint8_t) (i * 37u + i / 509u);
    for (unsigned i = 0; i < sizeof(u); i++) {
        u[i] = (uint8_t) (i * 19u + i / 251u);
        v[i] = (uint8_t) (i * 71u + i / 239u);
    }
    const int widths[] = {2, 8, 14, 128, 416, 426, 480};
    const int heights[] = {2, 6, 8, 10, 40, 46, 48, 50, 234, 240, 270, 272, 432};
    for (unsigned wi = 0; wi < sizeof(widths) / sizeof(*widths); wi++)
        for (unsigned hi = 0; hi < sizeof(heights) / sizeof(*heights); hi++) {
            int h = heights[hi];
            unsigned units = swdec_csc_unit_count(h);
            unsigned split = swdec_csc_cpu_first(units, actual, 512u, true);
            SwdecCscPlanes p = {y, u, v, 512, 256, widths[wi], reference, 512};
            memset(reference, 0xA5, sizeof(reference));
            swdec_csc_rows_c(&p, 0, h);
            /* Every possible prefix completion, including an unavailable
               ME: finish its remainder only after reclaim. Check padding
               and all untouched rows as well as visible pixels. */
            for (unsigned completed = 0; completed <= split; completed++) {
                memset(actual, 0xA5, sizeof(actual));
                SwdecCscMask done = {{0, 0}};
                p.dst = actual;
                for (unsigned b = 0; b < units; b++) {
                    if (b >= completed && b < split) continue;
                    int end = (int) (b + 1u) * 8;
                    if (end > h) end = h;
                    swdec_csc_rows_c(&p, (int) b * 8, end);
                    if (b < split) swdec_csc_mask_set(&done, b);
                }
                for (unsigned b = split; b < units; b++) swdec_csc_mask_set(&done, b);
                for (unsigned b = 0; b < units; b++) {
                    if (swdec_csc_mask_test(&done, b)) continue;
                    int end = (int) (b + 1u) * 8;
                    if (end > h) end = h;
                    swdec_csc_rows_c(&p, (int) b * 8, end);
                }
                CHECK(memcmp(reference, actual, sizeof(actual)) == 0);
            }
        }
    return 0;
}

/* ---- the bounded reclaim against a fake ME that never acknowledges:
   the CPU stops it before touching the slot, or keeps the slot quarantined
   when it cannot be stopped ---- */

typedef struct {
    SwdecCscClaim claim;
    volatile unsigned pixels[4];
    atomic_bool reset;       /* the ME is held in reset */
    atomic_bool stop_works;
    atomic_bool quit;
    atomic_uint writes_after_reset;
    unsigned stops;
    uint64_t base_ns;
} WedgedWorld;

static void *wedged_me_thread(void *argument)
{
    WedgedWorld *world = argument;
    while (swdec_csc_claim_take(&world->claim) < 0)
        if (atomic_load(&world->quit)) return NULL;
    /* Takes the batch and never ends it; keeps writing until reset. */
    while (!atomic_load(&world->quit)) {
        if (atomic_load(&world->reset)) break;
        world->pixels[0] = world->pixels[0] + 1u;
        usleep(100);
    }
    return NULL;
}

static unsigned wedged_now(void *context)
{
    WedgedWorld *world = context;
    return (unsigned) ((now_ns() - world->base_ns) / 1000u);
}
static void wedged_pause(void *context, unsigned waited_us)
{
    (void) context;
    (void) waited_us;
    usleep(200);
}
static bool wedged_stop(void *context)
{
    WedgedWorld *world = context;
    world->stops++;
    if (!atomic_load(&world->stop_works)) return false;
    atomic_store(&world->reset, true);
    return true;
}

static int wedged_run(bool stop_works, SwdecCscReclaim expected)
{
    static WedgedWorld world;
    swdec_csc_claim_init(&world.claim);
    world.pixels[0] = 0;
    world.stops = 0;
    world.base_ns = now_ns();
    atomic_store(&world.reset, false);
    atomic_store(&world.quit, false);
    atomic_store(&world.stop_works, stop_works);
    swdec_csc_claim_offer(&world.claim, 1);
    pthread_t me;
    CHECK(pthread_create(&me, NULL, wedged_me_thread, &world) == 0);
    while (!swdec_csc_claim_held(&world.claim)) sched_yield();
    SwdecCscReclaimOps ops = {
        wedged_now, wedged_pause, wedged_stop, &world
    };
    int held = -2;
    uint64_t started = now_ns();
    SwdecCscReclaim result = swdec_csc_claim_reclaim(
        &world.claim, 20000u, &ops, &held);
    uint64_t waited_us = (now_ns() - started) / 1000u;
    CHECK(result == expected);
    CHECK(world.stops == 1u);
    CHECK(waited_us >= 20000u);
    if (expected == SWDEC_CSC_RECLAIM_STOPPED) {
        /* Stopped first: no batch is announced and no write follows. */
        CHECK(held == -1);
        CHECK(!swdec_csc_claim_held(&world.claim));
        pthread_join(me, NULL);
        unsigned after = world.pixels[0];
        usleep(2000);
        CHECK(world.pixels[0] == after);
        CHECK(swdec_csc_claim_take(&world.claim) == -1);  /* nothing offered */
    } else {
        /* Not stoppable: the slot is reported as the ME's, which may
           still write it, and the batch stays announced. */
        CHECK(held == 1);
        CHECK(swdec_csc_claim_held(&world.claim));
        unsigned before = world.pixels[0];
        usleep(2000);
        CHECK(world.pixels[0] != before);
        atomic_store(&world.quit, true);
        pthread_join(me, NULL);
    }
    return 0;
}

static int test_csc_reclaim_timeout(void)
{
    if (wedged_run(true, SWDEC_CSC_RECLAIM_STOPPED) != 0) return 1;
    if (wedged_run(false, SWDEC_CSC_RECLAIM_HELD) != 0) return 1;
    return 0;
}

/* ---- Media Engine occupancy shares and their 95 % intervals ---- */
static int test_occupancy_share(void)
{
    unsigned share, ci;
    swdec_occupancy_share(0u, 0u, &share, &ci);
    CHECK(share == 0u && ci == 0u);
    swdec_occupancy_share(5000u, 10000u, &share, &ci);
    CHECK(share == 500u);          /* 50.0 % */
    CHECK(ci == 10u);              /* 1.96 * 0.5 % = 0.98 %, 1.0 % rounded */
    swdec_occupancy_share(100u, 10000u, &share, &ci);
    CHECK(share == 10u);           /* 1.0 % */
    CHECK(ci == 2u);               /* 1.96 * sqrt(.01 * .99 / 1e4) = 0.195 % */
    swdec_occupancy_share(10000u, 10000u, &share, &ci);
    CHECK(share == 1000u && ci == 0u);
    swdec_occupancy_share(400u, 1600u, &share, &ci);
    CHECK(share == 250u);
    CHECK(ci == 21u);              /* 1.96 * sqrt(.25 * .75 / 1600) = 2.12 % */
    swdec_occupancy_share(3u, 2u, &share, &ci);
    CHECK(share == 0u && ci == 0u);
    CHECK(swdec_occupancy_isqrt64(0u) == 0u);
    CHECK(swdec_occupancy_isqrt64(99u) == 9u);
    CHECK(swdec_occupancy_isqrt64(250000000000ull) == 500000u);
    return 0;
}

static int test_audio_clock(void)
{
    /* A picture due in 3ms must not wait an entire 16.7ms display refresh. */
    CHECK(swdec_audio_due_wait_us(123000, 40000, 100000, 120000, 16667) == 3000);
    CHECK(swdec_audio_due_wait_us(120000, 40000, 100000, 120000, 16667) == 1000);
    CHECK(swdec_audio_due_wait_us(110000, 40000, 100000, 120000, 16667) == 1000);
    /* Crossing an accepted-block boundary is a reason to recheck, not to
       wait a whole refresh. It must not extrapolate the presentation clock. */
    CHECK(swdec_audio_due_wait_us(123000, 40000, 100000, 100000, 16667) == 3000);
    CHECK(swdec_audio_due_wait_us(123000, 40000, 101054, 101894, 16667) == 1946);
    CHECK(swdec_audio_due_wait_us(123000, 40000, 0, 0, 16667) == 0);
    CHECK(swdec_audio_due_wait_us(123000, 40000, 100001, 100000, 16667) == 0);
    CHECK(swdec_audio_due_wait_us(180000, 40000, 100000, 200000, 16667) == 16667);
    CHECK(swdec_audio_due_wait_us(123000, 40000, 100000, 120000, 0) == 0);
    CHECK(swdec_audio_due_wait_us(120000, 40000, 100000, 120000, 100) == 100);
    CHECK(swdec_audio_due_wait_us(UINT64_MAX, 0, UINT64_MAX - 3000,
                                  UINT64_MAX, 16667) == 3000);
    CHECK(swdec_audio_due_wait_us(0, UINT64_MAX, 0, 1, 16667) == 1000);
    SwdecAudioClock clock;
    swdec_audio_clock_init(&clock);
    CHECK(swdec_audio_clock_read(&clock, 1000, 99) == 0);
    CHECK(swdec_audio_clock_pending(1024, 44100, 100000) == 23219);
    CHECK(swdec_audio_clock_pending(2048, 48000, 100000) == 42666);
    CHECK(swdec_audio_clock_pending(-1, 44100, 100000) == 0);
    CHECK(swdec_audio_clock_pending(2049, 44100, 100000) == 0);
    CHECK(swdec_audio_clock_pending(1024, 0, 100000) == 0);
    CHECK(swdec_audio_clock_pending(1024, 44100, 1000) == 1000);
    const uint64_t stamp = UINT64_C(0x100000001);
    swdec_audio_clock_write(&clock, 100000, stamp, 23000);
    uint64_t accepted = 0;
    CHECK(swdec_audio_clock_sample(&clock, stamp + 12000, 0, &accepted) == 89000);
    CHECK(accepted == 100000);
    CHECK(swdec_audio_clock_read(&clock, stamp - 1, 0) == 77000);
    CHECK(swdec_audio_clock_read(&clock, stamp, 0) == 77000);
    CHECK(swdec_audio_clock_read(&clock, stamp + 12000, 0) == 89000);
    CHECK(swdec_audio_clock_read(&clock, stamp + 23000, 0) == 100000);
    /* Underrun, pause, or an arbitrarily delayed reader cannot invent audio. */
    CHECK(swdec_audio_clock_read(&clock, UINT64_MAX, 0) == 100000);
    CHECK(swdec_audio_due_wait_us(123000, 40000,
        swdec_audio_clock_read(&clock, UINT64_MAX, 0), 100000, 16667) == 3000);
    CHECK(swdec_audio_clock_read(&clock, UINT64_MAX, 0) < 123000 - 40000 / 2);
    /* An interrupted publication leaves the entire prior observation valid. */
    unsigned slot = swdec_published_begin(&clock.end);
    swdec_published_store(&clock.end, slot, 500000);
    atomic_store(&clock.pending_us[slot], 9000);
    atomic_store(&clock.stamp_low[slot], 123);
    CHECK(swdec_audio_clock_read(&clock, stamp + 12000, 0) == 89000);
    /* A seek resets the old endpoint and timestamp together. */
    swdec_audio_clock_write(&clock, 0, 0, 0);
    CHECK(swdec_audio_clock_read(&clock, stamp + 12000, 99) == 0);
    /* Missing firmware sample count falls back to the block endpoint. */
    swdec_audio_clock_write(&clock, UINT64_MAX, stamp, 0);
    CHECK(swdec_audio_clock_read(&clock, stamp, 0) == UINT64_MAX);
    return 0;
}

int main(void)
{
    if (test_audio_clock() != 0) return 1;
    if (test_publication() != 0) return 1;
    if (test_audio_fence() != 0) return 1;
    if (test_reset_gate() != 0) return 1;
    if (test_audio_gaps() != 0) return 1;
    if (test_queue_plan() != 0) return 1;
    if (test_audio_and_recovery_policy() != 0) return 1;
    if (test_csc_claim() != 0) return 1;
    if (test_csc_parallel_pixels() != 0) return 1;
    if (test_csc_reclaim_timeout() != 0) return 1;
    if (test_occupancy_share() != 0) return 1;
    puts("swdec sync tests passed");
    return 0;
}
