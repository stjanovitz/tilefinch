#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include "../src/fetch/background_slot_policy.h"

typedef enum {
    MODEL_FREE = 0,
    MODEL_QUEUED,
    MODEL_RUNNING,
    MODEL_COMPLETE
} ModelState;

typedef struct {
    ModelState state;
    uint32_t generation;
    bool cancel_requested;
} ModelSlot;

static int failures;
#define CHECK(value) do { if (!(value)) { \
    fprintf(stderr, "background-ownership failed at %s:%d: %s\n", \
            __FILE__, __LINE__, #value); failures++; \
} } while (0)

static uint64_t model_claim(
    ModelSlot slots[FETCH_BACKGROUND_REQUEST_LIMIT],
    bool foreground, bool priority_active)
{
    unsigned limit = fetch_background_admission_slot_limit(
        foreground, priority_active);
    for (unsigned at = 0; at < limit; at++) {
        if (slots[at].state != MODEL_FREE) continue;
        slots[at].generation = fetch_background_generation_next(
            slots[at].generation);
        slots[at].cancel_requested = false;
        slots[at].state = MODEL_QUEUED;
        return fetch_background_request_id_make(
            at, slots[at].generation);
    }
    return 0;
}

static bool model_matches(const ModelSlot *slot, uint64_t id)
{
    return slot != NULL && slot->generation
        == fetch_background_request_id_generation(id);
}

static void model_cancel(
    ModelSlot slots[FETCH_BACKGROUND_REQUEST_LIMIT], uint64_t id)
{
    unsigned at = fetch_background_request_id_slot(id);
    if (at >= FETCH_BACKGROUND_REQUEST_LIMIT
        || !model_matches(&slots[at], id)) return;
    slots[at].cancel_requested = true;
    if (slots[at].state == MODEL_QUEUED
        || slots[at].state == MODEL_COMPLETE) {
        slots[at].state = MODEL_FREE;
    }
}

static void model_complete(
    ModelSlot slots[FETCH_BACKGROUND_REQUEST_LIMIT], uint64_t id)
{
    unsigned at = fetch_background_request_id_slot(id);
    /* A completion from an older generation is discarded. It must never
       publish into a descriptor which has since been reused. */
    if (at >= FETCH_BACKGROUND_REQUEST_LIMIT
        || !model_matches(&slots[at], id)
        || slots[at].state != MODEL_RUNNING) return;
    slots[at].state = slots[at].cancel_requested
        ? MODEL_FREE : MODEL_COMPLETE;
}

static uint32_t random_next(uint32_t *state)
{
    *state = *state * UINT32_C(1664525) + UINT32_C(1013904223);
    return *state;
}

static void test_reserved_eventual_admission(void)
{
    ModelSlot slots[FETCH_BACKGROUND_REQUEST_LIMIT] = {{0}};
    uint64_t normal[FETCH_BACKGROUND_REQUEST_LIMIT] = {0};
    unsigned normal_limit = FETCH_BACKGROUND_REQUEST_LIMIT
        - FETCH_BACKGROUND_MEDIA_RESERVED_SLOTS;
    for (unsigned at = 0; at < normal_limit; at++) {
        normal[at] = model_claim(slots, false, true);
        CHECK(normal[at] != 0);
    }
    CHECK(model_claim(slots, false, true) == 0);
    uint64_t foreground_a = model_claim(slots, true, true);
    uint64_t foreground_b = model_claim(slots, true, true);
    CHECK(foreground_a != 0 && foreground_b != 0);
    CHECK(fetch_background_request_id_slot(foreground_a) >= normal_limit);
    CHECK(fetch_background_request_id_slot(foreground_b) >= normal_limit);
    for (unsigned at = 0; at < normal_limit; at++) model_cancel(slots, normal[at]);
    model_cancel(slots, foreground_a);
    model_cancel(slots, foreground_b);
    CHECK(model_claim(slots, true, true) != 0);
}

static void test_generation_wrap_and_late_completion(void)
{
    ModelSlot slots[FETCH_BACKGROUND_REQUEST_LIMIT] = {{0}};
    slots[0].generation = UINT32_MAX;
    uint64_t first = model_claim(slots, true, true);
    CHECK(fetch_background_request_id_generation(first) == 1u);
    slots[0].state = MODEL_RUNNING;
    model_cancel(slots, first);
    model_complete(slots, first);
    CHECK(slots[0].state == MODEL_FREE);
    uint64_t replacement = model_claim(slots, true, true);
    CHECK(replacement != 0 && replacement != first);
    slots[0].state = MODEL_RUNNING;
    model_complete(slots, first);
    CHECK(slots[0].state == MODEL_RUNNING
          && slots[0].generation
                 == fetch_background_request_id_generation(replacement));
}

static void test_randomized_interleavings(void)
{
    ModelSlot slots[FETCH_BACKGROUND_REQUEST_LIMIT] = {{0}};
    uint64_t history[128] = {0};
    size_t history_at = 0;
    uint32_t random = UINT32_C(0x51a7c0de);
    for (unsigned step = 0; step < 200000u; step++) {
        uint32_t choice = random_next(&random);
        unsigned slot = (choice >> 8) % FETCH_BACKGROUND_REQUEST_LIMIT;
        switch (choice % 7u) {
        case 0:
        case 1: {
            bool foreground = (choice & 0x10000u) != 0;
            bool priority = (choice & 0x20000u) != 0;
            uint64_t id = model_claim(slots, foreground, priority);
            if (id != 0) {
                history[history_at++ % 128u] = id;
                if (priority && !foreground) {
                    CHECK(fetch_background_request_id_slot(id)
                          < FETCH_BACKGROUND_REQUEST_LIMIT
                                - FETCH_BACKGROUND_MEDIA_RESERVED_SLOTS);
                }
            }
            break;
        }
        case 2:
            if (slots[slot].state == MODEL_QUEUED)
                slots[slot].state = MODEL_RUNNING;
            break;
        case 3:
            if (slots[slot].state == MODEL_RUNNING) {
                model_complete(slots, fetch_background_request_id_make(
                    slot, slots[slot].generation));
            }
            break;
        case 4:
            model_cancel(slots, history[(choice >> 16) % 128u]);
            break;
        case 5:
            /* Deliberately deliver a potentially stale completion. */
            model_complete(slots, history[(choice >> 16) % 128u]);
            break;
        default:
            if (slots[slot].state == MODEL_COMPLETE)
                slots[slot].state = MODEL_FREE;
            break;
        }
        for (unsigned at = 0; at < FETCH_BACKGROUND_REQUEST_LIMIT; at++) {
            CHECK(slots[at].generation != 0 || slots[at].state == MODEL_FREE);
            for (unsigned other = at + 1u;
                 other < FETCH_BACKGROUND_REQUEST_LIMIT; other++) {
                if (slots[at].state == MODEL_FREE
                    || slots[other].state == MODEL_FREE) continue;
                CHECK(fetch_background_request_id_make(
                          at, slots[at].generation)
                      != fetch_background_request_id_make(
                          other, slots[other].generation));
            }
        }
    }

    /* Cancellation followed by bounded worker acknowledgement always returns
       every descriptor to the free pool and restores foreground admission. */
    for (unsigned at = 0; at < FETCH_BACKGROUND_REQUEST_LIMIT; at++) {
        uint64_t id = fetch_background_request_id_make(
            at, slots[at].generation);
        model_cancel(slots, id);
        if (slots[at].state == MODEL_RUNNING) model_complete(slots, id);
        if (slots[at].state == MODEL_COMPLETE) slots[at].state = MODEL_FREE;
        CHECK(slots[at].state == MODEL_FREE);
    }
    CHECK(model_claim(slots, true, true) != 0);
}

int main(void)
{
    test_reserved_eventual_admission();
    test_generation_wrap_and_late_completion();
    test_randomized_interleavings();
    if (failures != 0) return 1;
    puts("fetch background ownership tests passed");
    return 0;
}
