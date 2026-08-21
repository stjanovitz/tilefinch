#ifndef TILEFINCH_MEDIA_DISCOVERY_H
#define TILEFINCH_MEDIA_DISCOVERY_H

#include <stdbool.h>
#include <stddef.h>

#include "tilefinch/document.h"

typedef enum {
    MEDIA_DISCOVERY_NONE = 0,
    MEDIA_DISCOVERY_MP4,
    MEDIA_DISCOVERY_HLS,
    MEDIA_DISCOVERY_WEBM,
    MEDIA_DISCOVERY_AUDIO_MP4
} MediaDiscoveryKind;

#define MEDIA_STRUCTURED_AUDIO_CANDIDATE_LIMIT 12u

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

_Static_assert(sizeof(MediaStructuredAudioIndex) <= 1024u,
               "structured audio index must remain PSP-small");

typedef struct {
    MediaDiscoveryKind kind;
    unsigned quality;
    size_t inspected_bytes;
    size_t inspected_nodes;
} MediaDiscoveryResult;

/* Finds one bounded media reference embedded in server-rendered markup or a
   retained data script. Selection is content-shaped and hostname-agnostic:
   prefer direct MP4, then the lowest quality at or above 240p. The returned
   URL is still untrusted and must pass ordinary URL resolution, CSP, request,
   and media-probe policy before playback. */
bool media_discover_document_candidate(
    const PocDocument *document, char *url, size_t url_capacity,
    MediaDiscoveryResult *result);
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

#endif
