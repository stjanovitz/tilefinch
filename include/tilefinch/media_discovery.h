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
    /* An audio-only file: .m4a, or .mp3 (the audio open routes it by what
       it answers, media_http_probe_route_audio). */
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
   audio. embedUrl is never a candidate: it names a document/player, not
   media bytes suitable for the native decoder. The first VideoObject
   embedUrl is kept apart (embed_*) as the page's declared player page. */
typedef MediaStructuredAudioCandidate MediaStructuredVideoCandidate;

typedef struct {
    MediaStructuredVideoCandidate
        candidates[MEDIA_STRUCTURED_VIDEO_CANDIDATE_LIMIT];
    const char *embed_source;
    uint32_t embed_begin;
    uint32_t embed_end;
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
    /* The player page the site declares for its video (VideoObject
       embedUrl, an HTML og:video, twitter:player): an absolute or
       scheme-relative http(s) address without a media suffix, or empty.
       It names a document to load, never media. */
    char embed_url[MEDIA_DECLARED_VIDEO_URL_CAPACITY];
    char thumbnail_url[MEDIA_DECLARED_VIDEO_URL_CAPACITY];
    char title[MEDIA_DECLARED_VIDEO_TITLE_CAPACITY];
    char duration[MEDIA_DECLARED_VIDEO_DURATION_CAPACITY];
    MediaDiscoveryKind kind;
    unsigned quality;
    bool structured;
    bool og_video;
    bool og_type_video;
} MediaDeclaredVideo;

/* Finds one bounded media reference embedded in server-rendered markup or a
   retained data script. Selection is content-shaped and hostname-agnostic:
   references whose key, enclosing key or path names an ad, a promotion or
   an app download are dropped; then prefer a direct MP4 the PSP can
   decode, then HLS, then a direct MP4 hinted at 720p or more, then the
   lowest quality at or above 240p (a "/2026/" path segment is no
   quality). The returned
   URL is still untrusted and must pass ordinary URL resolution, CSP, request,
   and media-probe policy before playback. */
bool media_discover_document_candidate(
    const PocDocument *document, char *url, size_t url_capacity,
    MediaDiscoveryResult *result);
/* The same scan for activating a video, also reporting in streaming_url
   the best HLS reference when the overall winner is not HLS (empty when
   there is none). References are ranked first by how surely they belong to
   the activated video: an ID from page_url, the canonical URL, og:url, the
   og:image or the anchor's poster in the reference's path ranks first,
   then references inside the anchor's player container; preview,
   thumbnail and related-video references (by key, class, id or path) rank
   last but are never dropped. A data literal also ranks as the page's own
   when the nearest of the four objects around it that names an item names
   the page's video (a value carrying a page ID; an "id", "guid" or
   "videoId" naming something else makes it another video's), or, when
   none names one, when a page ID sits before it in the object holding it
   or the one holding that. Several VideoObjects none of which names the
   page's video rank as a list of other clips, not as its declaration.
   Only an own, unmarked literal may lack a media
   suffix, under its own strong video key (playAddr, videoUrl, streamSrc)
   with no sound key around it. When the markup leaves the winner or the
   HLS slot empty, or its winner is not ID-matched, string literals in
   executable inline scripts (absolute or root-relative .mp4/.m3u8
   references, within the same text budget, 64 KiB per script unless the
   script opens by assigning a literal) are read too, and so are the
   rendition lists in the JSON responses the page's scripts fetched (see
   media_json_response_remember), which rank as the player's own data.
   page_url and anchor (the activated <video>) may be NULL. */
bool media_discover_document_candidates(
    const PocDocument *document, const char *page_url,
    lxb_dom_node_t *anchor, char *url, size_t url_capacity,
    char *streaming_url, size_t streaming_capacity,
    MediaDiscoveryResult *result);
/* Resolves only page-declared video evidence: VideoObject.contentUrl or Open
   Graph video metadata, with an og:type=video + retained discovery candidate
   as the bounded fallback. Of several VideoObjects, one whose contentUrl or
   url names the page's video (by the canonical URL, og:url or og:image)
   wins. Returned strings remain unresolved/untrusted. */
bool media_discover_declared_video(
    const PocDocument *document, MediaDeclaredVideo *declared);
/* Returns the page-lifetime cached selection without copying its bounded URL
   buffers. The first call, and the first after a script mutation, scans;
   later calls return the cached selection; allocation refusal is cached as
   a soft miss.
   Declarations survive client scripts that remove them: the document keeps
   the last declaration a scan found (first the parsed server markup, see
   media_declared_video_keep_parsed) with the head's identity tokens, and a
   rescan that finds none, or only a part of the kept one, returns the kept
   one. A rescan naming another media or player-page address wins and is
   kept instead. A change of the document's URL forgets it
   (media_declared_video_cache_destroy). */
const MediaDeclaredVideo *media_declared_video_cached(
    const PocDocument *document);
/* The same cached scan, also returning a page that declares only a player
   page (media_url empty, embed_url set): the declared-video card offers
   it, and Play opens that page to play from (the embed hop). */
const MediaDeclaredVideo *media_declared_video_or_embed_cached(
    const PocDocument *document);
size_t media_declared_video_discovery_count(const PocDocument *document);
/* A script mutated the connected tree: the next lookup rescans, keeping
   the last found declaration as its fallback. */
void media_declared_video_cache_invalidate(PocDocument *document);
/* Takes the kept declaration from the parsed markup before page scripts
   can remove it: the parser calls this once the head is complete (or at
   end of input, or before scripts on a non-streamed document). Only the
   first call per document does anything, and it scans only when the
   markup so far has declaration-shaped content. No lookup is cached. */
void media_declared_video_keep_parsed(PocDocument *document);
/* The parser closed a script element: when nothing is kept yet and it is a
   data script naming a VideoObject (one that arrives in the body, after
   the head-close keep found nothing), keep the declarations now, before a
   later script can remove it. Costs nothing for any other script; a few
   such scripts at most rescan the document. */
void media_declared_video_keep_closed(PocDocument *document,
                                      lxb_dom_node_t *script);
/* Forgets the selection and the kept declaration: a new document, or a
   same-document URL change (history.pushState/replaceState to another
   URL, a fragment navigation, a history traversal). */
void media_declared_video_cache_destroy(PocDocument *document);
/* Many players fetch their stream addresses as JSON
   (`[{"quality":"240","videoUrl":"https://cdn/240.mp4"}]`,
   `{"data":"https://.../index.m3u8?token=..."}`) or fetch the playlist
   itself. Each successful fetch/XHR response the page could read is
   offered here as it is delivered: a 2xx body that opens a JSON object or
   array mentioning mp4/m3u8/mpegurl (the first MEDIA_JSON_RESPONSE_BYTES
   are read), or an #EXTM3U playlist. A content type alone never qualifies.
   Only its best two references and its best stream are kept, as short
   records in the document's ring of the last eight (bodies are never
   stored), charged to its Budget only while ample room stays free. A
   record is the page's own when the response's address carries an ID of
   the page, or the object holding the reference (or the one holding that)
   has a member equal to one; player_page adds the address's last path
   word and its query IDs (see the embed hop in browser_engine.h).
   media_discover_document_candidates reads the ring at activation, and
   media_json_own_candidate at any time. */
#define MEDIA_JSON_RESPONSE_BYTES (256u * 1024u)
void media_json_response_remember(
    PocDocument *document, const char *document_url,
    const char *response_url, long status, const char *body, size_t length,
    bool player_page);
/* Frees the ring and returns the bytes released: at teardown, and as an
   optional cache when the Budget needs room. Later responses are still
   remembered under the same headroom rule. */
size_t media_json_responses_release(PocDocument *document);
/* Changes whenever a reference is remembered, so a caller waiting for one
   reads the ring only when it may have changed. */
uint32_t media_json_response_generation(const PocDocument *document);
/* The ordinal-th best remembered reference that is the page's own and not
   a preview, related video or stand-in clip (raw, unresolved, untrusted),
   ranked by kind and quality. False past the last. */
bool media_json_own_candidate(
    const PocDocument *document, size_t ordinal, char *url, size_t capacity,
    MediaDiscoveryKind *kind);
MediaDiscoveryKind media_discovery_reference_kind(
    const char *value, size_t length);
/* The rendition quality policy shared by page discovery and <source>
   ranking: the lowest quality at or above 240p, then the highest below it;
   0 (unknown) ranks last. True when quality beats current. */
bool media_discovery_quality_preferred(unsigned quality, unsigned current);
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
