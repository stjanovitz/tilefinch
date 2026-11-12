#include "tilefinch/media_failure_message.h"
#include "tilefinch/media_hls.h"
#include "tilefinch/media_mp4.h"

#include <ctype.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

/*
 * Every text matched here is produced by the open path, the firmware
 * decoder's refusals (media_mp4.c), or the optional software decoder's
 * loader (psp_swdec_component.c). The decoder's own messages win over a
 * firmware refusal they follow ("unsupported H.264 stream (B-frame
 * pyramid); OPTIONAL VIDEO DECODER NOT INSTALLED - SEE README"): the stream
 * would have played through it.
 */

const char *media_failure_headline(MediaFailureKind kind)
{
    switch (kind) {
    case MEDIA_FAILURE_DECODER_NOT_INSTALLED:
        return "OPTIONAL VIDEO DECODER NEEDED";
    case MEDIA_FAILURE_DECODER_NOT_STARTED:
        return "VIDEO DECODER COULDN'T START";
    case MEDIA_FAILURE_FORMAT_UNSUPPORTED:
        return "PSP CAN'T PLAY THIS VIDEO FORMAT";
    case MEDIA_FAILURE_SOURCE_REFUSED:
        return "VIDEO SOURCE COULD NOT OPEN";
    case MEDIA_FAILURE_STREAM_UNAVAILABLE:
        return "STREAM OFFLINE OR NOT AVAILABLE";
    case MEDIA_FAILURE_SIGN_IN_REQUIRED:
        return "THIS VIDEO NEEDS SIGN-IN";
    case MEDIA_FAILURE_VIDEO_UNAVAILABLE:
        return "THIS VIDEO ISN'T AVAILABLE";
    case MEDIA_FAILURE_NOT_STARTED_YET:
        return "THIS VIDEO HASN'T STARTED YET";
    case MEDIA_FAILURE_NETWORK:
        return "NETWORK PROBLEM";
    case MEDIA_FAILURE_MEMORY:
        return "NOT ENOUGH MEMORY FOR VIDEO";
    case MEDIA_FAILURE_COPY_PROTECTED:
        return "THIS VIDEO IS COPY-PROTECTED";
    case MEDIA_FAILURE_INSECURE_AUDIO_QUESTION:
        return "PLAY UNENCRYPTED AUDIO?";
    case MEDIA_FAILURE_INSECURE_AUDIO_BLOCKED:
        return "UNENCRYPTED AUDIO BLOCKED";
    case MEDIA_FAILURE_AUDIO_DECODE:
        return "PSP CAN'T PLAY THIS AUDIO";
    case MEDIA_FAILURE_OTHER:
    default:
        return NULL;
    }
}

const char *media_failure_audio_headline(MediaFailureKind kind)
{
    switch (kind) {
    case MEDIA_FAILURE_DECODER_NOT_STARTED:
        return "AUDIO DECODER COULDN'T START";
    case MEDIA_FAILURE_SOURCE_REFUSED:
        return "AUDIO SOURCE COULD NOT OPEN";
    case MEDIA_FAILURE_MEMORY:
        return "NOT ENOUGH MEMORY FOR AUDIO";
    default:
        return media_failure_headline(kind);
    }
}

const char *media_failure_detail_text(MediaFailureDetail detail)
{
    switch (detail) {
    case MEDIA_FAILURE_DETAIL_INSTALL_FOR_LOW_RES:
        return "Install it for low res. See README";
    case MEDIA_FAILURE_DETAIL_MEMORY:
        return "Not enough memory";
    case MEDIA_FAILURE_DETAIL_REBUILD:
        return "Decoder rebuild needed";
    case MEDIA_FAILURE_DETAIL_FILES_MISSING:
        return "Files missing. See README";
    case MEDIA_FAILURE_DETAIL_PYRAMID:
        return "Uses a B-frame pyramid";
    case MEDIA_FAILURE_DETAIL_REFERENCES:
        return "Uses over 3 reference frames";
    case MEDIA_FAILURE_DETAIL_PICTURES_REFUSED:
        return "PSP decoder refused its pictures";
    case MEDIA_FAILURE_DETAIL_HIGH_TOO_LARGE:
        return "High profile, over 432x240";
    case MEDIA_FAILURE_DETAIL_PROFILE:
        return "Unsupported H.264 profile";
    case MEDIA_FAILURE_DETAIL_TOO_LARGE:
        return "Picture size too large";
    case MEDIA_FAILURE_DETAIL_CODEC:
        return "No supported codec";
    case MEDIA_FAILURE_DETAIL_SIGN_IN:
        return "Sign in on YouTube, then retry";
    case MEDIA_FAILURE_DETAIL_AGE:
        return "Age-restricted: sign in first";
    case MEDIA_FAILURE_DETAIL_PRIVATE:
        return "It may be private or removed";
    case MEDIA_FAILURE_DETAIL_REGION:
        return "Blocked in your region";
    case MEDIA_FAILURE_DETAIL_REFUSED:
        return "YouTube refused it. Try later";
    case MEDIA_FAILURE_DETAIL_TRY_WHEN_LIVE:
        return "Try again when it starts";
    case MEDIA_FAILURE_DETAIL_LIVE:
        return "Unsupported live stream";
    case MEDIA_FAILURE_DETAIL_CHECK_NETWORK:
        return "Check Wi-Fi, then press X to retry";
    case MEDIA_FAILURE_DETAIL_CLOSE_TABS:
        return "Close other tabs, then retry";
    case MEDIA_FAILURE_DETAIL_RESTART:
        return "Retry, or restart Tilefinch";
    case MEDIA_FAILURE_DETAIL_DRM:
        return "PSP can't play DRM videos";
    case MEDIA_FAILURE_DETAIL_PAID:
        return "PSP can't play paid videos";
    case MEDIA_FAILURE_DETAIL_NO_ENCRYPTED_STREAM:
        return "This station is not encrypted";
    case MEDIA_FAILURE_DETAIL_SITE_INFORMATION:
        return "Change it in Site information";
    case MEDIA_FAILURE_DETAIL_PLAYLIST_TOO_LONG:
        return "Video playlist too long";
    case MEDIA_FAILURE_DETAIL_NONE:
    case MEDIA_FAILURE_DETAIL_CODE:
    case MEDIA_FAILURE_DETAIL_HTTP_STATUS:
    default:
        return NULL;
    }
}

/* "(0x800200d9)" or "(-2147418111)" after `at`, as "0x800200D9". A zero
   code says nothing and is not shown. */
static bool failure_native_code(const char *at, char *code, size_t size)
{
    const char *open = strchr(at, '(');
    if (open == NULL) return false;
    open++;
    char *end = NULL;
    uint32_t value;
    if (open[0] == '0' && (open[1] == 'x' || open[1] == 'X'))
        value = (uint32_t) strtoul(open + 2, &end, 16);
    else
        value = (uint32_t) strtol(open, &end, 10);
    if (end == NULL || end == open || *end != ')' || value == 0)
        return false;
    snprintf(code, size, "0x%08lX", (unsigned long) value);
    return true;
}

/* "(HTTP 403)" or "(HTTP 403):" as "HTTP 403". */
static bool failure_http_status(const char *failure, char *code, size_t size)
{
    const char *at = strstr(failure, "(HTTP 4");
    if (at == NULL) return false;
    at += 6;
    if (at[1] < '0' || at[1] > '9' || at[2] < '0' || at[2] > '9'
        || (at[3] != ')' && at[3] != ' ')) return false;
    snprintf(code, size, "HTTP %.3s", at);
    return true;
}

/* The page media probe's own failure, "page audio range probe failed
   (HTTP 302): ...": whatever status the server answered (a redirect that
   could not be followed, a server error) the source did not open, and the
   status is its code. No status at all is no answer from the network. */
static bool failure_probe_status(
    const char *failure, MediaFailureKind *kind, MediaFailureDetail *detail,
    char *code, size_t size)
{
    static const char marker[] = "range probe failed (HTTP ";
    const char *at = strstr(failure, marker);
    if (at == NULL) return false;
    at += sizeof(marker) - 1u;
    char *end = NULL;
    long status = strtol(at, &end, 10);
    if (end == at || *end != ')' || status < 0 || status > 599) return false;
    if (status == 0) {
        *kind = MEDIA_FAILURE_NETWORK;
        *detail = MEDIA_FAILURE_DETAIL_CHECK_NETWORK;
        return true;
    }
    if (status < 100) return false;
    *kind = MEDIA_FAILURE_SOURCE_REFUSED;
    *detail = MEDIA_FAILURE_DETAIL_HTTP_STATUS;
    snprintf(code, size, "HTTP %ld", status);
    return true;
}

/* The installed software decoder's start failures. */
static bool failure_decoder_start(
    const char *failure, MediaFailureDetail *detail, char *code, size_t size)
{
    const char *at;
    if (strstr(failure, "OPTIONAL VIDEO DECODER NEEDS REBUILD") != NULL) {
        *detail = MEDIA_FAILURE_DETAIL_REBUILD;
    } else if (strstr(failure, "OPTIONAL VIDEO DECODER IS INCOMPLETE") != NULL
               || strstr(failure,
                         "software decoder component path is unavailable")
                      != NULL) {
        *detail = MEDIA_FAILURE_DETAIL_FILES_MISSING;
    } else if (strstr(failure, "swdec working set exceeds budget")
               != NULL) {
        *detail = MEDIA_FAILURE_DETAIL_MEMORY;
    } else if ((at = strstr(failure, "software decoder load failed")) != NULL
               || (at = strstr(failure, "software decoder start failed"))
                      != NULL
               || (at = strstr(failure,
                               "optional video decoder is unavailable"))
                      != NULL
               || (at = strstr(failure, "software decoder ME attach failed"))
                      != NULL) {
        *detail = failure_native_code(at, code, size)
            ? MEDIA_FAILURE_DETAIL_CODE : MEDIA_FAILURE_DETAIL_NONE;
    } else {
        return false;
    }
    return true;
}

/* Codings neither decoder route can play. */
static bool failure_format(const char *failure, MediaFailureDetail *detail)
{
    if (strstr(failure, MEDIA_H264_UNSUPPORTED_STREAM " (B-frame pyramid)")
        != NULL) {
        *detail = MEDIA_FAILURE_DETAIL_PYRAMID;
    } else if (strstr(failure, MEDIA_H264_UNSUPPORTED_STREAM
                      " (more than 3 reference frames)") != NULL) {
        *detail = MEDIA_FAILURE_DETAIL_REFERENCES;
    } else if (strstr(failure, MEDIA_H264_FIRST_PICTURE_REFUSAL) != NULL
               || strstr(failure, MEDIA_H264_OPENING_REFUSAL) != NULL) {
        *detail = MEDIA_FAILURE_DETAIL_PICTURES_REFUSED;
    } else if (strstr(failure, "software H.264 is limited") != NULL) {
        *detail = MEDIA_FAILURE_DETAIL_HIGH_TOO_LARGE;
    } else if (strstr(failure, "unsupported AVC") != NULL) {
        *detail = MEDIA_FAILURE_DETAIL_PROFILE;
    } else if (strstr(failure, "geometry unsupported") != NULL) {
        *detail = MEDIA_FAILURE_DETAIL_TOO_LARGE;
    } else if (strstr(failure, "has no supported track") != NULL
               || strstr(failure, "no compatible AVC rendition") != NULL) {
        *detail = MEDIA_FAILURE_DETAIL_CODEC;
    } else if (strstr(failure, MEDIA_HLS_PLAYLIST_TOO_LONG) != NULL) {
        *detail = MEDIA_FAILURE_DETAIL_PLAYLIST_TOO_LONG;
    } else {
        return false;
    }
    return true;
}

/* Whether a Content-Type's media type (before any parameter) begins with
   one of `types`, ignoring case. */
static bool response_type_is(const char *content_type,
                             const char *const *types, size_t count)
{
    if (content_type == NULL) return false;
    while (*content_type == ' ' || *content_type == '\t') content_type++;
    for (size_t i = 0; i < count; i++) {
        size_t length = strlen(types[i]);
        if (strncasecmp(content_type, types[i], length) == 0) return true;
    }
    return false;
}

MediaResponseKind media_response_kind(
    long status, const char *content_type,
    const unsigned char *body, size_t length)
{
    if (status == 404 || status == 410) return MEDIA_RESPONSE_UNAVAILABLE;
    if (status < 200 || status > 299) return MEDIA_RESPONSE_UNKNOWN;
    if (body == NULL) length = 0;
    /* A box starts at offset 0 with its size, then its type. */
    if (length >= 8u
        && (memcmp(body + 4, "ftyp", 4u) == 0
            || memcmp(body + 4, "styp", 4u) == 0
            || memcmp(body + 4, "moov", 4u) == 0))
        return MEDIA_RESPONSE_MP4;
    if (length >= 3u && memcmp(body, "\xef\xbb\xbf", 3u) == 0) {
        body += 3;
        length -= 3u;
    }
    while (length != 0u && isspace(*body)) {
        body++;
        length--;
    }
    if (length >= 7u && memcmp(body, "#EXTM3U", 7u) == 0)
        return MEDIA_RESPONSE_HLS;
    if (content_type != NULL && content_type[0] != '\0') {
        static const char *const mp4[] = {
            "video/mp4", "audio/mp4", "application/mp4", "video/quicktime"
        };
        static const char *const not_media[] = {
            "text/html", "application/json", "text/plain",
            "application/problem+json", "application/xhtml+xml"
        };
        /* application/vnd.apple.mpegurl, application/x-mpegURL, audio/... */
        for (const char *at = content_type; *at != '\0'; at++)
            if (strncasecmp(at, "mpegurl", 7u) == 0) return MEDIA_RESPONSE_HLS;
        if (response_type_is(content_type, mp4, sizeof(mp4) / sizeof(mp4[0])))
            return MEDIA_RESPONSE_MP4;
        if (response_type_is(content_type, not_media,
                             sizeof(not_media) / sizeof(not_media[0])))
            return MEDIA_RESPONSE_UNAVAILABLE;
        return MEDIA_RESPONSE_UNKNOWN;
    }
    /* No Content-Type: an HTML page or a JSON document says so itself. */
    if (length != 0u && (*body == '{' || *body == '['
                         || (length >= 2u && body[0] == '<'
                             && (body[1] == '!' || body[1] == 'h'
                                 || body[1] == 'H'))))
        return MEDIA_RESPONSE_UNAVAILABLE;
    return MEDIA_RESPONSE_UNKNOWN;
}

/*
 * The YouTube resolver's playability verdict: "playability: NAME: STATUS
 * (reason)" for the last client it asked, or "... client attempts (NAME)"
 * when the watch page itself gave up after them. NAME is
 * youtube_playability_name()'s and is kept as the code.
 */
static bool failure_playability(
    const char *failure, MediaFailureKind *kind, MediaFailureDetail *detail,
    char *code, size_t size)
{
    static const struct {
        const char *name;
        MediaFailureKind kind;
        MediaFailureDetail detail;
    } verdicts[] = {
        {"login-required", MEDIA_FAILURE_SIGN_IN_REQUIRED,
         MEDIA_FAILURE_DETAIL_SIGN_IN},
        {"age-restricted", MEDIA_FAILURE_SIGN_IN_REQUIRED,
         MEDIA_FAILURE_DETAIL_AGE},
        {"region-blocked", MEDIA_FAILURE_VIDEO_UNAVAILABLE,
         MEDIA_FAILURE_DETAIL_REGION},
        {"unavailable", MEDIA_FAILURE_VIDEO_UNAVAILABLE,
         MEDIA_FAILURE_DETAIL_PRIVATE},
        {"client-rejected", MEDIA_FAILURE_VIDEO_UNAVAILABLE,
         MEDIA_FAILURE_DETAIL_REFUSED},
        {"upcoming", MEDIA_FAILURE_NOT_STARTED_YET,
         MEDIA_FAILURE_DETAIL_TRY_WHEN_LIVE},
        {"live-unsupported", MEDIA_FAILURE_FORMAT_UNSUPPORTED,
         MEDIA_FAILURE_DETAIL_LIVE},
        {"drm-protected", MEDIA_FAILURE_COPY_PROTECTED,
         MEDIA_FAILURE_DETAIL_DRM},
        {"payment-required", MEDIA_FAILURE_VIDEO_UNAVAILABLE,
         MEDIA_FAILURE_DETAIL_PAID}
    };
    const char *name = strstr(failure, "playability: ");
    if (name != NULL) {
        name += 13;
    } else if ((name = strstr(failure, "client attempts (")) != NULL) {
        name += 17;
    } else {
        return false;
    }
    for (size_t i = 0; i < sizeof(verdicts) / sizeof(verdicts[0]); i++) {
        size_t length = strlen(verdicts[i].name);
        if (strncmp(name, verdicts[i].name, length) != 0
            || (name[length] != ':' && name[length] != ')'
                && name[length] != '\0')) continue;
        *kind = verdicts[i].kind;
        *detail = verdicts[i].detail;
        snprintf(code, size, "%s", verdicts[i].name);
        return true;
    }
    return false;
}

/* The firmware media modules' start failures: "AV WP 800200D2
   (poll-module-worker)". Out-of-memory statuses say so; any other one
   leaves the player unable to start until a retry or a restart. */
static bool failure_media_module(
    const char *failure, MediaFailureKind *kind, MediaFailureDetail *detail,
    char *code, size_t size)
{
    const char *at = failure;
    while ((at = strstr(at, "AV ")) != NULL) {
        bool shape = at[3] >= 'A' && at[3] <= 'Z'
            && at[4] >= 'A' && at[4] <= 'Z' && at[5] == ' ';
        size_t digits = 0;
        while (shape && digits < 8u && isxdigit((unsigned char) at[6 + digits]))
            digits++;
        if (shape && digits == 8u && !isxdigit((unsigned char) at[14])) {
            uint32_t status = (uint32_t) strtoul(at + 6, NULL, 16);
            /* SCE_KERNEL_ERROR_NO_MEMORY, SCE_KERNEL_ERROR_MEMBLOCK_ALLOC_FAILED */
            bool memory = status == UINT32_C(0x80020190)
                || status == UINT32_C(0x800200D9);
            *kind = memory ? MEDIA_FAILURE_MEMORY
                           : MEDIA_FAILURE_DECODER_NOT_STARTED;
            *detail = memory ? MEDIA_FAILURE_DETAIL_CLOSE_TABS
                             : MEDIA_FAILURE_DETAIL_RESTART;
            snprintf(code, size, "%.14s", at);
            return true;
        }
        at += 3;
    }
    return false;
}

/* The audio decoder's refusal: "MP3 decode: 0x807F00FD" (or AAC), shown
   as "MP3 0x807F00FD". */
static bool failure_audio_decode(const char *failure, char *code, size_t size)
{
    static const char *const codecs[] = {"MP3", "AAC"};
    for (size_t i = 0; i < sizeof(codecs) / sizeof(codecs[0]); i++) {
        char prefix[16];
        snprintf(prefix, sizeof(prefix), "%s decode: 0x", codecs[i]);
        const char *at = strstr(failure, prefix);
        if (at == NULL) continue;
        at += strlen(prefix);
        size_t digits = 0;
        while (digits < 8u && isxdigit((unsigned char) at[digits])) digits++;
        if (digits != 8u || isxdigit((unsigned char) at[8])) continue;
        snprintf(code, size, "%s 0x%.8s", codecs[i], at);
        for (char *c = code; *c != '\0'; c++)
            if (*c >= 'a' && *c <= 'f') *c = (char) (*c - 'a' + 'A');
        return true;
    }
    return false;
}

/* The resolver's own request failures. Network trouble names the step that
   failed; memory trouble is the shared response budget. */
static bool failure_player_request(
    const char *failure, MediaFailureKind *kind, MediaFailureDetail *detail,
    char *code, size_t size)
{
    if (strncmp(failure, "player: ", 8u) != 0
        && strncmp(failure, "watch: ", 7u) != 0) return false;
    if (strstr(failure, "exceeded shared memory budget") != NULL
        || strstr(failure, "allocation failed") != NULL) {
        *kind = MEDIA_FAILURE_MEMORY;
        *detail = MEDIA_FAILURE_DETAIL_CLOSE_TABS;
        snprintf(code, size, "%s", "player memory");
    } else if (strstr(failure, "timed out") != NULL) {
        *kind = MEDIA_FAILURE_NETWORK;
        *detail = MEDIA_FAILURE_DETAIL_CHECK_NETWORK;
        snprintf(code, size, "%s", "timed out");
    } else if (strstr(failure, "fetch failed") != NULL) {
        *kind = MEDIA_FAILURE_NETWORK;
        *detail = MEDIA_FAILURE_DETAIL_CHECK_NETWORK;
        snprintf(code, size, "%s", "fetch failed");
    } else {
        return false;
    }
    return true;
}

/* A stream address with nothing to play now: the open paths' own words
   (MEDIA_STREAM_UNAVAILABLE), an HLS playlist with no entry or that is no
   playlist at all, or an HLS request answered 404 or 410. */
static bool failure_stream_unavailable(const char *failure)
{
    if (strstr(failure, MEDIA_STREAM_UNAVAILABLE) != NULL
        || strstr(failure, "HLS playlist is empty") != NULL
        || strstr(failure, "HLS playlist lacks EXTM3U") != NULL)
        return true;
    return strncmp(failure, "HLS ", 4u) == 0
        && (strstr(failure, "(HTTP 404)") != NULL
            || strstr(failure, "(HTTP 410)") != NULL);
}

bool media_failure_playback_memory(const char *failure)
{
    return failure != NULL
        && (strstr(failure, "exceeds budget") != NULL
            || (strncmp(failure, "PSP ", 4u) == 0
                && strstr(failure, "exceeded page budget") != NULL)
            || strcmp(failure, "PSP aligned codec allocation failed") == 0);
}

bool media_failure_message(const char *failure, MediaFailureMessage *message)
{
    if (message == NULL) return false;
    memset(message, 0, sizeof(*message));
    if (failure == NULL) return false;
    MediaFailureDetail detail = MEDIA_FAILURE_DETAIL_NONE;
    if (strcmp(failure, MEDIA_INSECURE_AUDIO_QUESTION) == 0) {
        message->kind = MEDIA_FAILURE_INSECURE_AUDIO_QUESTION;
        detail = MEDIA_FAILURE_DETAIL_NO_ENCRYPTED_STREAM;
    } else if (strcmp(failure, MEDIA_INSECURE_AUDIO_BLOCKED) == 0) {
        message->kind = MEDIA_FAILURE_INSECURE_AUDIO_BLOCKED;
        detail = MEDIA_FAILURE_DETAIL_SITE_INFORMATION;
    } else if (failure_audio_decode(
                   failure, message->code, sizeof(message->code))) {
        message->kind = MEDIA_FAILURE_AUDIO_DECODE;
        detail = MEDIA_FAILURE_DETAIL_CODE;
    } else if (strstr(failure, "OPTIONAL VIDEO DECODER NOT INSTALLED") != NULL
        || strstr(failure, "OPTIONAL VIDEO DECODER IS NOT INSTALLED")
               != NULL) {
        message->kind = MEDIA_FAILURE_DECODER_NOT_INSTALLED;
        detail = MEDIA_FAILURE_DETAIL_INSTALL_FOR_LOW_RES;
    } else if (failure_decoder_start(
                   failure, &detail, message->code, sizeof(message->code))) {
        message->kind = MEDIA_FAILURE_DECODER_NOT_STARTED;
    } else if (media_failure_playback_memory(failure)) {
        message->kind = MEDIA_FAILURE_MEMORY;
        detail = MEDIA_FAILURE_DETAIL_CLOSE_TABS;
        snprintf(message->code, sizeof(message->code), "%s", "codec memory");
    } else if (failure_format(failure, &detail)) {
        message->kind = MEDIA_FAILURE_FORMAT_UNSUPPORTED;
    } else if (failure_playability(
                   failure, &message->kind, &detail,
                   message->code, sizeof(message->code))) {
        /* The verdict set the kind, its next step and its name. */
    } else if (failure_stream_unavailable(failure)) {
        message->kind = MEDIA_FAILURE_STREAM_UNAVAILABLE;
        detail = failure_http_status(
                     failure, message->code, sizeof(message->code))
            ? MEDIA_FAILURE_DETAIL_HTTP_STATUS : MEDIA_FAILURE_DETAIL_NONE;
    } else if (failure_probe_status(
                   failure, &message->kind, &detail,
                   message->code, sizeof(message->code))) {
        /* The probe's status set the kind and its code. */
    } else if (failure_http_status(
                   failure, message->code, sizeof(message->code))) {
        message->kind = MEDIA_FAILURE_SOURCE_REFUSED;
        detail = MEDIA_FAILURE_DETAIL_HTTP_STATUS;
    } else if (!failure_media_module(
                   failure, &message->kind, &detail,
                   message->code, sizeof(message->code))
               && !failure_player_request(
                   failure, &message->kind, &detail,
                   message->code, sizeof(message->code))) {
        return false;
    }
    message->detail = detail;
    message->headline = media_failure_headline(message->kind);
    const char *text = media_failure_detail_text(detail);
    if (text != NULL && message->code[0] != '\0') {
        snprintf(message->line, sizeof(message->line), "%s\n%s",
                 text, message->code);
        message->details = message->line;
    } else {
        message->details = text != NULL ? text
            : message->code[0] != '\0' ? message->code : "";
    }
    return true;
}
