#include "tilefinch/media_failure_message.h"
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
    case MEDIA_FAILURE_OTHER:
    default:
        return NULL;
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

bool media_failure_message(const char *failure, MediaFailureMessage *message)
{
    if (message == NULL) return false;
    memset(message, 0, sizeof(*message));
    if (failure == NULL) return false;
    MediaFailureDetail detail = MEDIA_FAILURE_DETAIL_NONE;
    if (strstr(failure, "OPTIONAL VIDEO DECODER NOT INSTALLED") != NULL
        || strstr(failure, "OPTIONAL VIDEO DECODER IS NOT INSTALLED")
               != NULL) {
        message->kind = MEDIA_FAILURE_DECODER_NOT_INSTALLED;
        detail = MEDIA_FAILURE_DETAIL_INSTALL_FOR_LOW_RES;
    } else if (failure_decoder_start(
                   failure, &detail, message->code, sizeof(message->code))) {
        message->kind = MEDIA_FAILURE_DECODER_NOT_STARTED;
    } else if (failure_format(failure, &detail)) {
        message->kind = MEDIA_FAILURE_FORMAT_UNSUPPORTED;
    } else if (failure_stream_unavailable(failure)) {
        message->kind = MEDIA_FAILURE_STREAM_UNAVAILABLE;
        detail = failure_http_status(
                     failure, message->code, sizeof(message->code))
            ? MEDIA_FAILURE_DETAIL_HTTP_STATUS : MEDIA_FAILURE_DETAIL_NONE;
    } else if (failure_http_status(
                   failure, message->code, sizeof(message->code))) {
        message->kind = MEDIA_FAILURE_SOURCE_REFUSED;
        detail = MEDIA_FAILURE_DETAIL_HTTP_STATUS;
    } else {
        return false;
    }
    message->detail = detail;
    message->headline = media_failure_headline(message->kind);
    const char *text = media_failure_detail_text(detail);
    message->details = text != NULL ? text
        : message->code[0] != '\0' ? message->code : "";
    return true;
}
