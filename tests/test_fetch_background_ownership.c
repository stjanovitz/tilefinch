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
    uint32_t route_generation;
    uint8_t owner;
} ModelSlot;

typedef enum {
    MODEL_OWNER_NONE = 0,
    MODEL_OWNER_DOCUMENT,
    MODEL_OWNER_THUMBNAIL,
    MODEL_OWNER_MEDIA_RANGE
} ModelOwner;

static int failures;
#define CHECK(value) do { if (!(value)) { \
    fprintf(stderr, "background-ownership failed at %s:%d: %s\n", \
            __FILE__, __LINE__, #value); failures++; \
} } while (0)

static void test_redirect_cookie_overflow_policy(void)
{
    CHECK(fetch_background_redirect_cookie_overflow_fatal(
        false, 302, true));
    CHECK(!fetch_background_redirect_cookie_overflow_fatal(
        true, 302, true));
    CHECK(!fetch_background_redirect_cookie_overflow_fatal(
        false, 302, false));
    CHECK(!fetch_background_redirect_cookie_overflow_fatal(
        false, 304, true));
    CHECK(!fetch_background_redirect_cookie_overflow_fatal(
        false, 200, true));

    CHECK(fetch_background_cookie_capture_policy(20u, 0u, 0u)
          == FETCH_BACKGROUND_COOKIE_CAPTURED);
    CHECK(fetch_background_cookie_capture_policy(
              FETCH_SET_COOKIE_LIMIT, 0u, 0u)
          == FETCH_BACKGROUND_COOKIE_REJECTED);
    CHECK(fetch_background_cookie_capture_policy(
              20u, FETCH_RESPONSE_COOKIE_CAPACITY, 0u)
          == FETCH_BACKGROUND_COOKIE_TRUNCATED);
    CHECK(fetch_background_cookie_capture_policy(
              4095u, 4u,
              FETCH_BACKGROUND_RESPONSE_COOKIE_BYTES - 4095u)
          == FETCH_BACKGROUND_COOKIE_TRUNCATED);
}

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

static uint64_t model_claim_owned(
    ModelSlot slots[FETCH_BACKGROUND_REQUEST_LIMIT], bool foreground,
    bool priority_active, ModelOwner owner, uint32_t route_generation)
{
    uint64_t id = model_claim(slots, foreground, priority_active);
    if (id == 0) return 0;
    unsigned at = fetch_background_request_id_slot(id);
    slots[at].owner = (uint8_t) owner;
    slots[at].route_generation = route_generation;
    return id;
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

static void model_cancel_all(
    ModelSlot slots[FETCH_BACKGROUND_REQUEST_LIMIT])
{
    for (unsigned at = 0; at < FETCH_BACKGROUND_REQUEST_LIMIT; at++) {
        if (slots[at].state == MODEL_FREE) continue;
        model_cancel(slots, fetch_background_request_id_make(
            at, slots[at].generation));
    }
}

static uint64_t model_claim_eventually(
    ModelSlot slots[FETCH_BACKGROUND_REQUEST_LIMIT], bool foreground,
    bool priority_active, ModelOwner owner, uint32_t route_generation)
{
    for (unsigned acknowledgement = 0;
         acknowledgement <= FETCH_BACKGROUND_REQUEST_LIMIT;
         acknowledgement++) {
        uint64_t id = model_claim_owned(
            slots, foreground, priority_active, owner, route_generation);
        if (id != 0) return id;
        for (unsigned at = 0; at < FETCH_BACKGROUND_REQUEST_LIMIT; at++) {
            if (slots[at].state != MODEL_RUNNING
                || !slots[at].cancel_requested) continue;
            model_complete(slots, fetch_background_request_id_make(
                at, slots[at].generation));
            break;
        }
    }
    return 0;
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

static void test_priority_release_restores_page_capacity(void)
{
    ModelSlot slots[FETCH_BACKGROUND_REQUEST_LIMIT] = {{0}};
    unsigned reduced_limit = FETCH_BACKGROUND_REQUEST_LIMIT
        - FETCH_BACKGROUND_MEDIA_RESERVED_SLOTS;

    /* Repeated player open/close cycles must not progressively reduce the
       page lane. `priority_active=false` models the session releasing its
       media reservation as soon as the player becomes hidden. */
    for (unsigned cycle = 0; cycle < 4096u; cycle++) {
        uint64_t requests[FETCH_BACKGROUND_REQUEST_LIMIT] = {0};
        for (unsigned at = 0; at < reduced_limit; at++) {
            requests[at] = model_claim(slots, false, true);
            CHECK(requests[at] != 0);
        }
        CHECK(model_claim(slots, false, true) == 0);

        for (unsigned at = 0; at < reduced_limit; at++)
            model_cancel(slots, requests[at]);

        for (unsigned at = 0; at < FETCH_BACKGROUND_REQUEST_LIMIT; at++) {
            requests[at] = model_claim(slots, false, false);
            CHECK(requests[at] != 0);
        }
        CHECK(model_claim(slots, false, false) == 0);
        for (unsigned at = 0; at < FETCH_BACKGROUND_REQUEST_LIMIT; at++)
            model_cancel(slots, requests[at]);
    }
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

static void test_randomized_route_churn(void)
{
    enum { ROUTE_HISTORY = 256, ROUTE_STEPS = 100000 };
    ModelSlot slots[FETCH_BACKGROUND_REQUEST_LIMIT] = {{0}};
    uint64_t history[ROUTE_HISTORY] = {0};
    size_t history_at = 0;
    uint32_t random = UINT32_C(0x7a11f17e);
    uint32_t route_generation = 1;
    bool media_priority = false;

    for (unsigned step = 0; step < ROUTE_STEPS; step++) {
        uint32_t choice = random_next(&random);
        unsigned action = choice % 10u;
        if (action <= 3u) {
            /* Search/results, video, Back, and Home all establish a new
               authoritative route. The incumbent's optional thumbnails and
               the player's ranges are cancelled together; running work may
               acknowledge late, but its generation can no longer publish. */
            model_cancel_all(slots);
            route_generation = fetch_background_generation_next(
                route_generation);
            media_priority = action == 1u;

            /* The single worker can retire at most one cancelled running
               descriptor at a time. Bounded acknowledgements must always
               restore foreground admission; queued descriptors were already
               released synchronously by cancel_all. */
            uint64_t document = model_claim_eventually(
                slots, true, media_priority, MODEL_OWNER_DOCUMENT,
                route_generation);
            CHECK(document != 0);
            if (document != 0) {
                history[history_at++ % ROUTE_HISTORY] = document;
            }

            if (media_priority) {
                /* The two reserved descriptors keep video ranges admissible
                   even when result thumbnails consumed the normal lane just
                   before the route change. */
                for (unsigned range = 0;
                     range < FETCH_BACKGROUND_MEDIA_RESERVED_SLOTS; range++) {
                    uint64_t id = model_claim_eventually(
                        slots, true, true, MODEL_OWNER_MEDIA_RANGE,
                        route_generation);
                    CHECK(id != 0);
                    if (id != 0) history[history_at++ % ROUTE_HISTORY] = id;
                }
            } else if (action == 0u) {
                unsigned normal_limit = FETCH_BACKGROUND_REQUEST_LIMIT
                    - FETCH_BACKGROUND_MEDIA_RESERVED_SLOTS;
                for (unsigned thumbnail = 0;
                     thumbnail < normal_limit; thumbnail++) {
                    uint64_t id = model_claim_owned(
                        slots, false, false, MODEL_OWNER_THUMBNAIL,
                        route_generation);
                    if (id == 0) break;
                    history[history_at++ % ROUTE_HISTORY] = id;
                }
            }
        } else if (action == 4u) {
            unsigned at = (choice >> 8) % FETCH_BACKGROUND_REQUEST_LIMIT;
            if (slots[at].state == MODEL_QUEUED) {
                slots[at].state = MODEL_RUNNING;
            }
        } else if (action == 5u) {
            unsigned at = (choice >> 8) % FETCH_BACKGROUND_REQUEST_LIMIT;
            if (slots[at].state == MODEL_RUNNING) {
                model_complete(slots, fetch_background_request_id_make(
                    at, slots[at].generation));
            }
        } else if (action == 6u) {
            model_complete(slots,
                history[(choice >> 16) % ROUTE_HISTORY]);
        } else if (action == 7u) {
            model_cancel(slots,
                history[(choice >> 16) % ROUTE_HISTORY]);
        } else {
            unsigned at = (choice >> 8) % FETCH_BACKGROUND_REQUEST_LIMIT;
            if (slots[at].state == MODEL_COMPLETE) {
                slots[at].state = MODEL_FREE;
            }
        }

        for (unsigned at = 0; at < FETCH_BACKGROUND_REQUEST_LIMIT; at++) {
            ModelSlot *slot = &slots[at];
            if (slot->state == MODEL_FREE) continue;
            CHECK(slot->generation != 0);
            if (slot->route_generation != route_generation) {
                /* Old-route work may exist only while a running request
                   acknowledges cancellation. It can never become a visible
                   completion belonging to the new page/player. */
                CHECK(slot->state == MODEL_RUNNING);
                CHECK(slot->cancel_requested);
            }
            if (slot->owner == MODEL_OWNER_MEDIA_RANGE) {
                CHECK(media_priority
                      || slot->cancel_requested
                      || slot->route_generation != route_generation);
            }
        }
    }

    model_cancel_all(slots);
    for (unsigned at = 0; at < FETCH_BACKGROUND_REQUEST_LIMIT; at++) {
        if (slots[at].state == MODEL_RUNNING) {
            model_complete(slots, fetch_background_request_id_make(
                at, slots[at].generation));
        }
        if (slots[at].state == MODEL_COMPLETE) slots[at].state = MODEL_FREE;
        CHECK(slots[at].state == MODEL_FREE);
    }
    CHECK(model_claim(slots, true, false) != 0);
}

int main(void)
{
    test_redirect_cookie_overflow_policy();
    test_reserved_eventual_admission();
    test_priority_release_restores_page_capacity();
    test_generation_wrap_and_late_completion();
    test_randomized_interleavings();
    test_randomized_route_churn();
    if (failures != 0) return 1;
    puts("fetch background ownership tests passed");
    return 0;
}
