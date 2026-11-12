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
        {"HLS playlist is too long (over 2048 entries)",
         MEDIA_FAILURE_FORMAT_UNSUPPORTED,
         MEDIA_FAILURE_DETAIL_PLAYLIST_TOO_LONG,
         "PSP CAN'T PLAY THIS VIDEO FORMAT", "Video playlist too long"},
        {"HLS playlist is too long (VOD text over 128 KiB)",
         MEDIA_FAILURE_FORMAT_UNSUPPORTED,
         MEDIA_FAILURE_DETAIL_PLAYLIST_TOO_LONG,
         "PSP CAN'T PLAY THIS VIDEO FORMAT", "Video playlist too long"},
        {"HLS stream has no supported track (no AVC video; audio-only HLS "
         "is not supported)",
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
        /* f: YouTube's playability verdict, by the resolver's name for it,
           whether from the last client or after the watch page gave up. */
        {"playability: login-required: LOGIN_REQUIRED (Sign in to confirm "
             "you're not a bot)",
         MEDIA_FAILURE_SIGN_IN_REQUIRED, MEDIA_FAILURE_DETAIL_SIGN_IN,
         "THIS VIDEO NEEDS SIGN-IN",
         "Sign in on YouTube, then retry\nlogin-required"},
        {"watch: fetch failed after 3 client attempts: playability: "
             "age-restricted: LOGIN_REQUIRED (Sign in to confirm your age)",
         MEDIA_FAILURE_SIGN_IN_REQUIRED, MEDIA_FAILURE_DETAIL_AGE,
         "THIS VIDEO NEEDS SIGN-IN",
         "Age-restricted: sign in first\nage-restricted"},
        {"watch: page omitted bounded player configuration after 4 client "
             "attempts (login-required)",
         MEDIA_FAILURE_SIGN_IN_REQUIRED, MEDIA_FAILURE_DETAIL_SIGN_IN,
         "THIS VIDEO NEEDS SIGN-IN",
         "Sign in on YouTube, then retry\nlogin-required"},
        {"playability: unavailable: ERROR (This video is private)",
         MEDIA_FAILURE_VIDEO_UNAVAILABLE, MEDIA_FAILURE_DETAIL_PRIVATE,
         "THIS VIDEO ISN'T AVAILABLE",
         "It may be private or removed\nunavailable"},
        {"playability: region-blocked: UNPLAYABLE (The uploader has not "
             "made this video available in your country)",
         MEDIA_FAILURE_VIDEO_UNAVAILABLE, MEDIA_FAILURE_DETAIL_REGION,
         "THIS VIDEO ISN'T AVAILABLE",
         "Blocked in your region\nregion-blocked"},
        {"playability: client-rejected: UNPLAYABLE",
         MEDIA_FAILURE_VIDEO_UNAVAILABLE, MEDIA_FAILURE_DETAIL_REFUSED,
         "THIS VIDEO ISN'T AVAILABLE",
         "YouTube refused it. Try later\nclient-rejected"},
        {"playability: upcoming: Premiere or live stream has not started yet",
         MEDIA_FAILURE_NOT_STARTED_YET, MEDIA_FAILURE_DETAIL_TRY_WHEN_LIVE,
         "THIS VIDEO HASN'T STARTED YET",
         "Try again when it starts\nupcoming"},
        {"playability: live-unsupported: OK",
         MEDIA_FAILURE_FORMAT_UNSUPPORTED, MEDIA_FAILURE_DETAIL_LIVE,
         "PSP CAN'T PLAY THIS VIDEO FORMAT",
         "Unsupported live stream\nlive-unsupported"},
        {"playability: drm-protected: OK (offers only DRM-protected "
             "streams)",
         MEDIA_FAILURE_COPY_PROTECTED, MEDIA_FAILURE_DETAIL_DRM,
         "THIS VIDEO IS COPY-PROTECTED",
         "PSP can't play DRM videos\ndrm-protected"},
        {"watch: page omitted bounded player configuration after 4 client "
             "attempts (drm-protected)",
         MEDIA_FAILURE_COPY_PROTECTED, MEDIA_FAILURE_DETAIL_DRM,
         "THIS VIDEO IS COPY-PROTECTED",
         "PSP can't play DRM videos\ndrm-protected"},
        {"playability: payment-required: UNPLAYABLE (This video requires "
             "payment to watch)",
         MEDIA_FAILURE_VIDEO_UNAVAILABLE, MEDIA_FAILURE_DETAIL_PAID,
         "THIS VIDEO ISN'T AVAILABLE",
         "PSP can't play paid videos\npayment-required"},
        {"watch: page omitted bounded player configuration after 4 client "
             "attempts (payment-required)",
         MEDIA_FAILURE_VIDEO_UNAVAILABLE, MEDIA_FAILURE_DETAIL_PAID,
         "THIS VIDEO ISN'T AVAILABLE",
         "PSP can't play paid videos\npayment-required"},
        /* g: the player request itself. */
        {"player: fetch failed: connection timed out",
         MEDIA_FAILURE_NETWORK, MEDIA_FAILURE_DETAIL_CHECK_NETWORK,
         "NETWORK PROBLEM",
         "Check Wi-Fi, then press X to retry\ntimed out"},
        {"player: enriched fetch failed: connection reset",
         MEDIA_FAILURE_NETWORK, MEDIA_FAILURE_DETAIL_CHECK_NETWORK,
         "NETWORK PROBLEM",
         "Check Wi-Fi, then press X to retry\nfetch failed"},
        {"watch: resolution timed out", MEDIA_FAILURE_NETWORK,
         MEDIA_FAILURE_DETAIL_CHECK_NETWORK, "NETWORK PROBLEM",
         "Check Wi-Fi, then press X to retry\ntimed out"},
        {"player: response exceeded shared memory budget",
         MEDIA_FAILURE_MEMORY, MEDIA_FAILURE_DETAIL_CLOSE_TABS,
         "NOT ENOUGH MEMORY FOR VIDEO",
         "Close other tabs, then retry\nplayer memory"},
        /* h: the firmware media modules would not start. */
        {"AV WP 800200D2 (poll-module-worker)",
         MEDIA_FAILURE_DECODER_NOT_STARTED, MEDIA_FAILURE_DETAIL_RESTART,
         "VIDEO DECODER COULDN'T START",
         "Retry, or restart Tilefinch\nAV WP 800200D2"},
        {"AV WC 800200D9 (create-module-worker)",
         MEDIA_FAILURE_MEMORY, MEDIA_FAILURE_DETAIL_CLOSE_TABS,
         "NOT ENOUGH MEMORY FOR VIDEO",
         "Close other tabs, then retry\nAV WC 800200D9"},
        {"AV ML 80020190 (load-mpeg-vsh)",
         MEDIA_FAILURE_MEMORY, MEDIA_FAILURE_DETAIL_CLOSE_TABS,
         "NOT ENOUGH MEMORY FOR VIDEO",
         "Close other tabs, then retry\nAV ML 80020190"},
        /* i: the firmware audio decoder refused the frames. */
        {"MP3 decode: 0x807F00FD", MEDIA_FAILURE_AUDIO_DECODE,
         MEDIA_FAILURE_DETAIL_CODE, "PSP CAN'T PLAY THIS AUDIO",
         "MP3 0x807F00FD"},
        {"AAC decode: 0x807f0002 calls=2 packets=2",
         MEDIA_FAILURE_AUDIO_DECODE, MEDIA_FAILURE_DETAIL_CODE,
         "PSP CAN'T PLAY THIS AUDIO", "AAC 0x807F0002"},
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
        /* Headline, newline and details share the player's 80-byte status
           (PSP_UI_STATUS_CAPACITY) whole. */
        CHECK(strlen(message.headline) + 1u + strlen(message.details) < 80u);
    }
    return 0;
}

/* Everything else keeps its existing presentation: HLS redirect
   refusals, cancellation and timeouts, and the player's own headlines. */
/* A page media probe that did not open names the status it got, whatever
   it was; no status at all is the network. */
static int test_probe_failures(void)
{
    static const struct {
        const char *failure;
        MediaFailureKind kind;
        const char *audio_headline;
        const char *details;
    } cases[] = {
        {"page audio range probe failed (HTTP 302): "
             "response redirect target is invalid",
         MEDIA_FAILURE_SOURCE_REFUSED, "AUDIO SOURCE COULD NOT OPEN",
         "HTTP 302"},
        {"page video range probe failed (HTTP 500): upstream",
         MEDIA_FAILURE_SOURCE_REFUSED, "AUDIO SOURCE COULD NOT OPEN",
         "HTTP 500"},
        {"page audio range probe failed (HTTP 200): "
             "server did not authorize byte ranges",
         MEDIA_FAILURE_SOURCE_REFUSED, "AUDIO SOURCE COULD NOT OPEN",
         "HTTP 200"},
        {"page audio range probe failed (HTTP 403): forbidden",
         MEDIA_FAILURE_SOURCE_REFUSED, "AUDIO SOURCE COULD NOT OPEN",
         "HTTP 403"},
        {"page video range probe failed (HTTP 0): connection reset",
         MEDIA_FAILURE_NETWORK, "NETWORK PROBLEM",
         "Check Wi-Fi, then press X to retry"}
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        MediaFailureMessage message;
        CHECK(media_failure_message(cases[i].failure, &message));
        CHECK(message.kind == cases[i].kind);
        CHECK(strcmp(media_failure_audio_headline(message.kind),
                     cases[i].audio_headline) == 0);
        CHECK(strcmp(message.details, cases[i].details) == 0);
    }
    MediaFailureMessage message;
    CHECK(media_failure_message(
              "page video range probe failed (HTTP 302): x", &message)
          && strcmp(message.headline, "VIDEO SOURCE COULD NOT OPEN") == 0);
    CHECK(!media_failure_message(
        "page audio range probe failed (HTTP 99): x", &message));
    CHECK(!media_failure_message(
        "page audio range probe failed (HTTP 302", &message));
    return 0;
}

static int test_other_failures_unchanged(void)
{
    static const char *const kept[] = {
        "HLS redirect refused (HTTP 302)",
        "VIDEO OPEN STOPPED",
        "VIDEO OPEN TIMED OUT IN VIDEO RANGE",
        "VIDEO DECODER NEEDS APP RESTART",
        "VIDEO DECODER MADE NO PROGRESS",
        "PSP AV module worker timed out",
        "page audio has no supported AAC track",
        "media open failed",
        /* Resolver texts with no cause a reader can act on. */
        "playability: unknown: ERROR",
        "watch: page omitted bounded player configuration after 4 client "
            "attempts (unknown)",
        "player: request exceeded bound",
        "format: no stream inventory",
        /* Not a module status. */
        "AV WP 8002 (poll-module-worker)",
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
    /* Audio-only media names audio where the video headline names video. */
    CHECK(media_failure_message("AV ML 80010002 (load-mpeg-vsh)", &message)
          && strcmp(media_failure_audio_headline(message.kind),
                    "AUDIO DECODER COULDN'T START") == 0);
    CHECK(strcmp(media_failure_audio_headline(MEDIA_FAILURE_SOURCE_REFUSED),
                 "AUDIO SOURCE COULD NOT OPEN") == 0
          && strcmp(media_failure_audio_headline(MEDIA_FAILURE_MEMORY),
                    "NOT ENOUGH MEMORY FOR AUDIO") == 0
          && media_failure_audio_headline(MEDIA_FAILURE_NETWORK)
                 == media_failure_headline(MEDIA_FAILURE_NETWORK)
          && media_failure_audio_headline(MEDIA_FAILURE_OTHER) == NULL);
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
    MediaFailureMessage allocation;
    CHECK(media_failure_message("PSP aligned codec allocation failed", &allocation));
    CHECK(allocation.kind == MEDIA_FAILURE_MEMORY
        && allocation.detail == MEDIA_FAILURE_DETAIL_CLOSE_TABS);
    CHECK(media_failure_message("PSP 240p codec working set exceeded page budget", &allocation));
    CHECK(allocation.kind == MEDIA_FAILURE_MEMORY);
    CHECK(media_failure_message("PSP backend state exceeded page budget", &allocation));
    CHECK(allocation.kind == MEDIA_FAILURE_MEMORY);
    CHECK(media_failure_message("PSP audio working set exceeded page budget", &allocation));
    CHECK(allocation.kind == MEDIA_FAILURE_MEMORY);
    CHECK(!media_failure_playback_memory(NULL));
    CHECK(!media_failure_playback_memory("PSP AVC SPS geometry unsupported"));
    CHECK(!media_failure_playback_memory("PSP codec teardown was quarantined; restart Tilefinch"));
    CHECK(!media_failure_playback_memory("player: fetch failed: timed out"));
    if (test_response_kinds() != 0) return 1;
    if (test_failure_kinds() != 0) return 1;
    if (test_probe_failures() != 0) return 1;
    if (test_other_failures_unchanged() != 0) return 1;
    puts("media failure message tests passed");
    return 0;
}
