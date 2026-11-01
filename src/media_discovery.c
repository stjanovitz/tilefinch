#include "tilefinch/media_discovery.h"

#include "tilefinch/js_runtime.h"
#include "tilefinch/json_scan.h"
#include "tilefinch/media_candidates.h"
#include "tilefinch/navigation.h"
#include "tilefinch/url.h"

#include <ctype.h>
#include <limits.h>
#include <stdint.h>
#include <string.h>
#include <strings.h>

#define MEDIA_DISCOVERY_NODE_LIMIT 16384u
#define MEDIA_DISCOVERY_TEXT_LIMIT (256u * 1024u)
#define MEDIA_DISCOVERY_WINDOW 96u
#define MEDIA_STRUCTURED_JSON_DEPTH_LIMIT 16u

typedef struct MediaDiscoveryBest {
    char *url;
    size_t capacity;
    MediaDiscoveryKind kind;
    unsigned quality;
    /* How strongly the reference belongs to the activated video (see
       media_reference_affinity); always 0 without a context. */
    int affinity;
    bool found;
    /* When set, also keeps the best HLS reference, which the overall
       winner hides whenever the page offers a direct MP4 too. */
    struct MediaDiscoveryBest *streaming;
    /* Activation only: the page's identity and the player's container. */
    const struct MediaDiscoveryContext *context;
} MediaDiscoveryBest;

/* What the activation-time scan knows about the video being played: ID
   tokens of the page's own video (numeric runs and a slug from its address,
   canonical URL and og:url; numeric runs from its poster and og:image) and
   the activated element's player container. Page-load discovery has none,
   so its ranking is the plain kind and quality policy. */
#define MEDIA_IDENTITY_TOKEN_LIMIT 8u
#define MEDIA_IDENTITY_TOKEN_CAPACITY 48u
#define MEDIA_IDENTITY_HEAD_NODE_LIMIT 512u

typedef struct MediaDiscoveryContext {
    char tokens[MEDIA_IDENTITY_TOKEN_LIMIT][MEDIA_IDENTITY_TOKEN_CAPACITY];
    uint8_t token_lengths[MEDIA_IDENTITY_TOKEN_LIMIT];
    bool token_numeric[MEDIA_IDENTITY_TOKEN_LIMIT];
    /* A player page's short address token (see media_identity_add_address)
       matches only as a whole word. */
    bool token_delimited[MEDIA_IDENTITY_TOKEN_LIMIT];
    size_t token_count;
    lxb_dom_node_t *container;
} MediaDiscoveryContext;

/* Where a reference sits: inside the activated player's container, or in
   an element, key or object marked as a preview or a related video. */
typedef struct {
    bool near;
    bool marked;
    /* The data around a literal names the page's own video: an ID token
       sits in the object holding it or the one holding that. */
    bool own;
} MediaReferenceSite;

static bool media_name_is(lxb_dom_node_t *node, const char *wanted)
{
    size_t length = 0;
    const char *name = document_element_name(node, &length);
    return name != NULL && strlen(wanted) == length
        && memcmp(name, wanted, length) == 0;
}

MediaDiscoveryKind media_discovery_reference_kind(
    const char *value, size_t length)
{
    for (size_t i = 0; i < length; i++) {
        if (value[i] != '.') continue;
        size_t remaining = length - i;
        MediaDiscoveryKind kind = MEDIA_DISCOVERY_NONE;
        size_t suffix = 0;
        if (remaining >= 4u && strncasecmp(value + i, ".mp4", 4u) == 0) {
            kind = MEDIA_DISCOVERY_MP4;
            suffix = 4u;
        } else if (remaining >= 4u
                   && strncasecmp(value + i, ".m4a", 4u) == 0) {
            kind = MEDIA_DISCOVERY_AUDIO_MP4;
            suffix = 4u;
        } else if (remaining >= 5u
                   && strncasecmp(value + i, ".m3u8", 5u) == 0) {
            kind = MEDIA_DISCOVERY_HLS;
            suffix = 5u;
        } else if (remaining >= 5u
                   && strncasecmp(value + i, ".webm", 5u) == 0) {
            kind = MEDIA_DISCOVERY_WEBM;
            suffix = 5u;
        }
        if (kind == MEDIA_DISCOVERY_NONE) continue;
        if (i + suffix == length || value[i + suffix] == '?'
            || value[i + suffix] == '#' || value[i + suffix] == '&') {
            return kind;
        }
    }
    return MEDIA_DISCOVERY_NONE;
}

static unsigned media_quality_hint(
    const char *source, size_t length, size_t begin, size_t end)
{
    size_t from = begin > MEDIA_DISCOVERY_WINDOW
        ? begin - MEDIA_DISCOVERY_WINDOW : 0;
    size_t to = length - end > MEDIA_DISCOVERY_WINDOW
        ? end + MEDIA_DISCOVERY_WINDOW : length;
    unsigned best = 0;
    size_t best_distance = SIZE_MAX;
    for (size_t i = from; i < to;) {
        if (!isdigit((unsigned char) source[i])) {
            i++;
            continue;
        }
        size_t number_begin = i;
        unsigned value = 0;
        size_t digits = 0;
        /* Read the whole digit run: a longer run is an ID, a timestamp or a
           size, and a 3-4 digit piece of it is not a quality. */
        while (i < to && isdigit((unsigned char) source[i])) {
            if (digits < 5u)
                value = value * 10u + (unsigned) (source[i] - '0');
            digits++;
            i++;
        }
        /* A run the window cuts is longer than it looks. */
        bool cut = (number_begin == from && from != 0u
                    && isdigit((unsigned char) source[from - 1u]))
            || (i == to && to < length
                && isdigit((unsigned char) source[to]));
        /* A year segment of the reference's own path ("/2026/10/") is a
           date, not a quality. */
        bool year = digits == 4u && value >= 1900u && value <= 2099u
            && number_begin > begin && i < end
            && source[number_begin - 1u] == '/'
            && (source[i] == '/' || source[i] == '\\');
        if (!cut && !year && digits >= 3u && digits <= 4u
            && value >= 144u && value <= 4320u) {
            size_t distance = number_begin < begin ? begin - number_begin
                : number_begin > end ? number_begin - end : 0;
            if (best == 0u || distance < best_distance) {
                best = value;
                best_distance = distance;
            }
        }
    }
    return best;
}

bool media_discovery_quality_preferred(unsigned quality, unsigned current)
{
    if (quality == current) return false;
    if (quality >= 240u) {
        return current < 240u || quality < current;
    }
    return current < 240u && quality > current;
}

/* A direct file hinted at this quality or above (720p, or a 1280/1920
   width beside it) is past what the PSP decodes; an HLS master may still
   offer a smaller rendition. */
#define MEDIA_DISCOVERY_UNDECODABLE_QUALITY 720u

/* Kind order: a direct MP4 the PSP can decode, then HLS, then a direct
   MP4 it cannot, then WebM. */
static unsigned media_kind_order(MediaDiscoveryKind kind, unsigned quality)
{
    switch (kind) {
    case MEDIA_DISCOVERY_MP4:
        return quality >= MEDIA_DISCOVERY_UNDECODABLE_QUALITY ? 3u : 1u;
    case MEDIA_DISCOVERY_HLS: return 2u;
    case MEDIA_DISCOVERY_WEBM: return 4u;
    default: return 5u + (unsigned) kind;
    }
}

/* Affinity outranks kind and quality: the page's own video in any
   playable form beats a related video's preview clip, and an earlier or a
   larger reference never beats it. */
static bool media_rank_better(
    bool found, int current_affinity, MediaDiscoveryKind current_kind,
    unsigned current_quality, int affinity, MediaDiscoveryKind kind,
    unsigned quality)
{
    if (!found) return true;
    if (affinity != current_affinity) return affinity > current_affinity;
    unsigned order = media_kind_order(kind, quality);
    unsigned current_order = media_kind_order(current_kind, current_quality);
    if (order != current_order) return order < current_order;
    return media_discovery_quality_preferred(quality, current_quality);
}

static bool media_candidate_better(
    const MediaDiscoveryBest *best, int affinity, MediaDiscoveryKind kind,
    unsigned quality)
{
    return media_rank_better(
        best->found, best->affinity, best->kind, best->quality,
        affinity, kind, quality);
}

/* Words that mark a key, class, id or path as a preview, thumbnail or
   related-video context. Separators are ignored, so "up-next",
   "js-videoPreview" and "next_video" all match. */
static const char *const media_preview_markers[] = {
    "preview", "thumb", "teaser", "trailer", "sprite", "hover", "related",
    "recommend", "suggest", "upnext", "nextvideo", "carousel"
};

#define MEDIA_MARKER_TEXT_LIMIT 512u

static bool media_marker_at(
    const char *text, size_t length, size_t at, const char *marker)
{
    for (const char *wanted = marker; *wanted != '\0'; wanted++) {
        while (wanted != marker && at < length
               && (text[at] == '-' || text[at] == '_')) at++;
        if (at >= length
            || tolower((unsigned char) text[at]) != (unsigned char) *wanted)
            return false;
        at++;
    }
    return true;
}

static bool media_text_marked(const char *text, size_t length)
{
    if (text == NULL) return false;
    if (length > MEDIA_MARKER_TEXT_LIMIT) length = MEDIA_MARKER_TEXT_LIMIT;
    for (size_t at = 0; at < length; at++) {
        int first = tolower((unsigned char) text[at]);
        if (first < 'a' || first > 'z') continue;
        for (size_t i = 0;
             i < sizeof(media_preview_markers)
                     / sizeof(media_preview_markers[0]); i++) {
            if (media_preview_markers[i][0] == first
                && media_marker_at(text, length, at,
                                   media_preview_markers[i])) return true;
        }
    }
    return false;
}

/* Whole words that mark a key or a path as an advertisement, a promotion
   or an app download: a reference there is never the page's video, and it
   is dropped rather than ranked, since offering nothing beats offering the
   wrong video. "download" alone is not one: a download link may well be
   the page's own file. */
static const char *const media_excluded_words[] = {
    "ad", "ads", "advert", "adverts", "advertisement", "advertising",
    "promo", "promos", "promotion", "promotional", "promoted",
    "sponsor", "sponsored", "banner", "banners", "apk",
    "preroll", "midroll", "postroll"
};

/* Words of a key that name a sound rather than a picture: an extensionless
   address under such a key is not taken for a video. */
static const char *const media_audio_words[] = {
    "music", "audio", "sound", "song", "songs", "podcast", "voice"
};

/* Words of a key naming the playing video, and words naming an address;
   a key with one of each (playAddr, playUrl, videoUrl, streamSrc,
   contentUrl) is strong enough to take an address without a media suffix. */
static const char *const media_video_key_words[] = {
    "play", "playback", "video", "stream", "content", "media", "mp4", "hls"
};
static const char *const media_locator_words[] = {
    "url", "addr", "address", "src", "uri"
};

#define MEDIA_WORD_LIST(list) (list), (sizeof(list) / sizeof((list)[0]))

/* Whether text holds one of the words, case-insensitively. Words split at
   anything not alphanumeric; with camel, also at a lower-to-upper change
   ("playAddr") and where a capital run meets a capitalised word
   ("HDUrl"). Paths are read without camel splitting, so a hash such as
   "a9AdF0" never yields a word. */
static bool media_text_has_word(
    const char *text, size_t length, bool camel,
    const char *const *words, size_t count)
{
    if (text == NULL) return false;
    if (length > MEDIA_MARKER_TEXT_LIMIT) length = MEDIA_MARKER_TEXT_LIMIT;
    size_t start = 0;
    for (size_t i = 0; i <= length; i++) {
        bool separator = i == length || !isalnum((unsigned char) text[i]);
        bool boundary = false;
        if (camel && !separator && i > start) {
            unsigned char before = (unsigned char) text[i - 1u];
            unsigned char at = (unsigned char) text[i];
            boundary = (islower(before) && isupper(at))
                || (isupper(before) && isupper(at) && i + 1u < length
                    && islower((unsigned char) text[i + 1u]));
        }
        if (!separator && !boundary) continue;
        size_t word = i - start;
        for (size_t w = 0; word != 0u && w < count; w++) {
            if (strlen(words[w]) == word
                && strncasecmp(text + start, words[w], word) == 0)
                return true;
        }
        start = separator ? i + 1u : i;
    }
    return false;
}

static bool media_key_text_excluded(const char *key, size_t length)
{
    return media_text_has_word(
        key, length, true, MEDIA_WORD_LIST(media_excluded_words));
}

/* The length of an escaped character at `at` written as a JSON \u
   escape for `wanted` (lower-case hex), or 0. */
static size_t media_unicode_escape_at(
    const char *raw, size_t length, size_t at, const char *wanted)
{
    return length - at >= 6u && raw[at] == '\\' && raw[at + 1u] == 'u'
        && strncasecmp(raw + at + 2u, wanted, 4u) == 0 ? 6u : 0u;
}

/* The path of a raw reference (slashes may be escaped as "\/" or
   "/"): after any scheme and host, before the query or fragment. */
static void media_reference_path(
    const char *raw, size_t length, size_t *begin, size_t *end)
{
    size_t at = 0;
    if (length > 6u && strncasecmp(raw, "https:", 6u) == 0) at = 6u;
    else if (length > 5u && strncasecmp(raw, "http:", 5u) == 0) at = 5u;
    /* Skip "//host" (or "\/\/host", "//host") when present. */
    size_t slashes = 0, probe = at;
    while (probe < length && slashes < 2u) {
        size_t escape = media_unicode_escape_at(raw, length, probe, "002f");
        if (escape != 0u) {
            slashes++;
            probe += escape;
        } else if (raw[probe] == '/' || raw[probe] == '\\') {
            if (raw[probe] == '/') slashes++;
            probe++;
        } else break;
    }
    if (slashes == 2u) {
        at = probe;
        while (at < length && raw[at] != '/' && raw[at] != '\\'
               && raw[at] != '?' && raw[at] != '#') at++;
    }
    *begin = at;
    while (at < length && raw[at] != '?' && raw[at] != '#'
           && media_unicode_escape_at(raw, length, at, "003f") == 0u) at++;
    *end = at;
}

/* A path names an ad, a promotion or an app download by a whole word of
   one of its segments; an escaped character ("/") separates words. */
static bool media_path_excluded(const char *raw, size_t length)
{
    size_t begin = 0, end = 0;
    media_reference_path(raw, length, &begin, &end);
    size_t start = begin;
    for (size_t at = begin; at <= end; at++) {
        size_t escape = at < end
            ? media_unicode_escape_at(raw, end, at, "002f") : 0u;
        if (at < end && escape == 0u && raw[at] != '/' && raw[at] != '\\')
            continue;
        if (at > start && media_text_has_word(
                raw + start, at - start, false,
                MEDIA_WORD_LIST(media_excluded_words))) return true;
        if (escape != 0u) at += escape - 1u;
        start = at + 1u;
    }
    return false;
}

/* Small frame sizes such as 160x90 or 320x180 name thumbnail strips and
   hover previews, never a page's main rendition. */
static bool media_path_has_thumbnail_size(const char *path, size_t length)
{
    for (size_t i = 0; i < length; i++) {
        if (!isdigit((unsigned char) path[i])
            || (i != 0u && isdigit((unsigned char) path[i - 1u]))) continue;
        unsigned width = 0;
        size_t at = i, digits = 0;
        while (at < length && isdigit((unsigned char) path[at])
               && digits < 4u) {
            width = width * 10u + (unsigned) (path[at++] - '0');
            digits++;
        }
        if (digits < 2u || digits > 3u || at + 2u >= length
            || (path[at] != 'x' && path[at] != 'X')) continue;
        size_t height_digits = 0;
        for (size_t h = at + 1u; h < length
             && isdigit((unsigned char) path[h]); h++) height_digits++;
        if (height_digits >= 2u && height_digits <= 3u && width <= 320u)
            return true;
    }
    return false;
}

static bool media_digit_run_is_date(const char *run, size_t length)
{
    if (length != 6u && length != 8u) return false;
    if (!(run[0] == '1' && run[1] == '9') && !(run[0] == '2' && run[1] == '0'))
        return false;
    unsigned month = (unsigned) (run[4] - '0') * 10u
        + (unsigned) (run[5] - '0');
    return month >= 1u && month <= 12u;
}

static void media_identity_add(
    MediaDiscoveryContext *context, const char *token, size_t length,
    bool numeric, bool delimited)
{
    if (length == 0u || length >= MEDIA_IDENTITY_TOKEN_CAPACITY
        || context->token_count >= MEDIA_IDENTITY_TOKEN_LIMIT) return;
    for (size_t i = 0; i < context->token_count; i++) {
        if (context->token_lengths[i] == length
            && strncasecmp(context->tokens[i], token, length) == 0) return;
    }
    size_t at = context->token_count++;
    memcpy(context->tokens[at], token, length);
    context->tokens[at][length] = '\0';
    context->token_lengths[at] = (uint8_t) length;
    context->token_numeric[at] = numeric;
    context->token_delimited[at] = delimited;
}

/* ID tokens of an address naming the page's own video: digit runs of 5 or
   more (not date-shaped such as 202403), and with slugs, a last path
   segment of 8 or more characters joined by '-' or '_'. */
static void media_identity_add_url(
    MediaDiscoveryContext *context, const char *url, size_t length,
    bool slugs)
{
    if (url == NULL || length == 0u) return;
    size_t begin = 0, end = 0;
    media_reference_path(url, length, &begin, &end);
    for (size_t i = begin; i < end;) {
        if (!isdigit((unsigned char) url[i])) {
            i++;
            continue;
        }
        size_t run = i;
        while (i < end && isdigit((unsigned char) url[i])) i++;
        if (i - run >= 5u && !media_digit_run_is_date(url + run, i - run))
            media_identity_add(context, url + run, i - run, true, false);
    }
    if (!slugs) return;
    while (end > begin && url[end - 1u] == '/') end--;
    size_t segment = end;
    while (segment > begin && url[segment - 1u] != '/') segment--;
    size_t slug_end = end;
    for (size_t i = segment; i < end; i++) {
        if (url[i] == '.') {
            slug_end = i;
            break;
        }
    }
    bool joined = false, letter = false;
    for (size_t i = segment; i < slug_end; i++) {
        char value = url[i];
        if (value == '-' || value == '_') joined = true;
        else if (isalpha((unsigned char) value)) letter = true;
        else if (!isdigit((unsigned char) value)) return;
    }
    if (joined && letter && slug_end - segment >= 8u)
        media_identity_add(context, url + segment, slug_end - segment, false,
                           false);
}

/* Path segments that name a player rather than what it plays. */
static const char *const media_player_words[] = {
    "embed", "player", "players", "play", "index", "iframe", "video",
    "videos", "watch", "live", "default", "main"
};

static bool media_identity_word_shaped(const char *value, size_t length)
{
    if (length < 3u || length >= MEDIA_IDENTITY_TOKEN_CAPACITY) return false;
    for (size_t i = 0; i < length; i++) {
        if (!isalnum((unsigned char) value[i]) && value[i] != '-'
            && value[i] != '_') return false;
    }
    return true;
}

/* A player page (the page a site declares as its video's player, opened
   to play it) names what it plays in its own address, often in a short
   word the page-address tokens above miss: the last path segment ("/xqc",
   "/9ae8e8a6dc58b...") unless it names a player, and query values of five
   or more characters holding a digit ("?videoId=6407513159112"). These
   match only as whole words. */
static void media_identity_add_address(
    MediaDiscoveryContext *context, const char *url)
{
    size_t length = url == NULL ? 0u : strlen(url);
    if (length == 0u) return;
    size_t begin = 0, end = 0;
    media_reference_path(url, length, &begin, &end);
    size_t segment_end = end;
    while (segment_end > begin && url[segment_end - 1u] == '/') segment_end--;
    size_t segment = segment_end;
    while (segment > begin && url[segment - 1u] != '/') segment--;
    size_t word_end = segment;
    while (word_end < segment_end && url[word_end] != '.') word_end++;
    bool player_word = false;
    for (size_t i = 0;
         i < sizeof(media_player_words) / sizeof(media_player_words[0]); i++)
        player_word = player_word
            || (strlen(media_player_words[i]) == word_end - segment
                && strncasecmp(url + segment, media_player_words[i],
                               word_end - segment) == 0);
    if (!player_word
        && media_identity_word_shaped(url + segment, word_end - segment))
        media_identity_add(context, url + segment, word_end - segment,
                           false, true);
    if (end >= length || url[end] != '?') return;
    for (size_t at = end + 1u; at < length && url[at] != '#';) {
        size_t value = at;
        while (value < length && url[value] != '=' && url[value] != '&'
               && url[value] != '#') value++;
        if (value >= length || url[value] != '=') {
            at = value + 1u;
            continue;
        }
        size_t value_end = ++value;
        bool digit = false;
        while (value_end < length && url[value_end] != '&'
               && url[value_end] != '#') {
            digit = digit || isdigit((unsigned char) url[value_end]);
            value_end++;
        }
        if (digit && value_end - value >= 5u
            && media_identity_word_shaped(url + value, value_end - value))
            media_identity_add(context, url + value, value_end - value,
                               false, true);
        at = value_end + 1u;
    }
}

static bool media_metadata_name_is(
    lxb_dom_node_t *node, const char *wanted);
static bool media_name_is(lxb_dom_node_t *node, const char *wanted);
static lxb_dom_node_t *media_walk_next(
    lxb_dom_node_t *root, lxb_dom_node_t *node);

/* The page-lifetime declaration record (see media_declared_video_cached):
   the last declaration a scan found, whether it names media (found) or
   only a player page, and the head's identity tokens read with it. */
struct MediaDeclaredVideoCache {
    MediaDeclaredVideo selected;
    bool found;
    uint8_t identity_count;
    bool identity_numeric[MEDIA_IDENTITY_TOKEN_LIMIT];
    bool identity_delimited[MEDIA_IDENTITY_TOKEN_LIMIT];
    uint8_t identity_lengths[MEDIA_IDENTITY_TOKEN_LIMIT];
    char identity[MEDIA_IDENTITY_TOKEN_LIMIT][MEDIA_IDENTITY_TOKEN_CAPACITY];
};

_Static_assert(sizeof(MediaDeclaredVideoCache)
                   <= sizeof(MediaDeclaredVideo) + 512u,
               "kept declaration record must remain PSP-small");

static void media_context_add_head(
    MediaDiscoveryContext *context, const PocDocument *document);

static void media_context_add_kept_identity(
    MediaDiscoveryContext *context, const PocDocument *document)
{
    const MediaDeclaredVideoCache *kept = document->declared_video_cache;
    if (kept == NULL) return;
    for (size_t i = 0; i < kept->identity_count; i++)
        media_identity_add(context, kept->identity[i],
                           kept->identity_lengths[i],
                           kept->identity_numeric[i],
                           kept->identity_delimited[i]);
}

static void media_context_init(
    MediaDiscoveryContext *context, const PocDocument *document,
    const char *page_url, lxb_dom_node_t *anchor)
{
    memset(context, 0, sizeof(*context));
    if (page_url != NULL)
        media_identity_add_url(context, page_url, strlen(page_url), true);
    if (anchor != NULL) {
        size_t length = 0;
        const char *poster = document_attribute(anchor, "poster", &length);
        media_identity_add_url(context, poster, poster == NULL ? 0 : length,
                               false);
        /* The player container: the element's parent and grandparent,
           never the page body itself. */
        lxb_dom_node_t *container = anchor;
        for (unsigned level = 0; level < 2u; level++) {
            lxb_dom_node_t *parent = container->parent;
            if (parent == NULL || parent->type != LXB_DOM_NODE_TYPE_ELEMENT
                || media_name_is(parent, "body")
                || media_name_is(parent, "html")) break;
            container = parent;
        }
        context->container = container;
    }
    size_t before = context->token_count;
    media_context_add_head(context, document);
    /* A client script removed the head's declarations: the identity read
       with the kept declaration still names the page's video. */
    if (context->token_count == before)
        media_context_add_kept_identity(context, document);
}

static void media_context_add_head(
    MediaDiscoveryContext *context, const PocDocument *document)
{
    lxb_html_head_element_t *head = document->html->head;
    lxb_dom_node_t *root = head == NULL ? NULL : lxb_dom_interface_node(head);
    size_t nodes = 0;
    for (lxb_dom_node_t *node = root;
         node != NULL && nodes++ < MEDIA_IDENTITY_HEAD_NODE_LIMIT;
         node = media_walk_next(root, node)) {
        if (node->type != LXB_DOM_NODE_TYPE_ELEMENT) continue;
        size_t length = 0;
        const char *value = NULL;
        bool slugs = true;
        if (media_name_is(node, "link")) {
            const char *rel = document_attribute(node, "rel", &length);
            if (rel == NULL || length != sizeof("canonical") - 1u
                || strncasecmp(rel, "canonical", length) != 0) continue;
            value = document_attribute(node, "href", &length);
        } else if (media_name_is(node, "meta")) {
            if (media_metadata_name_is(node, "og:url")) {
                value = document_attribute(node, "content", &length);
            } else {
                value = document_meta_image_content(node, &length);
                slugs = false;
            }
        }
        if (value != NULL)
            media_identity_add_url(context, value, length, slugs);
    }
}

static bool media_reference_matches_identity(
    const MediaDiscoveryContext *context, const char *raw, size_t length)
{
    for (size_t t = 0; t < context->token_count; t++) {
        const char *token = context->tokens[t];
        size_t token_length = context->token_lengths[t];
        if (token_length > length) continue;
        for (size_t i = 0; i + token_length <= length; i++) {
            if (context->token_numeric[t]) {
                if (raw[i] != token[0]
                    || (i != 0u && isdigit((unsigned char) raw[i - 1u]))
                    || memcmp(raw + i, token, token_length) != 0
                    || (i + token_length < length
                        && isdigit((unsigned char) raw[i + token_length])))
                    continue;
                return true;
            }
            if (strncasecmp(raw + i, token, token_length) != 0) continue;
            if (context->token_delimited[t]
                && ((i != 0u && isalnum((unsigned char) raw[i - 1u]))
                    || (i + token_length < length
                        && isalnum((unsigned char) raw[i + token_length]))))
                continue;
            return true;
        }
    }
    return false;
}

/* The page's own video ranks first (+2), then references in the player's
   container (+1); preview, thumbnail and related-video references rank
   last (-2). Nothing is dropped: a page that offers only previews still
   plays one. */
#define MEDIA_AFFINITY_OWN 2
#define MEDIA_AFFINITY_DECLARED 3

static int media_reference_affinity(
    const MediaDiscoveryContext *context, const char *raw, size_t length,
    MediaReferenceSite site)
{
    if (context == NULL) return 0;
    int affinity = 0;
    if (site.own || media_reference_matches_identity(context, raw, length))
        affinity += MEDIA_AFFINITY_OWN;
    if (site.near) affinity += 1;
    size_t path_begin = 0, path_end = 0;
    media_reference_path(raw, length, &path_begin, &path_end);
    if (site.marked
        || media_text_marked(raw + path_begin, path_end - path_begin)
        || media_path_has_thumbnail_size(
               raw + path_begin, path_end - path_begin))
        affinity -= 2;
    return affinity;
}

static bool media_node_near(
    const MediaDiscoveryContext *context, lxb_dom_node_t *node)
{
    if (context == NULL || context->container == NULL) return false;
    unsigned levels = 0;
    for (; node != NULL && levels < 24u; node = node->parent, levels++)
        if (node == context->container) return true;
    return false;
}

static bool media_element_marked(lxb_dom_node_t *node)
{
    static const char *const names[] = {"class", "id"};
    /* The element and three ancestors, short of the page-wide body. */
    for (unsigned level = 0;
         node != NULL && node->type == LXB_DOM_NODE_TYPE_ELEMENT
             && level < 4u && !media_name_is(node, "body")
             && !media_name_is(node, "html");
         node = node->parent, level++) {
        for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
            size_t length = 0;
            const char *value = document_attribute(node, names[i], &length);
            if (value != NULL && media_text_marked(value, length))
                return true;
        }
    }
    return false;
}

/* A reference held by an element: in the player's container, or marked by
   the element's (or a close ancestor's) class or id. */
static MediaReferenceSite media_element_site(
    const MediaDiscoveryContext *context, lxb_dom_node_t *node)
{
    MediaReferenceSite site = {0};
    if (context == NULL) return site;
    site.near = media_node_near(context, node);
    if (!site.near) site.marked = media_element_marked(node);
    return site;
}

#define MEDIA_SCRIPT_KEY_LIMIT 48u

/* The key a literal or an opening bracket at `at` belongs to: the property
   key before `:`, the callee before `(` or the assignee before `=`. */
static bool media_key_span(
    const char *source, size_t at, size_t *begin, size_t *end)
{
    while (at != 0u && isspace((unsigned char) source[at - 1u])) at--;
    if (at == 0u) return false;
    char separator = source[at - 1u];
    if (separator != ':' && separator != '(' && separator != '=') return false;
    if (separator == '=' && at >= 2u
        && (source[at - 2u] == '=' || source[at - 2u] == '!')) return false;
    at--;
    while (at != 0u && isspace((unsigned char) source[at - 1u])) at--;
    size_t key_end = at, key_begin = at;
    if (separator == ':' && at != 0u
        && (source[at - 1u] == '"' || source[at - 1u] == '\'')) {
        char key_quote = source[at - 1u];
        key_end = at - 1u;
        key_begin = key_end;
        while (key_begin != 0u && key_end - key_begin < MEDIA_SCRIPT_KEY_LIMIT
               && source[key_begin - 1u] != key_quote) key_begin--;
    } else {
        while (key_begin != 0u && key_end - key_begin < MEDIA_SCRIPT_KEY_LIMIT
               && (isalnum((unsigned char) source[key_begin - 1u])
                   || source[key_begin - 1u] == '_'
                   || source[key_begin - 1u] == '$'))
            key_begin--;
    }
    *begin = key_begin;
    *end = key_end;
    return key_end > key_begin;
}

static bool media_key_marked(const char *source, size_t at)
{
    size_t begin = 0, end = 0;
    return media_key_span(source, at, &begin, &end)
        && media_text_marked(source + begin, end - begin);
}

/* Whether the object, array or call a literal sits in was opened under a
   marked key, e.g. `nextVideo: {...}` or `related: [{...}]`. Opening a
   level only records where it opened; the keys are read when a media
   reference asks, which is rare next to the brackets of a large blob.
   Levels deeper than the tracked depth inherit the deepest tracked one. */
#define MEDIA_KEY_CHAIN_DEPTH 32u

typedef struct {
    uint32_t opener[MEDIA_KEY_CHAIN_DEPTH];
    uint32_t marked;
    /* Levels opened under an ad, promotion or app-download key. */
    uint32_t excluded;
    /* Activation only (see media_chain_note_value): levels whose object
       has so far directly held a value naming the page's own video, and
       levels whose object has so far directly held an ID of its own. */
    uint32_t item_own;
    uint32_t item_named;
    unsigned depth;
    unsigned evaluated;
} MediaKeyChain;

static void media_chain_reset(MediaKeyChain *chain)
{
    chain->marked = 0u;
    chain->excluded = 0u;
    chain->item_own = 0u;
    chain->item_named = 0u;
    chain->depth = 0u;
    chain->evaluated = 0u;
}

static void media_chain_open(MediaKeyChain *chain, size_t at)
{
    if (chain->depth < MEDIA_KEY_CHAIN_DEPTH) {
        uint32_t bit = (uint32_t) 1u << chain->depth;
        chain->opener[chain->depth] = (uint32_t) at;
        chain->item_own &= ~bit;
        chain->item_named &= ~bit;
    }
    if (chain->depth != UINT_MAX) chain->depth++;
}

static void media_chain_close(MediaKeyChain *chain)
{
    if (chain->depth != 0u) chain->depth--;
    if (chain->evaluated > chain->depth) chain->evaluated = chain->depth;
}

/* The key a level was opened under; a call's key is its callee, just
   before the parenthesis. */
static bool media_chain_key(
    const MediaKeyChain *chain, const char *source, unsigned level,
    size_t *begin, size_t *end)
{
    size_t at = chain->opener[level];
    return media_key_span(
        source, source[at] == '(' ? at + 1u : at, begin, end);
}

static unsigned media_chain_evaluate(MediaKeyChain *chain, const char *source)
{
    unsigned levels = chain->depth < MEDIA_KEY_CHAIN_DEPTH
        ? chain->depth : MEDIA_KEY_CHAIN_DEPTH;
    for (unsigned level = chain->evaluated; level < levels; level++) {
        bool marked = level != 0u
            && ((chain->marked >> (level - 1u)) & 1u) != 0u;
        bool excluded = level != 0u
            && ((chain->excluded >> (level - 1u)) & 1u) != 0u;
        size_t begin = 0, end = 0;
        if ((!marked || !excluded)
            && media_chain_key(chain, source, level, &begin, &end)) {
            if (!marked)
                marked = media_text_marked(source + begin, end - begin);
            if (!excluded)
                excluded = media_key_text_excluded(
                    source + begin, end - begin);
        }
        uint32_t bit = (uint32_t) 1u << level;
        chain->marked = marked ? chain->marked | bit : chain->marked & ~bit;
        chain->excluded = excluded
            ? chain->excluded | bit : chain->excluded & ~bit;
    }
    if (chain->evaluated < levels) chain->evaluated = levels;
    return levels;
}

static bool media_chain_marked(MediaKeyChain *chain, const char *source)
{
    unsigned levels = media_chain_evaluate(chain, source);
    return levels != 0u && ((chain->marked >> (levels - 1u)) & 1u) != 0u;
}

static bool media_chain_excluded(MediaKeyChain *chain, const char *source)
{
    unsigned levels = media_chain_evaluate(chain, source);
    return levels != 0u && ((chain->excluded >> (levels - 1u)) & 1u) != 0u;
}

/* Whether the literal whose opening quote is at `quote` is under an ad,
   promotion or app-download key, its own or an enclosing one. */
static bool media_literal_key_excluded(
    const char *source, size_t quote, MediaKeyChain *chain)
{
    size_t begin = 0, end = 0;
    if (media_key_span(source, quote, &begin, &end)
        && media_key_text_excluded(source + begin, end - begin)) return true;
    return chain != NULL && media_chain_excluded(chain, source);
}

static size_t media_decode_json_string(
    char *output, size_t capacity, const char *source,
    size_t begin, size_t end)
{
    if (source == NULL || begin >= end
        || (output != NULL && capacity == 0u)) return 0;
    size_t written = 0;
    for (size_t i = begin; i < end; i++) {
        unsigned char value = (unsigned char) source[i];
        if (value == '\\' && i + 1u < end) {
            unsigned char escaped = (unsigned char) source[++i];
            if (escaped == '/' || escaped == '\\' || escaped == '"') {
                value = escaped;
            } else if (escaped == 'u' && end - i >= 5u
                       && source[i + 1u] == '0'
                       && source[i + 2u] == '0') {
                const char *hex = source + i + 3u;
                if (strncasecmp(hex, "2f", 2u) == 0) value = '/';
                else if (strncasecmp(hex, "26", 2u) == 0) value = '&';
                else if (strncasecmp(hex, "3d", 2u) == 0) value = '=';
                else if (strncasecmp(hex, "3f", 2u) == 0) value = '?';
                else return 0;
                i += 4u;
            } else return 0;
        }
        if (value < 0x20u || written == SIZE_MAX) return 0;
        if (output != NULL) {
            if (written + 1u >= capacity) return 0;
            output[written] = (char) value;
        }
        written++;
    }
    if (output != NULL) output[written] = '\0';
    return written;
}

static size_t media_copy_json_string(
    char *output, size_t capacity, const char *source,
    size_t begin, size_t end)
{
    if (output == NULL || capacity == 0u) return 0u;
    /* Validate and size without touching the committed winner. A malformed
       better-ranked candidate must never splice its prefix onto the URL and
       metadata retained for the previous winner. */
    size_t needed = media_decode_json_string(
        NULL, 0u, source, begin, end);
    if (needed == 0u || needed >= capacity) return 0u;
    return media_decode_json_string(
        output, capacity, source, begin, end);
}

/* How far back from a literal, at most, the data around it is searched
   for the page's own ID (see media_chain_near_identity). */
#define MEDIA_IDENTITY_PROXIMITY_WINDOW (8u * 1024u)

/* Whether an ID token of the page's own video sits before the literal at
   `begin` in the object (or array) holding it or the one holding that,
   e.g. {"id":"6718335390845095173",...,"video":{"playAddr":"..."}}. Two
   levels keep a list's earlier items, each with its own ID, out of reach. */
static bool media_chain_near_identity(
    const MediaKeyChain *chain, const MediaDiscoveryContext *context,
    const char *source, size_t begin)
{
    if (context == NULL || context->token_count == 0u || chain == NULL
        || chain->depth == 0u || chain->depth > MEDIA_KEY_CHAIN_DEPTH)
        return false;
    size_t from = chain->opener[chain->depth >= 2u ? chain->depth - 2u : 0u];
    if (from >= begin) return false;
    if (begin - from > MEDIA_IDENTITY_PROXIMITY_WINDOW)
        from = begin - MEDIA_IDENTITY_PROXIMITY_WINDOW;
    return media_reference_matches_identity(
        context, source + from, begin - from);
}

/* Keys naming the ID of the item an object describes: "id", "guid",
   "uuid" (also "@id", "_id"), or an ID word joined to a video word
   ("videoId", "media_guid", "contentId"). Other IDs ("accountId",
   "playlistId") say nothing about which video the object is. */
static bool media_key_names_item(const char *key, size_t length)
{
    static const char *const id_words[] = {"id", "guid", "uuid"};
    while (length != 0u && (*key == '@' || *key == '_')) {
        key++;
        length--;
    }
    for (size_t i = 0; i < sizeof(id_words) / sizeof(id_words[0]); i++) {
        if (strlen(id_words[i]) == length
            && strncasecmp(key, id_words[i], length) == 0) return true;
    }
    return media_text_has_word(key, length, true, MEDIA_WORD_LIST(id_words))
        && media_text_has_word(key, length, true,
                               MEDIA_WORD_LIST(media_video_key_words));
}

/* Activation only: records what the object holding a keyed scalar value
   (a string whose opening quote, or a number whose first digit, is at
   `token`) says about which video it describes. A value naming the page's
   own video marks the level as the page's item; an item ID naming
   anything else marks it as another item. Nested objects and arrays are
   levels of their own, so a related list's earlier items never mark the
   level of a later one. Longer values (descriptions, markup) are not read:
   an ID or an address is short. */
#define MEDIA_ITEM_VALUE_LIMIT 512u

static void media_chain_note_value(
    MediaKeyChain *chain, const MediaDiscoveryContext *context,
    const char *source, size_t token, size_t begin, size_t end)
{
    if (context == NULL || context->token_count == 0u || chain->depth == 0u
        || chain->depth > MEDIA_KEY_CHAIN_DEPTH || begin >= end
        || end - begin > MEDIA_ITEM_VALUE_LIMIT) return;
    size_t at = token;
    while (at != 0u && isspace((unsigned char) source[at - 1u])) at--;
    if (at == 0u || source[at - 1u] != ':') return;
    size_t key_begin = 0, key_end = 0;
    if (!media_key_span(source, token, &key_begin, &key_end)) return;
    uint32_t bit = (uint32_t) 1u << (chain->depth - 1u);
    if (media_reference_matches_identity(context, source + begin, end - begin))
        chain->item_own |= bit;
    else if (media_key_names_item(source + key_begin, key_end - key_begin))
        chain->item_named |= bit;
}

/* How many levels up from a literal the objects holding it are asked
   which video they describe: enough for a rendition list inside a video's
   own object ({"id":..., "assets":[{"url":...}]}). */
#define MEDIA_ITEM_IDENTITY_LEVELS 4u

/* Which video the objects around a literal describe, nearest first: 1 when
   the nearest one that says names the page's own video, -1 when it names
   another one (its own ID, nothing of the page's), 0 when none says. */
static int media_chain_item_identity(const MediaKeyChain *chain)
{
    if (chain == NULL || chain->depth == 0u
        || chain->depth > MEDIA_KEY_CHAIN_DEPTH) return 0;
    for (unsigned n = 0; n < MEDIA_ITEM_IDENTITY_LEVELS && n < chain->depth;
         n++) {
        uint32_t bit = (uint32_t) 1u << (chain->depth - 1u - n);
        if ((chain->item_own & bit) != 0u) return 1;
        if ((chain->item_named & bit) != 0u) return -1;
    }
    return 0;
}

/* Whether the data around a literal names the page's own video: the
   objects holding it say so, or (when none of them names an item) the
   page's ID sits near it (media_chain_near_identity). */
static bool media_chain_own(
    const MediaKeyChain *chain, const MediaDiscoveryContext *context,
    const char *source, size_t begin)
{
    int item = media_chain_item_identity(chain);
    return item > 0
        || (item == 0
            && media_chain_near_identity(chain, context, source, begin));
}

static MediaDiscoveryKind media_rendition_format(
    const char *value, size_t length);
static bool media_script_reference_shaped(const char *raw, size_t length);

/* An address without a media suffix is taken only at activation, from
   data text, under its own strong video key (see media_video_key_words),
   with no key around it naming a sound, and with no extension in its
   last path segment (so no image or document). Its kind is what the key
   names (hlsUrl), else a direct file; the open path still probes the
   container. The caller also requires the page's own identity. */
static MediaDiscoveryKind media_extensionless_kind(
    const char *source, size_t begin, size_t end, MediaKeyChain *chain)
{
    const char *raw = source + begin;
    size_t raw_length = end - begin;
    if (!media_script_reference_shaped(raw, raw_length)) return
        MEDIA_DISCOVERY_NONE;
    size_t path_begin = 0, path_end = 0;
    media_reference_path(raw, raw_length, &path_begin, &path_end);
    for (size_t at = path_end; at > path_begin; at--) {
        char value = raw[at - 1u];
        if (value == '/' || value == '\\') break;
        if (value == '.') return MEDIA_DISCOVERY_NONE;
    }
    size_t key_begin = 0, key_end = 0;
    if (!media_key_span(source, begin - 1u, &key_begin, &key_end))
        return MEDIA_DISCOVERY_NONE;
    const char *key = source + key_begin;
    size_t key_length = key_end - key_begin;
    if (!media_text_has_word(key, key_length, true,
                             MEDIA_WORD_LIST(media_video_key_words))
        || !media_text_has_word(key, key_length, true,
                                MEDIA_WORD_LIST(media_locator_words))
        || media_text_has_word(key, key_length, true,
                               MEDIA_WORD_LIST(media_audio_words)))
        return MEDIA_DISCOVERY_NONE;
    unsigned levels = chain->depth < MEDIA_KEY_CHAIN_DEPTH
        ? chain->depth : MEDIA_KEY_CHAIN_DEPTH;
    for (unsigned level = 0; level < levels; level++) {
        size_t begin_at = 0, end_at = 0;
        if (media_chain_key(chain, source, level, &begin_at, &end_at)
            && media_text_has_word(
                   source + begin_at, end_at - begin_at, true,
                   MEDIA_WORD_LIST(media_audio_words)))
            return MEDIA_DISCOVERY_NONE;
    }
    MediaDiscoveryKind kind = media_rendition_format(key, key_length);
    return kind == MEDIA_DISCOVERY_HLS ? kind : MEDIA_DISCOVERY_MP4;
}

/* With a context, the reference's site is completed here, once it is
   known to be media: from the element holding it (node), and for a literal
   in data text from its own key and the key chain around it (chain).
   References under an ad, promotion or app-download key, or with such a
   word in their path, are dropped whatever the context. */
static void media_consider_span(
    MediaDiscoveryBest *best, const char *source, size_t length,
    size_t begin, size_t end, MediaReferenceSite site,
    lxb_dom_node_t *node, MediaKeyChain *chain)
{
    if (best == NULL || source == NULL || begin >= end || end > length) return;
    size_t raw_length = end - begin;
    MediaDiscoveryKind kind = media_discovery_reference_kind(
        source + begin, raw_length);
    /* Generic video discovery must not reinterpret an unrelated audio URL
       from a data blob. Structured audio has its own context-aware pass. */
    if (kind == MEDIA_DISCOVERY_AUDIO_MP4) return;
    bool extensionless = false;
    if (kind == MEDIA_DISCOVERY_NONE) {
        if (best->context == NULL || chain == NULL) return;
        kind = media_extensionless_kind(source, begin, end, chain);
        if (kind == MEDIA_DISCOVERY_NONE) return;
        extensionless = true;
    }
    if (media_path_excluded(source + begin, raw_length)
        || (chain != NULL
            && media_literal_key_excluded(source, begin - 1u, chain)))
        return;
    if (best->context != NULL) {
        if (node != NULL) site = media_element_site(best->context, node);
        if (chain != NULL && !site.marked)
            site.marked = media_key_marked(source, begin - 1u)
                || media_chain_marked(chain, source);
        if (chain != NULL && !site.own)
            site.own = media_chain_own(chain, best->context, source, begin);
    }
    /* Without a suffix, only the page's own, unmarked video is offered. */
    if (extensionless
        && (site.marked
            || (!site.own && !media_reference_matches_identity(
                    best->context, source + begin, raw_length)))) return;
    if (kind == MEDIA_DISCOVERY_HLS && best->streaming != NULL)
        media_consider_span(best->streaming, source, length, begin, end,
                            site, NULL, NULL);
    unsigned quality = media_quality_hint(source, length, begin, end);
    int affinity = media_reference_affinity(
        best->context, source + begin, raw_length, site);
    if (!media_candidate_better(best, affinity, kind, quality)
        || raw_length >= best->capacity) return;
    size_t candidate_length = media_copy_json_string(
        best->url, best->capacity, source, begin, end);
    if (candidate_length == 0) return;
    best->kind = kind;
    best->quality = quality;
    best->affinity = affinity;
    best->found = true;
}

/* Data-script text. The keys of the objects and arrays around a literal
   can drop it (an ad, a promotion); with a context (activation) they also
   mark it, or place it beside the page's own ID. */
static void media_scan_text(
    MediaDiscoveryBest *best, const char *source, size_t length,
    MediaReferenceSite site)
{
    if (source == NULL || length == 0) return;
    MediaKeyChain chain;
    media_chain_reset(&chain);
    const MediaDiscoveryContext *context = best->context;
    for (size_t i = 0; i < length;) {
        char quote = source[i];
        if (quote != '"' && quote != '\'') {
            if (quote == '{' || quote == '[' || quote == '(')
                media_chain_open(&chain, i);
            else if (quote == '}' || quote == ']' || quote == ')')
                media_chain_close(&chain);
            else if (context != NULL && isdigit((unsigned char) quote)
                     && (i == 0u
                         || !isalnum((unsigned char) source[i - 1u]))) {
                /* A numeric item ID ({"id":272375365653}). */
                size_t digits = i;
                while (i < length && isdigit((unsigned char) source[i])) i++;
                media_chain_note_value(&chain, context, source, digits,
                                       digits, i);
                continue;
            }
            i++;
            continue;
        }
        size_t begin = ++i;
        bool escaped = false;
        while (i < length) {
            char value = source[i];
            if (escaped) escaped = false;
            else if (value == '\\') escaped = true;
            else if (value == quote) break;
            i++;
        }
        if (i >= length) break;
        media_chain_note_value(&chain, context, source, begin - 1u, begin, i);
        /* embedUrl names a player/document, even when its route happens to
           carry a media-looking suffix. Never feed it to the native decoder. */
        bool embed_value = false;
        size_t before = begin - 1u;
        while (before != 0u
               && isspace((unsigned char) source[before - 1u])) before--;
        if (before != 0u && source[before - 1u] == ':') {
            size_t key_end = before - 1u;
            while (key_end != 0u
                   && isspace((unsigned char) source[key_end - 1u])) key_end--;
            if (key_end != 0u
                && (source[key_end - 1u] == '"'
                    || source[key_end - 1u] == '\'')) {
                char key_quote = source[key_end - 1u];
                size_t key_close = key_end - 1u;
                size_t key_open = key_close;
                while (key_open != 0u) {
                    key_open--;
                    if (source[key_open] == key_quote
                        && (key_open == 0u || source[key_open - 1u] != '\\')) {
                        size_t key_length = key_close - key_open - 1u;
                        embed_value = key_length == sizeof("embedUrl") - 1u
                            && strncasecmp(
                                   source + key_open + 1u, "embedUrl",
                                   key_length) == 0;
                        break;
                    }
                }
            }
        }
        if (!embed_value)
            media_consider_span(best, source, length, begin, i, site, NULL,
                                &chain);
        i++;
    }
}

/* Executable inline scripts: players are often set up from a call such as
   `player.setup({hls: "https:\/\/cdn\/clip\/master.m3u8"})`, and that call
   may be the page's only reference to a playable rendition. This pass reads
   string literals only, skipping comments and regular-expression literals,
   and keeps spans into the retained script text rather than copying, so it
   needs no URL buffer of its own. It runs at activation only, after the
   markup and data-script pass, and only when a script reference could
   still win (see media_discover_candidates). Each script is read to this
   cap, except one that opens by assigning a literal (see
   media_script_assignment_led). */
#define MEDIA_SCRIPT_SCAN_PER_SCRIPT_LIMIT (64u * 1024u)

typedef struct {
    const char *source;
    size_t begin;
    size_t end;
    size_t capacity;
    MediaDiscoveryKind kind;
    unsigned quality;
    int affinity;
    bool found;
} MediaScriptReference;

typedef struct {
    MediaScriptReference primary;
    MediaScriptReference streaming;
    const MediaDiscoveryContext *context;
    /* The script element's own site, and its key chain. */
    MediaReferenceSite site;
    MediaKeyChain chain;
} MediaScriptScan;

/* An absolute http(s) or root-relative reference, as a raw string literal
   (slashes may be escaped as "\/" or "\u002F"), with no character a URL
   never carries. */
static bool media_script_reference_shaped(const char *raw, size_t length)
{
    size_t scheme = 0;
    if (length > 6u && strncasecmp(raw, "https:", 6u) == 0) scheme = 6u;
    else if (length > 5u && strncasecmp(raw, "http:", 5u) == 0) scheme = 5u;
    if (raw[scheme] != '/'
        && !(raw[scheme] == '\\' && scheme + 1u < length
             && raw[scheme + 1u] == '/')
        && media_unicode_escape_at(raw, length, scheme, "002f") == 0u)
        return false;
    for (size_t i = 0; i < length; i++) {
        unsigned char value = (unsigned char) raw[i];
        if (value <= 0x20u || value == '<' || value == '>' || value == '"'
            || value == '\'' || value == '`' || value == '{'
            || value == '}') return false;
    }
    return true;
}

/* The discovery policy's quality for one whole word of a key, or 0. */
static unsigned media_script_word_quality(const char *word, size_t length)
{
    static const struct {
        const char *word;
        unsigned quality;
    } words[] = {
        {"low", 240u}, {"lowres", 240u}, {"small", 240u}, {"mobile", 240u},
        {"med", 480u}, {"medium", 480u}, {"mid", 480u},
        {"high", 720u}, {"highres", 720u}, {"hd", 720u}, {"hq", 720u}
    };
    for (size_t i = 0; i < sizeof(words) / sizeof(words[0]); i++) {
        if (strlen(words[i].word) == length
            && strncasecmp(word, words[i].word, length) == 0)
            return words[i].quality;
    }
    return 0;
}

/* The quality a literal's key names (see media_key_span). A number in it
   wins ("mp4_360", "url240"); otherwise low/medium/high words map onto the
   discovery policy's scale, the lowest named winning. Words are whole: the
   key splits at punctuation, at a lower-to-upper change ("hdUrl"), where a
   capital run meets a capitalised word ("HDUrl") and between letters and
   digits, so "download", "media" and "highlights" name nothing. 0 when the
   key names nothing. */
static unsigned media_script_key_quality(const char *source, size_t quote)
{
    size_t key_begin = 0, key_end = 0;
    if (!media_key_span(source, quote, &key_begin, &key_end)) return 0;
    size_t length = key_end - key_begin;
    const char *key = source + key_begin;
    unsigned number = media_candidate_quality_label(key, length);
    if (number != 0u) return number;
    unsigned quality = 0;
    size_t start = 0;
    for (size_t i = 0; i <= length; i++) {
        bool separator = i == length || !isalnum((unsigned char) key[i]);
        bool boundary = false;
        if (!separator && i > start) {
            unsigned char before = (unsigned char) key[i - 1u];
            unsigned char at = (unsigned char) key[i];
            boundary = (islower(before) && isupper(at))
                || ((isalpha(before) != 0) != (isalpha(at) != 0))
                || (isupper(before) && isupper(at) && i + 1u < length
                    && islower((unsigned char) key[i + 1u]));
        }
        if (!separator && !boundary) continue;
        if (i > start) {
            unsigned word = media_script_word_quality(key + start, i - start);
            if (word != 0u && (quality == 0u || word < quality))
                quality = word;
        }
        start = separator ? i + 1u : i;
    }
    return quality;
}

static void media_script_offer(
    MediaScriptReference *slot, const char *source, size_t begin,
    size_t end, size_t needed, int affinity, MediaDiscoveryKind kind,
    unsigned quality)
{
    if (needed >= slot->capacity
        || !media_rank_better(slot->found, slot->affinity, slot->kind,
                              slot->quality, affinity, kind, quality)) return;
    slot->source = source;
    slot->begin = begin;
    slot->end = end;
    slot->kind = kind;
    slot->quality = quality;
    slot->affinity = affinity;
    slot->found = true;
}

static void media_script_consider(
    MediaScriptScan *scan, const char *source, size_t length,
    size_t begin, size_t end)
{
    if (begin >= end) return;
    const char *raw = source + begin;
    size_t raw_length = end - begin;
    if (!media_script_reference_shaped(raw, raw_length)) return;
    MediaDiscoveryKind kind = media_discovery_reference_kind(raw, raw_length);
    if (kind != MEDIA_DISCOVERY_MP4 && kind != MEDIA_DISCOVERY_HLS) return;
    if (media_path_excluded(raw, raw_length)
        || media_literal_key_excluded(source, begin - 1u, &scan->chain))
        return;
    size_t needed = media_decode_json_string(NULL, 0u, source, begin, end);
    if (needed == 0u) return;
    unsigned quality = media_script_key_quality(source, begin - 1u);
    if (quality == 0u)
        quality = media_quality_hint(source, length, begin, end);
    MediaReferenceSite site = scan->site;
    if (scan->context != NULL && !site.marked)
        site.marked = media_key_marked(source, begin - 1u)
            || media_chain_marked(&scan->chain, source);
    if (!site.own)
        site.own = media_chain_own(
            &scan->chain, scan->context, source, begin);
    int affinity = media_reference_affinity(
        scan->context, raw, raw_length, site);
    if (kind == MEDIA_DISCOVERY_HLS)
        media_script_offer(&scan->streaming, source, begin, end, needed,
                           affinity, kind, quality);
    media_script_offer(&scan->primary, source, begin, end, needed,
                       affinity, kind, quality);
}

/* Whether a `/` after this significant character starts a regular
   expression literal rather than a division. */
static bool media_script_regexp_may_follow(char previous)
{
    return previous == '\0' || strchr("(,=:[!&|?{};+-*%<>~^", previous)
        != NULL;
}

static void media_scan_script_text(
    MediaScriptScan *scan, const char *source, size_t length)
{
    char previous = '\0';
    media_chain_reset(&scan->chain);
    for (size_t i = 0; i < length;) {
        char value = source[i];
        char next = i + 1u < length ? source[i + 1u] : '\0';
        if (value == '/' && next == '/') {
            while (i < length && source[i] != '\n') i++;
            continue;
        }
        if (value == '/' && next == '*') {
            i += 2u;
            while (i + 1u < length
                   && !(source[i] == '*' && source[i + 1u] == '/')) i++;
            i += 2u;
            continue;
        }
        if (value == '/' && media_script_regexp_may_follow(previous)) {
            bool in_class = false;
            size_t at = i + 1u;
            while (at < length && source[at] != '\n') {
                if (source[at] == '\\') at++;
                else if (source[at] == '[') in_class = true;
                else if (source[at] == ']') in_class = false;
                else if (source[at] == '/' && !in_class) break;
                at++;
            }
            if (at < length && source[at] == '/') {
                i = at + 1u;
                previous = 'r';
                continue;
            }
        }
        if (value == '"' || value == '\'' || value == '`') {
            size_t begin = i + 1u, at = begin;
            bool interpolated = false;
            while (at < length && source[at] != value
                   && (value == '`' || source[at] != '\n')) {
                if (source[at] == '\\') at++;
                else if (value == '`' && source[at] == '$'
                         && at + 1u < length && source[at + 1u] == '{')
                    interpolated = true;
                at++;
            }
            if (at >= length) return;
            if (source[at] == value && !interpolated) {
                media_chain_note_value(&scan->chain, scan->context, source,
                                       i, begin, at);
                media_script_consider(scan, source, length, begin, at);
            }
            i = at + 1u;
            previous = '"';
            continue;
        }
        if (value == '{' || value == '[' || value == '(')
            media_chain_open(&scan->chain, i);
        else if (value == '}' || value == ']' || value == ')')
            media_chain_close(&scan->chain);
        else if (isdigit((unsigned char) value) && previous == ':') {
            /* A numeric item ID ({id: 272375365653}). */
            size_t digits = i;
            while (i < length && isdigit((unsigned char) source[i])) i++;
            media_chain_note_value(&scan->chain, scan->context, source,
                                   digits, digits, i);
            previous = '0';
            continue;
        }
        if (!isspace((unsigned char) value)) previous = value;
        i++;
    }
}

/* Most executable script text names no media file at all. A memchr pass
   over its dots is several times cheaper than the literal scanner, which
   then runs only on text that may hold a reference. */
static bool media_script_mentions_media(const char *text, size_t length)
{
    const char *at = text, *end = text + length;
    while (at < end
           && (at = memchr(at, '.', (size_t) (end - at))) != NULL) {
        size_t remaining = (size_t) (end - at);
        if ((remaining >= 4u && strncasecmp(at, ".mp4", 4u) == 0)
            || (remaining >= 5u && strncasecmp(at, ".m3u8", 5u) == 0))
            return true;
        at++;
    }
    return false;
}

static bool media_script_is_executable(lxb_dom_node_t *node)
{
    size_t length = 0;
    if (document_attribute(node, "src", &length) != NULL) return false;
    const char *type = document_attribute(node, "type", &length);
    if (type == NULL || length == 0) return true;
    static const char *const types[] = {
        "text/javascript", "application/javascript", "module",
        "text/ecmascript", "application/ecmascript"
    };
    for (size_t i = 0; i < sizeof(types) / sizeof(types[0]); i++) {
        if (length == strlen(types[i])
            && strncasecmp(type, types[i], length) == 0) return true;
    }
    return false;
}

static bool media_script_is_data(lxb_dom_node_t *node, bool *strict_json);

static lxb_dom_node_t *media_walk_next(
    lxb_dom_node_t *root, lxb_dom_node_t *node);

/* Whether a script opens by assigning an object or array literal, as in
   `window.__INITIAL_STATE__={...};(function(){...}())`: its bulk is data,
   which a short tail of code keeps out of the data-script pass. Such a
   script is read past the per-script cap, within the shared text budget,
   since its player state often sits deep in the literal. */
static bool media_script_assignment_led(const char *text, size_t length)
{
    static const char *const prefixes[] = {"var ", "let ", "const "};
    size_t at = 0;
    while (at < length && isspace((unsigned char) text[at])) at++;
    for (size_t i = 0; i < sizeof(prefixes) / sizeof(prefixes[0]); i++) {
        size_t prefix = strlen(prefixes[i]);
        if (length - at > prefix
            && memcmp(text + at, prefixes[i], prefix) == 0) {
            at += prefix;
            break;
        }
    }
    size_t name = at;
    while (at < length && at - name < MEDIA_SCRIPT_KEY_LIMIT * 2u
           && (isalnum((unsigned char) text[at]) || text[at] == '_'
               || text[at] == '$' || text[at] == '.')) at++;
    if (at == name || isdigit((unsigned char) text[name])) return false;
    while (at < length && isspace((unsigned char) text[at])) at++;
    if (at >= length || text[at] != '=') return false;
    at++;
    if (at < length && text[at] == '=') return false;
    while (at < length && isspace((unsigned char) text[at])) at++;
    return at < length && (text[at] == '{' || text[at] == '[');
}

/* Walks the executable inline scripts in document order within the text
   budget the first pass left over. Returns the bytes read. */
static size_t media_scan_executable_scripts(
    lxb_dom_node_t *root, MediaScriptScan *scan, size_t budget)
{
    size_t bytes = 0, nodes = 0;
    for (lxb_dom_node_t *node = root;
         node != NULL && nodes < MEDIA_DISCOVERY_NODE_LIMIT
             && bytes < budget;
         node = media_walk_next(root, node)) {
        nodes++;
        if (node->type != LXB_DOM_NODE_TYPE_ELEMENT
            || !media_name_is(node, "script")
            || !media_script_is_executable(node)) continue;
        /* Data-shaped scripts were read by the first pass. Classifying one
           costs a scan of its own, so it is done only for text that
           mentions a media file. */
        int data_script = -1;
        size_t script_bytes = 0;
        size_t script_limit = MEDIA_SCRIPT_SCAN_PER_SCRIPT_LIMIT;
        for (lxb_dom_node_t *child = node->first_child;
             child != NULL && bytes < budget && script_bytes < script_limit;
             child = child->next) {
            size_t length = 0;
            const char *text = document_text_data(child, &length);
            if (text == NULL || length == 0) continue;
            if (script_bytes == 0u
                && media_script_assignment_led(text, length))
                script_limit = SIZE_MAX;
            size_t admitted = length;
            if (admitted > budget - bytes) admitted = budget - bytes;
            if (admitted > script_limit - script_bytes)
                admitted = script_limit - script_bytes;
            if (media_script_mentions_media(text, admitted)) {
                if (data_script < 0) {
                    data_script = media_script_is_data(node, NULL) ? 1 : 0;
                    scan->site = media_element_site(scan->context, node);
                }
                if (data_script == 0)
                    media_scan_script_text(scan, text, admitted);
            }
            bytes += admitted;
            script_bytes += admitted;
        }
    }
    return bytes;
}

static bool media_script_copy(
    const MediaScriptReference *reference, char *output)
{
    return media_copy_json_string(
               output, reference->capacity, reference->source,
               reference->begin, reference->end) != 0u;
}

static bool media_script_is_data(lxb_dom_node_t *node, bool *strict_json)
{
    if (strict_json != NULL) *strict_json = false;
    size_t length = 0;
    const char *type = document_attribute(node, "type", &length);
    if (type != NULL && length != 0) {
        bool json = (length == sizeof("application/json") - 1u
                     && strncasecmp(type, "application/json", length) == 0)
            || (length == sizeof("application/ld+json") - 1u
                && strncasecmp(type, "application/ld+json", length) == 0);
        if (strict_json != NULL) *strict_json = json;
        return json;
    }
    return script_runtime_inline_data_candidate(node, NULL);
}

static lxb_dom_node_t *media_walk_next(
    lxb_dom_node_t *root, lxb_dom_node_t *node)
{
    if (node->first_child != NULL) return node->first_child;
    while (node != NULL && node != root) {
        if (node->next != NULL) return node->next;
        node = node->parent;
    }
    return NULL;
}

/* Stream references in the responses the page's scripts fetched: JSON
   rendition lists or player configurations, e.g.
   [{"quality":"240","videoUrl":"https://cdn/240.mp4","format":"mp4"}],
   {"sources":{"mp4":{"240p":{"src":"..."}}}} or {"data":"https://...m3u8"},
   and bodies that are themselves an #EXTM3U playlist. Each response is
   read once, as it is delivered, and only its few best references are
   kept (see media_json_response_remember); bodies are never stored. In
   each object a string under a URL key, or any string naming a media
   suffix, is a reference; a quality key (or, failing that, the key the
   object sits under, or the URL itself) gives its quality, and a format
   key or the URL key's own name ("hls", "mp4") its kind when the URL has
   no media suffix. A reference is the page's own when the response's
   address carries an ID of the page, or when the object holding it (or
   the one holding that) has a scalar member equal to one. */
struct MediaJsonResponse {
    uint16_t url_length;
    uint16_t quality;
    uint8_t kind;
    bool own;
    bool marked;
    /* The raw (still JSON-escaped) reference, NUL-terminated. */
    char url[];
};

#define MEDIA_JSON_RESPONSE_SLOTS \
    (sizeof(((PocDocument *) 0)->media_json_responses) \
     / sizeof(((PocDocument *) 0)->media_json_responses[0]))
#define MEDIA_RENDITION_DEPTH 8u
#define MEDIA_RENDITION_URLS 4u
/* Objects this near the root are read twice: once for an ID member. */
#define MEDIA_RENDITION_IDENTITY_LEVELS 4u

enum { MEDIA_RENDITION_URL = 1, MEDIA_RENDITION_QUALITY,
       MEDIA_RENDITION_FORMAT };

typedef struct {
    size_t begin;
    size_t end;
    MediaDiscoveryKind kind;
    unsigned quality;
    bool own;
    bool marked;
} MediaRenditionPick;

typedef struct {
    const MediaDiscoveryContext *context;
    const char *source;
    MediaRenditionPick best[2];
    size_t best_count;
    MediaRenditionPick stream;
    bool have_stream;
} MediaRenditionWalk;

static int media_rendition_field(const char *key)
{
    /* Keys that hold a stream address by themselves; any key naming a
       locator (videoUrl, playback_url, m3u8_url, hlsSrc) is one too. */
    static const char *const names[] = {
        "file", "hls", "mp4", "m3u8", "data", "source", "stream",
        "manifest", "master",
        "quality", "label", "height", "res", "resolution",
        "format", "type", "mimeType"
    };
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
        if (strcasecmp(key, names[i]) == 0)
            return i < 9u ? MEDIA_RENDITION_URL
                : i < 14u ? MEDIA_RENDITION_QUALITY : MEDIA_RENDITION_FORMAT;
    }
    return media_text_has_word(key, strlen(key), true,
                               MEDIA_WORD_LIST(media_locator_words))
        ? MEDIA_RENDITION_URL : 0;
}

/* "hls", "m3u8" or "application/x-mpegURL" name HLS; "mp4" or "video/mp4"
   a direct file. */
static MediaDiscoveryKind media_rendition_format(
    const char *value, size_t length)
{
    for (size_t i = 0; i < length; i++) {
        size_t rest = length - i;
        if ((rest >= 3u && strncasecmp(value + i, "hls", 3u) == 0)
            || (rest >= 4u && strncasecmp(value + i, "m3u8", 4u) == 0)
            || (rest >= 7u && strncasecmp(value + i, "mpegurl", 7u) == 0))
            return MEDIA_DISCOVERY_HLS;
        if (rest >= 3u && strncasecmp(value + i, "mp4", 3u) == 0)
            return MEDIA_DISCOVERY_MP4;
    }
    return MEDIA_DISCOVERY_NONE;
}

/* Whole path words naming a player's stand-in clip (an error, a missing
   video, a placeholder) rather than the video itself. */
static const char *const media_stand_in_words[] = {
    "error", "errors", "unavailable", "placeholder", "fallback", "blank",
    "dummy", "notfound", "removed"
};

static bool media_path_stand_in(const char *raw, size_t length)
{
    size_t begin = 0, end = 0;
    media_reference_path(raw, length, &begin, &end);
    return media_text_has_word(raw + begin, end - begin, false,
                               MEDIA_WORD_LIST(media_stand_in_words));
}

static int media_rendition_rank_affinity(const MediaRenditionPick *pick)
{
    return (pick->own ? MEDIA_AFFINITY_OWN : 0) - (pick->marked ? 2 : 0);
}

static bool media_rendition_pick_better(
    const MediaRenditionPick *candidate, const MediaRenditionPick *current)
{
    return media_rank_better(
        current != NULL, current == NULL ? 0
            : media_rendition_rank_affinity(current),
        current == NULL ? MEDIA_DISCOVERY_NONE : current->kind,
        current == NULL ? 0u : current->quality,
        media_rendition_rank_affinity(candidate), candidate->kind,
        candidate->quality);
}

/* Keeps the best two references in order, and the best stream. */
static void media_rendition_keep(
    MediaRenditionWalk *walk, const MediaRenditionPick *pick)
{
    if (pick->kind == MEDIA_DISCOVERY_HLS
        && (!walk->have_stream
            || media_rendition_pick_better(pick, &walk->stream))) {
        walk->stream = *pick;
        walk->have_stream = true;
    }
    size_t at = walk->best_count;
    while (at > 0u && media_rendition_pick_better(pick, &walk->best[at - 1u]))
        at--;
    if (at >= 2u) return;
    if (walk->best_count == 2u) walk->best_count = 1u;
    for (size_t i = walk->best_count; i > at; i--)
        walk->best[i] = walk->best[i - 1u];
    walk->best[at] = *pick;
    walk->best_count++;
}

static void media_rendition_offer(
    MediaRenditionWalk *walk, size_t begin, size_t end,
    MediaDiscoveryKind kind, unsigned quality, bool marked, int identity)
{
    const char *raw = walk->source + begin;
    size_t raw_length = end - begin;
    if (raw_length == 0u || raw_length >= NAVIGATION_URL_LIMIT
        || !media_script_reference_shaped(raw, raw_length)
        || media_path_excluded(raw, raw_length))
        return;
    MediaDiscoveryKind named = media_discovery_reference_kind(raw, raw_length);
    if (named != MEDIA_DISCOVERY_NONE) kind = named;
    if (kind != MEDIA_DISCOVERY_MP4 && kind != MEDIA_DISCOVERY_HLS) return;
    if (media_decode_json_string(NULL, 0u, walk->source, begin, end) == 0u)
        return;
    if (quality == 0u)
        quality = media_quality_hint(raw, raw_length, 0u, raw_length);
    size_t path_begin = 0, path_end = 0;
    media_reference_path(raw, raw_length, &path_begin, &path_end);
    MediaRenditionPick pick = {
        .begin = begin, .end = end, .kind = kind, .quality = quality,
        .own = identity > 0
            || media_reference_matches_identity(
                   walk->context, raw, raw_length),
        .marked = marked || media_path_stand_in(raw, raw_length)
            || media_text_marked(raw + path_begin, path_end - path_begin)
            || media_path_has_thumbnail_size(
                   raw + path_begin, path_end - path_begin)
    };
    media_rendition_keep(walk, &pick);
}

/* Identity keys name what an object describes: "id", "videoId",
   "video_id", "slug", "guid", "uuid". */
static const char *const media_identity_key_words[] = {
    "id", "slug", "guid", "uuid"
};

static bool media_value_is_identity(
    const MediaDiscoveryContext *context, const char *value, size_t length)
{
    for (size_t t = 0; t < context->token_count; t++) {
        if (context->token_lengths[t] == length
            && strncasecmp(value, context->tokens[t], length) == 0)
            return true;
    }
    return false;
}

/* For the object at the cursor: 1 when one of its scalar members equals an
   ID of the page (an identity key's own value when it has one), -1 when
   it has an identity key naming something else, else 0. */
static int media_rendition_object_identity(
    const MediaRenditionWalk *walk, JsonCursor json)
{
    if (walk->context->token_count == 0u) return 0;
    json.at++;
    bool identity_key = false, identity_match = false, other_match = false;
    for (unsigned members = 0; members < 256u; members++) {
        json_scan_space(&json);
        if (json.at >= json.end || *json.at == '}') break;
        char key[32];
        JsonCursor before = json;
        if (!json_scan_member_key(&json, key, sizeof(key))) {
            json = before;
            if (!json_scan_skip_string(&json)) return 0;
            json_scan_space(&json);
            if (json.at >= json.end || *json.at++ != ':') return 0;
            key[0] = '\0';
        }
        json_scan_space(&json);
        if (json.at >= json.end) return 0;
        bool string = *json.at == '"';
        const char *value = json.at + (string ? 1 : 0);
        if (!json_scan_skip_value(&json, MEDIA_STRUCTURED_JSON_DEPTH_LIMIT))
            return 0;
        size_t length = (size_t) (json.at - value) - (string ? 1u : 0u);
        bool scalar = string || (*value != '{' && *value != '[');
        bool id_key = media_text_has_word(
            key, strlen(key), true, MEDIA_WORD_LIST(media_identity_key_words));
        bool match = scalar
            && media_value_is_identity(walk->context, value, length);
        identity_key = identity_key || (id_key && scalar);
        identity_match = identity_match || (id_key && match);
        other_match = other_match || match;
        json_scan_space(&json);
        if (json.at >= json.end || *json.at != ',') break;
        json.at++;
    }
    if (identity_match) return 1;
    if (identity_key) return -1;
    return other_match ? 1 : 0;
}

static bool media_rendition_value(
    MediaRenditionWalk *walk, JsonCursor *json, unsigned depth,
    unsigned quality, bool marked, int identity);

/* The object at the cursor ('{'); its references are offered once all its
   members are read, since a quality or format key may follow the URL. */
static bool media_rendition_object(
    MediaRenditionWalk *walk, JsonCursor *json, unsigned depth,
    unsigned quality, bool marked, int identity)
{
    size_t url_begin[MEDIA_RENDITION_URLS], url_end[MEDIA_RENDITION_URLS];
    MediaDiscoveryKind url_kind[MEDIA_RENDITION_URLS];
    bool url_marked[MEDIA_RENDITION_URLS];
    bool url_named[MEDIA_RENDITION_URLS];
    size_t urls = 0;
    unsigned own_quality = 0;
    MediaDiscoveryKind format = MEDIA_DISCOVERY_NONE;
    if (MEDIA_RENDITION_DEPTH - depth < MEDIA_RENDITION_IDENTITY_LEVELS) {
        /* An item naming another video stops what it inherits; the
           response itself is judged by its address first. */
        int found = media_rendition_object_identity(walk, *json);
        if (found > 0
            || (found < 0 && depth != MEDIA_RENDITION_DEPTH))
            identity = found;
    }
    json->at++;
    json_scan_space(json);
    bool empty = json->at < json->end && *json->at == '}';
    if (empty) json->at++;
    while (!empty) {
        char key[32];
        JsonCursor before = *json;
        if (!json_scan_member_key(json, key, sizeof(key))) {
            /* A key too long to decode is none of ours: step over it. */
            *json = before;
            if (!json_scan_skip_string(json)) return false;
            json_scan_space(json);
            if (json->at >= json->end || *json->at++ != ':') return false;
            key[0] = '\0';
        }
        json_scan_space(json);
        if (json->at >= json->end) return false;
        size_t key_length = strlen(key);
        char first = *json->at;
        if (media_key_text_excluded(key, key_length)) {
            /* An ad's or a promotion's renditions are none of the page's. */
            if (!json_scan_skip_value(json, MEDIA_STRUCTURED_JSON_DEPTH_LIMIT))
                return false;
        } else if (first == '{' || first == '[') {
            unsigned named = media_candidate_quality_label(key, key_length);
            if (!media_rendition_value(
                    walk, json, depth - 1u, named != 0u ? named : quality,
                    marked || media_text_marked(key, key_length), identity))
                return false;
        } else {
            bool string = first == '"';
            size_t begin = (size_t) (json->at - walk->source)
                + (string ? 1u : 0u);
            if (string ? !json_scan_skip_string(json)
                       : !json_scan_skip_value(json, 1u)) return false;
            size_t end = (size_t) (json->at - walk->source)
                - (string ? 1u : 0u);
            int field = media_rendition_field(key);
            bool named = string && media_discovery_reference_kind(
                walk->source + begin, end - begin) != MEDIA_DISCOVERY_NONE;
            if (string && (field == MEDIA_RENDITION_URL || named)) {
                /* A suffix-named address displaces an unnamed one. */
                size_t slot = urls;
                if (slot == MEDIA_RENDITION_URLS && named) {
                    for (size_t i = 0; i < urls; i++)
                        if (!url_named[i]) slot = i;
                }
                if (slot < MEDIA_RENDITION_URLS) {
                    url_begin[slot] = begin;
                    url_end[slot] = end;
                    url_kind[slot] = media_rendition_format(key, key_length);
                    url_marked[slot] = media_text_marked(key, key_length);
                    url_named[slot] = named;
                    if (slot == urls) urls++;
                }
            } else if (field == MEDIA_RENDITION_QUALITY) {
                own_quality = media_candidate_quality_label(
                    walk->source + begin, end - begin);
            } else if (field == MEDIA_RENDITION_FORMAT && string) {
                format = media_rendition_format(
                    walk->source + begin, end - begin);
            }
        }
        json_scan_space(json);
        if (json->at >= json->end) return false;
        char next = *json->at++;
        if (next == '}') break;
        if (next != ',') return false;
        json_scan_space(json);
    }
    if (own_quality != 0u) quality = own_quality;
    for (size_t i = 0; i < urls; i++)
        media_rendition_offer(
            walk, url_begin[i], url_end[i],
            url_kind[i] != MEDIA_DISCOVERY_NONE ? url_kind[i] : format,
            quality, marked || url_marked[i], identity);
    return true;
}

static bool media_rendition_value(
    MediaRenditionWalk *walk, JsonCursor *json, unsigned depth,
    unsigned quality, bool marked, int identity)
{
    json_scan_space(json);
    if (json->at >= json->end) return false;
    if (depth == 0u)
        return json_scan_skip_value(json, MEDIA_STRUCTURED_JSON_DEPTH_LIMIT);
    if (*json->at == '{')
        return media_rendition_object(
            walk, json, depth, quality, marked, identity);
    if (*json->at != '[') return json_scan_skip_value(json, 1u);
    json->at++;
    json_scan_space(json);
    if (json->at < json->end && *json->at == ']') {
        json->at++;
        return true;
    }
    for (;;) {
        if (!media_rendition_value(
                walk, json, depth - 1u, quality, marked, identity))
            return false;
        json_scan_space(json);
        if (json->at >= json->end) return false;
        char next = *json->at++;
        if (next == ']') return true;
        if (next != ',') return false;
    }
}

/* Activation: the kept references join the executable-script pass, as the
   player's own data (in the player's container), and as the page's own
   video when they were the page's own as they arrived. */
static void media_scan_json_responses(
    const PocDocument *document, MediaScriptScan *scan)
{
    for (size_t i = 0; i < MEDIA_JSON_RESPONSE_SLOTS; i++) {
        const struct MediaJsonResponse *entry =
            document->media_json_responses[i];
        if (entry == NULL) continue;
        int affinity = media_reference_affinity(
            scan->context, entry->url, entry->url_length,
            (MediaReferenceSite) {
                .near = true, .marked = entry->marked, .own = entry->own
            });
        size_t needed = media_decode_json_string(
            NULL, 0u, entry->url, 0u, entry->url_length);
        if (needed == 0u) continue;
        MediaDiscoveryKind kind = (MediaDiscoveryKind) entry->kind;
        if (kind == MEDIA_DISCOVERY_HLS)
            media_script_offer(&scan->streaming, entry->url, 0u,
                               entry->url_length, needed, affinity, kind,
                               entry->quality);
        media_script_offer(&scan->primary, entry->url, 0u, entry->url_length,
                           needed, affinity, kind, entry->quality);
    }
}

static bool media_scan_structured_json(
    const char *source, size_t length,
    MediaStructuredAudioIndex *audio_index,
    MediaStructuredVideoIndex *video_index);

static bool media_discover_candidates(
    const PocDocument *document, char *url, size_t url_capacity,
    char *streaming_url, size_t streaming_capacity,
    const MediaDiscoveryContext *context, MediaDiscoveryResult *result);

/* The best of a page's VideoObjects, or SIZE_MAX: ranked like any other
   reference, so with the page's identity known (context) the page's own
   VideoObject (its contentUrl or its url names the page's video) beats a
   list of other clips whatever their kind, size or order. The winner's
   URL must decode within `capacity`; with exclude_paths, an ad's or a
   promotion's path is passed over too. */
static size_t media_structured_video_select(
    const MediaStructuredVideoIndex *index,
    const MediaDiscoveryContext *context, size_t capacity,
    bool exclude_paths, MediaDiscoveryKind *kind_out,
    unsigned *quality_out, int *affinity_out)
{
    size_t selected = SIZE_MAX;
    MediaDiscoveryKind best_kind = MEDIA_DISCOVERY_NONE;
    unsigned best_quality = 0;
    int best_affinity = 0;
    for (size_t i = 0; i < index->candidate_count; i++) {
        const MediaStructuredVideoCandidate *candidate =
            &index->candidates[i];
        const char *raw = candidate->source + candidate->url_begin;
        size_t raw_length = candidate->url_end - candidate->url_begin;
        MediaDiscoveryKind kind = media_discovery_reference_kind(
            raw, raw_length);
        if (kind == MEDIA_DISCOVERY_NONE || kind == MEDIA_DISCOVERY_AUDIO_MP4
            || (exclude_paths && media_path_excluded(raw, raw_length)))
            continue;
        unsigned quality = media_quality_hint(
            candidate->source, candidate->url_end,
            candidate->url_begin, candidate->url_end);
        MediaReferenceSite site = {0};
        if (context != NULL && candidate->page_end > candidate->page_begin)
            site.own = media_reference_matches_identity(
                context, candidate->source + candidate->page_begin,
                candidate->page_end - candidate->page_begin);
        int affinity = media_reference_affinity(
            context, raw, raw_length, site);
        if (!media_rank_better(selected != SIZE_MAX, best_affinity,
                               best_kind, best_quality, affinity, kind,
                               quality)) continue;
        size_t needed = media_decode_json_string(
            NULL, 0u, candidate->source, candidate->url_begin,
            candidate->url_end);
        if (needed == 0u || needed >= capacity) continue;
        selected = i;
        best_kind = kind;
        best_quality = quality;
        best_affinity = affinity;
    }
    if (selected != SIZE_MAX) {
        *kind_out = best_kind;
        *quality_out = best_quality;
        *affinity_out = best_affinity;
    }
    return selected;
}

bool media_discover_document_candidate(
    const PocDocument *document, char *url, size_t url_capacity,
    MediaDiscoveryResult *result)
{
    return media_discover_candidates(
        document, url, url_capacity, NULL, 0, NULL, result);
}

bool media_discover_document_candidates(
    const PocDocument *document, const char *page_url,
    lxb_dom_node_t *anchor, char *url, size_t url_capacity,
    char *streaming_url, size_t streaming_capacity,
    MediaDiscoveryResult *result)
{
    if (document == NULL || document->html == NULL) {
        return media_discover_candidates(
            document, url, url_capacity, streaming_url, streaming_capacity,
            NULL, result);
    }
    MediaDiscoveryContext context;
    media_context_init(&context, document, page_url, anchor);
    return media_discover_candidates(
        document, url, url_capacity, streaming_url, streaming_capacity,
        &context, result);
}

/* With a context (activation only) references are ranked by affinity
   first and executable inline scripts are read too; without one (page
   load) the result is the markup-only kind and quality ranking. */
static bool media_discover_candidates(
    const PocDocument *document, char *url, size_t url_capacity,
    char *streaming_url, size_t streaming_capacity,
    const MediaDiscoveryContext *context, MediaDiscoveryResult *result)
{
    if (result != NULL) memset(result, 0, sizeof(*result));
    if (url != NULL && url_capacity != 0) url[0] = '\0';
    bool want_streaming = streaming_url != NULL && streaming_capacity >= 2u;
    if (want_streaming) streaming_url[0] = '\0';
    if (document == NULL || document->html == NULL || url == NULL
        || url_capacity < 2u) return false;
    MediaDiscoveryBest streaming = {
        .url = streaming_url, .capacity = streaming_capacity,
        .context = context
    };
    MediaDiscoveryBest best = {
        .url = url, .capacity = url_capacity,
        .streaming = want_streaming ? &streaming : NULL,
        .context = context
    };
    MediaStructuredVideoIndex video_index = {0};
    lxb_dom_node_t *root = lxb_dom_interface_node(document->html);
    size_t bytes = 0, nodes = 0;
    static const char *const attributes[] = {
        "src", "data-src", "data-url", "data-video-url", "content"
    };
    for (lxb_dom_node_t *node = root;
         node != NULL && nodes < MEDIA_DISCOVERY_NODE_LIMIT;
         node = media_walk_next(root, node)) {
        nodes++;
        if (node->type != LXB_DOM_NODE_TYPE_ELEMENT) continue;
        for (size_t i = 0; i < sizeof(attributes) / sizeof(attributes[0]); i++) {
            size_t length = 0;
            const char *value = document_attribute(
                node, attributes[i], &length);
            if (value == NULL || length == 0) continue;
            media_consider_span(&best, value, length, 0, length,
                                (MediaReferenceSite) {0}, node, NULL);
        }
        if (!media_name_is(node, "script")
            || !media_script_is_data(node, NULL)) {
            continue;
        }
        MediaReferenceSite script_site = context == NULL
            ? (MediaReferenceSite) {0} : media_element_site(context, node);
        for (lxb_dom_node_t *child = node->first_child; child != NULL;
             child = child->next) {
            size_t length = 0;
            const char *text = document_text_data(child, &length);
            if (text == NULL || length == 0) continue;
            size_t remaining = MEDIA_DISCOVERY_TEXT_LIMIT - bytes;
            size_t admitted = length < remaining ? length : remaining;
            media_scan_text(&best, text, admitted, script_site);
            (void) media_scan_structured_json(
                text, admitted, NULL, &video_index);
            bytes += admitted;
            if (bytes == MEDIA_DISCOVERY_TEXT_LIMIT) break;
        }
        if (bytes == MEDIA_DISCOVERY_TEXT_LIMIT) break;
    }
    /* A schema-declared VideoObject is stronger evidence than an unrelated
       MP4/HLS token elsewhere in retained data. Rank all bounded structured
       candidates with the same PSP quality policy, then replace the loose
       winner only when a real contentUrl was admitted. */
    MediaDiscoveryKind structured_kind = MEDIA_DISCOVERY_NONE;
    unsigned structured_quality = 0;
    int structured_affinity = 0;
    size_t structured = media_structured_video_select(
        &video_index, context, url_capacity, true, &structured_kind,
        &structured_quality, &structured_affinity);
    /* With the page's identity known, several VideoObjects none of which
       is the page's own are a list of other clips (a related rail, often
       added late by the page's script): they are ranked like any other
       reference rather than as the page's declaration. */
    bool declared = structured != SIZE_MAX
        && (context == NULL || context->token_count == 0u
            || video_index.candidate_count == 1u
            || structured_affinity >= MEDIA_AFFINITY_OWN);
    if (structured != SIZE_MAX
        && (declared
            || media_candidate_better(&best, structured_affinity,
                                      structured_kind, structured_quality))) {
        const MediaStructuredVideoCandidate *candidate =
            &video_index.candidates[structured];
        if (media_copy_json_string(
                url, url_capacity, candidate->source, candidate->url_begin,
                candidate->url_end) != 0u) {
            best.kind = structured_kind;
            best.quality = structured_quality;
            best.affinity = context != NULL && declared
                ? MEDIA_AFFINITY_DECLARED : structured_affinity;
            best.found = true;
        }
    }
    /* Executable inline scripts are read only for this activation-time
       entry point, so every page-load caller keeps the markup-only result.
       They are read when the markup left the winner or the HLS slot empty,
       or when its winner is not known to be the page's own video (no ID
       match): a player configuration can name the page's video while a
       related-video list in markup or data names only previews. A script
       reference replaces a markup one only with a strictly higher
       affinity, so the pass is skipped when none could reach one. The
       remembered JSON responses join this pass under their own bound
       (once the text budget is spent the script walk reads nothing). */
    int reachable = context == NULL ? 0
        : (context->token_count != 0u ? MEDIA_AFFINITY_OWN : 0)
            + (context->container != NULL ? 1 : 0);
    if (context != NULL
        && (!best.found || best.affinity < reachable
            || (want_streaming && best.kind != MEDIA_DISCOVERY_HLS
                && (!streaming.found || streaming.affinity < reachable)))) {
        MediaScriptScan scripts = {
            .primary = {.capacity = url_capacity},
            .streaming = {.capacity = want_streaming ? streaming_capacity : 0},
            .context = context
        };
        bytes += media_scan_executable_scripts(
            root, &scripts, MEDIA_DISCOVERY_TEXT_LIMIT - bytes);
        media_scan_json_responses(document, &scripts);
        if (scripts.primary.found
            && (!best.found || scripts.primary.affinity > best.affinity)
            && media_script_copy(&scripts.primary, url)) {
            best.kind = scripts.primary.kind;
            best.quality = scripts.primary.quality;
            best.affinity = scripts.primary.affinity;
            best.found = true;
        }
        if (want_streaming && scripts.streaming.found
            && (!streaming.found
                || scripts.streaming.affinity > streaming.affinity)
            && media_script_copy(&scripts.streaming, streaming_url))
            streaming.found = true;
    }
    /* Only a different reference is worth reporting. */
    if (want_streaming && best.found && best.kind == MEDIA_DISCOVERY_HLS)
        streaming_url[0] = '\0';
    if (result != NULL) {
        result->kind = best.kind;
        result->quality = best.quality;
        result->inspected_bytes = bytes;
        result->inspected_nodes = nodes;
    }
    return best.found;
}

typedef enum {
    MEDIA_JSON_KEY_NONE = 0,
    MEDIA_JSON_KEY_TYPE,
    MEDIA_JSON_KEY_AUDIO,
    MEDIA_JSON_KEY_CONTENT_URL,
    MEDIA_JSON_KEY_NAME,
    MEDIA_JSON_KEY_URL,
    MEDIA_JSON_KEY_THUMBNAIL,
    MEDIA_JSON_KEY_DURATION,
    MEDIA_JSON_KEY_EMBED_URL
} MediaJsonKey;

typedef struct {
    bool object;
    bool expect_key;
    bool audio_object;
    bool video_object;
    bool music_recording;
    bool forced_audio;
    bool nested_audio_value;
    bool nested_audio_pending;
    MediaJsonKey pending_key;
    /* Arrays retain the object key whose value they contain. This admits
       standard JSON-LD shapes such as `@type:[...]` and `audio:[{...}]`
       without building an object graph. */
    MediaJsonKey array_value_key;
    uint32_t content_begin, content_end;
    uint32_t name_begin, name_end;
    uint32_t page_begin, page_end;
    uint32_t thumbnail_begin, thumbnail_end;
    uint32_t duration_begin, duration_end;
    uint32_t embed_begin, embed_end;
    uint32_t nested_content_begin, nested_content_end;
    uint32_t nested_name_begin, nested_name_end;
    uint32_t nested_page_begin, nested_page_end;
    uint32_t nested_thumbnail_begin, nested_thumbnail_end;
    uint32_t nested_duration_begin, nested_duration_end;
} MediaJsonFrame;

static MediaJsonFrame *media_json_nearest_object(
    MediaJsonFrame *stack, size_t depth)
{
    while (depth != 0) {
        MediaJsonFrame *frame = &stack[--depth];
        if (frame->object) return frame;
    }
    return NULL;
}

static bool media_json_span_is(
    const char *source, size_t begin, size_t end, const char *wanted)
{
    size_t wanted_length = strlen(wanted);
    return end >= begin && end - begin == wanted_length
        && strncasecmp(source + begin, wanted, wanted_length) == 0;
}

static void media_json_note_type(
    MediaJsonFrame *frame, const char *source, uint32_t begin, uint32_t end)
{
    if (frame == NULL) return;
    if (media_json_span_is(source, begin, end, "AudioObject")) {
        frame->audio_object = true;
    } else if (media_json_span_is(source, begin, end, "VideoObject")) {
        frame->video_object = true;
    } else if (media_json_span_is(
                   source, begin, end, "MusicRecording")) {
        frame->music_recording = true;
    }
}

static MediaJsonKey media_json_key(
    const char *source, size_t begin, size_t end)
{
    if (media_json_span_is(source, begin, end, "@type"))
        return MEDIA_JSON_KEY_TYPE;
    if (media_json_span_is(source, begin, end, "audio"))
        return MEDIA_JSON_KEY_AUDIO;
    if (media_json_span_is(source, begin, end, "contentUrl"))
        return MEDIA_JSON_KEY_CONTENT_URL;
    if (media_json_span_is(source, begin, end, "name"))
        return MEDIA_JSON_KEY_NAME;
    if (media_json_span_is(source, begin, end, "url"))
        return MEDIA_JSON_KEY_URL;
    if (media_json_span_is(source, begin, end, "thumbnailUrl")
        || media_json_span_is(source, begin, end, "thumbnail"))
        return MEDIA_JSON_KEY_THUMBNAIL;
    if (media_json_span_is(source, begin, end, "duration"))
        return MEDIA_JSON_KEY_DURATION;
    if (media_json_span_is(source, begin, end, "embedUrl"))
        return MEDIA_JSON_KEY_EMBED_URL;
    return MEDIA_JSON_KEY_NONE;
}

static bool media_json_string_span(
    const char *source, size_t length, size_t *at,
    uint32_t *begin, uint32_t *end)
{
    if (source == NULL || at == NULL || *at >= length
        || source[*at] != '"') return false;
    size_t cursor = *at + 1u;
    size_t start = cursor;
    bool escaped = false;
    for (; cursor < length; cursor++) {
        unsigned char value = (unsigned char) source[cursor];
        if (value < 0x20u) return false;
        if (escaped) {
            escaped = false;
            continue;
        }
        if (value == '\\') {
            escaped = true;
            continue;
        }
        if (value == '"') {
            if (start > UINT32_MAX || cursor > UINT32_MAX) return false;
            *begin = (uint32_t) start;
            *end = (uint32_t) cursor;
            *at = cursor + 1u;
            return true;
        }
    }
    return false;
}

static void media_structured_commit(
    MediaStructuredAudioIndex *index, const char *source,
    const MediaJsonFrame *frame)
{
    if (index == NULL || source == NULL || frame == NULL
        || (!frame->audio_object && !frame->forced_audio)
        || frame->content_end <= frame->content_begin) return;
    MediaDiscoveryKind kind = media_discovery_reference_kind(
        source + frame->content_begin,
        frame->content_end - frame->content_begin);
    /* AudioObject may publish an audio-only MP4 with the generic .mp4
       spelling. Its schema context, not the extension, supplies the kind. */
    if (kind != MEDIA_DISCOVERY_AUDIO_MP4
        && kind != MEDIA_DISCOVERY_MP4) return;
    size_t url_length = frame->content_end - frame->content_begin;
    for (size_t i = 0; i < index->candidate_count; i++) {
        const MediaStructuredAudioCandidate *prior = &index->candidates[i];
        size_t prior_length = prior->url_end - prior->url_begin;
        if (prior_length == url_length
            && memcmp(prior->source + prior->url_begin,
                      source + frame->content_begin, url_length) == 0) {
            return;
        }
    }
    if (index->candidate_count == MEDIA_STRUCTURED_AUDIO_CANDIDATE_LIMIT) {
        index->candidate_overflow++;
        return;
    }
    MediaStructuredAudioCandidate *candidate =
        &index->candidates[index->candidate_count++];
    *candidate = (MediaStructuredAudioCandidate) {
        .source = source,
        .url_begin = frame->content_begin,
        .url_end = frame->content_end,
        .name_begin = frame->name_begin,
        .name_end = frame->name_end,
        .page_begin = frame->page_begin,
        .page_end = frame->page_end,
        .thumbnail_begin = frame->thumbnail_begin,
        .thumbnail_end = frame->thumbnail_end,
        .duration_begin = frame->duration_begin,
        .duration_end = frame->duration_end
    };
}

static void media_structured_video_commit(
    MediaStructuredVideoIndex *index, const char *source,
    const MediaJsonFrame *frame)
{
    if (index == NULL || source == NULL || frame == NULL
        || !frame->video_object
        || frame->content_end <= frame->content_begin) return;
    MediaDiscoveryKind kind = media_discovery_reference_kind(
        source + frame->content_begin,
        frame->content_end - frame->content_begin);
    if (kind != MEDIA_DISCOVERY_MP4 && kind != MEDIA_DISCOVERY_HLS
        && kind != MEDIA_DISCOVERY_WEBM) return;
    size_t url_length = frame->content_end - frame->content_begin;
    for (size_t i = 0; i < index->candidate_count; i++) {
        const MediaStructuredVideoCandidate *prior = &index->candidates[i];
        size_t prior_length = prior->url_end - prior->url_begin;
        if (prior_length == url_length
            && memcmp(prior->source + prior->url_begin,
                      source + frame->content_begin, url_length) == 0) {
            return;
        }
    }
    if (index->candidate_count == MEDIA_STRUCTURED_VIDEO_CANDIDATE_LIMIT) {
        index->candidate_overflow++;
        return;
    }
    MediaStructuredVideoCandidate *candidate =
        &index->candidates[index->candidate_count++];
    *candidate = (MediaStructuredVideoCandidate) {
        .source = source,
        .url_begin = frame->content_begin,
        .url_end = frame->content_end,
        .name_begin = frame->name_begin,
        .name_end = frame->name_end,
        .page_begin = frame->page_begin,
        .page_end = frame->page_end,
        .thumbnail_begin = frame->thumbnail_begin,
        .thumbnail_end = frame->thumbnail_end,
        .duration_begin = frame->duration_begin,
        .duration_end = frame->duration_end
    };
}

static bool media_scan_structured_json(
    const char *source, size_t length,
    MediaStructuredAudioIndex *audio_index,
    MediaStructuredVideoIndex *video_index)
{
    MediaJsonFrame stack[MEDIA_STRUCTURED_JSON_DEPTH_LIMIT] = {0};
    size_t depth = 0;
    bool malformed = false;
    for (size_t at = 0; at < length;) {
        unsigned char value = (unsigned char) source[at];
        if (isspace(value) || value == ':' || value == ',') {
            if (value == ',' && depth != 0 && stack[depth - 1u].object) {
                stack[depth - 1u].expect_key = true;
                stack[depth - 1u].pending_key = MEDIA_JSON_KEY_NONE;
            }
            at++;
            continue;
        }
        if (value == '{' || value == '[') {
            if (depth == MEDIA_STRUCTURED_JSON_DEPTH_LIMIT) {
                malformed = true;
                break;
            }
            bool nested_audio_value = false;
            MediaJsonKey inherited_key = MEDIA_JSON_KEY_NONE;
            if (depth != 0) {
                MediaJsonFrame *parent = &stack[depth - 1u];
                inherited_key = parent->object
                    ? parent->pending_key : parent->array_value_key;
                nested_audio_value = value == '{'
                    && inherited_key == MEDIA_JSON_KEY_AUDIO;
                if (parent->object) {
                    parent->pending_key = MEDIA_JSON_KEY_NONE;
                }
            }
            stack[depth++] = (MediaJsonFrame) {
                .object = value == '{',
                .expect_key = value == '{',
                .nested_audio_value = nested_audio_value,
                .array_value_key = value == '['
                    ? inherited_key : MEDIA_JSON_KEY_NONE
            };
            if (audio_index != NULL) audio_index->inspected_nodes++;
            if (video_index != NULL) video_index->inspected_nodes++;
            at++;
            continue;
        }
        if (value == '}' || value == ']') {
            bool object = value == '}';
            if (depth == 0 || stack[depth - 1u].object != object) {
                malformed = true;
                break;
            }
            if (object) {
                MediaJsonFrame *closed = &stack[depth - 1u];
                if (closed->audio_object && !closed->nested_audio_value) {
                    media_structured_commit(audio_index, source, closed);
                }
                if (closed->video_object && video_index != NULL
                    && video_index->embed_source == NULL
                    && closed->embed_end > closed->embed_begin) {
                    video_index->embed_source = source;
                    video_index->embed_begin = closed->embed_begin;
                    video_index->embed_end = closed->embed_end;
                }
                if (closed->video_object)
                    media_structured_video_commit(video_index, source, closed);
                if (closed->nested_audio_value) {
                    MediaJsonFrame *parent = media_json_nearest_object(
                        stack, depth - 1u);
                    if (parent != NULL) {
                        parent->nested_audio_pending = true;
                        parent->nested_content_begin = closed->content_begin;
                        parent->nested_content_end = closed->content_end;
                        parent->nested_name_begin = closed->name_begin;
                        parent->nested_name_end = closed->name_end;
                        parent->nested_page_begin = closed->page_begin;
                        parent->nested_page_end = closed->page_end;
                        parent->nested_thumbnail_begin =
                            closed->thumbnail_begin;
                        parent->nested_thumbnail_end = closed->thumbnail_end;
                        parent->nested_duration_begin = closed->duration_begin;
                        parent->nested_duration_end = closed->duration_end;
                    }
                }
                if (closed->music_recording
                    && closed->nested_audio_pending) {
                    MediaJsonFrame nested = {
                        .forced_audio = true,
                        .content_begin = closed->nested_content_begin,
                        .content_end = closed->nested_content_end,
                        .name_begin = closed->nested_name_end
                                > closed->nested_name_begin
                            ? closed->nested_name_begin : closed->name_begin,
                        .name_end = closed->nested_name_end
                                > closed->nested_name_begin
                            ? closed->nested_name_end : closed->name_end,
                        .page_begin = closed->nested_page_end
                                > closed->nested_page_begin
                            ? closed->nested_page_begin : closed->page_begin,
                        .page_end = closed->nested_page_end
                                > closed->nested_page_begin
                            ? closed->nested_page_end : closed->page_end,
                        .thumbnail_begin = closed->nested_thumbnail_end
                                > closed->nested_thumbnail_begin
                            ? closed->nested_thumbnail_begin
                            : closed->thumbnail_begin,
                        .thumbnail_end = closed->nested_thumbnail_end
                                > closed->nested_thumbnail_begin
                            ? closed->nested_thumbnail_end
                            : closed->thumbnail_end,
                        .duration_begin = closed->nested_duration_end
                                > closed->nested_duration_begin
                            ? closed->nested_duration_begin
                            : closed->duration_begin,
                        .duration_end = closed->nested_duration_end
                                > closed->nested_duration_begin
                            ? closed->nested_duration_end
                            : closed->duration_end
                    };
                    media_structured_commit(audio_index, source, &nested);
                }
            }
            depth--;
            if (depth != 0 && stack[depth - 1u].object) {
                stack[depth - 1u].pending_key = MEDIA_JSON_KEY_NONE;
            }
            at++;
            continue;
        }
        if (value == '"') {
            uint32_t begin = 0, end = 0;
            if (!media_json_string_span(
                    source, length, &at, &begin, &end)) {
                malformed = true;
                break;
            }
            if (depth == 0) continue;
            MediaJsonFrame *frame = &stack[depth - 1u];
            if (!frame->object) {
                if (frame->array_value_key == MEDIA_JSON_KEY_TYPE) {
                    media_json_note_type(
                        media_json_nearest_object(stack, depth - 1u),
                        source, begin, end);
                }
                continue;
            }
            if (frame->expect_key) {
                frame->pending_key = media_json_key(
                    source, begin, end);
                frame->expect_key = false;
                continue;
            }
            switch (frame->pending_key) {
                case MEDIA_JSON_KEY_TYPE:
                    media_json_note_type(frame, source, begin, end);
                    break;
                case MEDIA_JSON_KEY_CONTENT_URL:
                    frame->content_begin = begin;
                    frame->content_end = end;
                    break;
                case MEDIA_JSON_KEY_NAME:
                    frame->name_begin = begin;
                    frame->name_end = end;
                    break;
                case MEDIA_JSON_KEY_URL:
                    frame->page_begin = begin;
                    frame->page_end = end;
                    break;
                case MEDIA_JSON_KEY_THUMBNAIL:
                    frame->thumbnail_begin = begin;
                    frame->thumbnail_end = end;
                    break;
                case MEDIA_JSON_KEY_DURATION:
                    frame->duration_begin = begin;
                    frame->duration_end = end;
                    break;
                case MEDIA_JSON_KEY_EMBED_URL:
                    frame->embed_begin = begin;
                    frame->embed_end = end;
                    break;
                default:
                    break;
            }
            frame->pending_key = MEDIA_JSON_KEY_NONE;
            continue;
        }
        /* Numbers, booleans and null are irrelevant metadata values. Skip
           one bounded primitive token; any other punctuation is malformed. */
        if (value == '-' || isdigit(value)
            || value == 't' || value == 'f' || value == 'n') {
            while (at < length && !isspace((unsigned char) source[at])
                   && source[at] != ',' && source[at] != ']'
                   && source[at] != '}') at++;
            if (depth != 0 && stack[depth - 1u].object) {
                stack[depth - 1u].pending_key = MEDIA_JSON_KEY_NONE;
            }
            continue;
        }
        malformed = true;
        break;
    }
    if (depth != 0) malformed = true;
    if (malformed && audio_index != NULL) audio_index->malformed_scripts++;
    if (malformed && video_index != NULL) video_index->malformed_scripts++;
    return !malformed;
}

/* Both public indexes retain different candidate capacities, but use the
   same bounded script walk. Select counter addresses once, outside the walk;
   do not cast one index layout to the other or add per-node callbacks. */
static bool media_discover_structured_scripts(
    const PocDocument *document, MediaStructuredAudioIndex *audio,
    MediaStructuredVideoIndex *video)
{
    if (audio == NULL && video == NULL) return false;
    if (audio != NULL) memset(audio, 0, sizeof(*audio));
    else memset(video, 0, sizeof(*video));
    if (document == NULL || document->html == NULL) return false;
    size_t *inspected_bytes = audio != NULL
        ? &audio->inspected_bytes : &video->inspected_bytes;
    size_t *inspected_nodes = audio != NULL
        ? &audio->inspected_nodes : &video->inspected_nodes;
    size_t *malformed_scripts = audio != NULL
        ? &audio->malformed_scripts : &video->malformed_scripts;
    size_t *truncated_scripts = audio != NULL
        ? &audio->truncated_scripts : &video->truncated_scripts;
    lxb_dom_node_t *root = lxb_dom_interface_node(document->html);
    size_t dom_nodes = 0;
    for (lxb_dom_node_t *node = root;
         node != NULL && dom_nodes < MEDIA_DISCOVERY_NODE_LIMIT
         && *inspected_bytes < MEDIA_DISCOVERY_TEXT_LIMIT;
         node = media_walk_next(root, node)) {
        dom_nodes++;
        if (node->type != LXB_DOM_NODE_TYPE_ELEMENT
            || !media_name_is(node, "script")) continue;
        bool strict_json = false;
        if (!media_script_is_data(node, &strict_json)) continue;
        for (lxb_dom_node_t *child = node->first_child; child != NULL;
             child = child->next) {
            size_t length = 0;
            const char *text = document_text_data(child, &length);
            if (text == NULL || length == 0) continue;
            size_t remaining = MEDIA_DISCOVERY_TEXT_LIMIT
                - *inspected_bytes;
            size_t admitted = length < remaining ? length : remaining;
            size_t malformed_before = *malformed_scripts;
            (void) media_scan_structured_json(
                text, admitted, audio, video);
            /* Assignment-only object literals are useful data carriers but
               are not JSON and may legally contain unquoted identifiers.
               Keep them fail-soft without calling that expected syntax a
               malformed JSON document in diagnostics. */
            if (!strict_json) {
                *malformed_scripts = malformed_before;
            }
            *inspected_bytes += admitted;
            if (admitted != length) (*truncated_scripts)++;
            if (*inspected_bytes == MEDIA_DISCOVERY_TEXT_LIMIT) break;
        }
    }
    /* DOM visits and JSON containers are separate useful diagnostics. Keep
       the public node count as their bounded sum. */
    *inspected_nodes += dom_nodes;
    return audio != NULL ? audio->candidate_count != 0
                         : video->candidate_count != 0;
}

bool media_discover_structured_audio(
    const PocDocument *document, MediaStructuredAudioIndex *index)
{
    return media_discover_structured_scripts(document, index, NULL);
}

bool media_discover_structured_video(
    const PocDocument *document, MediaStructuredVideoIndex *index)
{
    return media_discover_structured_scripts(document, NULL, index);
}

static bool media_copy_bounded_text(
    char *output, size_t capacity, const char *value, size_t length)
{
    if (output == NULL || capacity == 0u) return false;
    output[0] = '\0';
    if (value == NULL || length == 0u || length >= capacity) return false;
    memcpy(output, value, length);
    output[length] = '\0';
    return true;
}

static bool media_metadata_name_is(
    lxb_dom_node_t *node, const char *wanted)
{
    size_t length = 0;
    const char *value = document_attribute(node, "property", &length);
    if (value == NULL || length == 0u)
        value = document_attribute(node, "name", &length);
    return value != NULL && length == strlen(wanted)
        && strncasecmp(value, wanted, length) == 0;
}

/* A declared player page: an absolute or scheme-relative http(s) address
   that names no media file (a direct file is media, not a page). */
static bool media_embed_url_admitted(const char *value, size_t length)
{
    while (length != 0u && isspace((unsigned char) *value)) {
        value++;
        length--;
    }
    while (length != 0u && isspace((unsigned char) value[length - 1u]))
        length--;
    if (length == 0u || length >= MEDIA_DECLARED_VIDEO_URL_CAPACITY
        || media_discovery_reference_kind(value, length)
               != MEDIA_DISCOVERY_NONE) return false;
    bool scheme = (length > 8u && strncasecmp(value, "https://", 8u) == 0)
        || (length > 7u && strncasecmp(value, "http://", 7u) == 0)
        || (length > 2u && value[0] == '/' && value[1] == '/');
    if (!scheme) return false;
    for (size_t i = 0; i < length; i++) {
        unsigned char c = (unsigned char) value[i];
        if (c <= 0x20u || c == '"' || c == '<' || c == '>' || c == '\\')
            return false;
    }
    return true;
}

static void media_declared_embed_offer(
    MediaDeclaredVideo *declared, const char *value, size_t length)
{
    if (declared->embed_url[0] != '\0' || value == NULL) return;
    while (length != 0u && isspace((unsigned char) *value)) {
        value++;
        length--;
    }
    while (length != 0u && isspace((unsigned char) value[length - 1u]))
        length--;
    if (media_embed_url_admitted(value, length))
        (void) media_copy_bounded_text(
            declared->embed_url, sizeof(declared->embed_url), value, length);
}

static bool media_discover_declared_video_uncached(
    const PocDocument *document, MediaDeclaredVideo *declared)
{
    if (declared == NULL) return false;
    memset(declared, 0, sizeof(*declared));
    if (document == NULL || document->html == NULL) return false;

    MediaStructuredVideoIndex structured = {0};
    bool structured_found = false;
    bool structured_scanned =
        media_discover_structured_video(document, &structured);
    if (structured.embed_source != NULL) {
        char embed[MEDIA_DECLARED_VIDEO_URL_CAPACITY];
        size_t copied = media_copy_json_string(
            embed, sizeof(embed), structured.embed_source,
            structured.embed_begin, structured.embed_end);
        if (copied != 0u) media_declared_embed_offer(declared, embed, copied);
    }
    if (structured_scanned) {
        /* The page's identity from its canonical URL, og:url and og:image:
           a VideoObject naming the page's video beats other clips' ones. */
        MediaDiscoveryContext context;
        media_context_init(&context, document, NULL, NULL);
        MediaDiscoveryKind kind = MEDIA_DISCOVERY_NONE;
        unsigned quality = 0;
        int affinity = 0;
        size_t selected = media_structured_video_select(
            &structured, &context, sizeof(declared->media_url), false,
            &kind, &quality, &affinity);
        if (selected != SIZE_MAX
            && media_copy_json_string(
                   declared->media_url, sizeof(declared->media_url),
                   structured.candidates[selected].source,
                   structured.candidates[selected].url_begin,
                   structured.candidates[selected].url_end) != 0u) {
            const MediaStructuredVideoCandidate *candidate =
                &structured.candidates[selected];
            declared->kind = kind;
            declared->quality = quality;
            declared->structured = true;
            (void) media_structured_video_copy_name(
                candidate, declared->title, sizeof(declared->title));
            (void) media_structured_video_copy_thumbnail(
                candidate, declared->thumbnail_url,
                sizeof(declared->thumbnail_url));
            (void) media_structured_video_copy_duration(
                candidate, declared->duration,
                sizeof(declared->duration));
            structured_found = true;
        }
    }

    char og_url[MEDIA_DECLARED_VIDEO_URL_CAPACITY];
    MediaDiscoveryBest og_best = {
        .url = structured_found ? og_url : declared->media_url,
        .capacity = structured_found ? sizeof(og_url)
                                     : sizeof(declared->media_url)
    };
    const char *og_embed = NULL, *twitter_player = NULL;
    size_t og_embed_length = 0u, twitter_player_length = 0u;
    bool og_type_media = false;
    lxb_dom_node_t *root = lxb_dom_interface_node(document->html);
    size_t nodes = 0u;
    for (lxb_dom_node_t *node = root;
         node != NULL && nodes++ < MEDIA_DISCOVERY_NODE_LIMIT;
         node = media_walk_next(root, node)) {
        if (node->type != LXB_DOM_NODE_TYPE_ELEMENT
            || !media_name_is(node, "meta")) continue;
        size_t length = 0u;
        const char *content = document_attribute(node, "content", &length);
        if (content == NULL || length == 0u) continue;
        bool og_video = media_metadata_name_is(node, "og:video")
            || media_metadata_name_is(node, "og:video:url")
            || media_metadata_name_is(node, "og:video:secure_url");
        /* An og:video naming no media file is an HTML player unless its
           og:video:type names a video; twitter:player always is one. */
        if (og_video && og_embed == NULL
            && media_discovery_reference_kind(content, length)
                   == MEDIA_DISCOVERY_NONE) {
            og_embed = content;
            og_embed_length = length;
        } else if (twitter_player == NULL
                   && media_metadata_name_is(node, "twitter:player")) {
            twitter_player = content;
            twitter_player_length = length;
        } else if (media_metadata_name_is(node, "og:video:type")
                   && length >= 6u
                   && (strncasecmp(content, "video/", 6u) == 0
                       || media_rendition_format(content, length)
                              != MEDIA_DISCOVERY_NONE)) {
            og_type_media = true;
        }
        if (!structured_found && og_video) {
            media_consider_span(&og_best, content, length, 0u, length,
                                (MediaReferenceSite) {0}, NULL, NULL);
        } else if (declared->thumbnail_url[0] == '\0'
                   && document_meta_image_content(node, &(size_t) {0}) != NULL) {
            (void) media_copy_bounded_text(
                declared->thumbnail_url, sizeof(declared->thumbnail_url),
                content, length);
        } else if (declared->title[0] == '\0'
                   && media_metadata_name_is(node, "og:title")) {
            (void) media_copy_bounded_text(
                declared->title, sizeof(declared->title), content, length);
        } else if (declared->duration[0] == '\0'
                   && (media_metadata_name_is(node, "video:duration")
                       || media_metadata_name_is(
                              node, "og:video:duration"))) {
            (void) media_copy_bounded_text(
                declared->duration, sizeof(declared->duration),
                content, length);
        } else if (media_metadata_name_is(node, "og:type")
                   && length >= sizeof("video") - 1u
                   && strncasecmp(content, "video", sizeof("video") - 1u)
                          == 0
                   && (length == sizeof("video") - 1u
                       || content[sizeof("video") - 1u] == '.')) {
            declared->og_type_video = true;
        }
    }
    if (!og_type_media)
        media_declared_embed_offer(declared, og_embed, og_embed_length);
    media_declared_embed_offer(
        declared, twitter_player, twitter_player_length);
    if (structured_found) return true;
    if (og_best.found) {
        declared->kind = og_best.kind;
        declared->quality = og_best.quality;
        declared->og_video = true;
        return true;
    }
    if (declared->og_type_video) {
        MediaDiscoveryResult result = {0};
        if (media_discover_document_candidate(
                document, declared->media_url,
                sizeof(declared->media_url), &result)) {
            declared->kind = result.kind;
            declared->quality = result.quality;
            return true;
        }
    }
    /* No media: a declared player page alone is kept, with the title,
       thumbnail and duration read beside it, but reported as no declared
       video (see media_declared_video_or_embed_cached). */
    if (declared->embed_url[0] != '\0') {
        declared->media_url[0] = '\0';
        declared->kind = MEDIA_DISCOVERY_NONE;
        declared->quality = 0u;
        declared->structured = false;
        declared->og_video = false;
        return false;
    }
    memset(declared, 0, sizeof(*declared));
    return false;
}

/* One scan into a new record: NULL when the document declares neither a
   video nor a player page, or when the record cannot be allocated. */
static MediaDeclaredVideoCache *media_declared_video_record_scan(
    const PocDocument *document)
{
    MediaDeclaredVideoCache *record = budget_calloc_category(
        document->budget, BUDGET_CATEGORY_RESOURCE, 1u, sizeof(*record));
    if (record == NULL) return NULL;
    record->found = media_discover_declared_video_uncached(
        document, &record->selected);
    if (!record->found && record->selected.embed_url[0] == '\0') {
        budget_free(document->budget, record);
        return NULL;
    }
    MediaDiscoveryContext head;
    memset(&head, 0, sizeof(head));
    media_context_add_head(&head, document);
    record->identity_count = (uint8_t) head.token_count;
    for (size_t i = 0; i < head.token_count; i++) {
        memcpy(record->identity[i], head.tokens[i],
               sizeof(record->identity[i]));
        record->identity_lengths[i] = head.token_lengths[i];
        record->identity_numeric[i] = head.token_numeric[i];
        record->identity_delimited[i] = head.token_delimited[i];
    }
    return record;
}

/* The rescan only lost part of the kept declaration (a script removed
   some of it): it names no media or player-page address the kept one
   does not, and lacks one the kept one has. */
static bool media_declared_video_kept_covers(
    const MediaDeclaredVideoCache *kept, const MediaDeclaredVideoCache *live)
{
    const MediaDeclaredVideo *k = &kept->selected, *l = &live->selected;
    if (l->media_url[0] != '\0' && strcmp(l->media_url, k->media_url) != 0)
        return false;
    if (l->embed_url[0] != '\0' && strcmp(l->embed_url, k->embed_url) != 0)
        return false;
    return (k->media_url[0] != '\0' && l->media_url[0] == '\0')
        || (k->embed_url[0] != '\0' && l->embed_url[0] == '\0');
}

const MediaDeclaredVideo *media_declared_video_cached(
    const PocDocument *document)
{
    if (document == NULL || document->html == NULL
        || document->budget == NULL) return NULL;
    /* This is a derived page cache, analogous to body_text. Callers expose a
       const document because discovery does not mutate authored content. */
    PocDocument *mutable_document = (PocDocument *) (uintptr_t) document;
    if (!mutable_document->declared_video_cache_scanned) {
        mutable_document->declared_video_cache_scanned = true;
        if (mutable_document->declared_video_discovery_count != UINT32_MAX)
            mutable_document->declared_video_discovery_count++;
        MediaDeclaredVideoCache *kept = mutable_document->declared_video_cache;
        MediaDeclaredVideoCache *live =
            media_declared_video_record_scan(document);
        /* Live declarations win unless a script only removed some of the
           kept ones; with none at all (or no memory) the kept one stays. */
        if (live != NULL
            && (kept == NULL || !media_declared_video_kept_covers(kept, live))) {
            if (kept != NULL && live->identity_count == 0u) {
                live->identity_count = kept->identity_count;
                memcpy(live->identity, kept->identity, sizeof(live->identity));
                memcpy(live->identity_lengths, kept->identity_lengths,
                       sizeof(live->identity_lengths));
                memcpy(live->identity_numeric, kept->identity_numeric,
                       sizeof(live->identity_numeric));
                memcpy(live->identity_delimited, kept->identity_delimited,
                       sizeof(live->identity_delimited));
            }
            budget_free(mutable_document->budget, kept);
            mutable_document->declared_video_cache = live;
        } else if (live != NULL) {
            budget_free(mutable_document->budget, live);
        }
        mutable_document->declared_video_cache_found =
            mutable_document->declared_video_cache != NULL
            && mutable_document->declared_video_cache->found;
    }
    return mutable_document->declared_video_cache_found
        && mutable_document->declared_video_cache != NULL
        ? &mutable_document->declared_video_cache->selected : NULL;
}

const MediaDeclaredVideo *media_declared_video_or_embed_cached(
    const PocDocument *document)
{
    const MediaDeclaredVideo *declared =
        media_declared_video_cached(document);
    if (declared != NULL || document == NULL) return declared;
    const MediaDeclaredVideoCache *cache = document->declared_video_cache;
    return cache != NULL && cache->selected.embed_url[0] != '\0'
        ? &cache->selected : NULL;
}

bool media_discover_declared_video(
    const PocDocument *document, MediaDeclaredVideo *declared)
{
    if (declared == NULL) return false;
    memset(declared, 0, sizeof(*declared));
    const MediaDeclaredVideo *cached =
        media_declared_video_cached(document);
    if (cached == NULL) return false;
    *declared = *cached;
    return true;
}

size_t media_declared_video_discovery_count(const PocDocument *document)
{
    return document == NULL ? 0u
        : document->declared_video_discovery_count;
}

void media_declared_video_cache_destroy(PocDocument *document)
{
    if (document == NULL || document->budget == NULL) return;
    budget_free(document->budget, document->declared_video_cache);
    document->declared_video_cache = NULL;
    document->declared_video_cache_scanned = false;
    document->declared_video_cache_found = false;
}

void media_declared_video_cache_invalidate(PocDocument *document)
{
    if (document == NULL) return;
    document->declared_video_cache_scanned = false;
    document->declared_video_cache_found = false;
}

static bool media_text_mentions_video_object(const char *text, size_t length)
{
    static const char wanted[] = "videoobject";
    const size_t wanted_length = sizeof(wanted) - 1u;
    for (size_t i = 0; i + wanted_length <= length; i++) {
        if ((text[i] | 0x20) == 'v'
            && strncasecmp(text + i, wanted, wanted_length) == 0)
            return true;
    }
    return false;
}

/* A data script whose text names a VideoObject, reading at most what is
   left of *bytes' share of MEDIA_DISCOVERY_TEXT_LIMIT. */
static bool media_data_script_names_video(lxb_dom_node_t *script,
                                          size_t *bytes)
{
    if (!media_script_is_data(script, NULL)) return false;
    for (lxb_dom_node_t *child = script->first_child; child != NULL;
         child = child->next) {
        size_t length = 0u;
        const char *text = document_text_data(child, &length);
        if (text == NULL || length == 0u) continue;
        size_t remaining = MEDIA_DISCOVERY_TEXT_LIMIT - *bytes;
        if (length > remaining) length = remaining;
        *bytes += length;
        if (media_text_mentions_video_object(text, length)) return true;
    }
    return false;
}

/* Cheap evidence that a declaration scan can find something: a video
   Open Graph or player-card meta, or a data script naming a VideoObject. */
static bool media_declarations_present(const PocDocument *document)
{
    lxb_dom_node_t *root = lxb_dom_interface_node(document->html);
    size_t nodes = 0u, bytes = 0u;
    for (lxb_dom_node_t *node = root;
         node != NULL && nodes++ < MEDIA_DISCOVERY_NODE_LIMIT
         && bytes < MEDIA_DISCOVERY_TEXT_LIMIT;
         node = media_walk_next(root, node)) {
        if (node->type != LXB_DOM_NODE_TYPE_ELEMENT) continue;
        if (media_name_is(node, "meta")) {
            size_t length = 0u;
            const char *type = media_metadata_name_is(node, "og:type")
                ? document_attribute(node, "content", &length) : NULL;
            if (media_metadata_name_is(node, "og:video")
                || media_metadata_name_is(node, "og:video:url")
                || media_metadata_name_is(node, "og:video:secure_url")
                || media_metadata_name_is(node, "twitter:player")
                || (type != NULL && length >= 5u
                    && strncasecmp(type, "video", 5u) == 0))
                return true;
            continue;
        }
        if (media_name_is(node, "script")
            && media_data_script_names_video(node, &bytes)) return true;
    }
    return false;
}

/* Whole-document keep scans one page may take for declarations that
   arrive in its body (media_declared_video_keep_closed). */
#define MEDIA_DECLARED_KEEP_SCANS 4u

void media_declared_video_keep_parsed(PocDocument *document)
{
    if (document == NULL || document->html == NULL
        || document->budget == NULL || document->declared_video_kept_checked)
        return;
    document->declared_video_kept_checked = true;
    if (document->declared_video_cache != NULL
        || document->declared_video_cache_scanned
        || !media_declarations_present(document)) return;
    /* Kept only: the parser has not finished, so this is no lookup. */
    document->declared_video_cache = media_declared_video_record_scan(document);
}

void media_declared_video_keep_closed(PocDocument *document,
                                      lxb_dom_node_t *script)
{
    size_t bytes = 0u;
    if (document == NULL || script == NULL || document->html == NULL
        || document->budget == NULL || document->declared_video_cache != NULL
        || document->declared_video_cache_scanned
        || document->declared_video_keep_scans >= MEDIA_DECLARED_KEEP_SCANS
        || !media_data_script_names_video(script, &bytes)) return;
    /* In the head, the head-close keep (with the head's identity tokens
       complete) takes it. */
    for (lxb_dom_node_t *at = script->parent; at != NULL; at = at->parent)
        if (at->type == LXB_DOM_NODE_TYPE_ELEMENT && media_name_is(at, "head")
            && !document->declared_video_kept_checked) return;
    document->declared_video_keep_scans++;
    document->declared_video_cache = media_declared_video_record_scan(document);
}

/* The memo must never be what brings a page near its ceiling: a reference
   is kept only while this much stays free beyond it. */
#define MEDIA_JSON_RESPONSE_HEADROOM (128u * 1024u)

static bool media_json_mentions_media(const char *text, size_t length)
{
    for (size_t i = 0; i < length; i++) {
        if ((text[i] | 0x20) != 'm') continue;
        size_t rest = length - i;
        if ((rest >= 3u && strncasecmp(text + i, "mp4", 3u) == 0)
            || (rest >= 4u && strncasecmp(text + i, "m3u8", 4u) == 0)
            || (rest >= 7u && strncasecmp(text + i, "mpegurl", 7u) == 0))
            return true;
    }
    return false;
}

static void media_json_keep(
    PocDocument *document, const char *raw, size_t length,
    MediaDiscoveryKind kind, unsigned quality, bool own, bool marked)
{
    if (length == 0u || length >= NAVIGATION_URL_LIMIT) return;
    size_t size = sizeof(struct MediaJsonResponse) + length + 1u;
    size_t remaining = budget_remaining(document->budget);
    if (remaining < MEDIA_JSON_RESPONSE_HEADROOM
        || remaining - MEDIA_JSON_RESPONSE_HEADROOM < size) return;
    struct MediaJsonResponse *entry = budget_malloc_category(
        document->budget, BUDGET_CATEGORY_RESOURCE, size);
    if (entry == NULL) return;
    entry->url_length = (uint16_t) length;
    entry->quality = (uint16_t) (quality > UINT16_MAX ? 0u : quality);
    entry->kind = (uint8_t) kind;
    entry->own = own;
    entry->marked = marked;
    memcpy(entry->url, raw, length);
    entry->url[length] = '\0';
    size_t slot = document->media_json_response_next
        % MEDIA_JSON_RESPONSE_SLOTS;
    budget_free(document->budget, document->media_json_responses[slot]);
    document->media_json_responses[slot] = entry;
    document->media_json_response_next =
        (uint8_t) ((slot + 1u) % MEDIA_JSON_RESPONSE_SLOTS);
    document->media_json_response_generation++;
}

void media_json_response_remember(
    PocDocument *document, const char *document_url,
    const char *response_url, long status, const char *body, size_t length,
    bool player_page)
{
    if (document == NULL || document->budget == NULL
        || document->html == NULL || body == NULL || status < 200
        || status > 299 || document_url == NULL || response_url == NULL)
        return;
    if (length >= 3u && memcmp(body, "\xef\xbb\xbf", 3u) == 0) {
        body += 3;
        length -= 3u;
    }
    while (length != 0u && isspace((unsigned char) *body)) {
        body++;
        length--;
    }
    size_t url_length = strnlen(response_url, NAVIGATION_URL_LIMIT);
    if (length > MEDIA_JSON_RESPONSE_BYTES) length = MEDIA_JSON_RESPONSE_BYTES;
    bool playlist = length >= 7u && memcmp(body, "#EXTM3U", 7u) == 0;
    if (length == 0u || url_length == 0u || url_length >= NAVIGATION_URL_LIMIT
        || (!playlist && ((*body != '{' && *body != '[')
                          || !media_json_mentions_media(body, length))))
        return;
    MediaDiscoveryContext context;
    media_context_init(&context, document, document_url, NULL);
    if (player_page) media_identity_add_address(&context, document_url);
    bool own = media_reference_matches_identity(
        &context, response_url, url_length);
    size_t path_begin = 0, path_end = 0;
    media_reference_path(response_url, url_length, &path_begin, &path_end);
    bool marked = media_text_marked(
        response_url + path_begin, path_end - path_begin);
    if (playlist) {
        /* The playlist's own address: the page fetched it to play it. */
        if (media_path_excluded(response_url, url_length)) return;
        media_json_keep(document, response_url, url_length,
                        MEDIA_DISCOVERY_HLS, 0u, own,
                        marked || media_path_stand_in(
                                      response_url, url_length));
        return;
    }
    MediaRenditionWalk walk = {
        .context = &context, .source = body
    };
    JsonCursor json = {body, body + length};
    (void) media_rendition_value(
        &walk, &json, MEDIA_RENDITION_DEPTH, 0u, marked, own ? 1 : 0);
    for (size_t i = 0; i < walk.best_count; i++) {
        const MediaRenditionPick *pick = &walk.best[i];
        media_json_keep(document, body + pick->begin, pick->end - pick->begin,
                        pick->kind, pick->quality, pick->own, pick->marked);
    }
    if (walk.have_stream
        && (walk.best_count == 0u || walk.stream.begin != walk.best[0].begin)
        && (walk.best_count < 2u || walk.stream.begin != walk.best[1].begin))
        media_json_keep(document, body + walk.stream.begin,
                        walk.stream.end - walk.stream.begin, walk.stream.kind,
                        walk.stream.quality, walk.stream.own,
                        walk.stream.marked);
}

size_t media_json_responses_release(PocDocument *document)
{
    if (document == NULL) return 0u;
    size_t released = 0;
    for (size_t i = 0; i < MEDIA_JSON_RESPONSE_SLOTS; i++) {
        struct MediaJsonResponse *entry = document->media_json_responses[i];
        if (entry == NULL) continue;
        released += sizeof(*entry) + entry->url_length + 1u;
        budget_free(document->budget, entry);
        document->media_json_responses[i] = NULL;
    }
    document->media_json_response_next = 0;
    return released;
}

uint32_t media_json_response_generation(const PocDocument *document)
{
    return document == NULL ? 0u : document->media_json_response_generation;
}

static bool media_json_rank_equal(
    const struct MediaJsonResponse *a, const struct MediaJsonResponse *b)
{
    return a->kind == b->kind && a->quality == b->quality;
}

/* Among equals the https copy goes first: an http one would be mixed
   content on a secure page. */
static bool media_json_https(const struct MediaJsonResponse *entry)
{
    return entry->url_length > 6u
        && strncasecmp(entry->url, "https:", 6u) == 0;
}

bool media_json_own_candidate(
    const PocDocument *document, size_t ordinal, char *url, size_t capacity,
    MediaDiscoveryKind *kind)
{
    if (url != NULL && capacity != 0u) url[0] = '\0';
    if (document == NULL || url == NULL || capacity < 2u) return false;
    /* Rank the own, unmarked references (at most the ring's few) and take
       the ordinal-th; equal ranks keep arrival order. */
    const struct MediaJsonResponse *ranked[MEDIA_JSON_RESPONSE_SLOTS];
    size_t count = 0;
    for (size_t n = 0; n < MEDIA_JSON_RESPONSE_SLOTS; n++) {
        size_t i = (document->media_json_response_next + n)
            % MEDIA_JSON_RESPONSE_SLOTS;
        const struct MediaJsonResponse *entry =
            document->media_json_responses[i];
        if (entry == NULL || !entry->own || entry->marked) continue;
        size_t at = count++;
        while (at > 0u
               && (media_rank_better(
                       true, 0, (MediaDiscoveryKind) ranked[at - 1u]->kind,
                       ranked[at - 1u]->quality, 0,
                       (MediaDiscoveryKind) entry->kind, entry->quality)
                   || (media_json_rank_equal(ranked[at - 1u], entry)
                       && media_json_https(entry)
                       && !media_json_https(ranked[at - 1u])))) {
            ranked[at] = ranked[at - 1u];
            at--;
        }
        ranked[at] = entry;
    }
    if (ordinal >= count) return false;
    const struct MediaJsonResponse *entry = ranked[ordinal];
    if (media_copy_json_string(url, capacity, entry->url, 0u,
                               entry->url_length) == 0u)
        return false;
    if (kind != NULL) *kind = (MediaDiscoveryKind) entry->kind;
    return true;
}

static bool media_structured_copy_span(
    const MediaStructuredAudioCandidate *candidate,
    uint32_t begin, uint32_t end, char *output, size_t capacity)
{
    if (output != NULL && capacity != 0) output[0] = '\0';
    if (candidate == NULL || candidate->source == NULL || output == NULL
        || capacity < 2u || end <= begin) return false;
    return media_copy_json_string(
        output, capacity, candidate->source, begin, end) != 0;
}

bool media_structured_audio_copy_url(
    const MediaStructuredAudioCandidate *candidate,
    char *output, size_t capacity)
{
    return candidate != NULL && media_structured_copy_span(
        candidate, candidate->url_begin, candidate->url_end,
        output, capacity);
}

bool media_structured_audio_copy_name(
    const MediaStructuredAudioCandidate *candidate,
    char *output, size_t capacity)
{
    return candidate != NULL && media_structured_copy_span(
        candidate, candidate->name_begin, candidate->name_end,
        output, capacity);
}

bool media_structured_audio_copy_page_url(
    const MediaStructuredAudioCandidate *candidate,
    char *output, size_t capacity)
{
    return candidate != NULL && media_structured_copy_span(
        candidate, candidate->page_begin, candidate->page_end,
        output, capacity);
}

bool media_structured_video_copy_url(
    const MediaStructuredVideoCandidate *candidate,
    char *output, size_t capacity)
{
    return candidate != NULL && media_structured_copy_span(
        candidate, candidate->url_begin, candidate->url_end,
        output, capacity);
}

bool media_structured_video_copy_name(
    const MediaStructuredVideoCandidate *candidate,
    char *output, size_t capacity)
{
    return candidate != NULL && media_structured_copy_span(
        candidate, candidate->name_begin, candidate->name_end,
        output, capacity);
}

bool media_structured_video_copy_thumbnail(
    const MediaStructuredVideoCandidate *candidate,
    char *output, size_t capacity)
{
    return candidate != NULL && media_structured_copy_span(
        candidate, candidate->thumbnail_begin, candidate->thumbnail_end,
        output, capacity);
}

bool media_structured_video_copy_duration(
    const MediaStructuredVideoCandidate *candidate,
    char *output, size_t capacity)
{
    return candidate != NULL && media_structured_copy_span(
        candidate, candidate->duration_begin, candidate->duration_end,
        output, capacity);
}
