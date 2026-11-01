#include "tilefinch/media_candidates.h"
#include "tilefinch/media_failure_message.h"
#include "tilefinch/media_mp4.h"

#include <string.h>

static bool candidate_digit(char value)
{
    return value >= '0' && value <= '9';
}

static size_t candidate_number(
    const char *value, size_t length, size_t at, unsigned *number)
{
    size_t digits = 0;
    *number = 0;
    while (at + digits < length && candidate_digit(value[at + digits])) {
        if (digits < 6u)
            *number = *number * 10u + (unsigned) (value[at + digits] - '0');
        digits++;
    }
    return digits;
}

unsigned media_candidate_quality_label(const char *value, size_t length)
{
    if (value == NULL) return 0;
    for (size_t at = 0; at < length;) {
        if (!candidate_digit(value[at])) {
            at++;
            continue;
        }
        unsigned number = 0;
        size_t digits = candidate_number(value, length, at, &number);
        at += digits;
        /* "1280x720" names the width first. */
        if (at + 1u < length && (value[at] == 'x' || value[at] == 'X')
            && candidate_digit(value[at + 1u])) {
            digits = candidate_number(value, length, at + 1u, &number);
            at += 1u + digits;
        }
        /* The same bounds page-level discovery reads quality with. */
        if (digits >= 3u && digits <= 4u && number >= 144u
            && number <= 4320u) return number;
    }
    return 0;
}

static bool candidate_entry_playable(char kind)
{
    return kind == (char) ('0' + MEDIA_DISCOVERY_MP4)
        || kind == (char) ('0' + MEDIA_DISCOVERY_HLS)
        || kind == (char) ('0' + MEDIA_DISCOVERY_NONE);
}

bool media_candidates_commit(
    char *packed, size_t capacity, size_t *length, const char *first_url)
{
    if (packed == NULL || length == NULL || *length > capacity
        || capacity - *length < 2u)
        return false;
    char *entry = packed + *length;
    const char *url = entry + 1;
    size_t url_length = strnlen(url, capacity - *length - 1u);
    if (url_length == 0 || url_length > capacity - *length - 2u
        || !candidate_entry_playable(entry[0])
        || (first_url != NULL && strcmp(first_url, url) == 0)) return false;
    /* Count and reject duplicates in the same bounded walk. */
    size_t cursor = 0, count = 0;
    for (const char *earlier;
         (earlier = media_candidates_next(
              packed, *length, &cursor, NULL)) != NULL;) {
        if (++count >= MEDIA_CANDIDATE_ALTERNATE_LIMIT
            || strcmp(earlier, url) == 0) return false;
    }
    *length += 2u + url_length;
    return true;
}

const char *media_candidates_next(
    const char *packed, size_t length, size_t *cursor,
    MediaDiscoveryKind *kind)
{
    if (packed == NULL || cursor == NULL || *cursor > length
        || length - *cursor < 2u)
        return NULL;
    const char *entry = packed + *cursor;
    size_t url_length = strnlen(entry + 1, length - *cursor - 1u);
    if (url_length == 0 || url_length > length - *cursor - 2u) return NULL;
    if (kind != NULL) {
        unsigned value = (unsigned char) entry[0];
        *kind = value >= '0' && value <= '0' + MEDIA_DISCOVERY_AUDIO_MP4
            ? (MediaDiscoveryKind) (value - '0') : MEDIA_DISCOVERY_NONE;
    }
    *cursor += 2u + url_length;
    return entry + 1;
}

size_t media_candidates_count(const char *packed, size_t length)
{
    size_t count = 0, cursor = 0;
    while (media_candidates_next(packed, length, &cursor, NULL) != NULL)
        count++;
    return count;
}

const char *media_candidate_failure_after_pictures(
    const char *error, size_t prior_pictures)
{
    static const char first_picture[] = MEDIA_H264_FIRST_PICTURE_REFUSAL ": ";
    if (error == NULL || prior_pictures == 0
        || strncmp(error, first_picture, sizeof(first_picture) - 1u) != 0)
        return error;
    return error + sizeof(first_picture) - 1u;
}

bool media_candidate_refused(const char *error)
{
    if (error == NULL) return false;
    /* Producers: the firmware decoder's geometry and reference-structure
       checks and its refusal of a stream's opening pictures, the software
       decoder's bound and the AVC route checks in the open path, the HLS
       master selection, and the page/HLS response checks, which all report
       their status as "(HTTP <code>)". */
    static const char *const refusals[] = {
        "geometry unsupported",
        "software H.264 is limited",
        "unsupported AVC",
        /* The firmware decoder's reference-structure refusals: declared in
           the SPS before open, or seen on the stream's opening pictures. */
        MEDIA_H264_UNSUPPORTED_STREAM,
        "no compatible AVC rendition",
        /* The firmware backend's own admission at open: a profile, level
           or picture shape its programs do not take, the wide program
           being off or latched off, and a config it cannot hold. */
        MEDIA_H264_FIRMWARE_LEVEL_REFUSAL,
        "needs the wide decoder program",
        "surface policy rejected geometry",
        "requires exactly one SPS and PPS",
        "High profile format coming soon",
        "has no supported track",
        "(HTTP 4",
        /* A stream address with nothing to play now: an empty or ended
           playlist, or an error body where media was expected
           (media_failure_message.h). */
        MEDIA_STREAM_UNAVAILABLE,
        "HLS playlist is empty",
        "HLS playlist lacks EXTM3U",
        /* A redirect hop the page's own policy refuses (CSP media-src, the
           content blocker, a disallowed scheme or credentials, a mixed
           downgrade) or that loops past the hop limit fails the same way on
           every retry: the scheduler's hop walk and the HLS worker path. */
        "redirect target is invalid",
        "redirect loop detected",
        "eight-hop redirect limit",
        "HLS redirect refused",
        /* The first hop of an HLS playlist, variant or segment request,
           admitted under the same gate as a redirect target. */
        "HLS request refused by page policy"
    };
    for (size_t i = 0; i < sizeof(refusals) / sizeof(refusals[0]); i++) {
        if (strstr(error, refusals[i]) != NULL) return true;
    }
    return false;
}
