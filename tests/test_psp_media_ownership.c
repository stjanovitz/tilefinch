#include "media_backend_psp_policy.h"
#include "psp_media_ownership.h"

#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>

#define FRAME_COUNT 50000u
#define PREPARED_COUNT 50000u

#define CHECK(condition) do {                                                \
    if (!(condition)) {                                                      \
        fprintf(stderr, "PSP MEDIA OWNERSHIP CHECK failed at %s:%d: %s\n", \
                __FILE__, __LINE__, #condition);                             \
        return 1;                                                            \
    }                                                                        \
} while (0)

typedef struct {
    PspMediaSurfaceSlot slots[PSP_MEDIA_SURFACE_SLOTS];
    atomic_bool failed;
    atomic_uint failure_code;
} SlotStress;

static uint32_t picture_signature(uint64_t identity)
{
    uint64_t mixed = identity * UINT64_C(0x9e3779b97f4a7c15);
    return (uint32_t) (mixed ^ (mixed >> 32));
}

static void jitter(unsigned value)
{
    if ((value & 7u) == 0u) sched_yield();
}

static void *slot_producer(void *opaque)
{
    SlotStress *stress = opaque;
    for (uint64_t identity = 1u; identity <= FRAME_COUNT; identity++) {
        int chosen;
        do {
            chosen = psp_media_slot_free_index(
                stress->slots, PSP_MEDIA_SURFACE_SLOTS);
            if (chosen < 0) sched_yield();
        } while (chosen < 0
                 && !atomic_load_explicit(
                        &stress->failed, memory_order_relaxed));
        if (chosen < 0) return NULL;
        PspMediaSurfaceSlot *slot = &stress->slots[chosen];
        slot->generation++;
        slot->epoch = PSP_MEDIA_EPOCH_FIRST;
        slot->identity = identity;
        slot->pts_us = identity * UINT64_C(40000);
        slot->duration_us = UINT64_C(40000);
        slot->sequence = identity;
        psp_media_slot_publish(slot, PSP_MEDIA_SLOT_ME_WRITING);
        slot->signature = picture_signature(identity);
        slot->canary_armed = true;
        slot->extent_validated = true;
        jitter((unsigned) identity);
        psp_media_slot_publish(slot, PSP_MEDIA_SLOT_READY);
    }
    return NULL;
}

static void *slot_consumer(void *opaque)
{
    SlotStress *stress = opaque;
    for (uint64_t expected = 1u; expected <= FRAME_COUNT; expected++) {
        int chosen;
        do {
            chosen = psp_media_slot_take_index(
                stress->slots, PSP_MEDIA_SURFACE_SLOTS,
                PSP_MEDIA_EPOCH_FIRST);
            if (chosen < 0) sched_yield();
        } while (chosen < 0
                 && !atomic_load_explicit(
                        &stress->failed, memory_order_relaxed));
        if (chosen < 0) return NULL;
        PspMediaSurfaceSlot *slot = &stress->slots[chosen];
        if (slot->identity != expected
            || slot->sequence != expected
            || slot->signature != picture_signature(expected)
            || !slot->canary_armed || !slot->extent_validated) {
            fprintf(stderr,
                    "claim expected=%llu actual=%llu sequence=%llu "
                    "signature=%08x/%08x canary=%d extent=%d state=%d\n",
                    (unsigned long long) expected,
                    (unsigned long long) slot->identity,
                    (unsigned long long) slot->sequence,
                    slot->signature, picture_signature(expected),
                    slot->canary_armed ? 1 : 0,
                    slot->extent_validated ? 1 : 0,
                    psp_media_slot_peek(slot));
            for (unsigned at = 0; at < PSP_MEDIA_SURFACE_SLOTS; at++) {
                fprintf(stderr, "slot[%u] state=%d id=%llu pts=%llu\n",
                        at, psp_media_slot_peek(&stress->slots[at]),
                        (unsigned long long) stress->slots[at].identity,
                        (unsigned long long) stress->slots[at].pts_us);
            }
            atomic_store_explicit(
                &stress->failed, true, memory_order_relaxed);
            atomic_store_explicit(
                &stress->failure_code, (unsigned) expected,
                memory_order_relaxed);
            return NULL;
        }
        psp_media_slot_publish(slot, PSP_MEDIA_SLOT_READING);
        jitter((unsigned) expected + 3u);
        /* A writer cannot reuse this storage until the release below. */
        if (slot->identity != expected
            || slot->signature != picture_signature(expected)) {
            fprintf(stderr,
                    "read expected=%llu actual=%llu signature=%08x/%08x "
                    "state=%d\n",
                    (unsigned long long) expected,
                    (unsigned long long) slot->identity,
                    slot->signature, picture_signature(expected),
                    psp_media_slot_peek(slot));
            atomic_store_explicit(
                &stress->failed, true, memory_order_relaxed);
            atomic_store_explicit(
                &stress->failure_code, (unsigned) expected,
                memory_order_relaxed);
            return NULL;
        }
        slot->canary_armed = false;
        slot->extent_validated = false;
        psp_media_slot_publish(slot, PSP_MEDIA_SLOT_FREE);
    }
    return NULL;
}

typedef struct {
    atomic_int state;
    uint64_t sequence;
    uint32_t signature;
    atomic_bool failed;
} PreparedStress;

static void *prepared_producer(void *opaque)
{
    PreparedStress *stress = opaque;
    for (uint64_t sequence = 1u; sequence <= PREPARED_COUNT; sequence++) {
        for (;;) {
            if (atomic_load_explicit(
                    &stress->state, memory_order_acquire)
                != PSP_MEDIA_CODEC_PREPARED_EMPTY) {
                sched_yield();
                continue;
            }
            stress->sequence = sequence;
            stress->signature = picture_signature(sequence);
            if (psp_media_prepared_try_publish(&stress->state)) break;
            jitter((unsigned) sequence);
        }
    }
    return NULL;
}

static void *prepared_worker(void *opaque)
{
    PreparedStress *stress = opaque;
    uint64_t expected = 1u;
    while (expected <= PREPARED_COUNT) {
        PspMediaPreparedWorkerResult result =
            psp_media_prepared_worker_take_or_close(&stress->state, true);
        if (result == PSP_MEDIA_PREPARED_WORKER_CLOSED) {
            if (!psp_media_prepared_reopen_closed(&stress->state)) {
                atomic_store_explicit(
                    &stress->failed, true, memory_order_relaxed);
                return NULL;
            }
            sched_yield();
            continue;
        }
        if (result != PSP_MEDIA_PREPARED_WORKER_CLAIMED) {
            sched_yield();
            continue;
        }
        if (stress->sequence != expected
            || stress->signature != picture_signature(expected)) {
            atomic_store_explicit(
                &stress->failed, true, memory_order_relaxed);
            return NULL;
        }
        if (!psp_media_prepared_release_claimed(&stress->state)) {
            atomic_store_explicit(
                &stress->failed, true, memory_order_relaxed);
            return NULL;
        }
        expected++;
    }
    return NULL;
}

int main(void)
{
    CHECK(!psp_media_startup_preroll_ready(0u));
    CHECK(!psp_media_startup_preroll_ready(1u));
    CHECK(psp_media_startup_preroll_ready(PSP_MEDIA_SURFACE_SLOTS));
    CHECK(psp_media_audio_cursor_advance_us(
              0u, 100000u, true, 500000u) == 100000u);
    CHECK(psp_media_audio_cursor_advance_us(
              100000u, 100000u, false, 500000u) == 100000u);
    CHECK(psp_media_audio_cursor_advance_us(
              100000u, 600000u, true, 500000u) == 500000u);

    /* Pin the ordering race which the concurrent stress first exposed: an
       older conversion can still be ME_WRITING when a newer slot is READY.
       It must become claimable first. A later in-flight conversion does not
       block an already-ready predecessor. */
    PspMediaSurfaceSlot ordered[PSP_MEDIA_SURFACE_SLOTS] = {0};
    for (unsigned at = 0; at < PSP_MEDIA_SURFACE_SLOTS; at++)
        atomic_init(&ordered[at].state, PSP_MEDIA_SLOT_FREE);
    ordered[0].epoch = PSP_MEDIA_EPOCH_FIRST;
    ordered[0].sequence = 1u;
    ordered[0].pts_us = 40000u;
    psp_media_slot_publish(&ordered[0], PSP_MEDIA_SLOT_ME_WRITING);
    ordered[1].epoch = PSP_MEDIA_EPOCH_FIRST;
    ordered[1].sequence = 2u;
    ordered[1].pts_us = 80000u;
    psp_media_slot_publish(&ordered[1], PSP_MEDIA_SLOT_READY);
    CHECK(psp_media_slot_take_index(
              ordered, PSP_MEDIA_SURFACE_SLOTS, PSP_MEDIA_EPOCH_FIRST) == -1);
    psp_media_slot_publish(&ordered[0], PSP_MEDIA_SLOT_READY);
    CHECK(psp_media_slot_take_index(
              ordered, PSP_MEDIA_SURFACE_SLOTS, PSP_MEDIA_EPOCH_FIRST) == 0);
    psp_media_slot_publish(&ordered[0], PSP_MEDIA_SLOT_FREE);
    psp_media_slot_publish(&ordered[1], PSP_MEDIA_SLOT_FREE);
    ordered[0].sequence = 3u;
    ordered[0].pts_us = 120000u;
    psp_media_slot_publish(&ordered[0], PSP_MEDIA_SLOT_READY);
    ordered[1].sequence = 4u;
    ordered[1].pts_us = 160000u;
    psp_media_slot_publish(&ordered[1], PSP_MEDIA_SLOT_ME_WRITING);
    CHECK(psp_media_slot_take_index(
              ordered, PSP_MEDIA_SURFACE_SLOTS, PSP_MEDIA_EPOCH_FIRST) == 0);

    SlotStress slots = {0};
    for (unsigned at = 0; at < PSP_MEDIA_SURFACE_SLOTS; at++)
        atomic_init(&slots.slots[at].state, PSP_MEDIA_SLOT_FREE);
    atomic_init(&slots.failed, false);
    atomic_init(&slots.failure_code, 0u);
    pthread_t producer;
    pthread_t consumer;
    CHECK(pthread_create(&producer, NULL, slot_producer, &slots) == 0);
    CHECK(pthread_create(&consumer, NULL, slot_consumer, &slots) == 0);
    CHECK(pthread_join(producer, NULL) == 0);
    CHECK(pthread_join(consumer, NULL) == 0);
    if (atomic_load_explicit(&slots.failed, memory_order_relaxed)) {
        fprintf(stderr, "slot failure at identity %u\n",
                atomic_load_explicit(
                    &slots.failure_code, memory_order_relaxed));
        return 1;
    }
    CHECK(psp_media_slot_free_count(
              slots.slots, PSP_MEDIA_SURFACE_SLOTS)
          == PSP_MEDIA_SURFACE_SLOTS);

    PreparedStress prepared = {0};
    atomic_init(&prepared.state, PSP_MEDIA_CODEC_PREPARED_EMPTY);
    atomic_init(&prepared.failed, false);
    CHECK(pthread_create(
              &producer, NULL, prepared_producer, &prepared) == 0);
    CHECK(pthread_create(&consumer, NULL, prepared_worker, &prepared) == 0);
    CHECK(pthread_join(producer, NULL) == 0);
    CHECK(pthread_join(consumer, NULL) == 0);
    CHECK(!atomic_load_explicit(&prepared.failed, memory_order_relaxed));
    CHECK(atomic_load_explicit(&prepared.state, memory_order_acquire)
          == PSP_MEDIA_CODEC_PREPARED_EMPTY);

    /* Pin the cancellation and completion-slot-full branches as well. */
    CHECK(psp_media_prepared_try_publish(&prepared.state));
    CHECK(psp_media_prepared_worker_take_or_close(&prepared.state, false)
          == PSP_MEDIA_PREPARED_WORKER_UNAVAILABLE);
    CHECK(psp_media_prepared_try_cancel(&prepared.state));
    CHECK(psp_media_prepared_release_claimed(&prepared.state));

    puts("tilefinch-psp-media-ownership-tests: all checks passed");
    return 0;
}
