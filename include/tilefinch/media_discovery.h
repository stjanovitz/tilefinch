#ifndef TILEFINCH_MEDIA_DISCOVERY_H
#define TILEFINCH_MEDIA_DISCOVERY_H

#include <stdbool.h>
#include <stddef.h>

#include "tilefinch/document.h"

typedef enum {
    MEDIA_DISCOVERY_NONE = 0,
    MEDIA_DISCOVERY_MP4,
    MEDIA_DISCOVERY_HLS,
    MEDIA_DISCOVERY_WEBM
} MediaDiscoveryKind;

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

#endif
