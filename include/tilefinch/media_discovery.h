#ifndef TILEFINCH_MEDIA_DISCOVERY_H
#define TILEFINCH_MEDIA_DISCOVERY_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "tilefinch/document.h"

typedef enum {
    MEDIA_DISCOVERY_NONE = 0,
    MEDIA_DISCOVERY_MP4,
    MEDIA_DISCOVERY_HLS,
    MEDIA_DISCOVERY_WEBM,
    MEDIA_DISCOVERY_AUDIO_MP4
} MediaDiscoveryKind;

#define MEDIA_STRUCTURED_AUDIO_CANDIDATE_LIMIT 12u
#define MEDIA_STRUCTURED_VIDEO_CANDIDATE_LIMIT 8u
#define MEDIA_DECLARED_VIDEO_URL_CAPACITY 2048u
#define MEDIA_DECLARED_VIDEO_TITLE_CAPACITY 192u
#define MEDIA_DECLARED_VIDEO_DURATION_CAPACITY 32u

typedef struct {
    /* Spans reference one retained data-script text node. They remain valid
       only for the lifetime of the committed PocDocument. */
    const char *source;
    uint32_t url_begin;
    uint32_t url_end;
    uint32_t name_begin;
    uint32_t name_end;
    uint32_t page_begin;
    uint32_t page_end;
    uint32_t thumbnail_begin;
    uint32_t thumbnail_end;
    uint32_t duration_begin;
    uint32_t duration_end;
} MediaStructuredAudioCandidate;

typedef struct {
    MediaStructuredAudioCandidate
        candidates[MEDIA_STRUCTURED_AUDIO_CANDIDATE_LIMIT];
    size_t candidate_count;
    size_t inspected_bytes;
    size_t inspected_nodes;
    size_t malformed_scripts;
    size_t truncated_scripts;
    size_t candidate_overflow;
} MediaStructuredAudioIndex;

/* VideoObject uses the same retained-span representation as structured
   audio. embedUrl is intentionally absent: it names a document/player, not
   media bytes suitable for the native decoder. */
typedef MediaStructuredAudioCandidate MediaStructuredVideoCandidate;

typedef struct {
    MediaStructuredVideoCandidate
        candidates[MEDIA_STRUCTURED_VIDEO_CANDIDATE_LIMIT];
    size_t candidate_count;
    size_t inspected_bytes;
    size_t inspected_nodes;
    size_t malformed_scripts;
    size_t truncated_scripts;
    size_t candidate_overflow;
} MediaStructuredVideoIndex;

_Static_assert(sizeof(MediaStructuredAudioIndex) <= 1024u,
               "structured audio index must remain PSP-small");
_Static_assert(sizeof(MediaStructuredVideoIndex) <= 768u,
               "structured video index must remain PSP-small");

typedef struct {
    MediaDiscoveryKind kind;
    unsigned quality;
    size_t inspected_bytes;
    size_t inspected_nodes;
} MediaDiscoveryResult;

typedef struct {
    char media_url[MEDIA_DECLARED_VIDEO_URL_CAPACITY];
    char thumbnail_url[MEDIA_DECLARED_VIDEO_URL_CAPACITY];
    char title[MEDIA_DECLARED_VIDEO_TITLE_CAPACITY];
    char duration[MEDIA_DECLARED_VIDEO_DURATION_CAPACITY];
    MediaDiscoveryKind kind;
    unsigned quality;
    bool structured;
    bool og_video;
    bool og_type_video;
} MediaDeclaredVideo;

struct MediaDeclaredVideoCache {
    MediaDeclaredVideo selected;
};

/* Finds one bounded media reference embedded in server-rendered markup or a
   retained data script. Selection is content-shaped and hostname-agnostic:
   prefer direct MP4, then the lowest quality at or above 240p. The returned
   URL is still untrusted and must pass ordinary URL resolution, CSP, request,
   and media-probe policy before playback. */
bool media_discover_document_candidate(
    const PocDocument *document, char *url, size_t url_capacity,
    MediaDiscoveryResult *result);
/* Resolves only page-declared video evidence: VideoObject.contentUrl or Open
   Graph video metadata, with an og:type=video + retained discovery candidate
   as the bounded fallback. Returned strings remain unresolved/untrusted. */
bool media_discover_declared_video(
    const PocDocument *document, MediaDeclaredVideo *declared);
/* Returns the page-lifetime cached selection without copying its bounded URL
   buffers. The first call performs the only declaration scan for this
   document; allocation refusal is cached as a soft miss. */
const MediaDeclaredVideo *media_declared_video_cached(
    const PocDocument *document);
size_t media_declared_video_discovery_count(const PocDocument *document);
void media_declared_video_cache_destroy(PocDocument *document);
MediaDiscoveryKind media_discovery_reference_kind(
    const char *value, size_t length);
/* Builds a compact, allocation-free index into retained JSON/JSON-LD text.
   Only AudioObject or MusicRecording.audio contexts may contribute an audio
   candidate; an unrelated contentUrl is deliberately ignored. */
bool media_discover_structured_audio(
    const PocDocument *document, MediaStructuredAudioIndex *index);
bool media_structured_audio_copy_url(
    const MediaStructuredAudioCandidate *candidate,
    char *output, size_t capacity);
bool media_structured_audio_copy_name(
    const MediaStructuredAudioCandidate *candidate,
    char *output, size_t capacity);
bool media_structured_audio_copy_page_url(
    const MediaStructuredAudioCandidate *candidate,
    char *output, size_t capacity);
/* VideoObject-only index. contentUrl is the sole media candidate; name,
   thumbnailUrl/thumbnail, and duration are retained presentation metadata. */
bool media_discover_structured_video(
    const PocDocument *document, MediaStructuredVideoIndex *index);
bool media_structured_video_copy_url(
    const MediaStructuredVideoCandidate *candidate,
    char *output, size_t capacity);
bool media_structured_video_copy_name(
    const MediaStructuredVideoCandidate *candidate,
    char *output, size_t capacity);
bool media_structured_video_copy_thumbnail(
    const MediaStructuredVideoCandidate *candidate,
    char *output, size_t capacity);
bool media_structured_video_copy_duration(
    const MediaStructuredVideoCandidate *candidate,
    char *output, size_t capacity);

#endif
