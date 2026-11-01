#ifndef TILEFINCH_MEDIA_CANDIDATES_H
#define TILEFINCH_MEDIA_CANDIDATES_H

#include <stdbool.h>
#include <stddef.h>

#include "tilefinch/media_discovery.h"

/* The renditions a page offers after the one chosen first, tried in order
   when the native route refuses the current one. The list is packed into
   one caller-owned buffer: per entry, one kind byte ('0' + kind), the
   absolute URL, and a NUL. It is bounded both in entries and in bytes, and
   never repeats a URL (including the first candidate's). */
#define MEDIA_CANDIDATE_ALTERNATE_LIMIT 3u
#define MEDIA_CANDIDATE_PACKED_CAPACITY 2048u

/* The picture height a quality hint names ("240", "480p", "HD 720p",
   "1280x720"), or 0 when it names none. */
unsigned media_candidate_quality_label(const char *value, size_t length);

/* The entry at packed + *length (kind byte, then a NUL-terminated URL, both
   already written within capacity) joins the list unless it repeats
   first_url or an earlier entry, is not playable video (WebM, audio), or
   the list is full. On success *length moves past it. */
bool media_candidates_commit(
    char *packed, size_t capacity, size_t *length, const char *first_url);
size_t media_candidates_count(const char *packed, size_t length);
/* The next entry at or after *cursor, or NULL at the end. */
const char *media_candidates_next(
    const char *packed, size_t length, size_t *cursor,
    MediaDiscoveryKind *kind);

/* True when a failed open says this rendition cannot work on this device
   while another one might: the decoder refused its picture size or AVC
   profile, it carries no supported track, an HLS master offers nothing
   decodable, its request was answered with an HTTP 4xx status, or a
   request or redirect hop was refused by the page's policy or the hop
   limit. Network failures, timeouts, cancellation and memory refusals are
   not candidate failures. The firmware decoder refusing a stream's H.264
   coding -- a B-frame pyramid or too many reference frames in its SPS, or a
   refusal of its opening pictures -- is a candidate failure too. */
bool media_candidate_refused(const char *error);

/* The backend marks a decode refusal before its first picture as the
   stream's coding (MEDIA_H264_FIRST_PICTURE_REFUSAL). The backend's count
   restarts when the same rendition reopens it, so once the rendition has
   decoded `prior_pictures` on an earlier backend, that classification is
   dropped and the decoder's own failure is returned. */
const char *media_candidate_failure_after_pictures(
    const char *error, size_t prior_pictures);

#endif
