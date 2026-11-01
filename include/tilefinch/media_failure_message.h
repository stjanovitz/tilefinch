#ifndef TILEFINCH_MEDIA_FAILURE_MESSAGE_H
#define TILEFINCH_MEDIA_FAILURE_MESSAGE_H

#include <stdbool.h>
#include <stddef.h>

/*
 * The user-facing words for a video that could not play. The open and decode
 * paths report engine text ("unsupported H.264 stream (B-frame pyramid);
 * OPTIONAL VIDEO DECODER NOT INSTALLED - SEE README"); the failed-video
 * panel shows a short catalog headline for the kinds below instead, with
 * the cause on its details line. Engine text this does not recognise keeps
 * its existing presentation.
 *
 * The panel is raised once, after page-candidate fallback has run out, so
 * the failure classified is the last candidate's.
 */
typedef enum {
    MEDIA_FAILURE_OTHER = 0,
    /* It would play with the optional software decoder, which is not
       installed. */
    MEDIA_FAILURE_DECODER_NOT_INSTALLED,
    /* The optional software decoder is installed but could not start: not
       enough memory, missing files, a rebuild needed, or a firmware code. */
    MEDIA_FAILURE_DECODER_NOT_STARTED,
    /* Neither the firmware decoder nor the software decoder can play this
       coding, and no other rendition could. */
    MEDIA_FAILURE_FORMAT_UNSUPPORTED,
    /* The video's server answered with an HTTP 4xx status. */
    MEDIA_FAILURE_SOURCE_REFUSED,
    /* The stream address answered that there is nothing to play now: 404
       or 410, a playlist with no variant or segment (an empty or ended
       live playlist), or an error body where media was expected. An
       offline live channel's address answers this way. */
    MEDIA_FAILURE_STREAM_UNAVAILABLE
} MediaFailureKind;

typedef enum {
    MEDIA_FAILURE_DETAIL_NONE = 0,
    MEDIA_FAILURE_DETAIL_INSTALL_FOR_LOW_RES,
    MEDIA_FAILURE_DETAIL_CODE,
    MEDIA_FAILURE_DETAIL_MEMORY,
    MEDIA_FAILURE_DETAIL_REBUILD,
    MEDIA_FAILURE_DETAIL_FILES_MISSING,
    MEDIA_FAILURE_DETAIL_PYRAMID,
    MEDIA_FAILURE_DETAIL_REFERENCES,
    MEDIA_FAILURE_DETAIL_PICTURES_REFUSED,
    MEDIA_FAILURE_DETAIL_HIGH_TOO_LARGE,
    MEDIA_FAILURE_DETAIL_PROFILE,
    MEDIA_FAILURE_DETAIL_TOO_LARGE,
    MEDIA_FAILURE_DETAIL_CODEC,
    MEDIA_FAILURE_DETAIL_HTTP_STATUS
} MediaFailureDetail;

typedef struct {
    MediaFailureKind kind;
    MediaFailureDetail detail;
    /* Catalog keys; NULL headline for MEDIA_FAILURE_OTHER. `details` is a
       catalog key, this message's own `code` ("0x800200D9", "HTTP 403"), or
       "" when there is nothing to add. */
    const char *headline;
    const char *details;
    char code[16];
} MediaFailureMessage;

/* Engine text an open path reports for MEDIA_FAILURE_STREAM_UNAVAILABLE
   ("page video: stream is offline or not available (HTTP 404)"). The HLS
   core's own "HLS playlist is empty" and "HLS playlist lacks EXTM3U", and
   an HLS request answered 404 or 410, are classified the same way. */
#define MEDIA_STREAM_UNAVAILABLE "stream is offline or not available"

/* What a media address's first response says it is, from its status,
   Content-Type and first body bytes (a range probe's, or a whole small
   body). Nothing here looks at the address itself, so an address without
   an extension is routed by what it answers. */
typedef enum {
    /* Nothing decisive: the caller keeps its route. */
    MEDIA_RESPONSE_UNKNOWN = 0,
    /* An MP4 Content-Type or an MP4 box at the start. */
    MEDIA_RESPONSE_MP4,
    /* A body starting with #EXTM3U, or an mpegurl Content-Type. */
    MEDIA_RESPONSE_HLS,
    /* 404 or 410, or a 2xx HTML, JSON or plain-text body. */
    MEDIA_RESPONSE_UNAVAILABLE
} MediaResponseKind;

MediaResponseKind media_response_kind(
    long status, const char *content_type,
    const unsigned char *body, size_t length);

/* Classify an engine failure. Returns false (kind OTHER) when the text is
   not one of the kinds above. */
bool media_failure_message(const char *failure, MediaFailureMessage *message);
/* The panel headline key of a kind, or NULL for MEDIA_FAILURE_OTHER. */
const char *media_failure_headline(MediaFailureKind kind);
/* The details-line key of a detail, or NULL for one that carries a code or
   nothing. */
const char *media_failure_detail_text(MediaFailureDetail detail);

#endif
