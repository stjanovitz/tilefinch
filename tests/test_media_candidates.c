#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "tilefinch/media_candidates.h"
#include "tilefinch/media_mp4.h"
#include "../src/media_backend_psp_policy.h"

#define CHECK(condition) do {                                                \
    if (!(condition)) {                                                      \
        fprintf(stderr, "CHECK failed at %s:%d: %s\n", __FILE__, __LINE__, \
                #condition);                                                 \
        return 1;                                                            \
    }                                                                        \
} while (0)

static bool append(char *packed, size_t *length, MediaDiscoveryKind kind,
                   const char *url, const char *first)
{
    size_t url_length = strlen(url);
    if (*length + 2u + url_length > MEDIA_CANDIDATE_PACKED_CAPACITY)
        return false;
    packed[*length] = (char) ('0' + kind);
    memcpy(packed + *length + 1u, url, url_length + 1u);
    return media_candidates_commit(
        packed, MEDIA_CANDIDATE_PACKED_CAPACITY, length, first);
}

static int test_quality_labels(void)
{
    static const struct { const char *value; unsigned quality; } cases[] = {
        {"240", 240u}, {"720p", 720u}, {"HD 1080p", 1080u},
        {"1280x720", 720u}, {"426X240", 240u}, {"144", 144u},
        {"hd", 0u}, {"4k", 0u}, {"96", 0u}, {"", 0u}, {"99999", 0u},
        {"360p60", 360u}
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        CHECK(media_candidate_quality_label(
                  cases[i].value, strlen(cases[i].value))
              == cases[i].quality);
    }
    return 0;
}

static int test_packing_bounds_and_repeats(void)
{
    char packed[MEDIA_CANDIDATE_PACKED_CAPACITY];
    size_t length = 0;
    const char *first = "https://media.test/v-240.mp4";
    CHECK(!append(packed, &length, MEDIA_DISCOVERY_MP4, first, first));
    CHECK(append(packed, &length, MEDIA_DISCOVERY_MP4,
                 "https://media.test/v-480.mp4", first));
    CHECK(!append(packed, &length, MEDIA_DISCOVERY_MP4,
                  "https://media.test/v-480.mp4", first));
    CHECK(!append(packed, &length, MEDIA_DISCOVERY_WEBM,
                  "https://media.test/v.webm", first));
    CHECK(!append(packed, &length, MEDIA_DISCOVERY_AUDIO_MP4,
                  "https://media.test/a.m4a", first));
    CHECK(append(packed, &length, MEDIA_DISCOVERY_HLS,
                 "https://media.test/master.m3u8", first));
    CHECK(append(packed, &length, MEDIA_DISCOVERY_NONE,
                 "https://media.test/stream?id=1", first));
    CHECK(media_candidates_count(packed, length)
          == MEDIA_CANDIDATE_ALTERNATE_LIMIT);
    CHECK(!append(packed, &length, MEDIA_DISCOVERY_MP4,
                  "https://media.test/v-720.mp4", first));
    size_t cursor = 0;
    MediaDiscoveryKind kind = MEDIA_DISCOVERY_NONE;
    const char *url = media_candidates_next(packed, length, &cursor, &kind);
    CHECK(url != NULL && strcmp(url, "https://media.test/v-480.mp4") == 0
          && kind == MEDIA_DISCOVERY_MP4);
    url = media_candidates_next(packed, length, &cursor, &kind);
    CHECK(url != NULL && kind == MEDIA_DISCOVERY_HLS);
    url = media_candidates_next(packed, length, &cursor, &kind);
    CHECK(url != NULL && kind == MEDIA_DISCOVERY_NONE);
    CHECK(media_candidates_next(packed, length, &cursor, &kind) == NULL);

    /* The byte bound holds as well as the entry bound. */
    char long_url[1400];
    memset(long_url, 'a', sizeof(long_url) - 1u);
    memcpy(long_url, "https://media.test/", 19u);
    long_url[sizeof(long_url) - 1u] = '\0';
    length = 0;
    CHECK(append(packed, &length, MEDIA_DISCOVERY_MP4, long_url, first));
    long_url[30] = 'b';
    CHECK(!append(packed, &length, MEDIA_DISCOVERY_MP4, long_url, first));
    CHECK(media_candidates_count(packed, length) == 1u);
    return 0;
}

static int test_packing_invalid_offsets(void)
{
    /* A wrapped offset must be refused before pointer arithmetic or reads.
       The guard bytes make the old additive check fail deterministically. */
    char storage[] = {'1', 'x', '\0', '\0'};
    char *packed = storage + 1;
    size_t cursor = SIZE_MAX;
    CHECK(media_candidates_next(packed, 2u, &cursor, NULL) == NULL);
    CHECK(cursor == SIZE_MAX);
    size_t length = SIZE_MAX;
    CHECK(!media_candidates_commit(packed, 2u, &length, NULL));
    CHECK(length == SIZE_MAX);

    char entry[] = {'1', 'x', '\0'};
    for (size_t available = 0; available < sizeof(entry); available++) {
        cursor = 0;
        CHECK(media_candidates_next(entry, available, &cursor, NULL) == NULL);
        CHECK(cursor == 0);
        length = 0;
        CHECK(!media_candidates_commit(entry, available, &length, NULL));
        CHECK(length == 0);
    }
    cursor = 0;
    CHECK(media_candidates_next(entry, sizeof(entry), &cursor, NULL)
          == entry + 1);
    CHECK(cursor == sizeof(entry));
    length = 0;
    CHECK(media_candidates_commit(entry, sizeof(entry), &length, NULL));
    CHECK(length == sizeof(entry));
    return 0;
}

/* The messages the native open path produces, and whether each lets the
   page's next rendition be tried. */
static int test_refusal_classification(void)
{
    static const char *const refused[] = {
        "PSP AVC geometry unsupported",
        "PSP AVC SPS geometry unsupported",
        /* The firmware decoder's H.264 coding refusals: from the SPS before
           open, alone or ahead of a missing software decoder's message, and
           from the first or opening pictures of a stream it accepted. */
        "unsupported H.264 stream (B-frame pyramid)",
        "unsupported H.264 stream (more than 3 reference frames)",
        "unsupported H.264 stream (B-frame pyramid); "
            "OPTIONAL VIDEO DECODER IS NOT INSTALLED",
        MEDIA_H264_FIRST_PICTURE_REFUSAL
            ": PSP AVC sceMpegAvcDecode failed: 0x80628002",
        MEDIA_H264_OPENING_REFUSAL,
        "software H.264 is limited to 432x240",
        "unsupported AVC profile",
        "HLS carries unsupported AVC syntax",
        "HLS master has no compatible AVC rendition",
        "PSP media has no supported track",
        /* A finite HLS playlist past its retained bounds (MD-10). */
        "HLS playlist is too long (over 2048 entries)",
        "HLS playlist is too long (VOD text over 128 KiB)",
        "page video range probe failed (HTTP 403): forbidden",
        "page video range probe failed (HTTP 404): "
            "server did not authorize byte ranges",
        "HLS response authority refused (HTTP 410)",
        /* A stream with nothing to play now: the next rendition may. */
        "HLS playlist is empty",
        "HLS playlist lacks EXTM3U",
        "page video: stream is offline or not available (HTTP 200)",
        /* Refused redirect hops: the page probe and range windows (the
           scheduler's messages) and the HLS worker path. */
        "page video range probe failed (HTTP 302): "
            "response redirect target is invalid",
        "range 0-65535 failed: HTTP 302, 0B, "
            "response redirect target is invalid",
        "page video range probe failed (HTTP 302): "
            "response exceeded the eight-hop redirect limit",
        "page video range probe failed (HTTP 302): "
            "response redirect loop detected",
        "HLS redirect refused (HTTP 302)",
        "HLS request refused by page policy",
        /* The firmware backend's own admission at open. */
        MEDIA_H264_FIRMWARE_LEVEL_REFUSAL ": 66/13 at 202x360",
        MEDIA_H264_FIRMWARE_LEVEL_REFUSAL,
        "PSP video 640x360 needs the wide decoder program",
        "PSP AVC surface policy rejected geometry",
        "PSP AVC requires exactly one SPS and PPS",
        "H.264 High profile format coming soon (100)"
    };
    static const char *const kept[] = {
        "VIDEO OPEN STOPPED",
        "VIDEO OPEN TIMED OUT IN VIDEO RANGE",
        "VIDEO DECODER NEEDS APP RESTART",
        "page video range probe failed (HTTP 500): upstream",
        "page video range probe failed (HTTP 200): "
            "server did not authorize byte ranges",
        "page video range probe failed (HTTP 0): connection reset",
        "page video request budget",
        "HLS playlist budget refused",
        "HLS request budget refused",
        "media open failed",
        /* An admitted redirect whose target then fails transiently is the
           transport's to retry, not a refusal of the rendition. */
        "page video range probe failed (HTTP 0): connection refused",
        "HLS fetch failed: transfer timed out"
    };
    for (size_t i = 0; i < sizeof(refused) / sizeof(refused[0]); i++)
        CHECK(media_candidate_refused(refused[i]));
    for (size_t i = 0; i < sizeof(kept) / sizeof(kept[0]); i++)
        CHECK(!media_candidate_refused(kept[i]));
    CHECK(!media_candidate_refused(NULL));
    return 0;
}

/* The walk the PSP open path makes: the first rendition is refused for its
   size, the next answers 403, the third opens. A failure that is not a
   refusal ends the walk with the remaining renditions untried. */
static int test_refused_then_next_walk(void)
{
    char packed[MEDIA_CANDIDATE_PACKED_CAPACITY];
    size_t length = 0;
    const char *first = "https://media.test/v-720.mp4";
    CHECK(append(packed, &length, MEDIA_DISCOVERY_MP4,
                 "https://media.test/v-480.mp4", first)
          && append(packed, &length, MEDIA_DISCOVERY_MP4,
                    "https://media.test/v-240.mp4", first)
          && append(packed, &length, MEDIA_DISCOVERY_HLS,
                    "https://media.test/master.m3u8", first));
    static const char *const outcomes[] = {
        "PSP AVC geometry unsupported",
        "page video range probe failed (HTTP 403): forbidden",
        NULL
    };
    size_t cursor = 0, tries = 1;
    const char *current = first;
    for (size_t at = 0; outcomes[at] != NULL; at++) {
        CHECK(media_candidate_refused(outcomes[at]));
        current = media_candidates_next(packed, length, &cursor, NULL);
        CHECK(current != NULL);
        tries++;
    }
    CHECK(tries == 3u && strcmp(current, "https://media.test/v-240.mp4") == 0);

    CHECK(!media_candidate_refused("VIDEO OPEN TIMED OUT IN VIDEO PRIME"));
    CHECK(media_candidates_next(packed, length, &cursor, NULL) != NULL
          && media_candidates_next(packed, length, &cursor, NULL) == NULL);
    return 0;
}

/* A first rendition the firmware backend refuses at open (profile, level
   or picture shape) moves on to the page's HLS rendition instead of ending
   the open; when that one is refused too, the failure that is reported is
   the last candidate's own reason. */
static int test_firmware_admission_then_hls(void)
{
    char packed[MEDIA_CANDIDATE_PACKED_CAPACITY];
    size_t length = 0;
    const char *first = "https://media.test/main.mp4";
    CHECK(append(packed, &length, MEDIA_DISCOVERY_HLS,
                 "https://media.test/master.m3u8", first)
          && append(packed, &length, MEDIA_DISCOVERY_MP4,
                    "https://media.test/alt.mp4", first));
    const char *failure = MEDIA_H264_FIRMWARE_LEVEL_REFUSAL
        ": 66/13 at 202x360";
    CHECK(media_candidate_refused(
        media_candidate_failure_after_pictures(failure, 0u)));
    size_t cursor = 0;
    MediaDiscoveryKind kind = MEDIA_DISCOVERY_NONE;
    const char *next = media_candidates_next(packed, length, &cursor, &kind);
    CHECK(next != NULL && kind == MEDIA_DISCOVERY_HLS
          && strcmp(next, "https://media.test/master.m3u8") == 0);
    const char *last = "PSP video 640x360 needs the wide decoder program";
    CHECK(media_candidate_refused(last));
    next = media_candidates_next(packed, length, &cursor, &kind);
    CHECK(next != NULL && kind == MEDIA_DISCOVERY_MP4);
    CHECK(media_candidates_next(packed, length, &cursor, &kind) == NULL);
    return 0;
}

/* Firmware refusing a stream's opening pictures is the stream's coding;
   a stray refusal among many pictures, or one after the opening seconds,
   is not. The B-pyramid device bisect decoded 11 and refused 16. */
static int test_opening_refusal(void)
{
    CHECK(psp_media_avc_opening_refused(11u, 16u));
    CHECK(psp_media_avc_opening_refused(0u, 3u));
    CHECK(!psp_media_avc_opening_refused(0u, 2u));
    CHECK(!psp_media_avc_opening_refused(40u, 3u));
    CHECK(!psp_media_avc_opening_refused(
        PSP_MEDIA_AVC_OPENING_PICTURES, PSP_MEDIA_AVC_OPENING_PICTURES + 5u));
    CHECK(!psp_media_avc_opening_refused(0u, 0u));
    /* An unrecognised decoder failure keeps its own (non-refusal) text. */
    CHECK(!media_candidate_refused(
        "PSP AVC sceMpegGetAvcNalAu failed: 0x80618001"));
    return 0;
}

/* A full backend reopen of the same rendition (a >=30 s rewind, a suspend,
   a track change) restarts the backend's counters. A refusal right after it
   is a mid-play failure: neither the opening judgement nor the backend's
   first-picture mark may move playback to another rendition. */
static int test_refusal_after_backend_reopen(void)
{
    /* The opening bisect still reads as the coding on a first backend. */
    CHECK(psp_media_avc_rendition_opening_refused(0u, 0u, 11u, 16u));
    /* 40 s of play on the first backend, then the reopened one refuses its
       first pictures. */
    CHECK(!psp_media_avc_rendition_opening_refused(1200u, 0u, 0u, 3u));
    CHECK(!psp_media_avc_rendition_opening_refused(1200u, 0u, 2u, 16u));
    /* A rendition whose first backend was itself refusing keeps counting. */
    CHECK(psp_media_avc_rendition_opening_refused(5u, 6u, 0u, 2u));
    CHECK(!psp_media_avc_rendition_opening_refused(SIZE_MAX, 0u, 1u, 3u));

    static const char refused[] = MEDIA_H264_FIRST_PICTURE_REFUSAL
        ": PSP AVC sceMpegAvcDecode failed: 0x80628002";
    CHECK(media_candidate_failure_after_pictures(refused, 0u) == refused
          && media_candidate_refused(refused));
    const char *after = media_candidate_failure_after_pictures(refused, 1200u);
    CHECK(strcmp(after, "PSP AVC sceMpegAvcDecode failed: 0x80628002") == 0
          && !media_candidate_refused(after));
    /* Other failures, and a declared coding refusal, pass through. */
    static const char pyramid[] =
        "unsupported H.264 stream (B-frame pyramid)";
    CHECK(media_candidate_failure_after_pictures(pyramid, 1200u) == pyramid);
    CHECK(media_candidate_failure_after_pictures(NULL, 1200u) == NULL);
    return 0;
}

int main(void)
{
    if (test_quality_labels() != 0) return 1;
    if (test_packing_bounds_and_repeats() != 0) return 1;
    if (test_packing_invalid_offsets() != 0) return 1;
    if (test_refusal_classification() != 0) return 1;
    if (test_refused_then_next_walk() != 0) return 1;
    if (test_firmware_admission_then_hls() != 0) return 1;
    if (test_opening_refusal() != 0) return 1;
    if (test_refusal_after_backend_reopen() != 0) return 1;
    puts("media candidate tests passed");
    return 0;
}
