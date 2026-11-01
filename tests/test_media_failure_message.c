#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "tilefinch/media_failure_message.h"
#include "tilefinch/media_mp4.h"

#define CHECK(condition) do {                                                \
    if (!(condition)) {                                                      \
        fprintf(stderr, "CHECK failed at %s:%d: %s\n", __FILE__, __LINE__, \
                #condition);                                                 \
        return 1;                                                            \
    }                                                                        \
} while (0)

#define PYRAMID MEDIA_H264_UNSUPPORTED_STREAM " (B-frame pyramid)"
#define REFERENCES \
    MEDIA_H264_UNSUPPORTED_STREAM " (more than 3 reference frames)"

/* The texts the open and decode paths report for a video that cannot play,
   as the last page candidate leaves them, and what the failed-video panel
   shows for each. */
static int test_failure_kinds(void)
{
    static const struct {
        const char *failure;
        MediaFailureKind kind;
        MediaFailureDetail detail;
        const char *headline;
        const char *details;
    } cases[] = {
        /* a: the stream fits the software decoder, which is missing. A
           firmware refusal it follows does not change that. */
        {PYRAMID "; OPTIONAL VIDEO DECODER NOT INSTALLED - SEE README",
         MEDIA_FAILURE_DECODER_NOT_INSTALLED,
         MEDIA_FAILURE_DETAIL_INSTALL_FOR_LOW_RES,
         "OPTIONAL VIDEO DECODER NEEDED",
         "Install it for low res. See README"},
        {REFERENCES "; OPTIONAL VIDEO DECODER NOT INSTALLED - SEE README",
         MEDIA_FAILURE_DECODER_NOT_INSTALLED,
         MEDIA_FAILURE_DETAIL_INSTALL_FOR_LOW_RES,
         "OPTIONAL VIDEO DECODER NEEDED",
         "Install it for low res. See README"},
        /* A High-profile stream inside 432x240. */
        {"OPTIONAL VIDEO DECODER NOT INSTALLED - SEE README",
         MEDIA_FAILURE_DECODER_NOT_INSTALLED,
         MEDIA_FAILURE_DETAIL_INSTALL_FOR_LOW_RES,
         "OPTIONAL VIDEO DECODER NEEDED",
         "Install it for low res. See README"},
        /* b: installed, but it could not start. */
        {PYRAMID "; software decoder load failed (0x800200d9)",
         MEDIA_FAILURE_DECODER_NOT_STARTED, MEDIA_FAILURE_DETAIL_CODE,
         "VIDEO DECODER COULDN'T START", "0x800200D9"},
        {"software decoder load failed (0x800200d9)",
         MEDIA_FAILURE_DECODER_NOT_STARTED, MEDIA_FAILURE_DETAIL_CODE,
         "VIDEO DECODER COULDN'T START", "0x800200D9"},
        /* A later activation sees the remembered failure. */
        {PYRAMID "; optional video decoder is unavailable (0x800200d9)",
         MEDIA_FAILURE_DECODER_NOT_STARTED, MEDIA_FAILURE_DETAIL_CODE,
         "VIDEO DECODER COULDN'T START", "0x800200D9"},
        {"optional video decoder is unavailable (0x00000000)",
         MEDIA_FAILURE_DECODER_NOT_STARTED, MEDIA_FAILURE_DETAIL_NONE,
         "VIDEO DECODER COULDN'T START", ""},
        {"software decoder ME attach failed (-2147418111)",
         MEDIA_FAILURE_DECODER_NOT_STARTED, MEDIA_FAILURE_DETAIL_CODE,
         "VIDEO DECODER COULDN'T START", "0x80010001"},
        /* The page could not give the decoder its working set. */
        {"swdec working set exceeds budget",
         MEDIA_FAILURE_DECODER_NOT_STARTED, MEDIA_FAILURE_DETAIL_MEMORY,
         "VIDEO DECODER COULDN'T START", "Not enough memory"},
        /* The firmware refused to start a current component: a code, not
           a rebuild. */
        {"software decoder start failed (0x80020190)",
         MEDIA_FAILURE_DECODER_NOT_STARTED, MEDIA_FAILURE_DETAIL_CODE,
         "VIDEO DECODER COULDN'T START", "0x80020190"},
        {"OPTIONAL VIDEO DECODER NEEDS REBUILD - SEE README",
         MEDIA_FAILURE_DECODER_NOT_STARTED, MEDIA_FAILURE_DETAIL_REBUILD,
         "VIDEO DECODER COULDN'T START", "Decoder rebuild needed"},
        {PYRAMID "; OPTIONAL VIDEO DECODER IS INCOMPLETE - SEE README",
         MEDIA_FAILURE_DECODER_NOT_STARTED,
         MEDIA_FAILURE_DETAIL_FILES_MISSING,
         "VIDEO DECODER COULDN'T START", "Files missing. See README"},
        {"software decoder component path is unavailable",
         MEDIA_FAILURE_DECODER_NOT_STARTED,
         MEDIA_FAILURE_DETAIL_FILES_MISSING,
         "VIDEO DECODER COULDN'T START", "Files missing. See README"},
        /* c: no route can play this coding. */
        {PYRAMID, MEDIA_FAILURE_FORMAT_UNSUPPORTED,
         MEDIA_FAILURE_DETAIL_PYRAMID,
         "PSP CAN'T PLAY THIS VIDEO FORMAT", "Uses a B-frame pyramid"},
        {REFERENCES, MEDIA_FAILURE_FORMAT_UNSUPPORTED,
         MEDIA_FAILURE_DETAIL_REFERENCES,
         "PSP CAN'T PLAY THIS VIDEO FORMAT", "Uses over 3 reference frames"},
        {MEDIA_H264_FIRST_PICTURE_REFUSAL
             ": PSP AVC sceMpegAvcDecode failed: 0x80628002",
         MEDIA_FAILURE_FORMAT_UNSUPPORTED,
         MEDIA_FAILURE_DETAIL_PICTURES_REFUSED,
         "PSP CAN'T PLAY THIS VIDEO FORMAT",
         "PSP decoder refused its pictures"},
        {MEDIA_H264_OPENING_REFUSAL, MEDIA_FAILURE_FORMAT_UNSUPPORTED,
         MEDIA_FAILURE_DETAIL_PICTURES_REFUSED,
         "PSP CAN'T PLAY THIS VIDEO FORMAT",
         "PSP decoder refused its pictures"},
        {"software H.264 is limited to 432x240",
         MEDIA_FAILURE_FORMAT_UNSUPPORTED,
         MEDIA_FAILURE_DETAIL_HIGH_TOO_LARGE,
         "PSP CAN'T PLAY THIS VIDEO FORMAT", "High profile, over 432x240"},
        {"unsupported AVC profile", MEDIA_FAILURE_FORMAT_UNSUPPORTED,
         MEDIA_FAILURE_DETAIL_PROFILE,
         "PSP CAN'T PLAY THIS VIDEO FORMAT", "Unsupported H.264 profile"},
        {"HLS carries unsupported AVC syntax",
         MEDIA_FAILURE_FORMAT_UNSUPPORTED, MEDIA_FAILURE_DETAIL_PROFILE,
         "PSP CAN'T PLAY THIS VIDEO FORMAT", "Unsupported H.264 profile"},
        {"PSP AVC geometry unsupported", MEDIA_FAILURE_FORMAT_UNSUPPORTED,
         MEDIA_FAILURE_DETAIL_TOO_LARGE,
         "PSP CAN'T PLAY THIS VIDEO FORMAT", "Picture size too large"},
        {"PSP AVC SPS geometry unsupported",
         MEDIA_FAILURE_FORMAT_UNSUPPORTED, MEDIA_FAILURE_DETAIL_TOO_LARGE,
         "PSP CAN'T PLAY THIS VIDEO FORMAT", "Picture size too large"},
        {"PSP media has no supported track",
         MEDIA_FAILURE_FORMAT_UNSUPPORTED, MEDIA_FAILURE_DETAIL_CODEC,
         "PSP CAN'T PLAY THIS VIDEO FORMAT", "No supported codec"},
        {"HLS master has no compatible AVC rendition",
         MEDIA_FAILURE_FORMAT_UNSUPPORTED, MEDIA_FAILURE_DETAIL_CODEC,
         "PSP CAN'T PLAY THIS VIDEO FORMAT", "No supported codec"},
        /* d: the server refused the request. */
        {"page video range probe failed (HTTP 403): forbidden",
         MEDIA_FAILURE_SOURCE_REFUSED, MEDIA_FAILURE_DETAIL_HTTP_STATUS,
         "VIDEO SOURCE COULD NOT OPEN", "HTTP 403"},
        {"HLS response authority refused (HTTP 403)",
         MEDIA_FAILURE_SOURCE_REFUSED, MEDIA_FAILURE_DETAIL_HTTP_STATUS,
         "VIDEO SOURCE COULD NOT OPEN", "HTTP 403"},
        /* e: the stream address says there is nothing to play now (an
           offline live channel): 404/410 on its playlist, an empty or
           ended playlist, an error body where a playlist or media was
           expected. */
        {"HLS response authority refused (HTTP 404)",
         MEDIA_FAILURE_STREAM_UNAVAILABLE, MEDIA_FAILURE_DETAIL_HTTP_STATUS,
         "STREAM OFFLINE OR NOT AVAILABLE", "HTTP 404"},
        {"HLS response authority refused (HTTP 410)",
         MEDIA_FAILURE_STREAM_UNAVAILABLE, MEDIA_FAILURE_DETAIL_HTTP_STATUS,
         "STREAM OFFLINE OR NOT AVAILABLE", "HTTP 410"},
        {"HLS playlist is empty", MEDIA_FAILURE_STREAM_UNAVAILABLE,
         MEDIA_FAILURE_DETAIL_NONE, "STREAM OFFLINE OR NOT AVAILABLE", ""},
        {"HLS playlist lacks EXTM3U", MEDIA_FAILURE_STREAM_UNAVAILABLE,
         MEDIA_FAILURE_DETAIL_NONE, "STREAM OFFLINE OR NOT AVAILABLE", ""},
        {"page video: " MEDIA_STREAM_UNAVAILABLE " (HTTP 404)",
         MEDIA_FAILURE_STREAM_UNAVAILABLE, MEDIA_FAILURE_DETAIL_HTTP_STATUS,
         "STREAM OFFLINE OR NOT AVAILABLE", "HTTP 404"},
        {"page video: " MEDIA_STREAM_UNAVAILABLE " (HTTP 200)",
         MEDIA_FAILURE_STREAM_UNAVAILABLE, MEDIA_FAILURE_DETAIL_NONE,
         "STREAM OFFLINE OR NOT AVAILABLE", ""},
        {"network media: " MEDIA_STREAM_UNAVAILABLE
             " (HLS playlist is empty)",
         MEDIA_FAILURE_STREAM_UNAVAILABLE, MEDIA_FAILURE_DETAIL_NONE,
         "STREAM OFFLINE OR NOT AVAILABLE", ""},
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        MediaFailureMessage message;
        if (!media_failure_message(cases[i].failure, &message)) {
            fprintf(stderr, "unclassified: %s\n", cases[i].failure);
            return 1;
        }
        if (message.kind != cases[i].kind
            || message.detail != cases[i].detail
            || strcmp(message.headline, cases[i].headline) != 0
            || strcmp(message.details, cases[i].details) != 0) {
            fprintf(stderr, "%s -> %d/%d \"%s\" \"%s\"\n", cases[i].failure,
                    (int) message.kind, (int) message.detail,
                    message.headline, message.details);
            return 1;
        }
        /* The panel keeps the details line only after a one-row headline
           (psp_ui_media_set_error_reason). */
        CHECK(strlen(message.headline) <= 32u);
    }
    return 0;
}

/* Everything else keeps its existing presentation: network and server
   errors, redirect refusals, cancellation and timeouts, and the player's
   own headlines. */
static int test_other_failures_unchanged(void)
{
    static const char *const kept[] = {
        "page video range probe failed (HTTP 0): connection reset",
        "page video range probe failed (HTTP 500): upstream",
        "page video range probe failed (HTTP 302): "
            "response redirect target is invalid",
        "HLS redirect refused (HTTP 302)",
        "VIDEO OPEN STOPPED",
        "VIDEO OPEN TIMED OUT IN VIDEO RANGE",
        "VIDEO DECODER NEEDS APP RESTART",
        "VIDEO DECODER MADE NO PROGRESS",
        "PSP AV module worker timed out",
        "page audio has no supported AAC track",
        "media open failed",
        ""
    };
    for (size_t i = 0; i < sizeof(kept) / sizeof(kept[0]); i++) {
        MediaFailureMessage message;
        CHECK(!media_failure_message(kept[i], &message));
        CHECK(message.kind == MEDIA_FAILURE_OTHER
              && message.headline == NULL);
    }
    MediaFailureMessage message;
    CHECK(!media_failure_message(NULL, &message));
    CHECK(!media_failure_message("unsupported AVC profile", NULL));
    CHECK(media_failure_headline(MEDIA_FAILURE_OTHER) == NULL);
    return 0;
}

/* What a stream address answers decides its route, whatever the address
   looks like; synthetic responses for each shape. */
static int test_response_kinds(void)
{
    static const unsigned char mp4_box[] = {
        0, 0, 0, 0x18, 'f', 't', 'y', 'p', 'i', 's', 'o', 'm'
    };
    static const struct {
        long status;
        const char *content_type;
        const char *body;
        size_t length;
        MediaResponseKind kind;
    } cases[] = {
        /* An offline channel's master: 404 with a JSON error. */
        {404, "application/json", "[{\"error\":\"Can not find channel\"}]",
         34u, MEDIA_RESPONSE_UNAVAILABLE},
        {410, "", "", 0u, MEDIA_RESPONSE_UNAVAILABLE},
        {404, "video/mp4", "", 0u, MEDIA_RESPONSE_UNAVAILABLE},
        /* An HTML or JSON error body answered 200. */
        {200, "text/html; charset=utf-8", "<!doctype html><p>Offline",
         25u, MEDIA_RESPONSE_UNAVAILABLE},
        {200, "application/json", "{\"live\":false}", 14u,
         MEDIA_RESPONSE_UNAVAILABLE},
        {200, "", "<html><body>gone", 16u, MEDIA_RESPONSE_UNAVAILABLE},
        {200, "", "{\"error\":1}", 11u, MEDIA_RESPONSE_UNAVAILABLE},
        /* An extensionless HLS address: by body, or by type when a Range
           probe saw only its first byte. */
        {200, "application/octet-stream", "#EXTM3U\n#EXT-X-VERSION:3\n",
         24u, MEDIA_RESPONSE_HLS},
        {200, "", "\xef\xbb\xbf \r\n#EXTM3U\n", 14u, MEDIA_RESPONSE_HLS},
        {206, "application/vnd.apple.mpegurl", "#", 1u, MEDIA_RESPONSE_HLS},
        {200, "Application/X-MpegURL", "", 0u, MEDIA_RESPONSE_HLS},
        /* A real MP4 stays MP4. */
        {206, "video/mp4", "\0", 1u, MEDIA_RESPONSE_MP4},
        {200, "application/octet-stream", (const char *) mp4_box,
         sizeof(mp4_box), MEDIA_RESPONSE_MP4},
        /* Nothing decisive: the route is kept. */
        {206, "application/octet-stream", "\0", 1u, MEDIA_RESPONSE_UNKNOWN},
        {206, "", "\0", 1u, MEDIA_RESPONSE_UNKNOWN},
        {403, "text/html", "<html>forbidden", 15u, MEDIA_RESPONSE_UNKNOWN},
        {500, "text/html", "<html>error", 11u, MEDIA_RESPONSE_UNKNOWN},
        {302, "", "", 0u, MEDIA_RESPONSE_UNKNOWN},
        {200, "video/mp2t", "G@", 2u, MEDIA_RESPONSE_UNKNOWN},
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        MediaResponseKind kind = media_response_kind(
            cases[i].status, cases[i].content_type,
            (const unsigned char *) cases[i].body, cases[i].length);
        if (kind != cases[i].kind) {
            fprintf(stderr, "response case %zu -> %d, expected %d\n", i,
                    (int) kind, (int) cases[i].kind);
            return 1;
        }
    }
    CHECK(media_response_kind(200, NULL, NULL, 0u) == MEDIA_RESPONSE_UNKNOWN);
    CHECK(media_response_kind(404, NULL, NULL, 0u)
          == MEDIA_RESPONSE_UNAVAILABLE);
    return 0;
}

int main(void)
{
    if (test_response_kinds() != 0) return 1;
    if (test_failure_kinds() != 0) return 1;
    if (test_other_failures_unchanged() != 0) return 1;
    puts("media failure message tests passed");
    return 0;
}
