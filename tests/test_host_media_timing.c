#include <stdio.h>
#include <string.h>

#include "../src/host_media_timing.h"

static int failures;

#define CHECK(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "FAIL %s:%d: %s\n", \
                __FILE__, __LINE__, #condition); \
        failures++; \
    } \
} while (0)

static uint64_t frame_time_us(uint64_t frame,
                              uint64_t numerator,
                              uint64_t denominator)
{
    return frame * denominator * UINT64_C(1000000) / numerator;
}

static void check_occasional_one_hour_timeline(
    uint64_t numerator, uint64_t denominator)
{
    static const unsigned tick_us[] = {
        7000, 13000, 29000, 41000, 17000, 53000, 11000
    };
    const uint64_t duration_us = UINT64_C(60) * 60u * 1000000u;
    const uint64_t maximum_frame_us =
        (denominator * UINT64_C(1000000) + numerator - 1u) / numerator;
    const uint64_t bound_us = (maximum_frame_us + 1u) / 2u + 1u;
    uint64_t presented_us = 0;
    uint64_t next_frame = 1;
    uint64_t cursor_us = 0;
    size_t tick = 0;

    while (cursor_us < duration_us) {
        cursor_us += tick_us[tick++ % (
            sizeof(tick_us) / sizeof(tick_us[0]))];
        while (true) {
            uint64_t candidate_us = frame_time_us(
                next_frame, numerator, denominator);
            if (!host_media_timing_should_present(
                    presented_us, cursor_us, candidate_us)) break;
            presented_us = candidate_us;
            next_frame++;
        }
        uint64_t absolute_skew = presented_us >= cursor_us
            ? presented_us - cursor_us : cursor_us - presented_us;
        CHECK(absolute_skew <= bound_us);
    }
}

static void check_bounded_audio_prefetch(void)
{
    const uint64_t total_samples = UINT64_C(480000) + 511u;
    const uint64_t decoded_frame_samples = 1024u;
    const uint64_t read_ahead_samples = 9600u;
    uint64_t presented = 0;
    uint64_t queued = 0;

    /*
     * A 16 ms output cadence and whole AAC-sized decode units must remain
     * bounded without dropping the unit that crosses the target.
     */
    while (presented < total_samples) {
        uint64_t target = host_media_audio_prefetch_target_samples(
            presented, 48000, 200);
        while (host_media_audio_prefetch_needed(queued, target)
               && queued < total_samples) {
            uint64_t retained = total_samples - queued;
            if (retained > decoded_frame_samples)
                retained = decoded_frame_samples;
            queued += retained;
        }
        CHECK(queued >= presented);
        CHECK(queued - presented
              <= read_ahead_samples + decoded_frame_samples - 1u);
        uint64_t consumed = total_samples - presented;
        if (consumed > 768u) consumed = 768u;
        presented += consumed;
        if (presented > queued) presented = queued;
    }
    CHECK(presented == total_samples);
    CHECK(queued == total_samples);
}

int main(int argc, char **argv)
{
    if (argc == 2
        && strcmp(argv[1], "--one-hour-simulation") == 0) {
        puts("occasional test: one-hour virtual nearest-frame timelines");
        check_occasional_one_hour_timeline(24, 1);
        check_occasional_one_hour_timeline(24000, 1001);
        check_occasional_one_hour_timeline(25, 1);
        check_occasional_one_hour_timeline(30, 1);
        check_occasional_one_hour_timeline(30000, 1001);
        check_occasional_one_hour_timeline(60, 1);
        if (failures != 0) {
            fprintf(
                stderr, "occasional-media-timing: %d failure(s)\n",
                failures);
            return 1;
        }
        puts("occasional-media-timing: all checks passed");
        return 0;
    }
    if (argc != 1) {
        fprintf(
            stderr,
            "usage: %s [--one-hour-simulation]\n",
            argc > 0 ? argv[0] : "tilefinch-host-media-timing-tests");
        return 2;
    }

    puts("test: native audio prefetch follows presented output");
    CHECK(host_media_audio_prefetch_target_samples(
              0, 48000, 200) == 9600);
    CHECK(host_media_audio_prefetch_target_samples(
              9600, 48000, 200) == 19200);
    CHECK(host_media_audio_prefetch_target_samples(
              UINT64_MAX - 5u, 48000, 200) == UINT64_MAX);
    CHECK(host_media_audio_prefetch_needed(12000, 12001));
    CHECK(!host_media_audio_prefetch_needed(12000, 12000));
    CHECK(host_media_audio_queue_admits(400, 100, 500));
    CHECK(!host_media_audio_queue_admits(401, 100, 500));
    CHECK(!host_media_audio_queue_admits(UINT64_MAX, 0, 500));
    CHECK(host_media_audio_clock_is_master(
              true, false, false, false));
    CHECK(host_media_audio_clock_is_master(
              true, true, true, true));
    CHECK(!host_media_audio_clock_is_master(
              true, true, true, false));
    CHECK(!host_media_audio_clock_is_master(
              true, false, true, false));
    CHECK(!host_media_audio_clock_is_master(
              false, false, false, true));

    puts("test: stale catch-up frames are dropped before conversion");
    CHECK(host_media_timing_frame_is_stale(
              UINT64_C(1000000), UINT64_C(1083334), UINT64_C(41667)));
    CHECK(!host_media_timing_frame_is_stale(
              UINT64_C(1041667), UINT64_C(1083333), UINT64_C(41667)));
    CHECK(!host_media_timing_frame_is_stale(
              UINT64_C(1083334), UINT64_C(1083334), 0));
    check_bounded_audio_prefetch();

    puts("test: nearest-frame presentation removes systematic video lag");
    CHECK(host_media_timing_should_present(
        UINT64_C(1000000), UINT64_C(1030000), UINT64_C(1041667)));
    CHECK(!host_media_timing_should_present(
        UINT64_C(1000000), UINT64_C(1010000), UINT64_C(1041667)));
    CHECK(host_media_timing_should_present(
        UINT64_C(1041667), UINT64_C(1083334), UINT64_C(1083334)));
    CHECK(!host_media_timing_should_present(
        UINT64_C(1041667), UINT64_C(1040000), UINT64_C(1083334)));
    CHECK(host_media_timing_should_present_seek(
        false, UINT64_C(5000000), 0, UINT64_C(41667)));
    CHECK(host_media_timing_should_present_seek(
        true, UINT64_C(1000000), UINT64_C(1030000), UINT64_C(1041667)));
    CHECK(!host_media_timing_should_present_seek(
        true, UINT64_C(1000000), UINT64_C(1010000), UINT64_C(1041667)));

    puts("test: startup waits for the native audio presentation clock");
    HostMediaTiming timing = host_media_timing_snapshot(
        UINT64_C(650000), 0, 0, true, false);
    CHECK(timing.presentation_limit_us == 0);
    CHECK(timing.current_time_us == 0);

    puts("test: presented PTS, not decode head, defines startup skew");
    timing = host_media_timing_snapshot(
        UINT64_C(1214000), UINT64_C(458792), UINT64_C(487270),
        true, true);
    CHECK(timing.presentation_limit_us == UINT64_C(487270));
    CHECK(timing.current_time_us == UINT64_C(487270));
    CHECK(timing.audio_video_skew_us == INT64_C(28478));

    puts("test: post-seek alignment retains the absolute media timeline");
    timing = host_media_timing_snapshot(
        UINT64_C(18470000), UINT64_C(18351667), UINT64_C(18406902),
        true, true);
    CHECK(timing.current_time_us == UINT64_C(18406902));
    CHECK(timing.audio_video_skew_us == INT64_C(55235));

    puts("test: decode-only playback keeps its deterministic caller clock");
    timing = host_media_timing_snapshot(
        UINT64_C(2000000), UINT64_C(1950000), UINT64_C(1980000),
        false, true);
    CHECK(timing.presentation_limit_us == UINT64_C(2000000));
    CHECK(timing.current_time_us == UINT64_C(2000000));
    CHECK(timing.audio_video_skew_us == INT64_C(30000));

    if (failures != 0) {
        fprintf(stderr, "host-media-timing-tests: %d failure(s)\n",
                failures);
        return 1;
    }
    puts("host-media-timing-tests: all checks passed");
    return 0;
}
