/*
 * Host-side promotion checks for the PSP media path.
 *
 * Firmware timing cannot be reproduced here.  The pieces surrounding it can:
 * exact authored frame cadence, a presentation clock which freezes during a
 * refill or pause, one-shot seek adoption, and the shared Budget boundary
 * between the 360p and 240p codec working sets.  Keep those checks together so
 * a proposed default-quality change has one deterministic local gate.
 */

#include "tilefinch/budget.h"

#include "../src/host_media_timing.h"
#include "../src/media_backend_psp_policy.h"

#include <inttypes.h>
#include <stdio.h>

#define MIB ((size_t) 1024u * 1024u)

static int failures;

#define CHECK(condition) do {                                                \
    if (!(condition)) {                                                      \
        fprintf(stderr, "FAIL %s:%d: %s\n",                               \
                __FILE__, __LINE__, #condition);                             \
        failures++;                                                          \
    }                                                                        \
} while (0)

static uint64_t frame_time_us(
    uint64_t frame, uint64_t numerator, uint64_t denominator)
{
    return frame * denominator * UINT64_C(1000000) / numerator;
}

static uint64_t magnitude_i64(int64_t value)
{
    if (value >= 0) return (uint64_t) value;
    return value == INT64_MIN
        ? (uint64_t) INT64_MAX + 1u : (uint64_t) -value;
}

/*
 * Exercise the production nearest-frame and audible-clock helpers against an
 * irregular PSP-shaped polling cadence.  Three refills and one user pause
 * freeze the presentation clock.  The selected picture must remain frozen as
 * well: resuming may continue from the next authored picture, never race to
 * catch wall time which the user did not hear.
 */
static void check_rate(uint64_t numerator, uint64_t denominator)
{
    static const uint32_t ticks_us[] = {
        29000u, 31000u, 33000u, 27000u, 35000u, 30000u, 32000u
    };
    const uint64_t frame_ceil_us =
        (denominator * UINT64_C(1000000) + numerator - 1u) / numerator;
    const uint64_t skew_bound_us = (frame_ceil_us + 1u) / 2u + 1u;
    uint64_t wall_us = 0;
    uint64_t audio_us = 0;
    uint64_t presented_us = 0;
    uint64_t next_frame = 1u;
    uint64_t maximum_skew_us = 0;
    uint64_t held_audio_us = 0;
    uint64_t held_video_us = 0;
    bool was_held = false;

    for (size_t tick = 0; wall_us < UINT64_C(180000000); tick++) {
        uint32_t delta_us = ticks_us[
            tick % (sizeof(ticks_us) / sizeof(ticks_us[0]))];
        wall_us += delta_us;
        bool held = (wall_us >= UINT64_C(10000000)
                     && wall_us < UINT64_C(10800000))
            || (wall_us >= UINT64_C(50000000)
                && wall_us < UINT64_C(52500000))
            || (wall_us >= UINT64_C(90000000)
                && wall_us < UINT64_C(90750000))
            || (wall_us >= UINT64_C(130000000)
                && wall_us < UINT64_C(130400000));
        if (held && !was_held) {
            held_audio_us = audio_us;
            held_video_us = presented_us;
        }
        uint64_t accepted_us = audio_us > UINT64_MAX - UINT64_C(500000)
            ? UINT64_MAX : audio_us + UINT64_C(500000);
        audio_us = psp_media_audio_cursor_advance_us(
            audio_us, delta_us, !held, accepted_us);
        while (true) {
            uint64_t candidate_us = frame_time_us(
                next_frame, numerator, denominator);
            if (!host_media_timing_should_present(
                    presented_us, audio_us, candidate_us)) break;
            presented_us = candidate_us;
            next_frame++;
        }
        if (held) {
            CHECK(audio_us == held_audio_us);
            CHECK(presented_us == held_video_us);
        }
        uint64_t skew_us = magnitude_i64(host_media_timing_delta(
            audio_us, presented_us));
        CHECK(skew_us <= skew_bound_us);
        if (skew_us > maximum_skew_us) maximum_skew_us = skew_us;
        was_held = held;
    }
    CHECK(audio_us >= UINT64_C(175000000));
    CHECK(maximum_skew_us <= skew_bound_us);
    printf("promotion-cadence rate=%" PRIu64 "/%" PRIu64
           " audio=%" PRIu64 "ms video=%" PRIu64
           "ms max-skew=%" PRIu64 "us\n",
           numerator, denominator, audio_us / 1000u,
           presented_us / 1000u, maximum_skew_us);
}

static void check_seek_alignment(uint64_t numerator, uint64_t denominator)
{
    static const uint64_t targets_us[] = {
        UINT64_C(1), UINT64_C(1234567), UINT64_C(75123456),
        UINT64_C(179999999)
    };
    uint64_t frame_ceil_us =
        (denominator * UINT64_C(1000000) + numerator - 1u) / numerator;
    for (size_t at = 0;
         at < sizeof(targets_us) / sizeof(targets_us[0]); at++) {
        uint64_t target_us = targets_us[at];
        uint64_t frame =
            (target_us * numerator
             + denominator * UINT64_C(1000000) - 1u)
            / (denominator * UINT64_C(1000000));
        uint64_t candidate_us = frame_time_us(
            frame, numerator, denominator);
        CHECK(candidate_us >= target_us);
        CHECK(candidate_us - target_us <= frame_ceil_us);
        CHECK(host_media_timing_should_present_seek(
            false, 0, target_us, candidate_us));
        /* Once the first legal post-seek picture is retained, the ordinary
           nearest-frame rule resumes and cannot admit another whole frame. */
        CHECK(!host_media_timing_should_present_seek(
            true, candidate_us, target_us,
            frame_time_us(frame + 1u, numerator, denominator)));
    }
}

static void check_memory_pressure_fallback(void)
{
    PspMediaSurfacePolicy low = {0};
    PspMediaSurfacePolicy high = {0};
    CHECK(psp_media_surface_policy(426u, 240u, &low));
    CHECK(psp_media_surface_policy(638u, 360u, &high));
    CHECK(high.external_reserve_bytes > low.external_reserve_bytes);

    unsigned fallback_band_samples = 0;
    const size_t limit = 32u * MIB;
    for (size_t occupied = 22u * MIB;
         occupied <= 29u * MIB; occupied += 64u * 1024u) {
        Budget budget;
        BudgetReservation page = {0};
        BudgetReservation codec = {0};
        budget_init(&budget, limit);
        CHECK(budget_reservation_acquire(
            &page, &budget, BUDGET_CATEGORY_DOM, occupied));
        bool high_admitted = budget_reservation_acquire(
            &codec, &budget, BUDGET_CATEGORY_RESOURCE,
            high.external_reserve_bytes);
        budget_reservation_release(&codec);
        bool low_admitted = budget_reservation_acquire(
            &codec, &budget, BUDGET_CATEGORY_RESOURCE,
            low.external_reserve_bytes);
        budget_reservation_release(&codec);
        /* A larger working set can never be admitted after the smaller one
           was refused.  The interval in between is the useful 240p fallback
           band rather than a terminal page failure. */
        CHECK(!high_admitted || low_admitted);
        if (!high_admitted && low_admitted) fallback_band_samples++;
        budget_reservation_release(&page);
        CHECK(budget.current == 0);
        CHECK(budget.external_reserved == 0);
        CHECK(budget_categories_reconcile(&budget));
    }
    CHECK(fallback_band_samples >= 12u);

    /* Deterministic allocator failure during the large reservation must not
       leave a charge which prevents the lower-quality retry. */
    Budget budget;
    BudgetReservation codec = {0};
    budget_init(&budget, 16u * MIB);
    budget_inject_failure_after(&budget, 0);
    CHECK(!budget_reservation_acquire(
        &codec, &budget, BUDGET_CATEGORY_RESOURCE,
        high.external_reserve_bytes));
    budget_clear_failure_injection(&budget);
    CHECK(budget_reservation_acquire(
        &codec, &budget, BUDGET_CATEGORY_RESOURCE,
        low.external_reserve_bytes));
    budget_reservation_release(&codec);
    CHECK(budget.current == 0 && budget_categories_reconcile(&budget));
}

int main(void)
{
    puts("test: mobile video cadences remain bounded across holds");
    check_rate(24000u, 1001u);
    check_rate(24u, 1u);
    check_rate(25u, 1u);
    check_rate(30000u, 1001u);
    check_rate(30u, 1u);

    puts("test: seek adoption returns to nearest-frame presentation");
    check_seek_alignment(24000u, 1001u);
    check_seek_alignment(24u, 1u);
    check_seek_alignment(25u, 1u);
    check_seek_alignment(30000u, 1001u);
    check_seek_alignment(30u, 1u);

    puts("test: memory pressure has a clean 360p-to-240p band");
    check_memory_pressure_fallback();

    if (failures != 0) {
        fprintf(stderr, "media-promotion-tests: %d failure(s)\n", failures);
        return 1;
    }
    puts("media-promotion-tests: all checks passed");
    return 0;
}
