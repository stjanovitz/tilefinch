#include "tilefinch/media_hls.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "swdec/swdec_ts.h"
#include "tilefinch/url.h"

#define HLS_SAMPLE_LIMIT 64u
#define HLS_QUEUE_MIXED_BYTES (576u * 1024u)
#define HLS_QUEUE_VIDEO_BYTES (512u * 1024u)
#define HLS_QUEUE_AUDIO_BYTES (64u * 1024u)
#define HLS_ADTS_PARSE_SLICE_BYTES (4u * 1024u)
#define HLS_TS_PARSE_SLICE_BYTES 188u
#define HLS_PARAMETER_SET_BYTES 4096u
#define HLS_LIVE_EDGE_SEGMENTS 3u
#define HLS_LIVE_REFRESH_RUNWAY_SEGMENTS 2u
#define HLS_LIVE_REFRESH_MINIMUM_US UINT64_C(1000000)
#define HLS_LIVE_STALL_LIMIT_US UINT64_C(60000000)
#define HLS_MASTER_GROUP_BYTES 64u
#define HLS_STREAM_LINE_BYTES 4096u

typedef enum {
    HLS_ENTRY_SEGMENT = 0,
    HLS_ENTRY_VARIANT,
    HLS_ENTRY_AUDIO_RENDITION
} HlsEntryKind;

typedef struct {
    uint32_t text_offset;
    uint32_t start_ms;
    uint16_t text_length;
    uint16_t width;
    uint16_t height;
    uint32_t duration_ms;
    uint32_t bandwidth;
    uint64_t sequence;
    char audio_group[HLS_MASTER_GROUP_BYTES];
    HlsEntryKind kind;
    bool codecs_compatible;
    bool discontinuity;
    bool default_rendition;
} HlsEntry;

struct MediaHlsPlaylist {
    Budget *budget;
    char *text;
    size_t text_length;
    char base_url[4096];
    HlsEntry entries[MEDIA_HLS_MAXIMUM_SEGMENTS];
    size_t entry_count;
    uint64_t duration_us;
    uint64_t media_sequence;
    uint32_t target_duration_ms;
    MediaHlsPlaylistKind kind;
    bool end_list;
};

typedef struct {
    char url[HLS_STREAM_LINE_BYTES];
    uint64_t sequence;
    uint32_t duration_ms;
    bool discontinuity;
} HlsStreamSegment;

struct MediaHlsPlaylistStream {
    Budget *budget;
    char playlist_url[4096];
    unsigned char raw[MEDIA_HLS_MAXIMUM_PLAYLIST_BYTES];
    size_t raw_length;
    char line[HLS_STREAM_LINE_BYTES];
    size_t line_length;
    HlsStreamSegment segments[MEDIA_HLS_RETAINED_LIVE_SEGMENTS];
    size_t segment_head;
    size_t segment_count;
    size_t bytes_seen;
    uint64_t media_sequence;
    uint64_t next_sequence;
    uint32_t target_duration_ms;
    uint32_t pending_duration_ms;
    bool saw_header;
    bool saw_master;
    bool saw_media;
    bool saw_end_list;
    bool pending_duration;
    bool pending_discontinuity;
    bool raw_overflow;
    bool failed;
};

typedef struct {
    uint64_t identity;
    uint64_t raw_pts90k;
    uint32_t duration90k;
    uint32_t payload_offset;
    uint32_t payload_length;
    MediaMp4TrackKind kind;
    uint8_t packet_format;
    bool keyframe;
} HlsQueuedSample;

typedef struct {
    uint64_t handle;
    size_t segment;
} HlsRequest;

typedef enum {
    HLS_AUDIO_SEGMENT_UNKNOWN = 0,
    HLS_AUDIO_SEGMENT_TS,
    HLS_AUDIO_SEGMENT_ADTS
} HlsAudioSegmentKind;

struct MediaHlsSource {
    Budget *budget;
    MediaHlsPlaylist *playlist;
    MediaHlsTransport transport;
    SwdecTs *ts;
    unsigned char *queue_bytes;
    size_t queue_capacity;
    unsigned char *transport_chunk;
    HlsQueuedSample queue[HLS_SAMPLE_LIMIT];
    size_t queue_head;
    size_t queue_count;
    size_t queue_read;
    size_t queue_write;
    size_t queue_used;
    uint64_t next_identity;
    HlsRequest requests[2];
    MediaHlsPlaylist *pending_playlist;
    size_t segment_index;
    size_t prepared_segment;
    size_t transport_tail;
    size_t transport_tail_capacity;
    HlsAudioSegmentKind audio_segment_kind;
    bool ended;
    bool failed;
    bool track_layout_known;
    bool seek_pristine;
    size_t seek_segment;
    bool video_info_valid;
    bool audio_info_valid;
    bool have_sps;
    bool have_pps;
    bool segment_origin_valid;
    uint64_t segment_origin90k;
    uint64_t segment_base90k;
    uint64_t live_timeline90k;
    uint64_t next_sequence;
    uint64_t refresh_next_us;
    uint64_t live_edge_since_us;
    uint64_t failed_sequence;
    uint64_t last_video_pts90k;
    uint64_t last_audio_pts90k;
    uint32_t audio_duration90k;
    MediaMp4TrackInfo video_info;
    MediaMp4TrackInfo audio_info;
    unsigned char parameter_sets[HLS_PARAMETER_SET_BYTES];
    size_t parameter_set_bytes;
    unsigned live_segment_failures;
    bool failed_sequence_valid;
    bool live_session;
    bool force_refresh;
    MediaHlsTrackSelection track_selection;
    unsigned request_limit;
    char error[256];
    MediaHlsStats stats;
};

static void hls_error(char *error, size_t error_size,
                      const char *format, ...)
{
    if (error == NULL || error_size == 0) return;
    va_list arguments;
    va_start(arguments, format);
    vsnprintf(error, error_size, format, arguments);
    va_end(arguments);
}

static void hls_source_fail(MediaHlsSource *source,
                            const char *format, ...)
{
    if (source == NULL || source->failed) return;
    va_list arguments;
    va_start(arguments, format);
    vsnprintf(source->error, sizeof(source->error), format, arguments);
    va_end(arguments);
    source->failed = true;
}

static bool hls_parse_decimal_ms(const char *text, uint32_t *milliseconds)
{
    if (text == NULL || milliseconds == NULL) return false;
    uint32_t whole = 0, fraction = 0, scale = 100u;
    bool any = false;
    while (*text >= '0' && *text <= '9') {
        unsigned digit = (unsigned) (*text++ - '0');
        if (whole > (UINT32_MAX - digit) / 10u) return false;
        whole = whole * 10u + digit;
        any = true;
    }
    if (*text == '.') {
        text++;
        while (*text >= '0' && *text <= '9' && scale != 0u) {
            fraction += (uint32_t) (*text++ - '0') * scale;
            scale /= 10u;
        }
        while (*text >= '0' && *text <= '9') text++;
    }
    if (!any || whole > (UINT32_MAX - fraction) / 1000u) return false;
    *milliseconds = whole * 1000u + fraction;
    return true;
}

static bool hls_attribute_unsigned(const char *line, const char *name,
                                   uint32_t *value)
{
    if (line == NULL || name == NULL || value == NULL) return false;
    size_t name_length = strlen(name);
    const char *at = line;
    for (;;) {
        at = strstr(at, name);
        if (at == NULL) return false;
        bool left_boundary = at == line || at[-1] == ','
            || at[-1] == ' ' || at[-1] == '\t';
        if (left_boundary && at[name_length] == '=') break;
        at += name_length;
    }
    at += name_length + 1u;
    uint32_t result = 0;
    bool any = false;
    while (*at >= '0' && *at <= '9') {
        unsigned digit = (unsigned) (*at++ - '0');
        if (result > (UINT32_MAX - digit) / 10u) return false;
        result = result * 10u + digit;
        any = true;
    }
    if (!any) return false;
    *value = result;
    return true;
}

static bool hls_attribute_text(char *line, const char *name,
                               char **value, size_t *length)
{
    if (line == NULL || name == NULL || value == NULL || length == NULL)
        return false;
    size_t name_length = strlen(name);
    char *at = line;
    for (;;) {
        at = strstr(at, name);
        if (at == NULL) return false;
        bool left_boundary = at == line || at[-1] == ','
            || at[-1] == ' ' || at[-1] == '\t';
        if (left_boundary && at[name_length] == '=') break;
        at += name_length;
    }
    at += name_length + 1u;
    char quote = (*at == '\'' || *at == '"') ? *at++ : '\0';
    char *end = at;
    while (*end != '\0'
           && (quote != '\0' ? *end != quote : *end != ',')) end++;
    if (end == at || (quote != '\0' && *end != quote)) return false;
    *value = at;
    *length = (size_t) (end - at);
    return true;
}

static bool hls_attribute_is(char *line, const char *name,
                             const char *expected)
{
    char *value = NULL;
    size_t length = 0;
    return hls_attribute_text(line, name, &value, &length)
        && strlen(expected) == length
        && strncmp(value, expected, length) == 0;
}

static bool hls_line_u64(const char *line, const char *prefix,
                         uint64_t *value)
{
    size_t prefix_length = strlen(prefix);
    if (line == NULL || value == NULL
        || strncmp(line, prefix, prefix_length) != 0) return false;
    const char *at = line + prefix_length;
    uint64_t result = 0;
    bool any = false;
    while (*at >= '0' && *at <= '9') {
        uint64_t digit = (uint64_t) (*at++ - '0');
        if (result > (UINT64_MAX - digit) / 10u) return false;
        result = result * 10u + digit;
        any = true;
    }
    if (!any || *at != '\0') return false;
    *value = result;
    return true;
}

static void hls_variant_geometry(const char *line,
                                 uint16_t *width, uint16_t *height)
{
    *width = 0;
    *height = 0;
    const char *at = strstr(line, "RESOLUTION=");
    if (at == NULL) return;
    at += strlen("RESOLUTION=");
    uint32_t w = 0, h = 0;
    while (*at >= '0' && *at <= '9') {
        w = w * 10u + (unsigned) (*at++ - '0');
        if (w > UINT16_MAX) return;
    }
    if (*at++ != 'x') return;
    while (*at >= '0' && *at <= '9') {
        h = h * 10u + (unsigned) (*at++ - '0');
        if (h > UINT16_MAX) return;
    }
    if (w != 0 && h != 0) {
        *width = (uint16_t) w;
        *height = (uint16_t) h;
    }
}

static bool hls_variant_codecs_compatible(const char *line)
{
    const char *at = strstr(line, "CODECS=");
    if (at == NULL) return true;
    at += strlen("CODECS=");
    char quote = (*at == '\'' || *at == '"') ? *at++ : '\0';
    const char *end = at;
    while (*end != '\0'
           && (quote != '\0' ? *end != quote : *end != ',')) end++;
    /* The standard firmware route consumes Annex-B AVC and AAC-LC. YouTube
       commonly lists an HE-AAC 240p rendition immediately before an AAC-LC
       rendition with otherwise identical video geometry, so accepting AVC
       alone can select a stream the audio backend must later reject. Missing
       audio metadata remains admissible and is verified by the ADTS probe. */
    bool saw_avc = false, saw_audio = false, saw_aac_lc = false;
    static const char aac_lc[] = "mp4a.40.2";
    for (const char *token = at; token < end; token++) {
        if ((token[0] == 'a' || token[0] == 'A')
            && token + 4u <= end
            && (token[1] == 'v' || token[1] == 'V')
            && (token[2] == 'c' || token[2] == 'C')
            && (token[3] == '1' || token[3] == '3')) saw_avc = true;
        if (token + 4u <= end
            && (token[0] == 'm' || token[0] == 'M')
            && (token[1] == 'p' || token[1] == 'P')
            && token[2] == '4' && (token[3] == 'a' || token[3] == 'A')) {
            saw_audio = true;
            size_t codec_length = sizeof(aac_lc) - 1u;
            if (token + codec_length <= end) {
                bool equal = true;
                for (size_t i = 0; i < codec_length; i++) {
                    char value = token[i];
                    if (value >= 'A' && value <= 'Z')
                        value = (char) (value - 'A' + 'a');
                    if (value != aac_lc[i]) { equal = false; break; }
                }
                char after = token + codec_length == end
                    ? '\0' : token[codec_length];
                if (equal && (after == '\0' || after == quote
                              || after == ','
                              || after == ' ' || after == '\t'))
                    saw_aac_lc = true;
            }
        }
    }
    return saw_avc && (!saw_audio || saw_aac_lc);
}

MediaHlsPlaylist *media_hls_playlist_parse(
    Budget *budget, const char *playlist_url,
    const unsigned char *bytes, size_t length,
    char *error, size_t error_size)
{
    if (error != NULL && error_size != 0) error[0] = '\0';
    if (budget == NULL || playlist_url == NULL || bytes == NULL
        || length == 0 || length > MEDIA_HLS_MAXIMUM_PLAYLIST_BYTES) {
        hls_error(error, error_size, "HLS playlist exceeds its bound");
        return NULL;
    }
    MediaHlsPlaylist *playlist = budget_calloc_category(
        budget, BUDGET_CATEGORY_NAVIGATION, 1, sizeof(*playlist));
    char *copy = budget_malloc_category(
        budget, BUDGET_CATEGORY_RESOURCE, length + 1u);
    if (playlist == NULL || copy == NULL) {
        budget_free(budget, copy);
        budget_free(budget, playlist);
        hls_error(error, error_size, "HLS playlist exceeds memory budget");
        return NULL;
    }
    memcpy(copy, bytes, length);
    copy[length] = '\0';
    playlist->budget = budget;
    playlist->text = copy;
    playlist->text_length = length;
    snprintf(playlist->base_url, sizeof(playlist->base_url), "%s",
             playlist_url);

    bool header = false, pending_variant = false;
    bool pending_discontinuity = false, pending_duration_valid = false;
    bool pending_codecs_compatible = true;
    char pending_audio_group[HLS_MASTER_GROUP_BYTES] = {0};
    uint32_t pending_duration = 0, pending_bandwidth = 0;
    uint32_t total_duration_ms = 0;
    uint16_t pending_width = 0, pending_height = 0;
    char *cursor = copy;
    char *end = copy + length;
    while (cursor < end) {
        char *line = cursor;
        char *newline = memchr(cursor, '\n', (size_t) (end - cursor));
        char *line_end = newline == NULL ? end : newline;
        while (line_end > line
               && (line_end[-1] == '\r' || line_end[-1] == ' '
                   || line_end[-1] == '\t')) line_end--;
        while (line < line_end && (*line == ' ' || *line == '\t')) line++;
        if (line_end < end) *line_end = '\0';
        size_t line_length = (size_t) (line_end - line);
        cursor = newline == NULL ? end : newline + 1u;
        if (line_length == 0) continue;
        if (!header) {
            if (strcmp(line, "#EXTM3U") != 0) {
                hls_error(error, error_size, "HLS playlist lacks EXTM3U");
                media_hls_playlist_destroy(playlist);
                return NULL;
            }
            header = true;
            continue;
        }
        if (strncmp(line, "#EXT-X-STREAM-INF:", 18u) == 0) {
            pending_variant = true;
            pending_bandwidth = 0;
            (void) hls_attribute_unsigned(
                line + 18u, "BANDWIDTH", &pending_bandwidth);
            hls_variant_geometry(
                line + 18u, &pending_width, &pending_height);
            /* Store this on the following URI entry, like geometry and
               bandwidth. */
            pending_codecs_compatible =
                hls_variant_codecs_compatible(line + 18u);
            char *group = NULL;
            size_t group_length = 0;
            pending_audio_group[0] = '\0';
            if (hls_attribute_text(
                    line + 18u, "AUDIO", &group, &group_length)) {
                if (group_length >= sizeof(pending_audio_group)) {
                    hls_error(error, error_size,
                              "HLS audio group exceeds its bound");
                    media_hls_playlist_destroy(playlist);
                    return NULL;
                }
                memcpy(pending_audio_group, group, group_length);
                pending_audio_group[group_length] = '\0';
            }
            continue;
        }
        if (strncmp(line, "#EXT-X-MEDIA:", 13u) == 0
            && hls_attribute_is(line + 13u, "TYPE", "AUDIO")) {
            char *group = NULL, *uri = NULL;
            size_t group_length = 0, uri_length = 0;
            if (!hls_attribute_text(
                    line + 13u, "GROUP-ID", &group, &group_length)
                || !hls_attribute_text(
                    line + 13u, "URI", &uri, &uri_length)
                || group_length >= HLS_MASTER_GROUP_BYTES
                || uri_length > UINT16_MAX
                || playlist->entry_count >= MEDIA_HLS_MAXIMUM_SEGMENTS) {
                hls_error(error, error_size,
                          "invalid HLS audio rendition");
                media_hls_playlist_destroy(playlist);
                return NULL;
            }
            HlsEntry *entry = &playlist->entries[playlist->entry_count++];
            entry->kind = HLS_ENTRY_AUDIO_RENDITION;
            entry->text_offset = (uint32_t) (uri - copy);
            entry->text_length = (uint16_t) uri_length;
            entry->default_rendition =
                hls_attribute_is(line + 13u, "DEFAULT", "YES");
            memcpy(entry->audio_group, group, group_length);
            entry->audio_group[group_length] = '\0';
            /* The URI lives inside this retained mutable line. Terminate it
               so URL resolution cannot consume the following attributes. */
            uri[uri_length] = '\0';
            playlist->kind = MEDIA_HLS_PLAYLIST_MASTER;
            continue;
        }
        if (strncmp(line, "#EXTINF:", 8u) == 0) {
            if (!hls_parse_decimal_ms(line + 8u, &pending_duration)) {
                hls_error(error, error_size, "invalid HLS segment duration");
                media_hls_playlist_destroy(playlist);
                return NULL;
            }
            pending_duration_valid = true;
            continue;
        }
        if (strncmp(line, "#EXT-X-MEDIA-SEQUENCE:", 22u) == 0) {
            if (!hls_line_u64(
                    line, "#EXT-X-MEDIA-SEQUENCE:",
                    &playlist->media_sequence)) {
                hls_error(error, error_size,
                          "invalid HLS media sequence");
                media_hls_playlist_destroy(playlist);
                return NULL;
            }
            continue;
        }
        if (strncmp(line, "#EXT-X-TARGETDURATION:", 22u) == 0) {
            uint64_t seconds = 0;
            if (!hls_line_u64(
                    line, "#EXT-X-TARGETDURATION:", &seconds)
                || seconds == 0 || seconds > UINT32_MAX / 1000u) {
                hls_error(error, error_size,
                          "invalid HLS target duration");
                media_hls_playlist_destroy(playlist);
                return NULL;
            }
            playlist->target_duration_ms = (uint32_t) seconds * 1000u;
            continue;
        }
        if (strcmp(line, "#EXT-X-DISCONTINUITY") == 0) {
            pending_discontinuity = true;
            continue;
        }
        if (strcmp(line, "#EXT-X-ENDLIST") == 0) {
            playlist->end_list = true;
            continue;
        }
        if (strncmp(line, "#EXT-X-KEY:", 11u) == 0
            && strstr(line + 11u, "METHOD=NONE") == NULL) {
            hls_error(error, error_size, "encrypted HLS is unsupported");
            media_hls_playlist_destroy(playlist);
            return NULL;
        }
        if (strncmp(line, "#EXT-X-MAP:", 11u) == 0
            || strncmp(line, "#EXT-X-BYTERANGE:", 17u) == 0) {
            hls_error(error, error_size,
                      "fragmented or byte-range HLS is unsupported");
            media_hls_playlist_destroy(playlist);
            return NULL;
        }
        if (*line == '#') continue;
        if (line_length > UINT16_MAX
            || playlist->entry_count >= MEDIA_HLS_MAXIMUM_SEGMENTS) {
            hls_error(error, error_size, "HLS playlist has too many entries");
            media_hls_playlist_destroy(playlist);
            return NULL;
        }
        HlsEntry *entry = &playlist->entries[playlist->entry_count++];
        entry->text_offset = (uint32_t) (line - copy);
        entry->text_length = (uint16_t) line_length;
        if (pending_variant) {
            if (playlist->kind == MEDIA_HLS_PLAYLIST_MEDIA
                && playlist->entry_count > 1u) {
                hls_error(error, error_size, "mixed HLS playlist forms");
                media_hls_playlist_destroy(playlist);
                return NULL;
            }
            playlist->kind = MEDIA_HLS_PLAYLIST_MASTER;
            entry->kind = HLS_ENTRY_VARIANT;
            entry->bandwidth = pending_bandwidth;
            entry->width = pending_width;
            entry->height = pending_height;
            entry->codecs_compatible = pending_codecs_compatible;
            snprintf(entry->audio_group, sizeof(entry->audio_group), "%s",
                     pending_audio_group);
            pending_variant = false;
            pending_codecs_compatible = true;
            pending_audio_group[0] = '\0';
        } else {
            if (playlist->kind == MEDIA_HLS_PLAYLIST_MASTER) {
                hls_error(error, error_size, "master HLS entry lacks metadata");
                media_hls_playlist_destroy(playlist);
                return NULL;
            }
            if (!pending_duration_valid
                || pending_duration > UINT32_MAX - total_duration_ms) {
                hls_error(error, error_size,
                          pending_duration_valid
                              ? "HLS duration exceeds its bound"
                              : "HLS segment lacks EXTINF");
                media_hls_playlist_destroy(playlist);
                return NULL;
            }
            playlist->kind = MEDIA_HLS_PLAYLIST_MEDIA;
            entry->kind = HLS_ENTRY_SEGMENT;
            entry->sequence = playlist->entry_count - 1u;
            entry->start_ms = total_duration_ms;
            entry->duration_ms = pending_duration;
            entry->discontinuity = pending_discontinuity;
            total_duration_ms += pending_duration;
            playlist->duration_us =
                (uint64_t) total_duration_ms * UINT64_C(1000);
            pending_duration = 0;
            pending_duration_valid = false;
            pending_discontinuity = false;
        }
    }
    if (!header || playlist->entry_count == 0
        || (playlist->kind == MEDIA_HLS_PLAYLIST_MEDIA
            && !playlist->end_list
            && playlist->target_duration_ms == 0)) {
        hls_error(error, error_size,
                  playlist->entry_count == 0
                      ? "HLS playlist is empty"
                      : "live HLS playlist lacks target duration");
        media_hls_playlist_destroy(playlist);
        return NULL;
    }
    if (playlist->kind == MEDIA_HLS_PLAYLIST_MEDIA) {
        for (size_t i = 0; i < playlist->entry_count; i++) {
            if (i > UINT64_MAX - playlist->media_sequence) {
                hls_error(error, error_size,
                          "HLS media sequence exceeds its bound");
                media_hls_playlist_destroy(playlist);
                return NULL;
            }
            playlist->entries[i].sequence =
                playlist->media_sequence + (uint64_t) i;
        }
    }
    return playlist;
}

MediaHlsPlaylistKind media_hls_playlist_kind(const MediaHlsPlaylist *playlist)
{
    return playlist == NULL ? MEDIA_HLS_PLAYLIST_MEDIA : playlist->kind;
}

static size_t hls_playlist_select_variant_index(
    const MediaHlsPlaylist *playlist, unsigned maximum_width,
    unsigned maximum_height, unsigned target_height)
{
    if (playlist == NULL || playlist->kind != MEDIA_HLS_PLAYLIST_MASTER
        ) return SIZE_MAX;
    size_t selected = SIZE_MAX;
    unsigned selected_class = UINT32_MAX;
    uint64_t selected_cost = UINT64_MAX;
    for (size_t i = 0; i < playlist->entry_count; i++) {
        const HlsEntry *entry = &playlist->entries[i];
        if (entry->kind != HLS_ENTRY_VARIANT) continue;
        if (!entry->codecs_compatible) continue;
        if (entry->width != 0 && entry->height != 0
            && (entry->width > maximum_width
                || entry->height > maximum_height)) continue;
        unsigned candidate_class;
        uint64_t bandwidth_cost = entry->bandwidth == 0
            ? UINT32_MAX : entry->bandwidth;
        uint64_t cost;
        if (entry->height == 0) {
            candidate_class = 2u;
            cost = bandwidth_cost;
        } else if (entry->height >= target_height) {
            candidate_class = 0u;
            cost = ((uint64_t) entry->height << 32u) | bandwidth_cost;
        } else {
            candidate_class = 1u;
            cost = ((uint64_t) (maximum_height - entry->height) << 32u)
                 | bandwidth_cost;
        }
        if (selected == SIZE_MAX || candidate_class < selected_class
            || (candidate_class == selected_class && cost < selected_cost)) {
            selected = i;
            selected_class = candidate_class;
            selected_cost = cost;
        }
    }
    return selected;
}

bool media_hls_playlist_select_variant(
    const MediaHlsPlaylist *playlist, unsigned maximum_width,
    unsigned maximum_height, unsigned target_height,
    char *url, size_t url_size)
{
    if (url == NULL || url_size == 0) return false;
    size_t selected = hls_playlist_select_variant_index(
        playlist, maximum_width, maximum_height, target_height);
    if (selected == SIZE_MAX) return false;
    return tilefinch_url_resolve(
        playlist->base_url,
        playlist->text + playlist->entries[selected].text_offset,
        url, url_size);
}

bool media_hls_playlist_select_streams(
    const MediaHlsPlaylist *playlist, unsigned maximum_width,
    unsigned maximum_height, unsigned target_height,
    char *video_url, size_t video_url_size,
    char *audio_url, size_t audio_url_size)
{
    if (video_url == NULL || video_url_size == 0
        || audio_url == NULL || audio_url_size == 0) return false;
    audio_url[0] = '\0';
    size_t selected = hls_playlist_select_variant_index(
        playlist, maximum_width, maximum_height, target_height);
    if (selected == SIZE_MAX) return false;
    const HlsEntry *entry = &playlist->entries[selected];
    const char *reference = playlist->text + entry->text_offset;
    if (!tilefinch_url_resolve(
            playlist->base_url, reference,
            video_url, video_url_size)) return false;
    if (entry->audio_group[0] == '\0') return true;
    size_t selected_audio = SIZE_MAX;
    for (size_t i = 0; i < playlist->entry_count; i++) {
        const HlsEntry *candidate = &playlist->entries[i];
        if (candidate->kind != HLS_ENTRY_AUDIO_RENDITION
            || strcmp(candidate->audio_group, entry->audio_group) != 0)
            continue;
        if (selected_audio == SIZE_MAX || candidate->default_rendition) {
            selected_audio = i;
            if (candidate->default_rendition) break;
        }
    }
    if (selected_audio == SIZE_MAX) return false;
    return tilefinch_url_resolve(
        playlist->base_url,
        playlist->text + playlist->entries[selected_audio].text_offset,
        audio_url, audio_url_size);
}

size_t media_hls_playlist_segment_count(const MediaHlsPlaylist *playlist)
{
    return playlist == NULL || playlist->kind != MEDIA_HLS_PLAYLIST_MEDIA
        ? 0 : playlist->entry_count;
}

uint64_t media_hls_playlist_duration_us(const MediaHlsPlaylist *playlist)
{
    return playlist == NULL ? 0 : playlist->duration_us;
}

bool media_hls_playlist_is_live(const MediaHlsPlaylist *playlist)
{
    return playlist != NULL
        && playlist->kind == MEDIA_HLS_PLAYLIST_MEDIA
        && !playlist->end_list;
}

void media_hls_playlist_destroy(MediaHlsPlaylist *playlist)
{
    if (playlist == NULL) return;
    Budget *budget = playlist->budget;
    budget_free(budget, playlist->text);
    budget_free(budget, playlist);
}

static bool hls_stream_append_raw(MediaHlsPlaylistStream *stream,
                                  const char *line, size_t length)
{
    if (stream->raw_overflow) return true;
    if (length > sizeof(stream->raw) - stream->raw_length
        || 1u > sizeof(stream->raw) - stream->raw_length - length) {
        stream->raw_overflow = true;
        return true;
    }
    memcpy(stream->raw + stream->raw_length, line, length);
    stream->raw_length += length;
    stream->raw[stream->raw_length++] = '\n';
    return true;
}

static bool hls_stream_process_line(MediaHlsPlaylistStream *stream,
                                    char *error, size_t error_size)
{
    char *line = stream->line;
    size_t length = stream->line_length;
    while (length != 0
           && (line[length - 1u] == '\r' || line[length - 1u] == ' '
               || line[length - 1u] == '\t')) length--;
    while (length != 0 && (*line == ' ' || *line == '\t')) {
        line++;
        length--;
    }
    line[length] = '\0';
    if (length == 0) return true;
    (void) hls_stream_append_raw(stream, line, length);
    if (!stream->saw_header) {
        if (strcmp(line, "#EXTM3U") != 0) {
            hls_error(error, error_size, "HLS playlist lacks EXTM3U");
            return false;
        }
        stream->saw_header = true;
        return true;
    }
    if (strncmp(line, "#EXT-X-STREAM-INF:", 18u) == 0
        || strncmp(line, "#EXT-X-MEDIA:", 13u) == 0) {
        stream->saw_master = true;
        if (stream->raw_overflow) {
            hls_error(error, error_size,
                      "malformed HLS master exceeds retained bound");
            return false;
        }
        return true;
    }
    if (strncmp(line, "#EXT-X-MEDIA-SEQUENCE:", 22u) == 0) {
        uint64_t value = 0;
        if (!hls_line_u64(line, "#EXT-X-MEDIA-SEQUENCE:", &value)) {
            hls_error(error, error_size, "invalid HLS media sequence");
            return false;
        }
        stream->media_sequence = value;
        stream->next_sequence = value;
        return true;
    }
    if (strncmp(line, "#EXT-X-TARGETDURATION:", 22u) == 0) {
        uint64_t seconds = 0;
        if (!hls_line_u64(line, "#EXT-X-TARGETDURATION:", &seconds)
            || seconds == 0 || seconds > UINT32_MAX / 1000u) {
            hls_error(error, error_size, "invalid HLS target duration");
            return false;
        }
        stream->target_duration_ms = (uint32_t) seconds * 1000u;
        return true;
    }
    if (strncmp(line, "#EXTINF:", 8u) == 0) {
        if (!hls_parse_decimal_ms(line + 8u,
                                  &stream->pending_duration_ms)) {
            hls_error(error, error_size, "invalid HLS segment duration");
            return false;
        }
        stream->pending_duration = true;
        stream->saw_media = true;
        return true;
    }
    if (strcmp(line, "#EXT-X-DISCONTINUITY") == 0) {
        stream->pending_discontinuity = true;
        return true;
    }
    if (strcmp(line, "#EXT-X-ENDLIST") == 0) {
        stream->saw_end_list = true;
        return true;
    }
    if (strncmp(line, "#EXT-X-KEY:", 11u) == 0
        && strstr(line + 11u, "METHOD=NONE") == NULL) {
        hls_error(error, error_size, "encrypted HLS is unsupported");
        return false;
    }
    if (strncmp(line, "#EXT-X-MAP:", 11u) == 0
        || strncmp(line, "#EXT-X-BYTERANGE:", 17u) == 0) {
        hls_error(error, error_size,
                  "fragmented or byte-range HLS is unsupported");
        return false;
    }
    if (*line == '#') return true;
    if (!stream->pending_duration) return true;
    size_t slot;
    if (stream->segment_count < MEDIA_HLS_RETAINED_LIVE_SEGMENTS) {
        slot = (stream->segment_head + stream->segment_count)
            % MEDIA_HLS_RETAINED_LIVE_SEGMENTS;
        stream->segment_count++;
    } else {
        slot = stream->segment_head;
        stream->segment_head = (stream->segment_head + 1u)
            % MEDIA_HLS_RETAINED_LIVE_SEGMENTS;
    }
    HlsStreamSegment *segment = &stream->segments[slot];
    if (length >= sizeof(segment->url)) {
        hls_error(error, error_size, "HLS segment URL exceeds its bound");
        return false;
    }
    memcpy(segment->url, line, length + 1u);
    segment->sequence = stream->next_sequence;
    segment->duration_ms = stream->pending_duration_ms;
    segment->discontinuity = stream->pending_discontinuity;
    if (stream->next_sequence != UINT64_MAX) stream->next_sequence++;
    stream->pending_duration = false;
    stream->pending_discontinuity = false;
    return true;
}

MediaHlsPlaylistStream *media_hls_playlist_stream_create(
    Budget *budget, const char *playlist_url,
    char *error, size_t error_size)
{
    if (error != NULL && error_size != 0) error[0] = '\0';
    if (budget == NULL || playlist_url == NULL
        || strlen(playlist_url) >= 4096u) {
        hls_error(error, error_size, "invalid HLS playlist stream");
        return NULL;
    }
    MediaHlsPlaylistStream *stream = budget_calloc_category(
        budget, BUDGET_CATEGORY_NAVIGATION, 1, sizeof(*stream));
    if (stream == NULL) {
        hls_error(error, error_size,
                  "HLS playlist stream exceeds memory budget");
        return NULL;
    }
    stream->budget = budget;
    snprintf(stream->playlist_url, sizeof(stream->playlist_url), "%s",
             playlist_url);
    return stream;
}

bool media_hls_playlist_stream_feed(
    MediaHlsPlaylistStream *stream,
    const unsigned char *bytes, size_t length,
    char *error, size_t error_size)
{
    if (error != NULL && error_size != 0) error[0] = '\0';
    if (stream == NULL || stream->failed
        || (bytes == NULL && length != 0)
        || length > MEDIA_HLS_MAXIMUM_PLAYLIST_DOWNLOAD_BYTES
                      - stream->bytes_seen) {
        hls_error(error, error_size,
                  stream != NULL && stream->failed
                      ? "HLS playlist stream already failed"
                      : "HLS playlist download exceeds its bound");
        if (stream != NULL) stream->failed = true;
        return false;
    }
    stream->bytes_seen += length;
    for (size_t i = 0; i < length; i++) {
        unsigned char byte = bytes[i];
        if (byte == '\n') {
            if (!hls_stream_process_line(stream, error, error_size)) {
                stream->failed = true;
                return false;
            }
            stream->line_length = 0;
            continue;
        }
        if (stream->line_length >= sizeof(stream->line) - 1u) {
            hls_error(error, error_size,
                      "HLS playlist line exceeds its bound");
            stream->failed = true;
            return false;
        }
        stream->line[stream->line_length++] = (char) byte;
    }
    return true;
}

static bool hls_stream_output_append(char *output, size_t capacity,
                                     size_t *length, const char *format, ...)
{
    if (*length >= capacity) return false;
    va_list arguments;
    va_start(arguments, format);
    int written = vsnprintf(
        output + *length, capacity - *length, format, arguments);
    va_end(arguments);
    if (written < 0 || (size_t) written >= capacity - *length) return false;
    *length += (size_t) written;
    return true;
}

MediaHlsPlaylist *media_hls_playlist_stream_finish(
    MediaHlsPlaylistStream *stream, char *error, size_t error_size)
{
    if (error != NULL && error_size != 0) error[0] = '\0';
    if (stream == NULL || stream->failed) {
        hls_error(error, error_size, "HLS playlist stream failed");
        return NULL;
    }
    if (stream->line_length != 0) {
        if (!hls_stream_process_line(stream, error, error_size)) {
            stream->failed = true;
            return NULL;
        }
        stream->line_length = 0;
    }
    if (!stream->raw_overflow) {
        return media_hls_playlist_parse(
            stream->budget, stream->playlist_url,
            stream->raw, stream->raw_length, error, error_size);
    }
    if (stream->saw_master || stream->saw_end_list || !stream->saw_media
        || stream->segment_count == 0) {
        hls_error(error, error_size,
                  stream->saw_master
                      ? "malformed HLS master exceeds retained bound"
                      : stream->saw_end_list
                          ? "HLS VOD playlist exceeds retained bound"
                      : "oversized HLS media playlist has no segments");
        return NULL;
    }
    char *normalized = budget_malloc_category(
        stream->budget, BUDGET_CATEGORY_RESOURCE,
        MEDIA_HLS_MAXIMUM_PLAYLIST_BYTES);
    if (normalized == NULL) {
        hls_error(error, error_size,
                  "HLS compact playlist exceeds memory budget");
        return NULL;
    }
    size_t length = 0;
    size_t first = stream->segment_head;
    uint64_t first_sequence = stream->segments[first].sequence;
    bool ok = hls_stream_output_append(
        normalized, MEDIA_HLS_MAXIMUM_PLAYLIST_BYTES, &length,
        "#EXTM3U\n#EXT-X-MEDIA-SEQUENCE:%llu\n",
        (unsigned long long) first_sequence);
    if (ok && !stream->saw_end_list) {
        ok = stream->target_duration_ms != 0
            && hls_stream_output_append(
                normalized, MEDIA_HLS_MAXIMUM_PLAYLIST_BYTES, &length,
                "#EXT-X-TARGETDURATION:%u\n",
                stream->target_duration_ms / 1000u);
    }
    for (size_t i = 0; ok && i < stream->segment_count; i++) {
        const HlsStreamSegment *segment = &stream->segments[
            (stream->segment_head + i) % MEDIA_HLS_RETAINED_LIVE_SEGMENTS];
        if (segment->discontinuity)
            ok = hls_stream_output_append(
                normalized, MEDIA_HLS_MAXIMUM_PLAYLIST_BYTES, &length,
                "#EXT-X-DISCONTINUITY\n");
        if (ok) ok = hls_stream_output_append(
            normalized, MEDIA_HLS_MAXIMUM_PLAYLIST_BYTES, &length,
            "#EXTINF:%u.%03u,\n%s\n",
            segment->duration_ms / 1000u,
            segment->duration_ms % 1000u, segment->url);
    }
    if (ok && stream->saw_end_list) ok = hls_stream_output_append(
        normalized, MEDIA_HLS_MAXIMUM_PLAYLIST_BYTES, &length,
        "#EXT-X-ENDLIST\n");
    MediaHlsPlaylist *playlist = ok ? media_hls_playlist_parse(
        stream->budget, stream->playlist_url,
        (const unsigned char *) normalized, length,
        error, error_size) : NULL;
    if (!ok) hls_error(error, error_size,
                       "HLS compact playlist exceeds retained bound");
    budget_free(stream->budget, normalized);
    return playlist;
}

size_t media_hls_playlist_stream_bytes_seen(
    const MediaHlsPlaylistStream *stream)
{
    return stream == NULL ? 0 : stream->bytes_seen;
}

bool media_hls_playlist_stream_was_compacted(
    const MediaHlsPlaylistStream *stream)
{
    return stream != NULL && stream->raw_overflow;
}

void media_hls_playlist_stream_destroy(MediaHlsPlaylistStream *stream)
{
    if (stream == NULL) return;
    Budget *budget = stream->budget;
    memset(stream, 0, sizeof(*stream));
    budget_free(budget, stream);
}

static bool hls_queue_write(MediaHlsSource *source,
                            const unsigned char *data, size_t length,
                            uint32_t *offset)
{
    if (length > source->queue_capacity - source->queue_used) return false;
    *offset = (uint32_t) source->queue_write;
    size_t first = source->queue_capacity - source->queue_write;
    if (first > length) first = length;
    memcpy(source->queue_bytes + source->queue_write, data, first);
    memcpy(source->queue_bytes, data + first, length - first);
    source->queue_write = (source->queue_write + length)
        % source->queue_capacity;
    source->queue_used += length;
    return true;
}

static bool hls_queue_push(MediaHlsSource *source, MediaMp4TrackKind kind,
                           uint8_t format, const unsigned char *data,
                           size_t length, uint64_t pts90k,
                           uint32_t duration90k, bool keyframe)
{
    if (source == NULL || source->failed) return false;
    if (source->queue_count >= HLS_SAMPLE_LIMIT
        || length > UINT32_MAX) {
        source->stats.queue_overflows++;
        hls_source_fail(source, "HLS sample queue exceeded its bound");
        return false;
    }
    size_t tail = (source->queue_head + source->queue_count)
        % HLS_SAMPLE_LIMIT;
    HlsQueuedSample *sample = &source->queue[tail];
    uint32_t offset = 0;
    if (!hls_queue_write(source, data, length, &offset)) {
        source->stats.queue_overflows++;
        hls_source_fail(source, "HLS payload queue exceeded its bound");
        return false;
    }
    *sample = (HlsQueuedSample) {
        .identity = ++source->next_identity,
        .raw_pts90k = pts90k,
        .duration90k = duration90k,
        .payload_offset = offset,
        .payload_length = (uint32_t) length,
        .kind = kind,
        .packet_format = format,
        .keyframe = keyframe
    };
    source->queue_count++;
    source->stats.queued_samples = source->queue_count;
    source->stats.queued_bytes = source->queue_used;
    return true;
}

static bool hls_queue_has_kind(const MediaHlsSource *source,
                               MediaMp4TrackKind kind)
{
    if (source == NULL) return false;
    for (size_t i = 0; i < source->queue_count; i++) {
        size_t at = (source->queue_head + i) % HLS_SAMPLE_LIMIT;
        if (source->queue[at].kind == kind) return true;
    }
    return false;
}

static bool hls_annexb_info(MediaHlsSource *source,
                            const unsigned char *data, size_t length,
                            bool *keyframe)
{
    size_t cursor = 0;
    bool found_sps = source->video_info_valid;
    *keyframe = false;
    while (cursor + 4u <= length) {
        size_t start = SIZE_MAX, prefix = 0;
        for (size_t i = cursor; i + 3u < length; i++) {
            if (data[i] == 0 && data[i + 1u] == 0
                && data[i + 2u] == 1u) {
                start = i; prefix = 3u; break;
            }
            if (i + 4u < length && data[i] == 0 && data[i + 1u] == 0
                && data[i + 2u] == 0 && data[i + 3u] == 1u) {
                start = i; prefix = 4u; break;
            }
        }
        if (start == SIZE_MAX || start + prefix >= length) break;
        size_t nal_start = start + prefix;
        size_t next = length;
        for (size_t i = nal_start + 1u; i + 3u < length; i++) {
            if (data[i] == 0 && data[i + 1u] == 0
                && (data[i + 2u] == 1u
                    || (i + 3u < length && data[i + 2u] == 0
                        && data[i + 3u] == 1u))) {
                next = i;
                break;
            }
        }
        size_t nal_length = next - nal_start;
        unsigned type = data[nal_start] & 0x1fu;
        if (type == 5u) *keyframe = true;
        bool retain_parameter_set = (type == 7u && !source->have_sps)
            || (type == 8u && !source->have_pps);
        if (retain_parameter_set
            && source->parameter_set_bytes + 4u + nal_length
                   <= sizeof(source->parameter_sets)) {
            unsigned char *out = source->parameter_sets
                + source->parameter_set_bytes;
            out[0] = 0; out[1] = 0; out[2] = 0; out[3] = 1;
            memcpy(out + 4u, data + nal_start, nal_length);
            source->parameter_set_bytes += 4u + nal_length;
            if (type == 7u) source->have_sps = true;
            else source->have_pps = true;
        }
        if (type == 7u && nal_length >= 4u) {
            uint16_t width = 0, height = 0;
            if (media_h264_sps_dimensions(
                    data + nal_start, nal_length, &width, &height)) {
                if (source->video_info_valid
                    && (source->video_info.width != width
                        || source->video_info.height != height)) {
                    hls_source_fail(
                        source,
                        "HLS resolution changed inside the selected stream");
                    return false;
                }
                source->video_info = (MediaMp4TrackInfo) {
                    .kind = MEDIA_MP4_TRACK_VIDEO,
                    .codec = MEDIA_MP4_FOURCC('a','v','c','1'),
                    .timescale = 90000u,
                    .largest_sample = SWDEC_TS_MAX_AU,
                    .width = width,
                    .height = height,
                    .codec_config = source->parameter_sets,
                    .codec_config_length = source->parameter_set_bytes,
                    .packet_format = MEDIA_PACKET_FORMAT_H264_ANNEX_B
                };
                source->video_info_valid = true;
                found_sps = true;
            }
        }
        cursor = next;
    }
    if (source->video_info_valid) {
        source->video_info.codec_config_length = source->parameter_set_bytes;
        source->video_info.codec_config = source->parameter_sets;
    }
    return found_sps;
}

static bool hls_adts_info(const unsigned char *data, size_t length,
                          MediaAacStreamInfo *info)
{
    static const uint32_t rates[] = {
        96000, 88200, 64000, 48000, 44100, 32000, 24000,
        22050, 16000, 12000, 11025, 8000, 7350
    };
    if (data == NULL || length < 7u || data[0] != 0xffu
        || (data[1] & 0xf0u) != 0xf0u) return false;
    unsigned profile = (data[2] >> 6) & 3u;
    unsigned rate_index = (data[2] >> 2) & 0x0fu;
    unsigned channels = ((data[2] & 1u) << 2u) | (data[3] >> 6u);
    if (profile != 1u || rate_index >= sizeof(rates) / sizeof(rates[0])
        || channels == 0 || channels > 2u) return false;
    *info = (MediaAacStreamInfo) {
        .sample_rate = rates[rate_index],
        .channels = (uint16_t) channels,
        .samples_per_frame = (uint16_t) (1024u * ((data[6] & 3u) + 1u))
    };
    return true;
}

static void hls_video_callback(void *opaque, const uint8_t *data,
                               size_t length, uint64_t pts90k)
{
    MediaHlsSource *source = opaque;
    if (source->track_selection == MEDIA_HLS_TRACK_AUDIO) return;
    bool keyframe = false;
    if (!hls_annexb_info(source, data, length, &keyframe)) {
        /* A live-edge segment may begin between parameter-set repetitions.
           Such access units cannot be submitted to either decoder and must
           not fill the bounded queue while prime waits for an SPS. Begin the
           retained timeline at the first independently decodable unit. */
        return;
    }
    if (!source->segment_origin_valid && pts90k != SWDEC_TS_NOPTS) {
        source->segment_origin_valid = true;
        source->segment_origin90k = pts90k;
    }
    if (pts90k != SWDEC_TS_NOPTS && source->segment_origin_valid)
        pts90k = pts90k >= source->segment_origin90k
            ? source->segment_base90k
                + pts90k - source->segment_origin90k
            : source->segment_base90k;
    if (pts90k == SWDEC_TS_NOPTS) {
        pts90k = source->last_video_pts90k == 0
            ? source->segment_base90k
            : source->last_video_pts90k + 3600u;
    }
    uint32_t duration = source->last_video_pts90k != 0
        && pts90k > source->last_video_pts90k
        && pts90k - source->last_video_pts90k <= UINT32_MAX
            ? (uint32_t) (pts90k - source->last_video_pts90k) : 3600u;
    source->last_video_pts90k = pts90k;
    (void) hls_queue_push(
        source, MEDIA_MP4_TRACK_VIDEO, MEDIA_PACKET_FORMAT_H264_ANNEX_B,
        data, length, pts90k, duration, keyframe);
}

static void hls_audio_callback(void *opaque, const uint8_t *data,
                               size_t length, uint64_t pts90k)
{
    MediaHlsSource *source = opaque;
    if (source->track_selection == MEDIA_HLS_TRACK_VIDEO) return;
    /* Track topology is fixed when stream_info is handed to the backend. A
       genuinely late audio PID cannot be added to that live pipeline; drop
       it rather than consuming the video queue with packets no backend owns. */
    if (source->track_layout_known && !source->audio_info_valid) return;
    MediaAacStreamInfo info = {0};
    bool valid_info = hls_adts_info(data, length, &info);
    if (!source->audio_info_valid && valid_info) {
        source->audio_info = (MediaMp4TrackInfo) {
            .kind = MEDIA_MP4_TRACK_AUDIO,
            .codec = MEDIA_MP4_FOURCC('m','p','4','a'),
            .timescale = 90000u,
            .largest_sample = SWDEC_TS_MAX_ADTS,
            .channels = info.channels,
            .sample_rate = info.sample_rate,
            .packet_format = MEDIA_PACKET_FORMAT_AAC_ADTS
        };
        source->audio_info_valid = true;
        source->audio_duration90k =
            (uint32_t) info.samples_per_frame * 90000u / info.sample_rate;
    }
    uint32_t duration = source->audio_duration90k != 0
        ? source->audio_duration90k : valid_info
        ? (uint32_t) info.samples_per_frame * 90000u / info.sample_rate
        : 2090u;
    if (!source->segment_origin_valid && pts90k != SWDEC_TS_NOPTS) {
        source->segment_origin_valid = true;
        source->segment_origin90k = pts90k;
    }
    if (pts90k != SWDEC_TS_NOPTS && source->segment_origin_valid)
        pts90k = pts90k >= source->segment_origin90k
            ? source->segment_base90k
                + pts90k - source->segment_origin90k
            : source->segment_base90k;
    if (pts90k == SWDEC_TS_NOPTS) {
        pts90k = source->last_audio_pts90k == 0
            ? source->segment_base90k
            : source->last_audio_pts90k + duration;
    }
    source->last_audio_pts90k = pts90k;
    (void) hls_queue_push(
        source, MEDIA_MP4_TRACK_AUDIO, MEDIA_PACKET_FORMAT_AAC_ADTS,
        data, length, pts90k, duration, false);
}

static bool hls_feed_adts_segment(MediaHlsSource *source, size_t total)
{
    size_t cursor = 0;
    while (cursor + 7u <= total) {
        MediaAacStreamInfo info = {0};
        const unsigned char *frame = source->transport_chunk + cursor;
        if (!hls_adts_info(frame, total - cursor, &info)) {
            cursor++;
            continue;
        }
        size_t frame_length = ((size_t) (frame[3] & 3u) << 11u)
                            | ((size_t) frame[4] << 3u)
                            | ((size_t) frame[5] >> 5u);
        if (frame_length < 7u || frame_length > SWDEC_TS_MAX_ADTS) {
            cursor++;
            continue;
        }
        if (frame_length > total - cursor) break;
        /* A raw-audio response can contain hundreds of tiny frames. Retain
           the unparsed suffix until the consumer makes room rather than
           converting a healthy live rendition into a queue-overflow error. */
        if (source->queue_count >= HLS_SAMPLE_LIMIT
            || frame_length > source->queue_capacity - source->queue_used)
            break;
        hls_audio_callback(source, frame, frame_length, SWDEC_TS_NOPTS);
        if (source->failed) return false;
        cursor += frame_length;
    }
    size_t tail = total - cursor;
    if (tail > source->transport_tail_capacity) {
        hls_source_fail(source, "HLS ADTS tail exceeded its bound");
        return false;
    }
    if (tail != 0)
        memmove(source->transport_chunk,
                source->transport_chunk + cursor, tail);
    source->transport_tail = tail;
    return true;
}

static void hls_cancel_requests(MediaHlsSource *source)
{
    for (size_t i = 0; i < 2u; i++) {
        if (source->requests[i].handle != 0)
            source->transport.cancel(
                source->transport.opaque, source->requests[i].handle);
        source->requests[i] = (HlsRequest) {0};
    }
}

static bool hls_start_request(MediaHlsSource *source, size_t request_slot,
                              size_t segment)
{
    if (segment >= source->playlist->entry_count) return true;
    const HlsEntry *entry = &source->playlist->entries[segment];
    char url[4096];
    if (!tilefinch_url_resolve(
            source->playlist->base_url,
            source->playlist->text + entry->text_offset,
            url, sizeof(url))) {
        hls_source_fail(source, "HLS segment URL is invalid");
        return false;
    }
    char error[160] = {0};
    uint64_t handle = source->transport.start(
        source->transport.opaque, url, MEDIA_HLS_MAXIMUM_SEGMENT_BYTES,
        error, sizeof(error));
    if (handle == 0) {
        if (error[0] != '\0') hls_source_fail(source, "%s", error);
        return false;
    }
    source->requests[request_slot] = (HlsRequest) {
        .handle = handle,
        .segment = segment
    };
    source->stats.segments_started++;
    return true;
}

static void hls_prepare_segment(MediaHlsSource *source, size_t segment)
{
    if (source->prepared_segment == segment) return;
    if (source->playlist->entries[segment].discontinuity) {
        swdec_ts_flush(source->ts);
        swdec_ts_init(
            source->ts, hls_video_callback, hls_audio_callback, source);
        source->transport_tail = 0;
    }
    if (source->live_session) {
        source->segment_base90k = source->live_timeline90k;
    } else {
        uint32_t start_ms = source->playlist->entries[segment].start_ms;
        source->segment_base90k =
            (uint64_t) (start_ms / 1000u) * UINT64_C(90000)
            + (uint32_t) (start_ms % 1000u) * 90u;
    }
    source->segment_origin90k = 0;
    source->segment_origin_valid = false;
    if (source->track_selection == MEDIA_HLS_TRACK_AUDIO)
        source->audio_segment_kind = HLS_AUDIO_SEGMENT_UNKNOWN;
    source->prepared_segment = segment;
}

static size_t hls_sequence_index(
    const MediaHlsPlaylist *playlist, uint64_t sequence)
{
    if (playlist == NULL) return SIZE_MAX;
    for (size_t i = 0; i < playlist->entry_count; i++)
        if (playlist->entries[i].sequence >= sequence) return i;
    return SIZE_MAX;
}

static bool hls_apply_pending_playlist(MediaHlsSource *source)
{
    if (source == NULL || source->pending_playlist == NULL
        || source->requests[0].handle != 0
        || source->requests[1].handle != 0) return false;
    MediaHlsPlaylist *replacement = source->pending_playlist;
    size_t selected = hls_sequence_index(replacement, source->next_sequence);
    if (selected == SIZE_MAX) {
        /* A refresh is allowed to be unchanged while the current edge is
           still being consumed. It is not allowed to move the cursor
           backwards; discard it and ask again at the target-duration pace. */
        source->pending_playlist = NULL;
        media_hls_playlist_destroy(replacement);
        return false;
    }
    uint64_t selected_sequence = replacement->entries[selected].sequence;
    bool advanced = selected_sequence > source->next_sequence;
    if (advanced) {
        uint64_t skipped = replacement->entries[selected].sequence
            - source->next_sequence;
        source->stats.skipped_live_segments =
            skipped > SIZE_MAX - source->stats.skipped_live_segments
                ? SIZE_MAX
                : source->stats.skipped_live_segments + (size_t) skipped;
        replacement->entries[selected].discontinuity = true;
    }
    media_hls_playlist_destroy(source->playlist);
    source->playlist = replacement;
    source->pending_playlist = NULL;
    source->segment_index = selected;
    source->next_sequence = selected_sequence;
    source->prepared_segment = SIZE_MAX;
    source->ended = false;
    source->force_refresh = false;
    /* A healthy manifest that still points at the same persistently failing
       segment is not delivery progress. Preserve the retry/stall incident
       until the live edge actually moves beyond that sequence. */
    if (advanced && (!source->failed_sequence_valid
                     || selected_sequence > source->failed_sequence)) {
        source->live_segment_failures = 0;
        source->failed_sequence_valid = false;
        source->live_edge_since_us = 0;
    }
    return true;
}

static void hls_complete_segment(MediaHlsSource *source)
{
    /* A TS segment is an independent delivery boundary. Publish its final
       access unit while the current segment's timestamp base is still live;
       otherwise that AU is emitted only after prepare_segment installs the
       next base and appears to jump at every boundary. */
    if (source->audio_segment_kind != HLS_AUDIO_SEGMENT_ADTS)
        swdec_ts_flush(source->ts);
    else
        source->transport_tail = 0;
    source->stats.segments_completed++;
    source->live_segment_failures = 0;
    source->failed_sequence_valid = false;
    source->live_edge_since_us = 0;
    const HlsEntry *completed =
        &source->playlist->entries[source->segment_index];
    if (source->live_session) {
        uint64_t increment = (uint64_t) completed->duration_ms * 90u;
        source->live_timeline90k =
            increment > UINT64_MAX - source->live_timeline90k
                ? UINT64_MAX : source->live_timeline90k + increment;
        source->next_sequence = completed->sequence == UINT64_MAX
            ? UINT64_MAX : completed->sequence + 1u;
    }
    source->segment_index++;
    source->requests[0] = (HlsRequest) {0};
    if (source->requests[1].handle != 0
        && source->requests[1].segment == source->segment_index) {
        source->requests[0] = source->requests[1];
        source->requests[1] = (HlsRequest) {0};
    }
    (void) hls_apply_pending_playlist(source);
    if (source->segment_index >= source->playlist->entry_count)
        source->ended = source->playlist->end_list;
}

static void hls_pump(MediaHlsSource *source, uint64_t now_us)
{
    if (source == NULL || source->failed || source->ended) return;
    (void) hls_apply_pending_playlist(source);
    if (source->segment_index >= source->playlist->entry_count
        || (source->live_session && source->force_refresh)) {
        if (source->live_session) {
            if (source->live_edge_since_us == 0)
                source->live_edge_since_us = now_us;
            if (now_us != 0 && source->live_edge_since_us != 0
                && now_us >= source->live_edge_since_us
                && now_us - source->live_edge_since_us
                     >= HLS_LIVE_STALL_LIMIT_US) {
                hls_source_fail(source,
                                "live HLS playlist stopped advancing");
            }
        }
        return;
    }
    if (source->requests[0].handle == 0) {
        (void) hls_start_request(source, 0u, source->segment_index);
        if (source->requests[0].handle == 0) return;
    }
    if (source->request_limit > 1u
        && source->requests[1].handle == 0
        && source->segment_index + 1u < source->playlist->entry_count
        && (!source->live_session
            || source->segment_index + HLS_LIVE_REFRESH_RUNWAY_SEGMENTS + 1u
                 < source->playlist->entry_count)) {
        (void) hls_start_request(source, 1u, source->segment_index + 1u);
    }
    hls_prepare_segment(source, source->segment_index);
    size_t parse_capacity = source->audio_segment_kind
            == HLS_AUDIO_SEGMENT_ADTS
        ? HLS_ADTS_PARSE_SLICE_BYTES : HLS_TS_PARSE_SLICE_BYTES;
    size_t length = 0;
    char error[160] = {0};
    MediaHlsTransportPollResult result = source->transport.poll(
        source->transport.opaque, source->requests[0].handle,
        source->transport_chunk + source->transport_tail,
        parse_capacity, &length, error, sizeof(error));
    if (result == MEDIA_HLS_TRANSPORT_WAIT) return;
    if (result == MEDIA_HLS_TRANSPORT_ERROR) {
        uint64_t failed_sequence = source->playlist->entries[
            source->segment_index].sequence;
        source->requests[0] = (HlsRequest) {0};
        if (source->live_session && source->live_segment_failures < 3u) {
            if (source->requests[1].handle != 0)
                source->transport.cancel(
                    source->transport.opaque,
                    source->requests[1].handle);
            source->requests[1] = (HlsRequest) {0};
            source->live_segment_failures++;
            source->failed_sequence = failed_sequence;
            source->failed_sequence_valid = true;
            source->force_refresh = true;
            source->refresh_next_us = 0;
            if (source->live_edge_since_us == 0)
                source->live_edge_since_us = now_us;
        } else {
            hls_source_fail(
                source, "%s", error[0] == '\0'
                    ? "HLS segment fetch failed" : error);
        }
        return;
    }
    if (result == MEDIA_HLS_TRANSPORT_CHUNK) {
        if (length == 0 || length > parse_capacity) {
            hls_source_fail(source, "HLS transport returned an invalid chunk");
            return;
        }
        source->stats.bytes_received = length
                > SIZE_MAX - source->stats.bytes_received
            ? SIZE_MAX : source->stats.bytes_received + length;
        size_t total = source->transport_tail + length;
        if (source->track_selection == MEDIA_HLS_TRACK_AUDIO
            && source->audio_segment_kind == HLS_AUDIO_SEGMENT_UNKNOWN) {
            source->audio_segment_kind = total != 0
                    && source->transport_chunk[0] == 0x47u
                ? HLS_AUDIO_SEGMENT_TS : HLS_AUDIO_SEGMENT_ADTS;
        }
        if (source->audio_segment_kind == HLS_AUDIO_SEGMENT_ADTS) {
            if (!hls_feed_adts_segment(source, total)) return;
        } else {
            int tail = swdec_ts_feed(
                source->ts, source->transport_chunk, total);
            if (tail < 0 || (size_t) tail >= 188u
                || (size_t) tail > total) {
                hls_source_fail(source, "HLS TS tail exceeded its bound");
                return;
            }
            source->transport_tail = (size_t) tail;
            if (source->transport_tail != 0) {
                memmove(
                    source->transport_chunk,
                    source->transport_chunk
                        + total - source->transport_tail,
                    source->transport_tail);
            }
        }
    } else if (result == MEDIA_HLS_TRANSPORT_COMPLETE) {
        if (source->transport_tail != 0
            && source->audio_segment_kind != HLS_AUDIO_SEGMENT_ADTS) {
            source->stats.malformed_segments++;
            hls_source_fail(source, "HLS segment ended inside a TS packet");
            return;
        }
        hls_complete_segment(source);
    }
    source->stats.ts_sync_losses = source->ts->stats.sync_losses;
    source->stats.ts_malformed_packets = source->ts->stats.malformed_packets;
    source->stats.ts_malformed_psi = source->ts->stats.malformed_psi;
}

MediaHlsSource *media_hls_source_create(
    Budget *budget, MediaHlsPlaylist *playlist,
    const MediaHlsTransport *transport, char *error, size_t error_size)
{
    return media_hls_source_create_track(
        budget, playlist, transport, MEDIA_HLS_TRACK_MIXED,
        error, error_size);
}

MediaHlsSource *media_hls_source_create_track(
    Budget *budget, MediaHlsPlaylist *playlist,
    const MediaHlsTransport *transport, MediaHlsTrackSelection selection,
    char *error, size_t error_size)
{
    if (error != NULL && error_size != 0) error[0] = '\0';
    if (budget == NULL || playlist == NULL
        || playlist->kind != MEDIA_HLS_PLAYLIST_MEDIA
        || selection > MEDIA_HLS_TRACK_AUDIO
        || transport == NULL || transport->start == NULL
        || transport->poll == NULL || transport->cancel == NULL) {
        hls_error(error, error_size, "invalid HLS source");
        return NULL;
    }
    MediaHlsSource *source = budget_calloc_category(
        budget, BUDGET_CATEGORY_NAVIGATION, 1, sizeof(*source));
    SwdecTs *ts = budget_malloc_category(
        budget, BUDGET_CATEGORY_RESOURCE, sizeof(*ts));
    size_t queue_capacity = selection == MEDIA_HLS_TRACK_AUDIO
        ? HLS_QUEUE_AUDIO_BYTES
        : selection == MEDIA_HLS_TRACK_VIDEO
            ? HLS_QUEUE_VIDEO_BYTES : HLS_QUEUE_MIXED_BYTES;
    unsigned char *queue = budget_malloc_category(
        budget, BUDGET_CATEGORY_RESOURCE, queue_capacity);
    size_t transport_tail_capacity = selection == MEDIA_HLS_TRACK_AUDIO
        ? SWDEC_TS_MAX_ADTS : 188u;
    unsigned char *chunk = budget_malloc_category(
        budget, BUDGET_CATEGORY_RESOURCE,
        MEDIA_HLS_TRANSPORT_CHUNK_BYTES + transport_tail_capacity);
    if (source == NULL || ts == NULL || queue == NULL || chunk == NULL) {
        budget_free(budget, chunk);
        budget_free(budget, queue);
        budget_free(budget, ts);
        budget_free(budget, source);
        hls_error(error, error_size, "HLS source exceeds memory budget");
        return NULL;
    }
    source->budget = budget;
    source->playlist = playlist;
    source->transport = *transport;
    source->ts = ts;
    source->queue_bytes = queue;
    source->queue_capacity = queue_capacity;
    source->transport_chunk = chunk;
    source->transport_tail_capacity = transport_tail_capacity;
    source->prepared_segment = SIZE_MAX;
    source->seek_segment = SIZE_MAX;
    source->live_session = !playlist->end_list;
    source->stats.live = source->live_session;
    source->track_selection = selection;
    source->request_limit = selection == MEDIA_HLS_TRACK_MIXED ? 2u : 1u;
    if (source->live_session) {
        source->segment_index = playlist->entry_count > HLS_LIVE_EDGE_SEGMENTS
            ? playlist->entry_count - HLS_LIVE_EDGE_SEGMENTS : 0u;
        source->next_sequence =
            playlist->entries[source->segment_index].sequence;
        source->seek_pristine = true;
    }
    swdec_ts_init(ts, hls_video_callback, hls_audio_callback, source);
    return source;
}

MediaHlsPrimeStatus media_hls_source_prime(
    MediaHlsSource *source, char *error, size_t error_size)
{
    if (error != NULL && error_size != 0) error[0] = '\0';
    if (source == NULL) {
        hls_error(error, error_size, "HLS source unavailable");
        return MEDIA_HLS_PRIME_FAILED;
    }
    if (source->failed) {
        hls_error(error, error_size, "%s", source->error);
        return MEDIA_HLS_PRIME_FAILED;
    }
    /* PMT parsing is atomic for this minimal single-program demuxer: once a
       video PID is known, audio_pid == -1 means the same PMT contained no AAC
       stream. This lets video-only VOD prime after its first picture instead
       of buffering the whole segment into a 64-sample queue. */
    bool layout_known = source->track_selection == MEDIA_HLS_TRACK_VIDEO
        || source->track_layout_known
        || source->audio_info_valid
        || (source->ts->video_pid >= 0 && source->ts->audio_pid < 0);
    bool ready = source->track_selection == MEDIA_HLS_TRACK_AUDIO
        ? source->audio_info_valid
            && hls_queue_has_kind(source, MEDIA_MP4_TRACK_AUDIO)
        : source->video_info_valid && layout_known
            && hls_queue_has_kind(source, MEDIA_MP4_TRACK_VIDEO);
    if (ready) {
        source->track_layout_known = true;
        return MEDIA_HLS_PRIME_READY;
    }
    hls_pump(source, 0);
    if (source->failed) {
        hls_error(error, error_size, "%s", source->error);
        return MEDIA_HLS_PRIME_FAILED;
    }
    layout_known = source->track_selection == MEDIA_HLS_TRACK_VIDEO
        || source->track_layout_known
        || source->audio_info_valid
        || (source->ts->video_pid >= 0 && source->ts->audio_pid < 0);
    ready = source->track_selection == MEDIA_HLS_TRACK_AUDIO
        ? source->audio_info_valid
            && hls_queue_has_kind(source, MEDIA_MP4_TRACK_AUDIO)
        : source->video_info_valid && layout_known
            && hls_queue_has_kind(source, MEDIA_MP4_TRACK_VIDEO);
    if (ready) {
        source->track_layout_known = true;
    }
    return ready ? MEDIA_HLS_PRIME_READY : MEDIA_HLS_PRIME_PENDING;
}

void media_hls_source_pump(MediaHlsSource *source, uint64_t now_us)
{
    if (source == NULL || source->failed || source->ended) return;
    /* One 16 KiB TS chunk can publish several audio and video samples. Leave
       enough descriptor and payload headroom before producing proactively;
       starvation reads may still pump immediately from an empty queue. */
    if (source->queue_count <= 16u
        && source->queue_used <= source->queue_capacity / 2u)
        hls_pump(source, now_us);
}

bool media_hls_source_wants_playlist_refresh(
    const MediaHlsSource *source, uint64_t now_us)
{
    if (source == NULL || source->failed || !source->live_session
        || source->ended || source->playlist->end_list
        || source->pending_playlist != NULL
        || now_us < source->refresh_next_us) return false;
    return source->force_refresh
        || source->segment_index + HLS_LIVE_REFRESH_RUNWAY_SEGMENTS
             >= source->playlist->entry_count;
}

bool media_hls_source_update_playlist(
    MediaHlsSource *source, MediaHlsPlaylist *replacement,
    uint64_t now_us, char *error, size_t error_size)
{
    if (error != NULL && error_size != 0) error[0] = '\0';
    if (source == NULL || replacement == NULL
        || replacement->kind != MEDIA_HLS_PLAYLIST_MEDIA
        || !source->live_session) {
        media_hls_playlist_destroy(replacement);
        hls_error(error, error_size, "invalid live HLS refresh");
        return false;
    }
    source->stats.playlist_refreshes++;
    media_hls_playlist_destroy(source->pending_playlist);
    source->pending_playlist = replacement;
    uint64_t delay = (uint64_t) replacement->target_duration_ms * 500u;
    if (delay < HLS_LIVE_REFRESH_MINIMUM_US)
        delay = HLS_LIVE_REFRESH_MINIMUM_US;
    source->refresh_next_us = delay > UINT64_MAX - now_us
        ? UINT64_MAX : now_us + delay;
    (void) hls_apply_pending_playlist(source);
    return true;
}

void media_hls_source_note_playlist_refresh_failure(
    MediaHlsSource *source, uint64_t now_us)
{
    if (source == NULL || !source->live_session || source->failed) return;
    source->stats.playlist_refresh_failures++;
    source->refresh_next_us = now_us > UINT64_MAX - HLS_LIVE_REFRESH_MINIMUM_US
        ? UINT64_MAX : now_us + HLS_LIVE_REFRESH_MINIMUM_US;
    if (source->live_edge_since_us == 0) source->live_edge_since_us = now_us;
}

static size_t hls_track_count(const void *opaque)
{
    const MediaHlsSource *source = opaque;
    if (source == NULL) return 0;
    if (source->track_selection == MEDIA_HLS_TRACK_VIDEO)
        return source->video_info_valid ? 1u : 0u;
    if (source->track_selection == MEDIA_HLS_TRACK_AUDIO)
        return source->audio_info_valid ? 1u : 0u;
    return (source->video_info_valid ? 1u : 0u)
        + (source->audio_info_valid ? 1u : 0u);
}

static bool hls_track_info(const void *opaque, size_t index,
                           MediaMp4TrackInfo *info)
{
    const MediaHlsSource *source = opaque;
    if (source == NULL || info == NULL) return false;
    bool audio_first = source->track_selection == MEDIA_HLS_TRACK_AUDIO;
    if (index == 0 && !audio_first && source->video_info_valid) {
        *info = source->video_info;
        info->duration = source->live_session ? 0
            : (source->playlist->duration_us / 1000000u) * 90000u
            + ((source->playlist->duration_us % 1000000u) * 90000u)
                / 1000000u;
        return true;
    }
    if (((index == 1 && !audio_first) || (index == 0 && audio_first))
        && source->audio_info_valid) {
        *info = source->audio_info;
        info->duration = source->live_session ? 0
            : (source->playlist->duration_us / 1000000u) * 90000u
            + ((source->playlist->duration_us % 1000000u) * 90000u)
                / 1000000u;
        return true;
    }
    return false;
}

static bool hls_next_sample(void *opaque, MediaMp4Sample *sample)
{
    MediaHlsSource *source = opaque;
    if (source == NULL || sample == NULL || source->failed) return false;
    if (source->queue_count == 0) hls_pump(source, 0);
    if (source->queue_count == 0) return false;
    const HlsQueuedSample *queued = &source->queue[source->queue_head];
    uint64_t dts = queued->raw_pts90k;
    *sample = (MediaMp4Sample) {
        .track_index = queued->kind == MEDIA_MP4_TRACK_AUDIO
            && source->track_selection == MEDIA_HLS_TRACK_MIXED ? 1u : 0u,
        .kind = queued->kind,
        .offset = queued->identity,
        .size = queued->payload_length,
        .dts = dts,
        .pts = dts > INT64_MAX ? INT64_MAX : (int64_t) dts,
        .duration = queued->duration90k,
        .timescale = 90000u,
        .keyframe = queued->keyframe,
        .packet_format = queued->packet_format
    };
    return true;
}

static bool hls_last_error(const void *opaque, char *error, size_t error_size)
{
    const MediaHlsSource *source = opaque;
    if (source == NULL || !source->failed) return false;
    hls_error(error, error_size, "%s", source->error);
    return true;
}

static bool hls_would_block(const void *opaque)
{
    const MediaHlsSource *source = opaque;
    return source != NULL && !source->failed && !source->ended
        && source->queue_count == 0;
}

static bool hls_sample_resident(const void *opaque,
                                const MediaMp4Sample *sample)
{
    const MediaHlsSource *source = opaque;
    return source != NULL && sample != NULL && source->queue_count != 0
        && source->queue[source->queue_head].identity == sample->offset;
}

static bool hls_read_sample(void *opaque, const MediaMp4Sample *sample,
                            void *destination, size_t capacity)
{
    MediaHlsSource *source = opaque;
    if (!hls_sample_resident(source, sample) || destination == NULL)
        return false;
    HlsQueuedSample *queued = &source->queue[source->queue_head];
    if (queued->payload_length > capacity) {
        hls_source_fail(source, "HLS sample exceeds packet buffer");
        return false;
    }
    size_t first = source->queue_capacity - queued->payload_offset;
    if (first > queued->payload_length) first = queued->payload_length;
    memcpy(destination, source->queue_bytes + queued->payload_offset, first);
    memcpy((unsigned char *) destination + first, source->queue_bytes,
           queued->payload_length - first);
    source->queue_read = (queued->payload_offset + queued->payload_length)
        % source->queue_capacity;
    source->queue_used -= queued->payload_length;
    source->queue_head = (source->queue_head + 1u) % HLS_SAMPLE_LIMIT;
    source->queue_count--;
    source->seek_pristine = false;
    source->stats.queued_samples = source->queue_count;
    source->stats.queued_bytes = source->queue_used;
    return true;
}

static void hls_reset_queue(MediaHlsSource *source)
{
    source->queue_head = 0;
    source->queue_count = 0;
    source->queue_read = 0;
    source->queue_write = 0;
    source->queue_used = 0;
    source->stats.queued_samples = 0;
    source->stats.queued_bytes = 0;
}

static size_t hls_segment_for_us(const MediaHlsSource *source,
                                 uint64_t target_us, bool strictly_after,
                                 uint64_t *actual_us)
{
    uint64_t cursor = 0;
    size_t selected = 0;
    for (size_t i = 0; i < source->playlist->entry_count; i++) {
        uint64_t next = cursor
            + (uint64_t) source->playlist->entries[i].duration_ms * 1000u;
        if (strictly_after ? cursor > target_us : next > target_us) {
            selected = i;
            break;
        }
        selected = i;
        cursor = next;
    }
    if (strictly_after && cursor <= target_us
        && selected + 1u < source->playlist->entry_count) {
        cursor += (uint64_t) source->playlist->entries[selected].duration_ms
            * 1000u;
        selected++;
    }
    if (actual_us != NULL) *actual_us = cursor;
    return selected;
}

static bool hls_seek_common(void *opaque, uint64_t target_us,
                            bool strictly_after, uint64_t *actual_us)
{
    MediaHlsSource *source = opaque;
    if (source == NULL || source->failed) return false;
    /* MediaPlayback probes zero while constructing and priming a source. A
       rolling window has no stable random-access timeline, but that initial
       no-op must succeed so the ordinary player can adopt the already-primed
       live edge. User seeking remains disabled and every later seek fails. */
    if (source->live_session) {
        if (actual_us != NULL) *actual_us = 0;
        return target_us == 0 && !strictly_after && source->seek_pristine;
    }
    size_t segment = hls_segment_for_us(
        source, target_us, strictly_after, actual_us);
    /* Priming repeatedly proves and restores the same source position. HLS
       seek is otherwise destructive (it cancels segment fetches), so preserve
       an untouched position until a sample is actually consumed. A later
       user seek within the same segment still resets because read_sample
       clears seek_pristine. */
    if (source->seek_pristine && source->seek_segment == segment) return true;
    hls_cancel_requests(source);
    hls_reset_queue(source);
    swdec_ts_init(
        source->ts, hls_video_callback, hls_audio_callback, source);
    source->segment_index = segment;
    source->prepared_segment = SIZE_MAX;
    source->transport_tail = 0;
    source->ended = false;
    source->seek_pristine = true;
    source->seek_segment = segment;
    source->last_video_pts90k = 0;
    source->last_audio_pts90k = 0;
    return true;
}

static bool hls_seek_us(void *opaque, uint64_t target_us,
                        uint64_t *actual_us)
{
    return hls_seek_common(opaque, target_us, false, actual_us);
}

static bool hls_seek_after_us(void *opaque, uint64_t target_us,
                              uint64_t *actual_us)
{
    return hls_seek_common(opaque, target_us, true, actual_us);
}

static void hls_rewind(void *opaque)
{
    MediaHlsSource *source = opaque;
    if (source != NULL && source->live_session) return;
    (void) hls_seek_common(source, 0, false, NULL);
}

static size_t hls_retained_bytes(const void *opaque)
{
    const MediaHlsSource *source = opaque;
    if (source == NULL) return 0;
    return sizeof(*source) + sizeof(*source->ts) + source->queue_capacity
        + MEDIA_HLS_TRANSPORT_CHUNK_BYTES
        + source->transport_tail_capacity
        + sizeof(*source->playlist) + source->playlist->text_length + 1u
        + (source->pending_playlist == NULL ? 0
            : sizeof(*source->pending_playlist)
              + source->pending_playlist->text_length + 1u);
}

static const MediaSampleSourceOps hls_source_ops = {
    .track_count = hls_track_count,
    .track_info = hls_track_info,
    .next_sample = hls_next_sample,
    .last_error = hls_last_error,
    .would_block = hls_would_block,
    .sample_resident = hls_sample_resident,
    .read_sample_waiting = hls_read_sample,
    .read_sample = hls_read_sample,
    .seek_us = hls_seek_us,
    .seek_after_us = hls_seek_after_us,
    .rewind = hls_rewind,
    .retained_bytes = hls_retained_bytes
};

bool media_hls_source_sample_source(
    MediaHlsSource *source, MediaSampleSource *sample_source)
{
    if (source == NULL || sample_source == NULL) return false;
    *sample_source = (MediaSampleSource) {
        .opaque = source,
        .ops = &hls_source_ops
    };
    return true;
}

bool media_hls_source_stream_info(
    const MediaHlsSource *source, MediaMp4TrackInfo *video,
    MediaMp4TrackInfo *audio)
{
    if (source == NULL || (video == NULL && audio == NULL)) return false;
    if (video != NULL) {
        if (!source->video_info_valid) return false;
        *video = source->video_info;
    }
    if (audio != NULL) {
        if (source->track_selection == MEDIA_HLS_TRACK_AUDIO
            && !source->audio_info_valid) return false;
        *audio = source->audio_info_valid ? source->audio_info
                                          : (MediaMp4TrackInfo) {0};
    }
    return true;
}

void media_hls_source_stats(const MediaHlsSource *source,
                            MediaHlsStats *stats)
{
    if (stats == NULL) return;
    *stats = source == NULL ? (MediaHlsStats) {0} : source->stats;
    if (source != NULL) {
        for (size_t i = 0; i < 2u; i++)
            if (source->requests[i].handle != 0) stats->active_requests++;
        uint64_t buffered90k = 0;
        for (size_t i = 0; i < source->queue_count; i++) {
            size_t at = (source->queue_head + i) % HLS_SAMPLE_LIMIT;
            uint64_t end = source->queue[at].raw_pts90k;
            if (source->queue[at].duration90k
                    <= UINT64_MAX - end)
                end += source->queue[at].duration90k;
            if (end > buffered90k) buffered90k = end;
        }
        stats->buffered_until_us = buffered90k / 90u * 1000u
            + (buffered90k % 90u) * 1000u / 90u;
        stats->live = source->live_session;
        stats->ended = source->ended;
    }
}

bool media_hls_source_failed(const MediaHlsSource *source)
{
    return source != NULL && source->failed;
}

void media_hls_source_destroy(MediaHlsSource *source)
{
    if (source == NULL) return;
    hls_cancel_requests(source);
    Budget *budget = source->budget;
    media_hls_playlist_destroy(source->pending_playlist);
    media_hls_playlist_destroy(source->playlist);
    budget_free(budget, source->transport_chunk);
    budget_free(budget, source->queue_bytes);
    budget_free(budget, source->ts);
    budget_free(budget, source);
}
